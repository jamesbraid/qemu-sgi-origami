/* SPDX-License-Identifier: BSD-3-Clause */
/* PsiTech RAD1 PCI graphics head on an Origin 200 Bridge slot. */
/*
 * Primary PCI Vendor/Device ID is 3d3d:0009, the 3Dlabs-badged TI/3Dlabs
 * TVP4020 (Permedia 2) die's own identity,
 * not PsiTech's 12d1:0101 board identity. IRIX's kernel driver registers
 * against 3d3d:0009 and, separately,
 * 104c:3d07 (the same chip under TI's own vendor ID), never against
 * 12d1:0101. IRIX's PCI bus-probe path matches
 * a live card's primary Vendor/Device ID against exactly that registered
 * table, so a card presenting 12d1:0101 as
 * its primary identity is never claimed by any driver. The IO6 PROM's RAD1
 * graphics-console installer dispatch
 * matches on the raw 16-bit PCI Device ID alone, with no Vendor ID check
 * anywhere in that dispatch: it recognizes
 * both device 0009 and device 3d07 (see the RAD1_VENDOR_ID/RAD1_DEVICE_ID
 * comment below), so either identity
 * reaches that installer equally. 3d3d:0009 is an approximation. 3d3d is
 * 3Dlabs' own PCI-SIG-registered vendor ID
 * and 0009 a device ID both the PROM and the IRIX kernel driver accept, but
 * which physical chip revision this
 * project's modeled RAD1 should present is not established. 12d1:0101 remains
 * real and is exposed separately as the
 * PCI Subsystem Vendor/Device ID (a different, standard PCI config-space
 * field from the primary Vendor/Device ID)
 * because it is PsiTech's own disclosed board identity, not because any
 * driver path is known to check it. The
 * standalone PROM textport library's base-address lookup has four entries; no
 * subsystem-ID check is established
 * anywhere in that driver.
 *
 * Unlike psitech-rad4.c, RAD1 is built directly on the TI/3Dlabs TVP4020
 * (Permedia2/Permedia2V) with 8 MB of
 * onboard SGRAM, an integrated RAMDAC, and no PLX bridge or external
 * RGB561-style RAMDAC. It has no PLX-style
 * scatter/gather DMA engine: every rectangle-move path the shipped driver
 * uses (`rect_pio_move`, `rect_dma_move`,
 * `rad1_dma_patch_move`, `bigx_update`) drives the same Permedia2
 * rasterizer/texture-read GP register set through
 * the FIFO-tagged block at Region Zero 0x8000-0xFFFF; what the driver calls
 * "DMA" is Permedia2's own texture-mapped
 * rectangle copy, using an ordinary kernel DMA mapping only to get a
 * device-visible host physical address for
 * TextureBaseAddress, not a separate descriptor-chain format.
 *
 * This models a bounded compatibility profile: the flat, non-interpolated
 * texture-read rectangle copy the shipped
 * `rad1pci` kernel driver performs; the constant-colour point and rectangle
 * fills both textports draw with (the IO6
 * PROM's console=g textport and the kernel's rad1_tport.o plot every glyph
 * pixel as a point primitive and every box
 * as a rectangle primitive, colour from FBWriteData under LogicalOpMode's
 * UseConstantFBWriteData, or from
 * FBBlockColor under FastFill; see rad1_do_point_render() and
 * rad1_do_rectangle_fill()); a solid trapezoid/span
 * rasterizer; a real RAMDAC palette write/read protocol; and acceptance of
 * the video-timing block. This is still
 * not full Permedia2 hardware fidelity: no 3D rendering, blending, logical
 * ops, texture filtering, bitmask/stipple
 * rendering, Color DDA fragment colours, line primitives,
 * decreasing-direction rectangle fills or mode switching is
 * implemented.
 *
 * Register offsets, bit layouts and semantics the driver does not directly
 * establish are cross-checked against the
 * public TI/3Dlabs TVP4020 (Permedia 2) Programmer's Reference Manual
 * (bitsavers.org); manual page numbers cited in
 * comments below are the PDF's own page count, not the document's printed
 * page numbers, which run about nine lower
 * throughout the register reference chapter.
 */

#include "qemu/osdep.h"
#include "qemu/bitops.h"
#include "qemu/log.h"
#include "qemu/module.h"
#include "qemu/units.h"
#include "hw/pci/pci_device.h"
#include "hw/pci/pci_ids.h"
#include "hw/core/resettable.h"
#include "migration/vmstate.h"
#include "qapi/error.h"
#include "ui/console.h"
#include "qom/object.h"

#define TYPE_PSITECH_RAD1 "psitech-rad1"
OBJECT_DECLARE_SIMPLE_TYPE(PsitechRad1State, PSITECH_RAD1)

/*
 * Primary identity: the 3Dlabs-badged Permedia 2 die, 3d3d:0009, one of two
 * identities the IRIX kernel driver's
 * boot-time registration accepts. The IO6 PROM's RAD1 graphics-console
 * installer dispatch matches on the raw 16-bit
 * PCI Device ID alone, with no Vendor ID check anywhere in that dispatch:
 * device 0009 is recognized, and device
 * 3d07 is recognized too, at several sites, each running the same substantive
 * RAMDAC/video-timing register setup as
 * the 0009 path before reaching the graphics-console installer routine.
 * Neither 3d3d nor 104c is checked as a PCI
 * Vendor ID anywhere in that dispatch. Contrast the same PROM's QLogic ISP
 * probe, which does check a
 * Vendor+Device pair, so the missing vendor check here is this dispatch's own
 * design. Presenting either 3d3d:0009
 * or 104c:3d07 therefore reaches the PROM's RAD1 console installer equally;
 * picking 3d3d:0009 is an approximation.
 * 3d3d is 3Dlabs' own PCI-SIG-registered vendor ID and 0009 a device ID
 * both the PROM and the IRIX kernel driver
 * accept, but which physical chip revision this project's modeled RAD1 should
 * present is not established; see the
 * public TI/3Dlabs TVP4020 (Permedia 2) Programmer's Reference Manual cited
 * in the file comment above. The kernel
 * driver's own attach is unaffected either way, since it registers against
 * both identities.
 */
#define RAD1_VENDOR_ID          0x3d3d
#define RAD1_DEVICE_ID          0x0009

/*
 * PsiTech's own board/subsystem identity (public 1999 product sheet), exposed
 * as the PCI Subsystem Vendor/Device ID
 * rather than the primary Vendor/Device ID because it is PsiTech's real,
 * disclosed board identity, not because
 * any driver path, including the standalone PROM textport library, is
 * established to check it. See the file comment
 * above.
 */
#define RAD1_SUBSYSTEM_VENDOR_ID 0x12d1
#define RAD1_SUBSYSTEM_DEVICE_ID 0x0101

/*
 * BAR sizing. Region Zero's 128 KiB size and its role (control/RAMDAC/
 * GP-register aperture) are established as the
 * driver's PCI selector 8. Which PCI base-address register the driver's own
 * selector numbering actually names for
 * the framebuffer (selector 9, most likely) and for the flash/EEPROM pointer
 * is not established. BAR1 for the
 * framebuffer and BAR2 for flash are this file's own minimum-reasonable
 * choice of PCI resource numbering to make a
 * guest attach possible, not a confirmed hardware fact.
 */
#define RAD1_REGZERO_SIZE       (128 * KiB)
#define RAD1_FB_SIZE            (8 * MiB)
#define RAD1_FLASH_SIZE         (64 * KiB)     /* Not a confirmed size. */

/*
 * Region Zero range table, matching the TI/3Dlabs Permedia 2 manual's own
 * Region Zero address map (Table 3.1, PDF
 * page 15). Control & Status, RAMDAC and GP Registers are established as
 * driver-reached. Memory Control and Video
 * Control each have one narrow slice modeled (RAD1_REG_BYPASS_WRITE_MASK at
 * 0x1100, and the video-timing block at
 * 0x3000-0x3078; see their own comments below); GP FIFO, Video Streams,
 * SVGA Control and Reserved remain
 * unimplemented and visible to guest error logging rather than guessed at.
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
 * Control & Status block. Offset 0x10 is the interrupt status word
 * rad1_inthandler() reads and write-clears; offset
 * 0x08 is a second register the same handler unconditionally clears
 * afterward. The public TI/3Dlabs TVP4020
 * Hardware Reference Manual, Issue 6 (see the Bypass DMA block comment below
 * for its full citation) names these two
 * offsets IntEnable (0x08, "3.2.2 Interrupt Enable Register", PDF page 34)
 * and IntFlags (0x10, "3.2.3 Interrupt
 * Flags", PDF page 35), each with a documented Bypass DMA Interrupt
 * Enable/Flag pair at bit 7; see RAD1_INT_BIT7
 * below. This model still treats offset 0x08 as a plain read/write register
 * with no independent side effect on IRQ
 * delivery (rad1_update_irq() does not gate on it). This is a known
 * simplification: gating delivery on an enable bit the
 * driver is not confirmed to set risks turning a working (if imprecise)
 * interrupt path into a silent one, so this
 * is left as a disclosed gap rather than "fixed" without evidence the driver
 * arms it first. Offset 0x18 is an
 * inferred "InFIFOSpace" location (the driver polls some counter here against
 * thresholds before GP-register writes;
 * the exact offset is not a named manual entry, only inferred from the
 * driver's own polling pattern): since this
 * model executes every GP-register batch synchronously with no real FIFO
 * depth limit, this register always reports
 * far more space than any threshold the driver checks, so the driver's
 * polling loop never stalls.
 */
#define RAD1_REG_CTRL_ACK       0x08
#define RAD1_REG_INT_STATUS     0x10
#define RAD1_REG_FIFO_SPACE     0x18
#define RAD1_FIFO_SPACE_READY   0x00010000u

/*
 * Offset 0x40: `rad1pciattach` reads this word through the mapped Region Zero
 * aperture itself, not a validity
 * check on the softc pointer for PCI selector 9 (the framebuffer BAR).
 * `rad1pciattach` converts the raw word to a
 * boolean and both (a) passes it as `rad1_pinit`'s second argument and (b)
 * branches on it directly to decide
 * whether attach continues toward
 * `pciio_intr_alloc`/`hwgraph_char_device_add` at all. Before this offset was
 * modeled, an unhandled read here fell through to the Control & Status
 * block's default handler and returned 0,
 * which `rad1pciattach` reads as false and aborts attach on. This directly
 * caused the missing `/hw` graphics vertex.
 *
 * Which Permedia2 Control & Status register (if any named one) actually lives
 * at this offset is not established,
 * and `rad1_pinit`'s own use of the flag (skipping one extra block of
 * GP-register zeroing when false) does not by
 * itself name the bit. This model returns a fixed nonzero word here,
 * sufficient for every known use (a plain
 * zero/nonzero gate, not a specific bit pattern) but not a confirmed hardware
 * value.
 */
#define RAD1_REG_ATTACH_GATE    0x40
#define RAD1_ATTACH_GATE_VALUE  0x00000001u

/*
 * Interrupt status bits rad1_inthandler() decodes: bit 0, bit 1 and bit 7
 * each signal a distinct softc wait-queue
 * slot. Which bit corresponds to which of rad1_dma_patch_move()'s four
 * completion paths for bit 0 and bit 1 is not
 * established. Bit 7 is established: the public TI/3Dlabs TVP4020 Hardware
 * Reference Manual, Issue 6 (see the
 * Bypass DMA block comment below for its full citation) names IntFlags bit 7
 * the "Bypass DMA Interrupt Flag"
 * (IntEnable bit 7, offset 0x08, its enable counterpart), matching this
 * bit's existing name and
 * rad1_do_bypass_dma()'s own use of it below. This model still has every
 * completed Render set all three known bits
 * together rather than discriminating between them (bit 7 is therefore
 * overloaded between Render and Bypass DMA
 * completion here, where real hardware would raise it only for the latter),
 * and interrupt delivery is unconditional
 * (no enable gate); see the note at RAD1_REG_CTRL_ACK above. Any other
 * status bit is unimplemented and left
 * visible in the status word rather than silently swallowed.
 */
#define RAD1_INT_BIT0           (1u << 0)
#define RAD1_INT_BIT1           (1u << 1)
#define RAD1_INT_BIT7           (1u << 7)
#define RAD1_INT_KNOWN_MASK     (RAD1_INT_BIT0 | RAD1_INT_BIT1 | RAD1_INT_BIT7)

/*
 * Bypass DMA block, Region Zero 0x00a0-0x00d8: a separate host<->local-memory
 * transfer engine from the FIFO-tagged
 * GP registers below. This is the register range behind a "write dma int
 * timed out" driver failure when its
 * completion interrupt (RAD1_INT_BIT7 above) is not delivered: the driver
 * posts a transfer through these six
 * registers, in offset order, then sleeps on that interrupt.
 *
 * Register names, offsets and bit layouts below come from the public
 * TI/3Dlabs TVP4020 Hardware Reference Manual,
 * Issue 6 (a distinct copy from the bitsavers Programmer's Reference Manual
 * this file's top comment already cites),
 * pages 48-52 of that PDF's own count, cross-checked against the shipped
 * kernel driver's own write-DMA posting
 * sequence. All six registers' section numbers and pages are cited
 * per-register below. A seventh, adjacent register
 * in the same manual family, ByDMAComplete (offset E8h, "3.2.25 Bypass DMA
 * Complete", page 52, a manual-trigger
 * "restart graphics processor" register distinct from ByDMAControl's own
 * RestartGP bit, plus a read-only
 * texture-invalid status bit) is not modeled: no driver access is known to
 * reach it, so this offset range stops at
 * ByDMAControl (D8h) and E8h is unimplemented, a disclosed gap rather than a
 * resolved "not needed."
 *
 * ByDMAAddress (A0h, "3.2.19 Bypass DMA Start Address", page 48): PCI source
 * address, the driver's own host
 * physical address. ByDMAStride (B8h, "3.2.20 Bypass DMA Stride", page 48):
 * source scanline stride in bytes, a
 * per-mode constant, distinct from the destination framebuffer's own mode
 * stride. ByDMAMemAddr (C0h, "3.2.21 Bypass
 * Memory Base Address", page 49): destination base address in Permedia2 local
 * memory, in texel units in bits 0-23. The driver computes this as base + row
 * * stride + col, matching the
 * manual's account once "texel units" is read
 * as a pixel count. This model's pixel is always RAD1_BYTES_PER_PIXEL bytes,
 * so a texel offset converts to a byte
 * offset by that fixed factor, the same approximation as TextureBaseAddress
 * above. ByDMASize (C8h, "3.2.22 Bypass
 * DMA Transfer Size", page 49): Width bits 0-11, Height bits 16-27, packed
 * the same RectangleSize-style way as
 * RAD1_REG_RECT_SIZE above, with narrower (12-bit) fields. ByDMAByteMask
 * (D0h, "3.2.23 Bypass DMA Byte Mask", page
 * 50): left edge mask bits 0-3, right edge mask bits 16-19, one bit per byte
 * lane of the edge pixel word. The
 * driver's only confirmed value is 0x000f000f (both edges fully open). This
 * model honors a closed lane bit as
 * "leave that destination byte unchanged", applied only to the first and last
 * column of a transfer
 * (RAD1_REG_FB_HW_WRITE_MASK's own masked-write style above, but at edges
 * rather than everywhere). Only the fully-open case is confirmed; any other
 * value's precise behavior is not
 * established. ByDMAControl (D8h, "3.2.24
 * Bypass DMA Control", pages 51-52): Mode bits 0-1, RestartGP bit 2, Data
 * format bits 3-5, Patch Type bits 6-7,
 * PP0-2 bits 8-16, the last MMIO write in the driver's own posting
 * sequence, read from a fixed softc field. This
 * model stores the value and triggers the transfer on every write, the same
 * "last write starts it" contract
 * RAD1_REG_RENDER already uses for the GP path; the individual
 * Mode/RestartGP/format/Patch Type/PP fields are
 * stored but not separately applied, matching this file's own established
 * treatment of dSdx/dSdyDom/dTdx above,
 * since the qualified guest workload (a flat write-DMA completion) does not
 * exercise them independently.
 */
#define RAD1_REG_BYDMA_ADDRESS      0x00a0
#define RAD1_REG_BYDMA_STRIDE       0x00b8
#define RAD1_REG_BYDMA_MEM_ADDR     0x00c0
#define RAD1_REG_BYDMA_SIZE         0x00c8
#define RAD1_REG_BYDMA_BYTE_MASK    0x00d0
#define RAD1_REG_BYDMA_CONTROL      0x00d8

#define RAD1_BYDMA_DIM_MASK              0x0fffu
#define RAD1_BYDMA_HEIGHT_SHIFT          16
#define RAD1_BYDMA_MEMADDR_MASK          0x00ffffffu
#define RAD1_BYDMA_BYTE_MASK_RIGHT_SHIFT 16
#define RAD1_BYDMA_BYTE_MASK_LANE_MASK   0xfu

/*
 * Memory Control block, Region Zero 0x1000-0x1FFF: only offset 0x1100
 * (PM2R_BYPASS_WRITE_MASK, public Linux
 * include/video/permedia2.h) is written by the PROM/standalone textport
 * console path (console=g), a single write,
 * the same shape as Linux drivers/video/fbdev/pm2fb.c's reset_config(), which
 * writes nearly every GP register to a
 * power-on default exactly once at mode set. This offset previously fell
 * through this file's whole dispatch to the
 * final "unsupported Region Zero write" case (no Memory Control range existed
 * at all). Stored only, no modeled side
 * effect: no known access reads it back or depends on a specific bit pattern.
 */
#define RAD1_REG_BYPASS_WRITE_MASK  0x1100

/*
 * Video Timing block, Region Zero 0x3000-0x3FFF. Offsets are the public
 * Permedia2 register set (Linux
 * include/video/permedia2.h: PM2R_SCREEN_BASE 0x3000, PM2R_SCREEN_STRIDE
 * 0x3008, PM2R_H_TOTAL 0x3010, PM2R_HG_END
 * 0x3018, PM2R_HB_END 0x3020, PM2R_HS_START 0x3028, PM2R_HS_END 0x3030,
 * PM2R_V_TOTAL 0x3038, PM2R_VB_END 0x3040,
 * PM2R_VS_START 0x3048, PM2R_VS_END 0x3050, PM2R_VIDEO_CONTROL 0x3058,
 * PM2R_LINE_COUNT 0x3070, PM2R_FIFO_CONTROL
 * 0x3078). The whole block is written once per mode set, previously falling
 * through to this file's default
 * "unsupported Region Zero write" case. Linux drivers/video/fbdev/pm2fb.c's
 * pm2fb_set_par() writes the H/V timing
 * and SCREEN_* registers as one block during a mode set, then set_video()
 * writes VIDEO_CONTROL separately for sync
 * polarity/blank/enable, the same two-phase pattern this device's own write
 * pattern shows.
 *
 * This model stores every one of these registers (so the writes are no longer
 * silently dropped) but does not resize
 * the emulated console or otherwise change the active mode/timing in response
 * to them: the fixed single active mode
 * (RAD1_ACTIVE_MODE, see rad1_mode_records above) remains unchanged, matching
 * this file's existing
 * startup-mode-selection limitation.
 *
 * SCREEN_BASE and SCREEN_STRIDE are stored but deliberately not wired into
 * rad1_do_trapezoid_render()'s destination
 * addressing. Linux drivers/video/fbdev/pm2fb.c's pm2fb_set_par() computes
 * the values it writes here via a local
 * helper, to3264(): `stride = to3264(width, depth, 1); base =
 * to3264(yoffset*xres+xoffset, depth, 1);`. to3264()
 * right-shifts its pixel-count input by an amount that depends on bpp (0/1/2
 * bits depending on 8/16/24/32) and
 * unconditionally shifts once more for 64-bit-word units (its third argument
 * is the literal 1, not the driver's
 * data64 flag). So SCREEN_STRIDE/SCREEN_BASE are pixel quantities
 * re-expressed in 8-byte words under a
 * depth-dependent formula, not a raw byte pitch/offset. Reconstructing the
 * byte pitch this model would need
 * requires knowing which of that formula's bpp branches RAD1's guest-visible
 * framebuffer format actually takes,
 * which is not established against a specific PM2R_FB_READ_PIXEL/depth
 * setting on this board. Treating the raw
 * register value as a byte pitch without that confirmation risks silently
 * misaddressing every scanline the
 * trapezoid renderer draws, which is worse than not consulting the register
 * at all. RAD1_REG_RENDER's trapezoid
 * path therefore uses the same fixed mode's own stride and a zero base that
 * the existing flat
 * rectangle/texture-copy path (rad1_do_render(), unchanged) already uses as
 * established working behavior.
 *
 * VIDEO_CONTROL's PM2F_VIDEO_ENABLE bit is stored but not honored as a
 * scanout gate, for the same reason: there is
 * no evidence the PROM/kernel path here ever needs the emulated display
 * blanked, and gating an already-working
 * scanout on an unverified enable bit risks turning a working display black
 * instead of fixing anything.
 */
#define RAD1_REG_SCREEN_BASE        0x3000
#define RAD1_REG_SCREEN_STRIDE      0x3008
#define RAD1_REG_H_TOTAL            0x3010
#define RAD1_REG_HG_END             0x3018
#define RAD1_REG_HB_END             0x3020
#define RAD1_REG_HS_START           0x3028
#define RAD1_REG_HS_END             0x3030
#define RAD1_REG_V_TOTAL            0x3038
#define RAD1_REG_VB_END             0x3040
#define RAD1_REG_VS_START           0x3048
#define RAD1_REG_VS_END             0x3050
#define RAD1_REG_VIDEO_CONTROL      0x3058
#define RAD1_REG_LINE_COUNT         0x3070
#define RAD1_REG_FIFO_CONTROL       0x3078

/*
 * RAMDAC block, Region Zero 0x4000-0x4FFF: rad1_init_RAMDAC()/
 * rad1_load_lut() write exactly these seven offsets.
 * Two of those seven, 0x4008 and 0x4018, carry real palette-protocol
 * behavior, broken out below rather than
 * left in the generic RAD1_RAMDAC_R0..R6 array (whose R0/R1 members, 0x4000
 * and 0x4010, skip 0x4008 and 0x4018
 * entirely). The remaining five offsets (cursor color address/data, two
 * further unnamed slots, and the
 * indexed-register data port) are plain storage with no modeled side effect.
 */
#define RAD1_RAMDAC_R0          0x4000
#define RAD1_RAMDAC_R1          0x4010
#define RAD1_RAMDAC_R2          0x4020
#define RAD1_RAMDAC_R3          0x4028
#define RAD1_RAMDAC_R4          0x4030
#define RAD1_RAMDAC_R5          0x4038
#define RAD1_RAMDAC_R6          0x4050

/*
 * Palette write/read registers (Linux include/video/permedia2.h:
 * PM2R_RD_PALETTE_WRITE_ADDRESS 0x4000 ==
 * RAD1_RAMDAC_R0 above, PM2R_RD_PALETTE_DATA 0x4008,
 * PM2R_RD_PALETTE_READ_ADDRESS 0x4018). Programming sequence and
 * the auto-increment-after-each-triple behavior are both shown directly by
 * Linux drivers/video/fbdev/pm2fb.c's
 * clear_palette(): it writes PM2R_RD_PALETTE_WRITE_ADDRESS exactly once (to
 * 0), then writes PM2R_RD_PALETTE_DATA
 * three times (R, G, B) per palette index, for all 256 indices, with no
 * further write-address write in between. The only way that sequence can
 * reach all 256 entries is if the hardware
 * auto-increments the write index
 * internally after each complete (R,G,B) triple. This model implements
 * exactly that state machine. A symmetric
 * read-address/read-index pair is modeled for PM2R_RD_PALETTE_READ_ADDRESS on
 * the same evidence (the header defines
 * a separate read-address register, implying the same triple-byte protocol in
 * the read direction), though nothing
 * observed reads the palette back.
 */
#define RAD1_RAMDAC_PALETTE_DATA        0x4008
#define RAD1_RAMDAC_PALETTE_READ_ADDR   0x4018

/*
 * FIFO-tagged GP (rasterizer/texture) registers, Region Zero 0x8000-0xFFFF.
 * Offsets match the TVP4020 Programmer's
 * Reference Manual's own per-register table entries; page numbers cited in
 * comments below are the PDF's own page
 * count (e.g. RectangleOrigin/RectangleSize page 241), about nine higher than
 * the manual's printed page numbers.
 */
/*
 * Trapezoid/span rasterizer registers, Region Zero 0x8000-0x8038 (public
 * Linux include/video/permedia2.h:
 * PM2R_START_X_DOM 0x8000, PM2R_D_X_DOM 0x8008, PM2R_START_X_SUB 0x8010,
 * PM2R_D_X_SUB 0x8018, PM2R_START_Y 0x8020,
 * PM2R_D_Y 0x8028, PM2R_COUNT 0x8030; RAD1_REG_RENDER below is PM2R_RENDER
 * 0x8038, already established here for the
 * flat rectangle/ texture-copy path). The PROM/standalone textport console
 * path (console=g) writes here heavily,
 * one Render per glyph pixel and per box fill.
 *
 * Public source grounds these seven offsets and the primitive-selector bits
 * in RENDER
 * (RAD1_RENDER_PRIMITIVE_MASK/_TRAPEZOID/_RECTANGLE/_LINE/ _POINT below, from
 * the same header's PM2F_RENDER_*
 * bits); the manual grounds their formats: every rasterizer coordinate and
 * delta is 2's complement 16.16 fixed
 * point (manual page 70, "5.3.14 Registers") and Count is a 12-bit scanline
 * count (manual page 73, Table 5.5). The
 * PROM textport initialiser agrees, writing dY = 0x10000 (1.0) and zero
 * dXDom/dXSub once at mode set, and the point
 * renders put Y<<16 in StartY. Neither textport renders a trapezoid; only the
 * kernel driver's fill_fb() (rad1drv.o)
 * does.
 */
#define RAD1_REG_START_X_DOM        0x8000
#define RAD1_REG_D_X_DOM            0x8008
#define RAD1_REG_START_X_SUB        0x8010
#define RAD1_REG_D_X_SUB            0x8018
#define RAD1_REG_START_Y            0x8020
#define RAD1_REG_D_Y                0x8028
#define RAD1_REG_COUNT               0x8030

/* PM2F_RENDER_PRIMITIVE_MASK and its four primitive values, permedia2.h. */
#define RAD1_RENDER_PRIMITIVE_MASK  (3u << 6)
#define RAD1_RENDER_LINE            (0u << 6)
#define RAD1_RENDER_TRAPEZOID       (1u << 6)
#define RAD1_RENDER_POINT           (2u << 6)
#define RAD1_RENDER_RECTANGLE       (3u << 6)

/*
 * Render's other short-term mode bits this model dispatches on (manual pages
 * 242-243; permedia2.h
 * PM2F_RENDER_FASTFILL and PM2F_RENDER_TEXTURE_ENABLE). FastFillEnable
 * selects the block-fill path whose colour is
 * FBBlockColor (manual page 41); TextureEnable is what turns a rectangle into
 * the texture-read copy
 * rad1_do_render() models. The kernel driver's
 * rect_pio_move()/rect_dma_move() write 0x6020c0, the textports' box
 * fills 0x6000c0 and their initialisers' clears 0x6000c8. IncreaseX/IncreaseY
 * (bits 21-22) choose the fill
 * direction; only the increasing direction every known writer uses is
 * modeled.
 */
#define RAD1_RENDER_FASTFILL        (1u << 3)
#define RAD1_RENDER_TEXTURE_ENABLE  (1u << 13)
#define RAD1_RENDER_INCREASE_X      (1u << 21)
#define RAD1_RENDER_INCREASE_Y      (1u << 22)

#define RAD1_REG_RENDER             0x8038  /* manual: write-only command */
#define RAD1_REG_WAIT_FOR_COMPLETE 0x8088  /* manual page 275: write-only */
#define RAD1_REG_RECT_ORIGIN        0x80d0  /* manual page 241 */
#define RAD1_REG_RECT_SIZE          0x80d8  /* manual page 241 */

/*
 * RasterizerMode, Region Zero 0x80a0 (public Linux include/video/
 * permedia2.h: PM2R_RASTERIZER_MODE). Stored only;
 * this model's trapezoid path does not implement RasterizerMode's
 * bitmask-invert/antialiasing controls. Linux
 * drivers/video/fbdev/pm2fb.c's own use of this register is confined to
 * pm2fb_imageblit()'s monochrome-glyph
 * BIT_MASK_PATTERN path, which neither IRIX textport uses: the IO6 PROM
 * textport and the kernel's rad1_tport.o
 * RAD1TpDrawbitmap plot one point primitive per set glyph bit (StartXDom,
 * StartY, Render), see
 * rad1_do_point_render().
 */
#define RAD1_REG_RASTERIZER_MODE    0x80a0

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
 * FBWriteData, Region Zero 0x8830 (manual page 217): the colour written to
 * the framebuffer in place of the fragment
 * colour while LogicalOpMode's UseConstantFBWriteData bit (bit 5, manual page
 * 231) is set. It is not in the public
 * Linux include/video/permedia2.h list. Both textports draw every text pixel
 * and box this way: the PROM textport
 * and the kernel's rad1_tport.o RAD1TpDrawbitmap, RAD1TpPnt2i and
 * RAD1TpSboxfi each set bit 5 in 0x8828, write the
 * colour here, render, then clear bit 5. The values used (0xf0f0f0
 * background, 0x606070 text) are the textport's
 * own colours in the framebuffer's raw format.
 */
#define RAD1_REG_FB_WRITE_DATA      0x8830
#define RAD1_LOGICOP_USE_CONSTANT_FB_WRITE_DATA (1u << 5)

/*
 * FBBlockColor, Region Zero 0x8ac8 (permedia2.h PM2R_FB_BLOCK_COLOR; manual
 * page 41, "Block Writes"): the value a
 * FastFillEnable Render writes to every pixel. Both textport initialisers
 * zero it together with FBWriteData before
 * their full-screen FastFill clear (the PROM textport and rad1_tpinit.o's
 * RAD1InitBoard).
 */
#define RAD1_REG_FB_BLOCK_COLOR     0x8ac8

/*
 * Unnamed GP register, Region Zero 0x8d80: written once as part of the same
 * reset/init sequence as STATISTICS_MODE
 * (0x8c08), YUV_MODE (0x8f00) and DELTA_MODE (0x9300) below, the same shape
 * as Linux drivers/video/fbdev/pm2fb.c's
 * reset_config(), which writes nearly every GP register to a power-on default
 * exactly once. This offset sits eight
 * bytes before PM2R_FB_SOURCE_DELTA (0x8d88) and is not itself a named
 * register in permedia2.h. Stored only, no
 * modeled side effect.
 */
#define RAD1_REG_GP_UNNAMED_8D80     0x8d80

/*
 * Three further single-write registers from the same reset/init cluster
 * (public Linux include/video/permedia2.h:
 * PM2R_STATISTICS_MODE 0x8c08, PM2R_YUV_MODE 0x8f00, PM2R_DELTA_MODE 0x9300),
 * each written exactly once, matching
 * reset_config()'s own one-shot power-on-default write to each of these
 * registers in pm2fb.c. Stored only, no
 * modeled side effect: none of the drawing paths implemented here reads them
 * back or depends on a specific bit
 * pattern.
 */
#define RAD1_REG_STATISTICS_MODE    0x8c08
#define RAD1_REG_YUV_MODE           0x8f00
#define RAD1_REG_DELTA_MODE         0x9300

/*
 * RectangleOrigin/RectangleSize bit packing, confirmed directly against the
 * manual's own register diagram (page
 * 241): X/Width in bits 0-15, Y/ Height in bits 16-31.
 */
#define RAD1_RECT_LOW_MASK      0xffffu
#define RAD1_RECT_HIGH_SHIFT    16

/*
 * TextureBaseAddress bit 30 (manual page 260): "If bit 30 is set the texture
 * resides in system memory rather than
 * local buffer and should be fetched across the PCI bus." This resolves, from
 * the public manual itself, whether
 * TextureBaseAddress names a host or local address: the driver writes either
 * a host DMA address or a same-surface
 * framebuffer address into this register. This model honors bit 30 as the
 * manual documents it. The low bits are
 * nominally a texel count "from the base of the memory" per the manual; the
 * driver's own DMA path writes a raw
 * kernel-computed physical address directly into this register, so this model
 * treats bits 0-23 as a plain byte
 * offset/address rather than re-deriving a texel-to-byte scale; this is the
 * model's own choice, not an
 * independently confirmed scale.
 *
 * Bit 31 is deliberately left in place by RAD1_TEXBASE_ADDR_MASK (it only
 * clears bit 30): a system-memory address
 * the driver computes through IRIX's pciio_dmatrans_addr() carries the
 * Bridge's own PCI_DMA_DIRECT
 * address-space-selector bit as part of the same 32-bit value, and
 * pci_dma_read()/pci_dma_write() need that bit
 * intact to resolve the address at all, matching psitech-rad4.c's own DMA
 * descriptor handling, which never strips
 * it either.
 */
#define RAD1_TEXBASE_SYSTEM_MEMORY  (1u << 30)
#define RAD1_TEXBASE_ADDR_MASK      0xbfffffffu

/*
 * FBReadMode bits 9/10 (manual page 211): Read Source Enable and Read
 * Destination Enable. This model treats Read
 * Destination Enable (bit 10) as the signal that a Render targets a
 * host-directed framebuffer read-back (the "Host
 * Out" path FilterMode/Sync also describe) rather than an ordinary forward
 * copy into the framebuffer; this
 * conflates the manual's general read-enable bit with the specific
 * host-readback use the driver makes of it, and is
 * the model's own simplification rather than an independently confirmed
 * bit-to-behavior mapping.
 */
#define RAD1_FBREADMODE_READBACK_BIT   (1u << 10)

/*
 * Flat-copy pixel size: 32-bit words, matching the RAD4 sibling product's
 * True-color framebuffer layout. RAD1's own
 * pixel format is not independently confirmed; this model assumes the same
 * layout.
 */
#define RAD1_BYTES_PER_PIXEL    4

/*
 * The three fixed mode records: NumPER2Modes == 3, matching the product
 * sheet's three listed resolutions exactly.
 * How the driver picks among them at startup is not modeled: this model
 * always runs mode 0 (the product sheet's
 * first-listed resolution) and does not implement mode switching; this is the
 * minimum choice to get a boot attempt
 * working, not a modeled selection mechanism.
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
 * Flash/EEPROM programming: a generic JEDEC/AMD two-cycle parallel NOR flash
 * unlock-and-program sequence,
 * industry-standard and independent of any RAD1-specific source. Which PCI
 * resource carries the flash pointer, and
 * the flash part's actual size, are not confirmed. RAD1_FLASH_SIZE and its
 * BAR number are this file's own minimum
 * choice. Byte-wide access (unlike Region Zero's 32-bit-only accesses)
 * follows from the classic parallel NOR unlock
 * algorithm itself, which only makes sense as byte offsets into an 8-bit-wide
 * parallel flash part.
 *
 * Unlock addresses: the driver's own CPU-side literal offsets are 0x5556 and
 * 0x2AA9, one off from the universal
 * JEDEC/AMD standard unlock addresses 0x5555 and 0x2AAA (used industry-wide,
 * e.g. the Am29F0x0 family). The Bridge
 * applies a real byte-lane swap (addr ^ 3) to sub-word PCI accesses from this
 * big-endian host to a little-endian
 * target, the same class of hardware behavior this project's Bridge model
 * already documents for DMA. The driver's
 * own literal byte offset is therefore the pre-swap, CPU-side address; what
 * the flash chip itself sees, and what
 * this model's state machine below matches against, is the standard post-swap
 * JEDEC address. This model uses the
 * standard addresses directly rather than re-deriving the swap itself, since
 * the swap is the Bridge's behavior, not
 * this device's.
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
     * RAM-backed MemoryRegion: the Bridge's PIO
     * window applies the same byte-lane behavior to every device on it, and
     * this codebase's own working convention
     * (hw/display/psitech-rad4.c's BAR0 and BAR2 both) is to declare
     * DEVICE_LITTLE_ENDIAN MemoryRegionOps and store
     * multi-byte values explicitly big-endian (ldl_be_p()/stl_be_p()) inside
     * a plain byte array, not to rely on a
     * RAM region's own default handling, which does not carry that same
     * correction. `display_buffer` is a separate,
     * host-native-order copy rad1_gfx_update() maintains for scanout, exactly
     * matching rad4_convert_scanout()'s
     * role in the sibling device.
     */
    uint8_t *framebuffer;
    uint32_t *display_buffer;
    uint8_t *flash_data;

    /* Control & Status block. */
    uint32_t int_status;
    uint32_t ctrl_ack;

    /* RAMDAC block: seven established offsets, stored only. */
    uint32_t ramdac[7];

    /*
     * RAMDAC palette storage and the write/read index + component-counter
     * state that implements clear_palette()'s
     * three-writes-per-index protocol; see the palette register comment
     * above.
     */
    uint8_t palette[256][3];
    uint8_t palette_write_index;
    uint8_t palette_write_comp;
    uint8_t palette_read_index;
    uint8_t palette_read_comp;

    /*
     * Video Timing block, Region Zero 0x3000-0x3078: stored only except for
     * screen_base/screen_stride, see the
     * block comment above.
     */
    uint32_t screen_base;
    uint32_t screen_stride;
    uint32_t h_total;
    uint32_t hg_end;
    uint32_t hb_end;
    uint32_t hs_start;
    uint32_t hs_end;
    uint32_t v_total;
    uint32_t vb_end;
    uint32_t vs_start;
    uint32_t vs_end;
    uint32_t video_control;
    uint32_t line_count;
    uint32_t fifo_control;

    /* Memory Control block: only offset 0x1100 is modeled, stored only. */
    uint32_t bypass_write_mask;

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

    /*
     * Trapezoid/span rasterizer state (RAD1_REG_START_X_DOM etc., see the
     * block comment above): raw register
     * storage, interpreted as 16.16 fixed point only inside
     * rad1_do_trapezoid_render() itself.
     */
    uint32_t start_x_dom;
    uint32_t d_x_dom;
    uint32_t start_x_sub;
    uint32_t d_x_sub;
    uint32_t start_y;
    uint32_t d_y;
    uint32_t count;
    uint32_t render_cmd;
    uint32_t rasterizer_mode;
    uint32_t fb_write_data;
    uint32_t fb_block_color;
    uint32_t gp_unnamed_8d80;
    uint32_t statistics_mode;
    uint32_t yuv_mode;
    uint32_t delta_mode;

    /* Bypass DMA block: the write-DMA completion path, see above. */
    uint32_t bydma_addr;
    uint32_t bydma_stride;
    uint32_t bydma_mem_addr;
    uint32_t bydma_size;
    uint32_t bydma_byte_mask;
    uint32_t bydma_control;

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
     * No mask/enable register is known, so this model asserts INTA# whenever
     * any status bit is pending and
     * deasserts once the guest write-clears them all.
     */
    pci_set_irq(&s->pci, s->int_status != 0);
}

/*
 * Perform one flat, non-interpolated rectangle copy per the Render
 * command. This model runs the whole operation synchronously inside the
 * register write that triggers it. There is no real FIFO depth or
 * rasterizer latency to model for the one operation kind this driver
 * uses.
 *
 * Two directions are modeled, selected by RAD1_FBREADMODE_READBACK_BIT
 * (see its definition above, itself a simplification):
 *   - forward (default): copy from the texture-read source named by
 *     TextureBaseAddress into the framebuffer rectangle named by
 *     RectangleOrigin/RectangleSize, honoring FBHardwareWriteMask.
 *   - read-back: copy the framebuffer rectangle named by
 *     RectangleOrigin/RectangleSize out to the host/PCI address named by
 *     TextureBaseAddress. FBHardwareWriteMask does not apply to a
 *     host-memory destination.
 *
 * Source and destination are both treated as one contiguous run of
 * RAD1_BYTES_PER_PIXEL-sized words per scanline, width RectangleSize.width.
 * This model assumes the source rectangle's own stride equals its
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
    /*
     * RectangleOrigin's X and Y are each a 12-bit two's complement value
     * (manual page 73, Table 5.5; bits
     * 12-15/28-31 of each 16-bit half are "Ignored" per the register diagram,
     * page 241), matching
     * rad1_do_rectangle_fill()'s decode below. This path draws one contiguous
     * block starting at the corner rather
     * than clipping pixel by pixel, so a negative origin, unlike in the
     * per-pixel fill, always puts the whole
     * rectangle outside the framebuffer.
     */
    int32_t dst_x = sextract32(s->rect_origin, 0, 12);
    int32_t dst_y = sextract32(s->rect_origin, RAD1_RECT_HIGH_SHIFT, 12);
    const Rad1ModeRecord *mode = &rad1_mode_records[RAD1_ACTIVE_MODE];
    uint32_t fb_stride = mode->width * RAD1_BYTES_PER_PIXEL;
    bool readback = (s->fb_read_mode & RAD1_FBREADMODE_READBACK_BIT) != 0;
    uint32_t row_bytes;
    uint8_t *linebuf;
    uint32_t row;

    if (width != 0 && height != 0) {
        /*
         * The rasterizer's FBWriteMode target is the framebuffer's actual
         * VRAM (RAD1_FB_SIZE), not the
         * RAMDAC/CRTC's currently active scanout mode. Those are
         * independent hardware concerns on a real
         * Permedia2: a Render write below the visible mode height is routine
         * (off-screen scratch, cursor storage, a
         * driver clearing VRAM rows the CRTC never scans out) and public
         * Permedia2 sources describe no
         * rasterizer-side clip against the active mode. Only the X axis is
         * bounded by mode->width, since fb_stride
         * is fixed at mode->width * RAD1_BYTES_PER_PIXEL and a wider write
         * would spill into the next scanline's
         * memory. dst_x/dst_y are checked for negative first. That alone
         * puts the rectangle outside the framebuffer, and the '||' chain's
         * left-to-right short-circuit
         * keeps that check ahead of udst_x/udst_y,
         * which are only meaningful once both are known non-negative.
         */
        int64_t dst_end = (int64_t)(dst_y + (int64_t)height) * fb_stride;

        if (dst_x < 0 || dst_y < 0 || dst_x > (int32_t)mode->width ||
            (int64_t)width > (int64_t)mode->width - dst_x ||
            dst_end > RAD1_FB_SIZE) {
            qemu_log_mask(LOG_GUEST_ERROR,
                         "psitech-rad1: Render rectangle (%d,%d)+(%ux%u) "
                         "falls outside the %" PRIu64 "-byte framebuffer\n",
                         dst_x, dst_y, width, height,
                         (uint64_t)RAD1_FB_SIZE);
        } else {
            uint32_t udst_x = (uint32_t)dst_x;
            uint32_t udst_y = (uint32_t)dst_y;

            row_bytes = width * RAD1_BYTES_PER_PIXEL;
            linebuf = g_malloc(row_bytes);

            if (readback) {
                hwaddr host_addr = s->tex_base_addr & RAD1_TEXBASE_ADDR_MASK;

                for (row = 0; row < height; row++) {
                    uint8_t *src = s->framebuffer +
                                  (udst_y + row) * fb_stride +
                                  udst_x * RAD1_BYTES_PER_PIXEL;

                    if (pci_dma_write(&s->pci,
                                      host_addr + (hwaddr)row * row_bytes,
                                      src, row_bytes) != MEMTX_OK) {
                        qemu_log_mask(LOG_GUEST_ERROR,
                                     "psitech-rad1: read-back write failed "
                                     "at 0x%"HWADDR_PRIx"\n", host_addr);
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
                    uint8_t *dst = s->framebuffer +
                                   (udst_y + row) * fb_stride +
                                   udst_x * RAD1_BYTES_PER_PIXEL;
                    hwaddr src_addr = src_origin + (hwaddr)row * row_bytes;

                    if (system_memory) {
                        if (pci_dma_read(&s->pci, src_addr, linebuf,
                                         row_bytes) != MEMTX_OK) {
                            qemu_log_mask(LOG_GUEST_ERROR,
                                         "psitech-rad1: texture-read failed "
                                         "at 0x%"HWADDR_PRIx"\n", src_addr);
                            break;
                        }
                    } else {
                        if (src_addr + row_bytes > RAD1_FB_SIZE) {
                            qemu_log_mask(LOG_GUEST_ERROR,
                                         "psitech-rad1: local texture read "
                                         "at 0x%"HWADDR_PRIx" falls outside "
                                         "the framebuffer\n", src_addr);
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
        }
    }

    /*
     * Completion signaling, simplified; see RAD1_INT_KNOWN_MASK above. This
     * fires even when the rectangle above was
     * empty or fell outside the active mode: rad1_inthandler()'s wait queue
     * has no separate "command rejected"
     * path, so a caller that issues an out-of-bounds Render and then waits on
     * completion needs this wake
     * regardless.
     */
    s->int_status |= RAD1_INT_KNOWN_MASK;
    rad1_update_irq(s);
}

/*
 * Resolve the constant colour a Render without texturing writes: FastFill
 * takes FBBlockColor (manual page 41),
 * otherwise LogicalOpMode's UseConstantFBWriteData selects FBWriteData
 * (manual pages 217 and 231). Every textport
 * and initialiser fill sets one of the two. The remaining case, the Color DDA
 * unit's own fragment colour, is not
 * modeled: nothing is drawn rather than a guessed colour, and the omission is
 * logged.
 */
static bool rad1_constant_fill_color(PsitechRad1State *s, uint32_t *color)
{
    if (s->render_cmd & RAD1_RENDER_FASTFILL) {
        *color = s->fb_block_color;
        return true;
    }
    if (s->logical_op_mode & RAD1_LOGICOP_USE_CONSTANT_FB_WRITE_DATA) {
        *color = s->fb_write_data;
        return true;
    }
    qemu_log_mask(LOG_UNIMP,
                 "psitech-rad1: Render 0x%08x with neither FastFill nor "
                 "UseConstantFBWriteData: Color DDA fragment colour not "
                 "modeled, nothing drawn\n", s->render_cmd);
    return false;
}

/*
 * Write one pixel through FBHardwareWriteMask. Rows are addressed with the
 * fixed active mode's stride, as
 * rad1_do_render() does, and the row bound is the framebuffer itself rather
 * than the visible mode height for the
 * same reason that function gives: a write below the scanned-out rows is
 * ordinary VRAM traffic, not an error.
 */
static void rad1_put_pixel(PsitechRad1State *s, int32_t x, int32_t y,
                           uint32_t color)
{
    const Rad1ModeRecord *mode = &rad1_mode_records[RAD1_ACTIVE_MODE];
    uint32_t mask = s->fb_hw_write_mask;
    uint64_t offset;
    uint8_t *px;

    if (x < 0 || y < 0 || x >= (int32_t)mode->width) {
        return;
    }
    offset = ((uint64_t)y * mode->width + (uint32_t)x) * RAD1_BYTES_PER_PIXEL;
    if (offset + RAD1_BYTES_PER_PIXEL > RAD1_FB_SIZE) {
        return;
    }
    px = s->framebuffer + offset;
    if (mask == 0xffffffffu) {
        stl_be_p(px, color);
    } else {
        stl_be_p(px, (ldl_be_p(px) & ~mask) | (color & mask));
    }
}

/*
 * Render one point primitive (manual page 62, "5.3.3 Points"): a single
 * aliased pixel at StartXDom/StartY, both
 * 16.16 fixed point ("StartXDom (X<<16), StartY (Y<<16)" in the manual's own
 * pseudocode; page 70 makes 16.16 the
 * format of every rasterizer coordinate). This is how both textports draw
 * text: one point per set glyph bit, colour
 * from FBWriteData, as both the PROM textport and the kernel driver's
 * rad1_tport.o RAD1TpDrawbitmap do. Completion
 * signaling is simplified exactly as rad1_do_render()'s, see
 * RAD1_INT_KNOWN_MASK.
 */
static void rad1_do_point_render(PsitechRad1State *s)
{
    uint32_t color;

    if (rad1_constant_fill_color(s, &color)) {
        rad1_put_pixel(s, (int32_t)s->start_x_dom >> 16,
                       (int32_t)s->start_y >> 16, color);
    }

    s->int_status |= RAD1_INT_KNOWN_MASK;
    rad1_update_irq(s);
}

/*
 * Render one rectangle primitive as a constant fill (manual page 62, "5.3.4
 * Rectangles"): RectangleOrigin's corner,
 * RectangleSize's width and height (manual page 241; the origin's X and Y are
 * 12-bit two's complement, manual page
 * 73, Table 5.5; matching rad1_do_render()'s decode above), filled toward
 * increasing X and Y, which is the only
 * direction any known writer uses (the PROM textport's box fill and the
 * kernel's RAD1TpSboxfi both write increasing
 * X/Y; the initialisers' clears do the same). A decreasing direction would
 * move the covered range to the other side
 * of the origin; unmodeled and logged rather than guessed. Completion
 * signaling is simplified as in
 * rad1_do_render().
 */
static void rad1_do_rectangle_fill(PsitechRad1State *s)
{
    uint32_t width = s->rect_size & RAD1_RECT_LOW_MASK;
    uint32_t height = (s->rect_size >> RAD1_RECT_HIGH_SHIFT) &
                      RAD1_RECT_LOW_MASK;
    int32_t x0 = sextract32(s->rect_origin, 0, 12);
    int32_t y0 = sextract32(s->rect_origin, RAD1_RECT_HIGH_SHIFT, 12);
    uint32_t color;
    uint32_t row, col;

    if ((s->render_cmd & (RAD1_RENDER_INCREASE_X | RAD1_RENDER_INCREASE_Y)) !=
        (RAD1_RENDER_INCREASE_X | RAD1_RENDER_INCREASE_Y)) {
        qemu_log_mask(LOG_UNIMP,
                     "psitech-rad1: decreasing-direction rectangle Render "
                     "0x%08x not modeled, nothing drawn\n", s->render_cmd);
    } else if (rad1_constant_fill_color(s, &color)) {
        for (row = 0; row < height; row++) {
            for (col = 0; col < width; col++) {
                rad1_put_pixel(s, x0 + (int32_t)col, y0 + (int32_t)row,
                               color);
            }
        }
    }

    s->int_status |= RAD1_INT_KNOWN_MASK;
    rad1_update_irq(s);
}

/*
 * Render one trapezoid/span primitive, selected by RAD1_RENDER_TRAPEZOID
 * (register formats: see the trapezoid/span
 * rasterizer register block comment above). StartXDom/StartXSub and StartY
 * are 16.16 start values, advanced once
 * per scanline by dXDom/dXSub and dY; each of Count scanlines fills the
 * half-open span [min(xdom, xsub), max(xdom,
 * xsub)) with the constant colour rad1_constant_fill_color() resolves,
 * FBBlockColor under FastFill, the case the
 * manual's block-write description (page 41) names for trapezoids,
 * FBWriteData otherwise. Which pixels a real
 * Permedia2 includes at a fractional edge is approximated by that truncating
 * half-open rule; no known writer draws
 * a non-integer edge. Completion signaling is simplified as in
 * rad1_do_render().
 *
 * Destination stride/base: the fixed active mode's own stride and a zero
 * base, the same as rad1_do_render(); see
 * the Video Timing block comment above for why SCREEN_STRIDE/SCREEN_BASE are
 * not wired in here.
 */
static void rad1_do_trapezoid_render(PsitechRad1State *s)
{
    const Rad1ModeRecord *mode = &rad1_mode_records[RAD1_ACTIVE_MODE];
    int32_t xdom = (int32_t)s->start_x_dom;
    int32_t xsub = (int32_t)s->start_x_sub;
    int32_t y = (int32_t)s->start_y;
    int32_t dxdom = (int32_t)s->d_x_dom;
    int32_t dxsub = (int32_t)s->d_x_sub;
    int32_t dy = (int32_t)s->d_y;
    uint32_t count = s->count & 0xfffu; /* 12-bit, manual page 73 */
    uint32_t color;
    uint32_t i;

    if (rad1_constant_fill_color(s, &color)) {
        for (i = 0; i < count; i++) {
            int32_t lo = MAX(MIN(xdom >> 16, xsub >> 16), 0);
            int32_t hi = MIN(MAX(xdom >> 16, xsub >> 16),
                             (int32_t)mode->width);
            int32_t x;

            for (x = lo; x < hi; x++) {
                rad1_put_pixel(s, x, y >> 16, color);
            }
            xdom += dxdom;
            xsub += dxsub;
            y += dy;
        }
    }

    /* Completion signaling, simplified; see RAD1_INT_KNOWN_MASK above. */
    s->int_status |= RAD1_INT_KNOWN_MASK;
    rad1_update_irq(s);
}

/*
 * Expand one 4-bit ByDMAByteMask edge nibble into a 32-bit lane mask, one bit
 * per byte lane of the pixel word (lane
 * 0 = bits 7:0 of the value ldl_be_p()/stl_be_p() normalize to, matching the
 * little-endian PCI byte-lane numbering
 * this codebase's own Bridge byte-lane-swap handling already assumes
 * elsewhere). Nothing ties a specific lane bit
 * to a specific memory byte beyond the driver's own fully-open 0xf value,
 * which this mapping's choice of lane order
 * cannot affect either way; this mapping is the model's own choice, see the
 * Bypass DMA block comment above.
 */
static uint32_t rad1_bydma_edge_mask(uint32_t nibble)
{
    uint32_t mask = 0;
    int lane;

    for (lane = 0; lane < 4; lane++) {
        if (nibble & (1u << lane)) {
            mask |= 0xffu << (8 * lane);
        }
    }
    return mask;
}

/*
 * Perform one Bypass DMA write transfer: read RectangleSize-style width/
 * height scanlines from the PCI source
 * address named by ByDMAAddress, at ByDMAStride bytes per source scanline,
 * into the local-memory destination named
 * by ByDMAMemAddr (texel units, converted to a byte offset by
 * RAD1_BYTES_PER_PIXEL; see the Bypass DMA block
 * comment above), at the active mode's own scanline stride, honoring
 * ByDMAByteMask at the first and last column of
 * each row. This is the write-DMA completion path `rad1pci`'s posted write
 * times out waiting for; see
 * RAD1_REG_BYDMA_* above and RAD1_INT_BIT7.
 */
static void rad1_do_bypass_dma(PsitechRad1State *s)
{
    uint32_t width = s->bydma_size & RAD1_BYDMA_DIM_MASK;
    uint32_t height = (s->bydma_size >> RAD1_BYDMA_HEIGHT_SHIFT) &
                      RAD1_BYDMA_DIM_MASK;
    const Rad1ModeRecord *mode = &rad1_mode_records[RAD1_ACTIVE_MODE];
    uint32_t fb_stride = mode->width * RAD1_BYTES_PER_PIXEL;
    hwaddr dst_base = (hwaddr)(s->bydma_mem_addr & RAD1_BYDMA_MEMADDR_MASK) *
                      RAD1_BYTES_PER_PIXEL;
    hwaddr src_base = s->bydma_addr;
    uint32_t src_stride = s->bydma_stride;
    uint32_t left_mask = rad1_bydma_edge_mask(
        s->bydma_byte_mask & RAD1_BYDMA_BYTE_MASK_LANE_MASK);
    uint32_t right_mask = rad1_bydma_edge_mask(
        (s->bydma_byte_mask >> RAD1_BYDMA_BYTE_MASK_RIGHT_SHIFT) &
        RAD1_BYDMA_BYTE_MASK_LANE_MASK);
    uint32_t row_bytes;
    uint8_t *linebuf;
    uint32_t row, col;

    if (width == 0 || height == 0) {
        return;
    }

    row_bytes = width * RAD1_BYTES_PER_PIXEL;
    if (row_bytes > fb_stride ||
        dst_base + (hwaddr)(height - 1) * fb_stride + row_bytes >
        RAD1_FB_SIZE) {
        qemu_log_mask(LOG_GUEST_ERROR,
                     "psitech-rad1: Bypass DMA destination at "
                     "0x%"HWADDR_PRIx" (%ux%u) falls outside the "
                     "framebuffer\n", dst_base, width, height);
        return;
    }

    linebuf = g_malloc(row_bytes);

    for (row = 0; row < height; row++) {
        hwaddr src_addr = src_base + (hwaddr)row * src_stride;
        uint8_t *dst = s->framebuffer + dst_base + (hwaddr)row * fb_stride;

        if (pci_dma_read(&s->pci, src_addr, linebuf,
                         row_bytes) != MEMTX_OK) {
            qemu_log_mask(LOG_GUEST_ERROR,
                         "psitech-rad1: Bypass DMA read failed at "
                         "0x%"HWADDR_PRIx"\n", src_addr);
            break;
        }

        for (col = 0; col < width; col++) {
            uint32_t mask = 0xffffffffu;

            if (col == 0) {
                mask &= left_mask;
            }
            if (col == width - 1) {
                mask &= right_mask;
            }

            if (mask == 0xffffffffu) {
                memcpy(dst + col * 4, linebuf + col * 4, 4);
            } else if (mask != 0) {
                uint32_t old = ldl_be_p(dst + col * 4);
                uint32_t incoming = ldl_be_p(linebuf + col * 4);

                stl_be_p(dst + col * 4, (old & ~mask) | (incoming & mask));
            }
        }
    }

    g_free(linebuf);

    /* IntFlags bit 7: Bypass DMA Interrupt Flag, see RAD1_INT_BIT7 above. */
    s->int_status |= RAD1_INT_BIT7;
    rad1_update_irq(s);
}

static uint64_t rad1_regzero_read(void *opaque, hwaddr addr, unsigned size)
{
    PsitechRad1State *s = opaque;
    int ramdac_idx;
    uint8_t palette_comp;

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
        case RAD1_REG_BYDMA_ADDRESS: return s->bydma_addr;
        case RAD1_REG_BYDMA_STRIDE: return s->bydma_stride;
        case RAD1_REG_BYDMA_MEM_ADDR: return s->bydma_mem_addr;
        case RAD1_REG_BYDMA_SIZE: return s->bydma_size;
        case RAD1_REG_BYDMA_BYTE_MASK: return s->bydma_byte_mask;
        case RAD1_REG_BYDMA_CONTROL: return s->bydma_control;
        default:
            qemu_log_mask(LOG_UNIMP,
                         "psitech-rad1: unsupported Control & Status read "
                         "at 0x%04x\n", (unsigned)addr);
            return 0;
        }
    }

    if (addr <= RAD1_RANGE_MEMCTRL_LAST) {
        switch (addr) {
        case RAD1_REG_BYPASS_WRITE_MASK:
            return s->bypass_write_mask;
        default:
            qemu_log_mask(LOG_UNIMP,
                         "psitech-rad1: unsupported Memory Control read at "
                         "0x%04x\n", (unsigned)addr);
            return 0;
        }
    }

    if (addr > RAD1_RANGE_GPFIFO_LAST && addr <= RAD1_RANGE_VIDEOCTRL_LAST) {
        switch (addr) {
        case RAD1_REG_SCREEN_BASE: return s->screen_base;
        case RAD1_REG_SCREEN_STRIDE: return s->screen_stride;
        case RAD1_REG_H_TOTAL: return s->h_total;
        case RAD1_REG_HG_END: return s->hg_end;
        case RAD1_REG_HB_END: return s->hb_end;
        case RAD1_REG_HS_START: return s->hs_start;
        case RAD1_REG_HS_END: return s->hs_end;
        case RAD1_REG_V_TOTAL: return s->v_total;
        case RAD1_REG_VB_END: return s->vb_end;
        case RAD1_REG_VS_START: return s->vs_start;
        case RAD1_REG_VS_END: return s->vs_end;
        case RAD1_REG_VIDEO_CONTROL: return s->video_control;
        case RAD1_REG_LINE_COUNT: return s->line_count;
        case RAD1_REG_FIFO_CONTROL: return s->fifo_control;
        default:
            qemu_log_mask(LOG_UNIMP,
                         "psitech-rad1: unsupported Video Timing read at "
                         "0x%04x\n", (unsigned)addr);
            return 0;
        }
    }

    /*
     * Palette write/read-index and data registers get real protocol behavior
     * (see their own comment above); every
     * other RAMDAC offset keeps falling through to the generic ramdac_idx
     * dispatch below unchanged.
     */
    switch (addr) {
    case RAD1_RAMDAC_R0:
        return s->ramdac[0];
    case RAD1_RAMDAC_PALETTE_DATA:
        palette_comp = s->palette[s->palette_read_index][s->palette_read_comp];
        s->palette_read_comp++;
        if (s->palette_read_comp == 3) {
            s->palette_read_comp = 0;
            s->palette_read_index++;
        }
        return palette_comp;
    case RAD1_RAMDAC_PALETTE_READ_ADDR:
        return s->palette_read_index;
    default:
        break;
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
             * status bits. This model completes every
             * Render synchronously, so there is nothing to wait for by the
             * time a read could observe it; return 0
             * rather than asserting a specific "busy" bit semantics the
             * manual does not document.
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
        case RAD1_REG_START_X_DOM: return s->start_x_dom;
        case RAD1_REG_D_X_DOM: return s->d_x_dom;
        case RAD1_REG_START_X_SUB: return s->start_x_sub;
        case RAD1_REG_D_X_SUB: return s->d_x_sub;
        case RAD1_REG_START_Y: return s->start_y;
        case RAD1_REG_D_Y: return s->d_y;
        case RAD1_REG_COUNT: return s->count;
        case RAD1_REG_RASTERIZER_MODE: return s->rasterizer_mode;
        case RAD1_REG_FB_WRITE_DATA: return s->fb_write_data;
        case RAD1_REG_FB_BLOCK_COLOR: return s->fb_block_color;
        case RAD1_REG_GP_UNNAMED_8D80: return s->gp_unnamed_8d80;
        case RAD1_REG_STATISTICS_MODE: return s->statistics_mode;
        case RAD1_REG_YUV_MODE: return s->yuv_mode;
        case RAD1_REG_DELTA_MODE: return s->delta_mode;
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
        case RAD1_REG_BYDMA_ADDRESS:
            s->bydma_addr = val;
            break;
        case RAD1_REG_BYDMA_STRIDE:
            s->bydma_stride = val;
            break;
        case RAD1_REG_BYDMA_MEM_ADDR:
            s->bydma_mem_addr = val;
            break;
        case RAD1_REG_BYDMA_SIZE:
            s->bydma_size = val;
            break;
        case RAD1_REG_BYDMA_BYTE_MASK:
            s->bydma_byte_mask = val;
            break;
        case RAD1_REG_BYDMA_CONTROL:
            /* Last write in the posting sequence: starts the transfer. */
            s->bydma_control = val;
            rad1_do_bypass_dma(s);
            break;
        default:
            qemu_log_mask(LOG_UNIMP,
                         "psitech-rad1: unsupported Control & Status write "
                         "at 0x%04x\n", (unsigned)addr);
            break;
        }
        return;
    }

    if (addr <= RAD1_RANGE_MEMCTRL_LAST) {
        switch (addr) {
        case RAD1_REG_BYPASS_WRITE_MASK:
            s->bypass_write_mask = val;
            break;
        default:
            qemu_log_mask(LOG_UNIMP,
                         "psitech-rad1: unsupported Memory Control write "
                         "at 0x%04x (value 0x%"PRIx64")\n",
                         (unsigned)addr, val);
            break;
        }
        return;
    }

    if (addr > RAD1_RANGE_GPFIFO_LAST && addr <= RAD1_RANGE_VIDEOCTRL_LAST) {
        switch (addr) {
        case RAD1_REG_SCREEN_BASE:
            s->screen_base = val;
            break;
        case RAD1_REG_SCREEN_STRIDE:
            s->screen_stride = val;
            break;
        case RAD1_REG_H_TOTAL:
            s->h_total = val;
            break;
        case RAD1_REG_HG_END:
            s->hg_end = val;
            break;
        case RAD1_REG_HB_END:
            s->hb_end = val;
            break;
        case RAD1_REG_HS_START:
            s->hs_start = val;
            break;
        case RAD1_REG_HS_END:
            s->hs_end = val;
            break;
        case RAD1_REG_V_TOTAL:
            s->v_total = val;
            break;
        case RAD1_REG_VB_END:
            s->vb_end = val;
            break;
        case RAD1_REG_VS_START:
            s->vs_start = val;
            break;
        case RAD1_REG_VS_END:
            s->vs_end = val;
            break;
        case RAD1_REG_VIDEO_CONTROL:
            s->video_control = val;
            break;
        case RAD1_REG_LINE_COUNT:
            s->line_count = val;
            break;
        case RAD1_REG_FIFO_CONTROL:
            s->fifo_control = val;
            break;
        default:
            qemu_log_mask(LOG_UNIMP,
                         "psitech-rad1: unsupported Video Timing write "
                         "at 0x%04x (value 0x%"PRIx64")\n",
                         (unsigned)addr, val);
            break;
        }
        return;
    }

    /*
     * Palette write-index/data and read-index registers get real protocol
     * behavior (see their own comment above);
     * every other RAMDAC offset keeps falling through to the generic
     * ramdac_idx dispatch below unchanged.
     */
    switch (addr) {
    case RAD1_RAMDAC_R0:
        s->ramdac[0] = val;
        s->palette_write_index = (uint8_t)val;
        s->palette_write_comp = 0;
        return;
    case RAD1_RAMDAC_PALETTE_DATA:
        s->palette[s->palette_write_index][s->palette_write_comp] =
            (uint8_t)val;
        s->palette_write_comp++;
        if (s->palette_write_comp == 3) {
            s->palette_write_comp = 0;
            s->palette_write_index++;
        }
        return;
    case RAD1_RAMDAC_PALETTE_READ_ADDR:
        s->palette_read_index = (uint8_t)val;
        s->palette_read_comp = 0;
        return;
    default:
        break;
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
            /*
             * Dispatch on the primitive-selector bits (manual page 242, bits
             * 6-7). A rectangle is the kernel
             * driver's texture-read copy only when it carries TextureEnable
             * (rect_pio_move()/ rect_dma_move() write
             * 0x6020c0) or when FBReadMode selects this model's read-back
             * (see rad1_do_render()); the textports'
             * 0x6000c0 boxes and the initialisers' 0x6000c8 FastFill clears
             * are constant fills.
             */
            s->render_cmd = val;
            switch (val & RAD1_RENDER_PRIMITIVE_MASK) {
            case RAD1_RENDER_TRAPEZOID:
                rad1_do_trapezoid_render(s);
                break;
            case RAD1_RENDER_POINT:
                rad1_do_point_render(s);
                break;
            case RAD1_RENDER_RECTANGLE:
                if ((val & RAD1_RENDER_TEXTURE_ENABLE) ||
                    (s->fb_read_mode & RAD1_FBREADMODE_READBACK_BIT)) {
                    rad1_do_render(s);
                } else {
                    rad1_do_rectangle_fill(s);
                }
                break;
            default:
                qemu_log_mask(LOG_UNIMP,
                             "psitech-rad1: line primitive Render 0x%08x "
                             "not modeled\n", (uint32_t)val);
                break;
            }
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
        case RAD1_REG_START_X_DOM:
            s->start_x_dom = val;
            break;
        case RAD1_REG_D_X_DOM:
            s->d_x_dom = val;
            break;
        case RAD1_REG_START_X_SUB:
            s->start_x_sub = val;
            break;
        case RAD1_REG_D_X_SUB:
            s->d_x_sub = val;
            break;
        case RAD1_REG_START_Y:
            s->start_y = val;
            break;
        case RAD1_REG_D_Y:
            s->d_y = val;
            break;
        case RAD1_REG_COUNT:
            s->count = val;
            break;
        case RAD1_REG_RASTERIZER_MODE:
            s->rasterizer_mode = val;
            break;
        case RAD1_REG_FB_WRITE_DATA:
            s->fb_write_data = val;
            break;
        case RAD1_REG_FB_BLOCK_COLOR:
            s->fb_block_color = val;
            break;
        case RAD1_REG_GP_UNNAMED_8D80:
            s->gp_unnamed_8d80 = val;
            break;
        case RAD1_REG_STATISTICS_MODE:
            s->statistics_mode = val;
            break;
        case RAD1_REG_YUV_MODE:
            s->yuv_mode = val;
            break;
        case RAD1_REG_DELTA_MODE:
            s->delta_mode = val;
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
 * model. Real NOR flash can only clear bits
 * on a program cycle (an erase sets them), so a program applies a bitwise
 * AND.
 * This matches real hardware
 * and gives the qtest register-contract coverage a discriminating check:
 * skipping the unlock sequence leaves the
 * erased (all-ones) content unchanged, reproducing the byte-mismatch
 * condition program_eeprom_sector()'s own caller
 * detects, without this model needing to know that caller's error-code
 * convention.
 *
 * Access width: byte-wide (impl/valid min=max=1), unlike every other region
 * on this device. This is forced by the
 * unlock addresses themselves: 0x5556 and 0x2aa9 are not 4-byte aligned, so a
 * 32-bit-only region would never see a
 * 4-byte access land exactly on them. The driver's own access width for
 * program_eeprom_sector() is not
 * independently confirmed, but a byte-wide parallel NOR part addressed at odd
 * byte offsets is the ordinary,
 * industry-standard shape for this JEDEC/AMD sequence.
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
 * Framebuffer BAR MemoryRegionOps. Region Zero's 32-bit-only access width is
 * extended here too: nothing
 * distinguishes the framebuffer aperture's own access width, and 32-bit only
 * is this device's established
 * convention everywhere else.
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
 * pixels (PIXMAN_x8r8g8b8) with no
 * palette/gamma indirection, unlike RAD4's RGB561 window-ID/WAT/LUT path,
 * nothing in the guest workload (a flat
 * rectangle copy and the PROM/standalone textport) needs indexed color.
 * RAD1's actual pixel format is not
 * independently confirmed; this model assumes True-color.
 *
 * `framebuffer` stores big-endian words (see the struct field's own comment);
 * `display_buffer` is a
 * host-native-order copy this function maintains for
 * qemu_create_displaysurface_from(), the same two-buffer split
 * psitech-rad4.c's rad4_convert_scanout()/display_buffer use for the same
 * reason.
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

    memset(s->palette, 0, sizeof(s->palette));
    s->palette_write_index = 0;
    s->palette_write_comp = 0;
    s->palette_read_index = 0;
    s->palette_read_comp = 0;

    s->screen_base = 0;
    s->screen_stride = 0;
    s->h_total = 0;
    s->hg_end = 0;
    s->hb_end = 0;
    s->hs_start = 0;
    s->hs_end = 0;
    s->v_total = 0;
    s->vb_end = 0;
    s->vs_start = 0;
    s->vs_end = 0;
    s->video_control = 0;
    s->line_count = 0;
    s->fifo_control = 0;

    s->bypass_write_mask = 0;

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

    s->start_x_dom = 0;
    s->d_x_dom = 0;
    s->start_x_sub = 0;
    s->d_x_sub = 0;
    s->start_y = 0;
    s->d_y = 0;
    s->count = 0;
    s->render_cmd = 0;
    s->rasterizer_mode = 0;
    s->fb_write_data = 0;
    s->fb_block_color = 0;
    s->gp_unnamed_8d80 = 0;
    s->statistics_mode = 0;
    s->yuv_mode = 0;
    s->delta_mode = 0;

    s->bydma_addr = 0;
    s->bydma_stride = 0;
    s->bydma_mem_addr = 0;
    s->bydma_size = 0;
    s->bydma_byte_mask = 0;
    s->bydma_control = 0;

    memset(s->framebuffer, 0, RAD1_FB_SIZE);
    /* Erased NOR flash reads as all-ones. */
    memset(s->flash_data, 0xff, RAD1_FLASH_SIZE);
    s->flash_state = RAD1_FLASH_IDLE;

    rad1_update_irq(s);

    /*
     * Startup mode-record selection is not modeled; this model always runs
     * RAD1_ACTIVE_MODE and logs that it does,
     * rather than silently picking a resolution.
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
    /*
     * Bumped from 1 to 2 for the Video Timing and Memory Control state, from
     * 2 to 3 for the RAMDAC palette state,
     * and from 3 to 4 for the trapezoid/span rasterizer state below. This
     * device does not maintain snapshot
     * compatibility across versions; each bump only tracks the changed field
     * list.
     */
    .version_id = 4,
    .minimum_version_id = 4,
    .post_load = rad1_post_load,
    .fields = (const VMStateField[]) {
        VMSTATE_PCI_DEVICE(pci, PsitechRad1State),
        VMSTATE_BUFFER_POINTER_UNSAFE(framebuffer, PsitechRad1State, 1,
                                      RAD1_FB_SIZE),
        VMSTATE_UINT32(int_status, PsitechRad1State),
        VMSTATE_UINT32(ctrl_ack, PsitechRad1State),
        VMSTATE_UINT32_ARRAY(ramdac, PsitechRad1State, 7),
        VMSTATE_UINT8_2DARRAY(palette, PsitechRad1State, 256, 3),
        VMSTATE_UINT8(palette_write_index, PsitechRad1State),
        VMSTATE_UINT8(palette_write_comp, PsitechRad1State),
        VMSTATE_UINT8(palette_read_index, PsitechRad1State),
        VMSTATE_UINT8(palette_read_comp, PsitechRad1State),
        VMSTATE_UINT32(screen_base, PsitechRad1State),
        VMSTATE_UINT32(screen_stride, PsitechRad1State),
        VMSTATE_UINT32(h_total, PsitechRad1State),
        VMSTATE_UINT32(hg_end, PsitechRad1State),
        VMSTATE_UINT32(hb_end, PsitechRad1State),
        VMSTATE_UINT32(hs_start, PsitechRad1State),
        VMSTATE_UINT32(hs_end, PsitechRad1State),
        VMSTATE_UINT32(v_total, PsitechRad1State),
        VMSTATE_UINT32(vb_end, PsitechRad1State),
        VMSTATE_UINT32(vs_start, PsitechRad1State),
        VMSTATE_UINT32(vs_end, PsitechRad1State),
        VMSTATE_UINT32(video_control, PsitechRad1State),
        VMSTATE_UINT32(line_count, PsitechRad1State),
        VMSTATE_UINT32(fifo_control, PsitechRad1State),
        VMSTATE_UINT32(bypass_write_mask, PsitechRad1State),
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
        VMSTATE_UINT32(start_x_dom, PsitechRad1State),
        VMSTATE_UINT32(d_x_dom, PsitechRad1State),
        VMSTATE_UINT32(start_x_sub, PsitechRad1State),
        VMSTATE_UINT32(d_x_sub, PsitechRad1State),
        VMSTATE_UINT32(start_y, PsitechRad1State),
        VMSTATE_UINT32(d_y, PsitechRad1State),
        VMSTATE_UINT32(count, PsitechRad1State),
        VMSTATE_UINT32(render_cmd, PsitechRad1State),
        VMSTATE_UINT32(rasterizer_mode, PsitechRad1State),
        VMSTATE_UINT32(fb_write_data, PsitechRad1State),
        VMSTATE_UINT32(fb_block_color, PsitechRad1State),
        VMSTATE_UINT32(gp_unnamed_8d80, PsitechRad1State),
        VMSTATE_UINT32(statistics_mode, PsitechRad1State),
        VMSTATE_UINT32(yuv_mode, PsitechRad1State),
        VMSTATE_UINT32(delta_mode, PsitechRad1State),
        VMSTATE_UINT32(bydma_addr, PsitechRad1State),
        VMSTATE_UINT32(bydma_stride, PsitechRad1State),
        VMSTATE_UINT32(bydma_mem_addr, PsitechRad1State),
        VMSTATE_UINT32(bydma_size, PsitechRad1State),
        VMSTATE_UINT32(bydma_byte_mask, PsitechRad1State),
        VMSTATE_UINT32(bydma_control, PsitechRad1State),
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
