# Datasheet: MGRAS raster, memory, and display contract

References rendered as citation identities name material outside this document
set. Those targets are not included or cleared for redistribution. Review and
runtime statements retain their recorded dates and scope.

**Status:** draft contract. The public two-dimensional path is bounded; exact
plane packing, complete raster grammar, WID encoding, and mode programming
remain unresolved.

**Sources used:** SGI Indigo2 IMPACT Field Upgrade; SGI OCTANE Technical
Report; Octane inventory and channel-option documentation; retained MGRAS
initialization material; OpenBSD IMPACT and X.Org IMPACT 0.2.0 public driver
behavior; US5548709A as texture lineage only. Source identity is recorded in
`docs/provenance.md` and the MGRAS research record.

The contract uses original descriptions. It does not reproduce private source
declarations, structure, comments, tables, or diagnostic vectors.
The independent SI evidence review (citation: `docs/reviews/graphics-chipset-si-evidence-20260915.md`)
records the restricted evidence boundary. Implementers and implementation
reviewers use these independently worded contracts and their public sources,
not the restricted inputs named in provenance.

## Stock 2D software boundary

The following observations come from a bounded reading of stock IRIX 6.5.7
textport and diagnostic material. They record software expectations, not a
complete description of the silicon. The source reader is excluded from MGRAS
implementation and clean implementation review.

| Area | Established software expectation | Inferred or unknown hardware behavior |
| --- | --- | --- |
| Wide glyph | The caller selects a character rectangle, supplies row data as 16-bit words, and writes separate high and low character data registers. A low-register write is the normal execute step. The caller carries descriptor fields for size, origin, software cursor advance, and row storage rounded to 16-bit words. | Grouping four row words into the high-then-low transfer, once assumed as the caller-derived packing, does not match a live capture and is superseded below by a FILLMODE-selected pair of modes with their own bit-to-pixel order and row-advance rule. Clipping and background/transparency for those two modes are established for the retained capture; general timing remains unknown. |
| XMAP and WID | The stock textport initialization selects indexed 8-bit color in its main mode entry and enables display through the display-interface control. XMAP has separate main and overlay mode files and buffer-selection state. | Complete WID encoding, direct-color fields, front/back selection for every mode, and raster-operation effects on WID remain unknown. The mode-file structure does not clear a general visual resolver. |
| Block copy | No stock MGRAS textport copy callback or FIFO payload grammar was identified. The diagnostic rectangle test fills and reads a region; it is not evidence of source-to-destination copy. | Source and destination fields, overlap direction, clipping, masks, logic operations, buffer selection, visual mismatch, and copied planes remain unknown. Keep block-copy traffic observable and unsupported until a matching trace is captured. |

The glyph approximation below retains separate high and low latches, executes
only on the execute-qualified write, consumes row words, and clips to the
selected rectangle -- all still true of both FILLMODE-selected modes -- and
leaves WID unchanged except where a mode's own paint sets it. This replaces
the single narrow-glyph guess with two modes selected by register 0x110
(FILLMODE); see "Wide-glyph text: FILLMODE-selected paint modes" below.
The stock textport initialization selects indexed 8-bit color for its main mode
entry. Keep that fact scoped to initialization. A display approximation should
retain a distinct WID plane and make unknown visual entries observable rather
than reducing WID to a Boolean tag.

These software findings do not clear an implementation. A real-SI experiment
must use asymmetric glyph rows with one-bit markers in each word and bit
position, compare high-only and low-register execution, exercise short tails
and clipped edges, and measure background, transparency, cursor advance,
native color, and WID results. A copy experiment must seed asymmetric source
and destination rectangles in color, WID, and auxiliary planes, then test
overlap in every direction, clipping, masks, logic operations, buffer and
visual selection, and plane preservation. An XMAP experiment must program
asymmetric main and overlay modes and buffer selectors, apply fills, glyphs,
masks, logic operations, and any captured copy path, and compare native WID,
native color, and displayed RGB for indexed, direct, front, and back cases.

## Promotion and stop criteria

These experiments produce evidence, not implementation clearance. Retain the
raw accesses, fixture inputs, initial state, machine and mode metadata, native
readbacks, display captures, and a SHA-256 manifest for every retained file.

| Experiment | Retain | Promote only when | Stop when |
| --- | --- | --- | --- |
| Wide glyph | Raw register or FIFO accesses, the `char-modes` replay fixture, initial mode/state, native color and WID readbacks, the display capture, and their SHA-256 manifest. | A FILLMODE-selected pair of modes (bitmap glyph row, byte-pixel transfer) is implemented and covered by direct-record qtests and the `char-modes` replay fixture; QEMU PR 735 is open, held pending the independent review that would promote it. Native readback and the replay's pixel/WID assertions agree for the retained capture. | A run varies without an identified state change, only RGB output is available, native WID or color cannot be read back, any register access remains unexplained, or the skip/s_r padding approximation is contradicted by a wider capture. |
| XMAP and WID | Raw mode and buffer writes, exercised WID and color values, CMAP state, native readbacks, display captures, and their SHA-256 manifest. | Repeated runs determine the exercised main and overlay entries, indexed and direct conversion, front/back selection, buffer selection, and operation-side WID effects. Native WID and color readback agree with displayed RGB for every exercised case. An independent reviewer clears the exact retained artifacts. | Mode or buffer programming cannot be isolated, native WID readback is unavailable, an operation changes WID ambiguously, or a Boolean WID rule is needed to explain the display. |

## First target and pipeline

The first target has one raster engine, two pixel processors, 12 MiB of pixel
memory, no texture hardware, XMAP, two color maps, VC3, a RAMDAC, and one
display path. Texture and multiple-raster variants follow only after this path
has independent coverage.

```text
command or DMA input -> raster state and pixel/WID planes
  -> XMAP visual and buffer selection -> CMAP or direct-color conversion
  -> VC3 timing, blanking, and cursor latch -> host scanout
```

Raster writes and scanout interpretation are separate operations. Host output
is derived from latched guest state, not used as native storage.

## Confidence and prototype reconciliation

| Area | Confidence | Contract requirement | Prototype status |
| --- | --- | --- | --- |
| Bounded fills, logic operations, masks | inferred | preserve native destination bits | partial |
| Glyph expansion (FILLMODE-selected bitmap-glyph-row and byte-pixel-transfer modes) | replay-driven approximation, held pending review (QEMU PR 735); byte-transfer row-advance offset is a stated approximation | preserve native destination bits; log and stay inert on any other FILLMODE value | implemented and qtested, unreviewed |
| Pixel, overlay and window-ID storage | table layout inferred from MAME's Newport VC2 walk, matched by every table the IRIX 6.5.30 server builds on the VC3; table roles and overlay selection inferred | native pixels, a separate overlay store, and window IDs from the VC3 main and overlay display-ID tables | no drawing writes a window ID; overlay-form draws (DRBPOINTERS 0x101c0, COLORMASKMSBS bits 6:4) write only the overlay (QEMU PR 845) |
| XMAP visual resolution | scanout path inferred; the mode word's format field unknown | resolve every pixel's WID to a colour map at scanout and keep an unresolvable visual observable | `cmap[((xmap_mode[wid] >> 5) & 0x1f) * 256 + (native & 0xff)]` for every pixel; direct-colour scanout is unsupported and unmodelled (QEMU PR 738) |
| CMAP capacity | 32 maps of 256 entries inferred from the guest's addressing; physical capacity unknown | retain the observed 8,192-entry candidate until review settles it | the selector's map (bits 4:0) and first entry (bits 15:8) address the 8,192 entries as 32 maps of 256 (QEMU PR 738) |
| Clip rectangle (0x147, 0x148, 0x14f) | field layout established from the X server's disassembly; clipping effect inferred; the enable bit within `0x11` unknown | drop colour and overlay writes outside the inclusive box; clipping only while 0x14f reads exactly `0x11` is an approximation | implemented for every colour and overlay write (QEMU branch `sgi-origami-mgras-clip`); `fill-honours-clip-rectangle`, `overlay-fill-honours-clip-rectangle`, `re-line-clip-follows-xywin-origin` |
| Rectangle fill colour source | inferred; which FILLMODE bit carries the meaning is unknown | take BKGRD_RG under FILLMODE bit 20 or bit 22, PACKEDCOLOR otherwise | implemented; `fill-color-follows-fillmode` and the `cmap-xmap-wid` replay fixture (QEMU PR 738) |
| CMAP entry byte alignment | inferred from guest software, not established hardware | read bits 31:8 of each 32-bit CMAP entry as the 24-bit colour; the low byte's meaning is unknown | implemented in `si_cmap_rgb`; discriminating tests `cmap-high-byte-packing` and the `cmap-high-byte` replay fixture (QEMU PR 732) |
| Palette refresh | emulator consistency requirement; hardware latch timing unknown | refresh existing indexed scanout in the first synchronous model | implemented in the target-first candidate, while latch timing remains approximate |
| VC3 cursor SRAM refresh | emulator consistency requirement; hardware latch timing unknown | refresh an enabled cursor in the first synchronous model | missing |
| Block-copy grammar | unknown | retain raw traffic; do not invent payload or direction | speculative dispatch must be removed |
| Timing and retrace | event classes established; periods unknown | separate completion, latch, scanout, retrace, interrupt | immediate updates are approximate |
| DMA and readback coherence | unknown | define visibility only from evidence | incomplete |

## Target-first functional approximation

QEMU PR 717 (private change-record identity) landed on
`sgi-origami` at `4ddb0e1ce4652f87696b43cde838aa8b2b2be715` and implements the
retained Xsgi workload as a functional approximation. The code-and-test
commit landed as `a352a7d12b8dbebc69228df721a56e0f9526f72d`, patch-identical
to the reviewed `58b5ddc5e1cac2e481381f83429ecbba771b5fe1`. It interprets a
CMAP selector and
its 0 through 31 write position as:

```text
((selector & 0x1f) << 8) |
(((selector >> 13) & 0x7) << 5) |
position
```

The candidate retains 32 XMAP modes and 8,192 CMAP entries. Native WID selects
an XMAP mode, and mode bits 9:5 select a CMAP bank for indexed scanout. It
enforces the one-byte XMAP index, four-byte XMAP mode, two-byte CMAP selector,
and four-byte CMAP data widths observed in the retained workload.

These rules are replay-driven approximations. The retained trace and public
drivers do not establish the selector fields, bank independence, WID encoding,
XMAP mode bit meaning, physical capacity, auto-increment timing, or byte lanes.
The `0x00BBGGRR` color interpretation came from public software and was never
a hardware packing claim; the desktop capture below replaces it with the
channel order the guest's own palette reads in. Unexpected widths remain
observable errors.

Synthetic tests distinguish literal selectors `0xe00f` and `0xe01d`, two
visual selections, two candidate palette banks, auto-increment wrap, reset,
and identical-build migration. At the time this landed, the retained replay
and a live chooser produced byte-identical PPM output; the rules below change
what that replay displays, so the two are no longer compared byte for byte.
This clears the bounded model workload. It does not clear physical SI display
behavior, a stable desktop, or diagnostics.

A later live-trace census found the guest packing each CMAP entry
left-justified in its 32-bit data-port write, `0xRRGGBB00`: every one of
24,708 observed writes has a zero low byte, and reading bits 31:8 of the
guest-written bank recovers an exact primary/secondary ramp with no
rounding, where the prior bits-23:0 read collapsed distinct guest colours
onto the same near-black value. QEMU PR 732 (private change-record identity)
changes `si_cmap_rgb` to read bits 31:8 as the colour; the low byte's
meaning stays unknown. This is inferred from guest software behavior, not
established hardware behavior: hardware that instead latched bits 23:0
would simply display near-black, which no captured evidence rules out. The
discriminating tests are `cmap-high-byte-packing` and the `cmap-high-byte`
replay fixture.

### Selector, WID and scanout after the desktop capture

QEMU PR 738 (private change-record identity) is open on
`sgi-origami`, held pending independent review. A complete capture of a
stock IRIX 6.5.30 X server starting a desktop
(`irix-20260922T133830-79813f`) replaces three of the rules above. It was
read without running the emulator; the reading is retained as
`wp4/xstart/decode/cmap-xmap-rule.md` in the SI 2D scratch tree, and every
statement below carries the classification that reading gives it.

**CMAP addressing. Established for what the guest intends.** A selector
write selects the colour map in bits 4:0 and the first entry in bits 15:8;
the n-th data-port write after it stores at
`map * 256 + ((start + n) & 0xff)`, and a selector write resets the count.
Bits 7:5 are zero in every selector the guest writes. This supersedes the
`(selector >> 13) & 7` block field above, which folded the 29 single-entry
colour-cell allocations the window manager makes onto entries 0 and 32 of
one map. Runs longer than 32 writes and wrap past entry 255 are unobserved
and remain unknown; the earlier literal selectors `0xe00f` and `0xe01d`
still address entries 4080 and 7664, because their bits 12:8 are zero.
Entry packing stays `0xRRGGBB00`. Discriminating tests:
`cmap-selector-start-entry` and `cmap-selector-addresses-the-right-slot`.

**Displayed channel order. Established for what the guest intends.** Entry
bits 31:24, 23:16 and 15:8 are red, green and blue. The guest's own
palette reads as an exact IRIS default set in that order. This is the
guest's intent, not a measurement of the DAC's byte lanes, which stay
unknown.

**Window IDs come from the VC3 display-ID tables. Layout inferred; roles
inferred.** No drawing primitive writes a window ID. The VC3 keeps
display-ID tables in its SRAM in a layout inferred from MAME's Newport VC2
walk (`refs/mame` at `b1da4a1b`, `src/devices/bus/gio64/newport.cpp`,
`vc2_device::begin_did_line`/`next_did_line_entry`), matched by every table
the stock IRIX 6.5.30 server builds on the VC3. Linux
`include/video/newport.h` names only the VC2's register 5, `DENTRY`; it
publishes the register name, not the run-list format, and on the VC2,
register 4 is `CCURSX`, the cursor X position, not a second table. The
VC3's register roles below -- register 4 for the main planes, register 5
for the overlay -- come from the traces alone, not from a VC2
correspondence. A VC3 register holds the address of a frame table with one
word per scanline, and each word points at that line's run list. A
run-list word carries a starting column in bits 15:5 and a window ID in
bits 4:0. The first word applies from column 0, each later word from its
own column, and a column past the screen (`0xffe0`) ends the line. The
stock server keeps two tables:

- **Register 4, main planes (table at `0x500`).** On the greeter, ID 1
  covers the whole screen, which puts the greeter through map 17 as the
  colour evidence above requires. RGB windows get ID 9, whose main mode
  0x7a4 is an RGB mode.
- **Register 5, overlay planes (table at `0x901`).** Posting the Toolchest
  menu rewrites this table with ID 1 over exactly the menu's rectangle
  (run word `0x0d41` is column 106, ID 1, from row 37); the trace has no
  removal, so whether taking it down restores ID 0 is unevidenced. A 4Dwm
  popup gets ID 2 just after the server loads map 26 with the popup's
  palette, and taking it down restores ID 0.

This supersedes the earlier reading that a rectangle fill's nonzero
COLORMASKMSBS wrote a per-pixel window ID from DRBPOINTERS bits 23:16 for
visual classes 11 and 13. The fills that reading rested on are all overlay
draws and clears. Each carries DRBPOINTERS 0x101c0 and COLORMASKMSBS 0x70
or 0xf0. The whole-screen form with logic op CLEAR is exactly how the
server erases a popup over the popup's own rectangle, and the root
window's ID 1 comes from the register-4 SRAM table, written with no fill
involved -- `0x101c0 >> 16 = 1` matched the earlier reading by coincidence.
The two start-up fills with COLORMASKMSBS 0xff and DRBPOINTERS 0x90240 (a
clip-ID set-up and a plane clear) write planes that nothing displayed is
known to depend on; they are not modelled. Evidence: desktop traces
irix-20260925T171500-2f65cc (popup, seq 4051342-4061680) and
irix-20260926T052228-910b39 (Toolchest menu), and greeter trace
irix-20260925T060238-09a4e7. Discriminating tests:
`wid-from-display-id-table` and the `fill`, `cmap-xmap-wid` and `overlay`
replay fixtures (QEMU PR 845).

**Overlay planes. Inferred.** A primitive draws into a separate overlay
store when DRBPOINTERS is 0x101c0 and COLORMASKMSBS enables bits 6:4. The
server also zeroes COLORMASKLSBSA, so the main planes keep their pixels.
Fills take FILLCOLOR_R's index, and glyph set bits and lines take
`RED >> 12`, both through the logic op. XMAP `0x071e80` holds one overlay
mode per window ID, selected by the same `0x071c88` index as the main
mode. The server writes 0xc1, 0xc9, 0xd1, 0xd9, 0x1f1 and 0x1f9 for IDs
0-5. Bit 0 enables the overlay and bits 7:3 name its colour map (24-27
and 30-31); bit 8 is unexplained. The server loads map 25 with the menu
palette and map 26 with the popup palette before it gives those IDs to
their rectangles. A nonzero overlay index covers the main pixel through
its overlay ID's map. Index 0 is transparent, matching the transparent-pixel
record for visual `0x23` in `SERVER_OVERLAY_VISUALS` (run
irix-20260926T050306-f62509, console capture line 272: `0x23, 0x1, 0x0,
0x1, 0x24, 0x0, 0x0, 0x1` -- visual 0x23 carries transparent type 1
(TransparentPixel), value 0, layer 1; visual 0x24 carries no
transparency).

**Scanout. Inferred.** A main CI8 pixel resolves
`cmap[((xmap_mode[main_id] >> 5) & 0x1f) * 256 + (native & 0xff)]`, and an
RGB pixel bypasses the map. A visible overlay pixel resolves
`cmap[((xmap_overlay_mode[overlay_id] >> 3) & 0x1f) * 256 + index]`.
Replaying the greeter trace this way gives the RAD4 reference's panel
(217), outer form (230), hostname (230/179) and Log In/Help (189) values.
The earlier rule put the greeter through ID 9 and rendered those areas at
50-72. Discriminating tests: `target-first-cmap-visual-selection`,
`pseudocolor-scanout-looks-up-cmap` and `display-surface-conversion`.

**Rectangle fill colour. Inferred.** A BLOCKXYENDI fill takes BKGRD_RG,
masked by COLORMASKLSBSA, when FILLMODE has bit 20 or bit 22 set, and
PACKEDCOLOR otherwise. The guest always sets one of those bits and leaves
PACKEDCOLOR zero; OpenBSD's driver writes FILLMODE zero and its colour in
PACKEDCOLOR. Which of the two bits carries the meaning, and whether a
third value exists, is unknown. Lines stay on PACKEDCOLOR: the guest draws
them under four more FILLMODE values and splits its colours across both
registers, so the capture does not determine that path. Discriminating
test: `fill-color-follows-fillmode`.

**RE_POINT and RE_LINE addressing. Inferred.** RE_POINT (0x040) and
RE_LINE (0x041) paint at their own command value, not at
BLOCKXYSTARTI/BLOCKXYENDI, and a line's start point is the pen position
the previous RE_POINT or RE_LINE left behind, not BLOCKXYSTARTI. A
retained Toolchest window-chrome capture drives both registers by direct
form exclusively: all 12 of its RE_POINT executes and all 2,862 of its
RE_LINE executes carry the drawn coordinate in the command word itself,
which the model already latches at `rss[reg]` like any other register.
BLOCKXYSTARTI/BLOCKXYENDI instead sit frozen on an unrelated rectangle
across thousands of consecutive point/line draws in that capture, so
reading them for point/line position paints the wrong pixels. Treating
RE_POINT as a moveto -- it paints its own point and also latches that
position as the pen -- and RE_LINE as a lineto that draws from the
latched pen to its own value and then re-latches the pen there explains
2,861 of the capture's 2,862 RE_LINE executes exactly (the one exception
is the capture's first RE_LINE, whose true predecessor point falls
outside the recording window); the chained strokes trace closed button
and glyph-drawer outlines, most visibly as a rectangle border stroked
twice in immediate succession with a roughly one-pixel offset, matching
Motif's light/dark double-stroke bevel technique. The capture carries no
FIFO-form RE_POINT or RE_LINE execute, so how a FIFO EXEC token's own
declared payload should feed the pen position remains unknown; the model
keeps that path unchanged; a bounded declared payload there is still
consumed so it cannot desync the parser. Discriminating tests:
`re-point-execute`, `re-point-execute-after-bounded-payload`,
`re-line-execute` and `re-line-chains-from-re-point`.

**XYWIN window origin. Established for the field split, inferred for the
flip.** RE_POINT and RE_LINE draw at a per-window screen origin carried in
XYWIN (0x115); `mgrasLineSS` writes it before either register, but
`mgrasPolyFillRectSolid` never writes it before BLOCKXYSTARTI/BLOCKXYENDI.
OpenBSD's `impact(4)` driver (`sys/arch/sgi/dev/impactreg.h`) names the
register `IMPACTSR_CMD_XYWIN`, comments it "YX coords", and packs it
through `IMPACTSR_YXCOORDS(x, y)` expanding to `(y << 16) | x` -- the low
16 bits carry x, the high 16 bits carry y, established from that public
source rather than the capture alone. The origin only applies when the x
field is nonzero: a retained greeter capture's dominant XYWIN value pairs
a zero x field with a nonzero y field (0x03ff0000), and 1,456 of its 1,953
RE_LINE executes carry it while already painting at their correct
absolute position, so gating on the whole register being nonzero would
push those lines off-window. The y field's sense is inferred, not public:
it counts top-down (0 at the window's bottom, 1023 at its top), the
opposite of the framebuffer's own row order, so the model adds
`SGI_MGRAS_HEIGHT - 1 - oy` rather than `oy`. Unflipped, the same
capture's icon line art painted roughly 800 pixels below the window it
targets; flipped, its largest RE_LINE cluster (XYWIN 0x03390146, 379
executes) lands at y 214-311 against the EZsetup icon's own measured rect
(214-312), and its panel-outline cluster (XYWIN 0x033c0143) lands at y
195-671 against the panel's own measured 201-666, the same few-pixel
margin the fill's own inset bounding box already carries. The flip's
magnitude is not capture-only: the driver's one XYWIN write,
`IMPACTSR_YXCOORDS(0, 0x3ff)`, sets y to 1023 (`SGI_MGRAS_HEIGHT - 1`),
exactly the flip's fixed point, matching the capture's own dominant
reset-carrying value above -- but the driver never writes a nonzero x
field, so it settles the fixed point and the field split, not the general
in-window transform for a genuine nonzero window, which remains grounded
in the capture alone. Discriminating tests:
`re-point-execute-applies-xywin-origin`,
`re-line-execute-applies-xywin-origin` and
`re-line-execute-ignores-zero-x-field-xywin`.

**Clip rectangle, 0x147, 0x148 and 0x14f. Established for the field
layout, inferred for the effect.** The IRIX 6.5.30 X server draws a
primitive once for each box of the window's clip list. Before each pass
`mgrasPolyPoint` in the shipped `impactsr.so` (SHA-256 `726dc1e2...`,
disassembled with `tools/origami disasm build`, around `0x43be2c`) loads
the box into 0x147 and 0x148 and `0x11` into 0x14f. After the last box it
loads 0 into 0x14f and restores XYWIN. Its arithmetic on an X box
(x1, y1)-(x2, y2), end exclusive, is:

```text
0x147 = x1 << 16 | (x2 - 1)
0x148 = (H - y2) << 16 | (H - 1 - y1)    H = screen height, 1024
```

Both are inclusive ranges, and 0x148 counts rows from the bottom, the same
sense as XYWIN's y field. The layout is read from the guest's own code.
That the raster engine drops pixels outside the box is inferred. No public
source names 0x147 or 0x148; gxemul's BSD-licensed
`src/include/thirdparty/impactsr-bsd.h` names 0x14f
`IMPACTSR_CMD_WINMODE` but gives none of its bits or values. Redrawing the same primitive once
per box only makes sense if the hardware discards what falls outside. The
desktop session in run `irix-20260927T102421-204445` fits that reading.
Of the executes drawn with 0x14f set, the BLOCKXYSTARTI-BLOCKXYENDI
rectangle of all 99,540 CHAR_L executes (88,515 glyph rows and 11,025
byte-transfer units) and all 1,738 IR_ALIAS transfer heads
overlaps its box under the bottom-up reading, and none overlaps it under a
top-down one. Of the 3,715 clipped RE_LINEs placed by a nonzero XYWIN,
3,617 overlap their box only after the origin is applied, 70 overlap it
either way and 28 not at all, so the box is in screen coordinates. Only 0 and `0x11` appear in 0x14f, so which of its bits
enables the clip is unknown. As an approximation the model clips every
colour and overlay write only while 0x14f reads exactly `0x11`, and logs
any other nonzero value and leaves the clip off. The session's 1,709
clipped overlay-form executes run in the same per-box loop and all meet
their box. Every DMA image draw in that session ran with the
clip off, so whether DMA writes are clipped is untested.

Before the model honoured the box, the Icon Catalog's icon shadows painted
through the Background Setting panel stacked over it. At sequence 5094536
the server draws one diamond shadow (x 293-332, y 176-194) three times,
under boxes x 129-683 by y 125-127, x 129-151 by y 128-447 and x 500-683
by y 128-447. Those are the pane's strips around the panel at x 152-499,
and the diamond meets none of them. A replay of the session up to the end
of the second login shows the panel clean with the clip and the shadows
present without it. The same replay shows the server also uses the box to
trim padded byte transfers: the panel's greyed-out radio labels are sent
as transfers one or two columns wider than their box (x 384-386 under box
x 385-386, for example), and without the clip those extra columns paint
stray pixels through the label text. Discriminating tests:
`fill-honours-clip-rectangle`, `overlay-fill-honours-clip-rectangle` and
`re-line-clip-follows-xywin-origin`.

**Stippled lines. Established for the colour roles, inferred for the
pattern alignment.** The IRIX 6.5.30 server draws stippled spans and
polygons as one-row RE_LINEs under FILLMODE `0x20` or `0x60`, writing a
32-bit pattern to register 0x15a before each row. `0x60` also loads
BKGRD_RG/BKGRD_BA and `0x20` does not. A set pattern bit paints the line
colour (RED bits 19:12 under CI8, PACKEDCOLOR under RGB). A clear bit
paints the background under FILLMODE bit 6 and leaves the pixel alone
otherwise. The background is BKGRD_RG bits 7:0 as an index under CI8.
Under RGB the model takes red from BKGRD_RG bits 11:4, green from bits
23:16 and blue from BKGRD_BA bits 11:4. That channel assignment is
inferred: only a white background appears in the captures. OpenBSD's
`impact(4)` `impact_putchar()` packs its opaque-text background the same
way (red's top nibble in BKGRD_RG bits 11:8, green in bits 23:16, blue's
top nibble in BKGRD_BA bits 11:8). The greeter capture shows both roles against the RAD4 reference
frame: the login icons' bodies are `0x60` spans with PACKEDCOLOR
`0xaaaaaa`, BKGRD white and patterns `0xaaaaaaaa`/`0x55555555`, and the
reference has a 170/255 checkerboard there; the text cursor is `0x20`
with pattern `0x30c30c30` over the name field, and the reference has a
2-pixel I-beam stem with the field colour on both sides.

The pattern starts at the line's first pixel, bit 31 first. Each of the
greeter's 798 opaque spans carries `0xaaaaaaaa` when its start x + y is
even and `0x55555555` when odd, which keeps the checkerboard fixed to the
screen only if the pattern restarts at each span; a replay of the greeter
capture under this rule matches the reference's checkerboard phase pixel
for pixel on the rows checked. The parity itself is the server's stipple
origin, not a hardware rule: the desktop capture
`irix-20260925T171500-2f65cc` uses the opposite parity for 7,320 of its
12,185 opaque spans, constant within each drawable's batch. Every stippled line in the captures is
horizontal and preceded by its own RE_POINT and pattern write, so
stepping one bit per drawn pixel along a diagonal or vertical line, and
restarting at an RE_LINE chained from the previous one, are untested. No
capture draws a stippled line into the overlay; the model paints an
opaque background into the main planes only. Discriminating tests:
`re-line-opaque-stipple-starts-at-line` and
`re-line-transparent-stipple-keeps-clear-bits`.

**Direct-colour DMA pixel byte order. Inferred.** A 4-byte-per-pixel DMA
image transfer carries big-endian 32-bit pixels with red in bits 7:0:
bytes 3, 2 and 1 are red, green and blue, and byte 0 is not stored. The
public drivers move this layout under the same HQ pixel format (`0x600`)
and PP1FILLMODE format bits (`0x62xx`) the IRIX server uses: X.Org's
impact driver (`impact_shadow.c`, visual masks 0xff/0xff00/0xff0000) and
the Linux Impact framebuffer patch posted to linux-mips on 2011-09-11
(red at offset 0). That patch's `impact_smallcopy()` reads with
PP1FILLMODE `0x2200` and pixel format `0x200`, the IRIX server's
save-under read values, and writes back with `0x6200` and `0x600`, the
round trip the model's read path mirrors. The greeter's EZsetup icon is
such a transfer. Read with red in byte 1 it
showed a cyan background and a blue-grey box where the same server
dithers the icon to yellow and red on the 8-bit RAD4 reference; red and
blue exchanged, each of its main colours takes the reference's hue
((58,235,255) becomes (255,235,58), (9,191,254) becomes (254,191,9)) and
the box turns cardboard tan. The screen-to-host read is the exact
inverse.

The same drivers pass that `0x00BBGGRR` word as PACKEDCOLOR and as
pair-packed CHAR_H/CHAR_L pixels (0x159 bit 7), and the model reads both
paths with red in bits 7:0 too. FILLCOLOR_R/G/B and BKGRD are per-channel
registers and are not affected. Every PACKEDCOLOR and pair value in the
three greeter and desktop captures has red equal to blue, so none of them
discriminates. The IRIX 6.5.30 Icon Catalog does: trace
`irix-20260927T035120-7dd191` fills its icon pane olive with FILLCOLOR
0x800/0x800/0x300 and stipples the same colour as PACKEDCOLOR 0x308080,
and the two agree only when the word is read with red low. Discriminating
tests: `dma-image-transfer-paints-from-host-memory`,
`dma-image-transfer-reads-rgb-red-in-low-byte`,
`rgb-pixel-words-read-back-unchanged` and
`packedcolor-matches-fillcolor-channels` (QEMU PR 890 (private change-record identity)).

Replaying the capture through the model reproduces the same rules applied
offline to within 3,610 of 1,310,720 pixels, the differences being the
composited cursor, the glyph cells the model still cannot draw, and the
byte-transfer cells the offline reading drops. The retained chooser replay
keeps legible labels. This clears none of these rules as hardware
behavior: what would settle them is an authoritative MGRAS display
specification, or a real-board capture that tags two regions with
different WID values, paints the same index into both, and reads back
native colour, WID and displayed RGB.

**12-bit RGB windows. Inferred.** A paint whose PP1FILLMODE has bit 13
clear, bit 14 set and bits 10:8 zero belongs to a window on the
server's 12-bit TrueColor visual. The pixel keeps the top four bits of
each channel, red in bits 3:0, green in bits 7:4 and blue in bits 11:8,
and scanout widens each four-bit channel to eight bits by repeating it
and bypasses the colour map, as for 24-bit pixels. FILLCOLOR_R/G/B and
BKGRD feed it per channel, and PACKEDCOLOR as a whole word with red in
bits 7:0, as for a 24-bit paint. Evidence:

- The IRIX 6.5.30 Icon Catalog's icon pane is such a window. On a live
  model boot (run `irix-20260927T045444-f4307c`), `xwininfo` gave the
  pane (555x323) depth 12 and visual `0x29` (read live, not kept), and
  `xdpyinfo` lists
  TrueColor visuals `0x29` and `0x2a` at depth 12 with red, green and
  blue masks `0xf`, `0xf0` and `0xf00`. Those masks are the only
  evidence for the channel positions: no capture reads a 12-bit pixel
  back.
- In the traced session `irix-20260927T035120-7dd191`, the server gives
  the pane display ID 14 over x 97-651, y 93-415 and writes that ID's
  XMAP mode as `0x405` once, when the window opens. It clears the pane
  five times under PP1FILLMODE `0xc804` with COLORMASKLSBSA `0xffffff`,
  and issues 2,699 fills, lines and glyph rows in it under `0x4004`
  with COLORMASKLSBSA `0xfff` (seq 3803845-3829194). Its colour
  registers hold whole nibbles, FILLCOLOR `0x500/0x800/0xa00` for the
  pane's blue. Read as CI8, as the model did before, every one of these
  paints stored index 0 and the pane showed black.
- Across all four retained MGRAS traces the rule changes no other
  paint's format: 0x4004 and 0xc804 occur only in this one, and the
  other encodings (CI8 `0x500`, `0x4203`, `0x4300`, `0x4504`; 24-bit
  `0x2200`, `0x6204`, `0x6300` and the rest) keep their earlier
  classification. The public drivers use only `0x62xx`/`0x63xx`, so the
  rule rests on these traces.

Replaying the trace under this rule draws the pane's field, icons and
labels where the RAD4 reference frame has them and changes no pixel
outside the pane. Unknown: what PP1FILLMODE bits 14, 11 and 10:8 mean
on their own, including what bit 11 adds to the clear; how the hardware
widens a four-bit channel (repetition keeps 12-bit white at 255,
shifting would give 240, and the reference cannot separate the two
through the SI DAC gamma); the server's 12-bit PseudoColor visuals
`0x25`-`0x28`, which no capture draws to and the model does not
recognise; a 4-byte DMA read of a 12-bit pixel, which returns the
nibble word as if it were a 24-bit pixel; a 4-byte DMA draw into a
12-bit window, which the model tags 24-bit; a pair-packed CHAR_H/CHAR_L
transfer into a 12-bit window, which the model reads as PACKEDCOLOR
words with red in bits 7:0 and keeps the nibbles of; and a PIO pixel
pair (BKGRD_BA execute) into a 12-bit window, which the model logs as
unimplemented. No capture does any of the last four. What would settle
them is a real-board readback of a 12-bit window's pixels and its
displayed colour. Discriminating tests:
`rgb12-fill-packs-four-bit-channels`,
`rgb12-line-and-glyph-take-top-nibbles` and
`rgb12-pixel-word-reads-red-low` (QEMU PR 906 (private change-record identity)).

## Display-ID tables and the overlay plane

QEMU PR 845 (private change-record identity) is open on
`sgi-origami`, held pending independent review. It replaces the WID-plane
fill rule above -- a fill with a nonzero COLORMASKMSBS tagging pixels
directly -- with the VC3's own per-pixel window-ID resolution, read from
its SRAM rather than written by paint operations, and adds the overlay
plane the fill rule's evidence could not otherwise explain.

**Table layout and register assignment. Inferred.** Register 4 points at
a frame table for the main planes and register 5 at one for the overlay.
Each frame table holds one SRAM word per scanline, and each frame word
points at that scanline's run list: a run-list entry is a 16-bit word, a
starting column in bits 15:5 and a window ID in bits 4:0. The first entry
applies from column 0, each later one from its own column, and a column
at or past the screen edge (0xffe0) ends the line. This layout is not
published: MAME's Newport model walks its own display-ID table this way
(`refs/mame` `b1da4a1b278cf41300e95cce35b4d1f03a22023e`), and every table
a stock IRIX 6.5.30 X server builds -- for popup menus, the Toolchest
menu and its main-plane windows -- matches a walk under that layout. It
is not the VC2's published DENTRY register: on the VC2, register 5 is
DENTRY but register 4 is CCURSX, the hardware cursor's X position (Linux
`include/video/newport.h`), a different purpose entirely. Which VC3
register serves which planes -- 4 for the main planes, 5 for the overlay
-- likewise comes from the trace, not from the VC2's roles: the server
rebuilds the table at register 5's address with a menu's own ID over
exactly the menu's rectangle when it posts a popup or Toolchest menu on
its overlay visual, loading that ID's overlay colour map just before; it
rebuilds register 4's table with ID 9, whose main mode is an RGB mode,
over its RGB windows and ID 1 (the root window's map) everywhere else.
Discriminating test: `wid-from-display-id-table`.

**A zero frame-table entry is an ordinary pointer, not an empty row.
Unknown.** MAME's walk has no zero-pointer case: it addresses SRAM word 0
like any other run list, and no capture exercises a scanline whose frame
entry is still its post-reset zero. An earlier revision of this model
special-cased a zero frame entry as an untagged, all-WID-0 row; PR 845's
review found that case inferred from a qtest helper's own read-back
convenience rather than from the hardware, and it has been reverted for
lack of evidence. The model dereferences the frame entry unconditionally,
matching MAME, and no test pins this case either way. What would settle
it: a real-board capture, or an authoritative VC3 specification, showing
what an untagged scanline's frame entry actually holds and what the
hardware does when it is zero.

### The GL window's display ID

**Why the GL window shows black before anything draws. Established for
the model; unknown for the board.** When the probe creates its
128x128 window at (256, 256) on the 12-bit-per-channel visual `0x30`,
the X server writes XMAP mode `0x1408` for display ID 14 (index byte
`0x38`, run `irix-20260927T135825-92207b`, seq 3899859-3899860) and
rebuilds the register-4 table with ID 14 over exactly the window. The
window has no background, so nothing repaints its pixels: they keep the
desktop's indexed backdrop (native value `0x0e` at (300, 300)). Mode
`0x1408` has bits 9:5 zero, so under the scanout rule above an indexed
pixel resolves through colour map 0. The server had not loaded map 0
by seq 3909700: colour-state snapshots at seq 3900999 and 3909700 both
show only maps 17, 25, 28 and 29 loaded and all 256 entries of map 0
zero. So the window shows black.
A replay of the capture agrees: the frame just before the case matches
the live desktop, and the frame after the mode write and the display-ID
rebuild matches the live black window, both except for the 188 cursor
pixels the replay does not draw.

This is a property of the model's scanout, not a GL step. What the board
shows for leftover indexed pixels under mode `0x1408` is unknown; the
mode is presumably a direct-colour one (its colour-map field is zero and
the visual has 12 bits a channel), so the board would show those planes
reinterpreted, not a map lookup. The model's GL clear and triangle store
direct-colour pixels, which bypass the map, so they display correctly
under the same ID. A capture that reads back the planes and the
displayed colour of a fresh window on this visual would settle the
board's behaviour. Test: `gl-clear-shows-through-a-direct-colour-display-id`.

## Wide-glyph text: FILLMODE-selected paint modes

QEMU PR 735 (private change-record identity) is open on
`sgi-origami`, held pending independent review, at
`e84435d2a37d46fa2f50150ea764debea7d28d82`. A live capture
(`irix-20260922T055519-8b72de`) found that a CHAR_H/CHAR_L execute's effect
is selected by register 0x110, publicly named FILLMODE by OpenBSD's
`impact(4)` driver -- not the caller-derived "four row words, high-then-low"
packing the stock-software reading above once assumed. An earlier revision
of this candidate instead read CHAR_L as row-pairing (from disassembly of
`mgrasImageGlyphBlt` alone) and was dropped for producing no legible text
against the model's own scroll-window replay; the capture below is a
direct empirical replacement, not a refinement of that reading. Both modes
below are retained as replay-driven functional approximations pending the
review this PR is held for:

- FILLMODE == `0x10400008` (bitmap glyph mode): an execute on either CHAR_H
  or CHAR_L paints one row of up to 64 columns. CHAR_H bits 31:0 are columns
  0-31 and CHAR_L bits 31:0 are columns 32-63, both MSB first; a set bit
  paints PACKEDCOLOR and a clear bit paints BKGRD_RG, matching the existing
  narrow 16-column CHAR_H-only rule's color roles. Neither changes the WID
  plane, which only a rectangle fill writes. The row counter advances by one per
  execute regardless of which register carried it; BLOCKXYSTARTI resets it
  and BLOCKXYENDI clips it, both unchanged from the existing bounded-fill
  rule.
- FILLMODE == `0x10c00000` (byte-pixel transfer mode): only a CHAR_L
  execute paints -- CHAR_H stages first and never executes on its own --
  delivering eight 8-bit indexed pixels per execute: CHAR_H's four bytes
  MSB to LSB, then CHAR_L's four, left to right in raster order. The
  capture also writes 0x102 = 2 and size registers 0x158 = 0x153 =
  `(height << 16) | width` alongside this mode, consistent with a plain
  byte-per-pixel transfer rather than a bit-packed glyph; the model does
  not read these back. The server sends each row as the 8-byte-aligned
  words of its source buffer that cover the row, so a row's first execute
  unit does not always start at column 0, and the row only advances once
  enough units cover its width. The offset is
  `skip = (row * pitch) % 8`, with the source pitch equal to the width
  plus 0x159 bits 16:14. **Inferred**: that pitch predicts the execute
  count of every one of 1,445 eight-pixel transfers in the IRIX 6.5.30
  greeter and desktop captures (`irix-20260925T060238-09a4e7`,
  `irix-20260925T171500-2f65cc`, `irix-20260926T233350-0df6d0`). The
  width rounded up to 32 bits, the reading this replaced, fits all of them
  except the 68 greeter text-cursor restores: 6 pixels wide, 0x159 =
  `0x18000` (field 6). Their execute count alone does not fix the pitch:
  any pitch of 2, 4 or 6 mod 8 gives 30 executes for 20 rows, the bare
  width included (which fails only two other transfers). Two things
  settle pitch 12. The save-under DMA read that fills the restore buffer
  has descriptor pitch 12 (desc-20000 register 4 = `0xc`, 6 bytes per
  row, live trace seq 3685701). And the restore data (`16161616
  16160000`, then `00000000 16161616`, `16160000 00000012`, ...) places
  the name field's index 0x16 at pitch 12, every odd row starting 4 bytes
  into a unit. Under the 32-bit
  reading the restored area showed black rungs across the cursor; with
  the field it shows the RAD4 reference's I-beam. Only the execute counts
  and this one restore's content test the field; no transfer with a
  non-periodic image separates it from the 32-bit reading elsewhere.
- Byte-pixel transfer mode carries a second guest-side convention under
  the same FILLMODE value, discriminated by register 0x159. **Inferred**,
  from exactly two observed values in the guest's own command stream, not
  from an independent datasheet citation or manual reading: 0x159 ==
  `0x8000` keeps the eight-pixel convention above (the Toolchest boxes and
  desktop transfers, `char-modes.replay`); 0x159 == `0x0080` selects a
  different packing used by the xdm chooser's own icons (captured in
  `xsgi-mask-dynamic-0x81.replay`, group ROOT of `root-icon.replay`),
  where each CHAR_L execute instead delivers two indexed pixels: pixel 0
  from CHAR_H bits 7:0, pixel 1 from CHAR_L bits 7:0 (the capture's other
  bytes did not reliably replicate that value, so only bits 7:0 are read),
  `ceil(width / 2)` executes per row, no skip, and the row advances after
  the last unit. WID, mask and colour operation resolve through the same
  per-pixel path as the eight-pixel convention. Any other 0x159 value in
  byte-transfer mode is unimplemented and stays observable (`LOG_UNIMP`)
  and inert. QEMU PR 744 (private change-record identity)
  is open on `sgi-origami`, held pending independent review, at
  `aa5191ce647410c4c52f2a64267dcbd5ca3937bf`.

Any other FILLMODE value, or an execute on the register that mode does not
use, is unimplemented and stays observable (`LOG_UNIMP`) and inert --
including FILLMODE's own reset default of zero. Discriminating tests are
`test_direct_glyph_row`, `test_direct_glyph_row_char_h_only`,
`test_direct_glyph_row_char_l_execute`, `test_direct_byte_row`,
`test_direct_byte_row_pitch_from_padding_field` and
`test_direct_byte_row_pairs` in `tests/qtest/sgi-mgras-fifo-test.c`,
and the `char-modes` and `root-icon` replay fixtures
(`tests/data/mgras/char-modes.replay`, `tests/data/mgras/root-icon.replay`),
the latter checked to fail on the model before PR 744 and pass after. This
clears the bounded model workload captured. It does not clear physical SI
display behavior, the padding-field reading of 0x159 bits 16:14, or the
0x159 register's meaning beyond these observed values.

## Native resources

Represent separate guest-visible resources even when physical packing is
unknown: main front and back color storage; depth and stencil storage;
overlay storage; the VC3 display-ID tables; XMAP state mapping window IDs
to visual and buffer selection; and host scanout storage.

Plane widths, packing, stride, interleave, bank ownership and clear values
remain unknown, and so does how the overlay index is packed into the
COLORMASKMSBS planes. Unknown visuals must not silently alias the default
visual. Window IDs are not stored per pixel; they come from the display-ID
tables described above. Fixtures must still settle GXnoop, partial-mask and
mixed-visual overlay effects before promotion.

## Display state

Display state includes XMAP mode and buffer selection, the promoted CMAP
address space, indexed and direct-color conversion, VC3 timing RAM and
blanking, retrace, cursor origin and image SRAM, cursor colors and clipping,
RAMDAC format and sync state, and display enable independent of raster progress.

The first captured mode is 1280x1024. Cursor colors come from selected color
map state; a hard-coded white cursor is not a contract.

**Cursor position origin -- inferred.** VC3 registers 2 and 3 hold the
hardware cursor's X and Y, the same indices the VC2 uses for `CURSX` and
`CURSY` (Linux `include/video/newport.h`). The model takes the value as
the column and row of the 32x32 glyph's top-left pixel plus 31, so a
register pair of (31, 31) puts the glyph's corner at the screen's corner.

The evidence is what the IRIX 6.5.30 desktop writes in model run
`irix-20260927T102421-204445`. Every enabled write fits register =
pointer - hotspot + K, with one K and a fixed hotspot per cursor shape:

| Shape's hotspot if K = 31 | Pointer | Written |
| --- | --- | --- |
| (0, 0) | (0, 0), (48, 48), (640, 512) | (31, 31), (79, 79), (671, 543) |
| (1, 1), the common arrow | (0, 0), (640, 512), (640, 700) | (30, 30), (670, 542), (670, 730) |
| (14, 1) | (0, 0) | (17, 30) |
| (13, 0) | (48, 48) | (66, 79) |

The trace fixes K minus the hotspot for each shape, not K alone. 31 is the
smallest K that keeps every observed hotspot non-negative, since the first
shape then has its hotspot at the corner. A larger K with every hotspot
shifted by the same amount is not excluded. X server clicks land at the
pointer position, and the model's earlier scanout, which drew the glyph at
the register value itself, put the arrow's first black pixel 31 pixels
right of and below the pointer.

The first cursor writes in the trace are a raw pair of (640, 512), at
trace lines 10574 and 2203739. Register 0x1d has not yet been written at
the first; three lines later it is written 0xfffc, and it still holds
0xfffc at the second. Its bit 1, the model's inferred cursor enable, is
clear throughout. They look like driver initialisation, not pointer
positions, and do not bear on the offset.

The VC2 sibling is consistent with this. Linux
`drivers/video/console/newport_con.c` programs `CURSY` as the text row's
top plus 31, and `CURSX` as the column's left edge plus 29, or 21 on board
revision 3. That driver's glyph layout was not checked, so it supports the
convention without proving it. The board revision dependence suggests the
X offset can vary with the video pipeline. The IRIX server writes the same
offset on both axes for the VC3 in the 1280x1024 mode, and no other mode
has been captured.

A VC3 specification would settle it. So would real SI output with the
pointer warped to a known position, but at different precision: a
photograph tells 31 from 0, while telling 31 from 32 needs a pixel-exact
capture of the video output.

The first synchronous model does not claim compatibility with stock
`impact.so` timing data or `vof/*.sdb` selection. A selected target must retain
the guest-selected timing-data identity and values, then a reviewed capture
must establish the safe update boundary before stock timing compatibility can
be accepted.

## Visibility ordering

| Event | Required ordering |
| --- | --- |
| Raster completion | native pixel and WID writes complete first |
| CPU or DMA readback | observes the minimum preceding work required by the fence |
| Buffer, WID, or mode update | reaches scanout only at its latch boundary |
| CMAP write | first model refreshes indexed scanout; hardware latch point unknown |
| VC3 cursor SRAM write | first model refreshes the enabled cursor; hardware latch point unknown |
| Cursor movement | damages old and new cursor rectangles |
| Retrace | latches updates and raises only evidenced indication; the retrace interrupt enable and delivery are in the [HQ4 retrace interrupt](mgras-hq4.md#retrace-interrupt) section |
| Blank or unblank | changes presentation without implying raster completion |

The first implementation uses synchronous visibility and a full refresh for
palette, cursor, mode, and timing changes. This is an explicit functional
approximation, not a hardware timing claim. Guest-visible latch and retrace
ordering remains unimplemented until a reviewed fixture establishes it.

## Evidence from SGI's Octane IDE diagnostic

SGI's IRIX 6.5.30 Octane standalone diagnostic, `ide.IP30` (SHA-256
`19d7a194cb417330475df9a187379404d413c72433cfc97fc9e238f0fe170d69`), tests
the XMAP, VC3 and DAC by writing values and failing the board unless it
reads back what it expects. Its listing and routine names are published under
`docs/analysis/ide.IP30-stand/` and `docs/library/ide.IP30-stand.symbols.txt`;
addresses below are listing addresses in that object. These are software
expectations of a working board, not silicon measurements. Where the
diagnostic accepts more than one value, the physical value is still unknown.

**XMAP indexed registers.** The XMAP test (`mg_xmapdcbreg`,
`0xa80000002086907c`) sets the index at `0x071c88` and then:

- reads `0x071d00` under index byte 4 as the XMAP revision and passes only on
  0 or 1; the shared IRIX bring-up path reads the same register the same way
  and keeps its low three bits (`0xa8000000208a99cc`). In the IRIX 6.5.30
  kernel, `MgrasGatherInfo` stores that value in its board-information
  record and is the only kernel routine that touches that byte; programs
  that read the record through the kernel were not checked;
- reads `0x071d00` under index byte 8 as the FIFO capacity and passes only
  when the low nibble is 1;
- writes eight patterns to `0x071d80` under index byte 0 and reads them back
  in full ("main buffer select"), then under index byte 4 and compares the low
  8 bits ("overlay buffer select").

The bring-up path writes byte `0x48` to `0x071d08` under index 1 and
`((RE count - 1) & 3) << 20 | 0x8073f` to `0x071d00` under index 0
(`0xa8000000208a9458`). This contradicts the [HQ4 swap-scheduling
reading](mgras-hq4.md#swap-scheduling) that index 4 of `0x071d80` is a second
swap-mask word for windows 32 and up: the diagnostic names it the overlay
buffer select and tests only 8 bits. Unresolved until the kernel's index-4
write is read against this. The mode files `0x071e00` (tested mask `0x7fffff`)
and `0x071e80` (mask `0x3ff`) are addressed with two index bytes, the entry
number and then 0, where the buffer select takes one. How the XMAP combines
the two bytes is unknown.

**VC3 register names.** The VC3 register test (`mg_vc3internalreg`,
`0xa80000002083da2c`) stops timing, then writes walking-one, walking-zero, 0
and `0xffff` patterns to registers 0 through `0x13` and requires each to read
back in full. It names them: 0 VID_EP, 1 CURS_EP, 2 CURS_X_LOC, 3 CURS_Y_LOC,
4 DID_EP1, 5 DID_EP2, 6 SCAN_LENGTH, 7 RAM_ADDR, 8 VT_FRAME_TBL_PTR,
9 VT_LINE_SEQ_PTR, `0xa` VT_LINES_RUN, `0xb` VERT_LINE_COUNT, `0xc`
CURS_TBL_PTR, `0xd` WORKING_CURS_Y, `0xe` CUR_CURSOR_X, `0xf` unused, `0x10`
DID_FRAMETAB1, `0x11` DID_LINETAB1, `0x12` DID_FRAMETAB2, `0x13`
DID_LINETAB2. Registers 4 and 5 are therefore two display-ID entry points,
each with its own frame-table and line-table registers, which agrees with the
inferred frame-table/run-list layout above but does not say which of the two
serves the main planes. Register 7 is the SRAM address, so it reads back as
the address the write loaded. Register `0x1f` carries the revision in bits 7:6
(`0xa80000002083e6b0`) and six status fields the diagnostic reports without
judging (normal operation or soft reset, VTG, DID1 and DID2 FIFO underflow,
cursor error, pixel-clock range).

**DAC register access and the gamma-load preamble.** The DAC reset
(`mg_dacreset`, `0xa800000020854920`) addresses DAC registers through the
halfword at `0x071810`, register number in the high byte, and moves their
data a byte at a time through `0x071910`. A byte write at `0x071811` sets the
register number, not the halfword's low byte: the DAC address test
(`mg_dacaddrreg`, `0xa800000020853c2c`) writes a byte there and passes only
when it reads back in bits 15:8 of the halfword at `0x071810`, then writes the
halfword with the pattern in its high byte and checks the same bits. The
halfword's low byte is set only by a halfword write, as in `0x0402`, and the
diagnostic never reads it back. The DAC reset pulses `0x071988` (3, 2, 3),
writes `0x89` to command register 1 through the byte port (`0x071811` = 5,
then data), writes register `0x0d00` = `0x40`, sets address 0 and requires it to read back 0, then reads
register `0x0300` (DAC ID) and `0x0b00` (revision): ID `0x79` with revision 1
passes, ID `0x76` with revision 0 passes with a warning. It then writes and
reads back, masked `0xff`, pixel mask `0x0400` = `0xff`, command registers
`0x0500` = `0x89`, `0x0600` = `0xe4`, `0x0700` = `0xc0`, `0x0800` = `0x06`,
and cursor register `0x0402` = `0x01`. A captured IRIX 6.5.30 desktop session
(gamma capture (citation: `docs/notebooks/si-dac-gamma-20260924.md`), attributed there to
the X server's DDX) writes `0x071811` = 5 and `0x89`, then the same `0x071810`
addresses in the same order before each gamma-table load (`0xd00`, `0x400`,
`0x500`, `0x600`, `0x700`, `0x800`, `0x402`, then 0), with the same data bytes
except `0x0600`, which gets `0xec` there and `0xe4` in the diagnostic. Those
writes, recorded as unexplained in the model, are this register programming
followed by address 0 for the table data. Established as software expectation
from two SGI programs; the DAC part itself is not identified.

**Identity reads.** The diagnostic's own inventory (`0xa80000002083e540`)
reads the board version bytes as the top bytes of 32-bit reads of `0x072408`
and `0x072488`. From the second: GE count in bits 1:0, GD revision in bits 3:2
(the kernel and the shared bring-up path take bits 4:2), product ID in bits
6:5. From the first: bits 1:0 equal to 1 mean four TRAMs. The diagnostic's
own inventory decodes product ID 2 as one RE4 and 3 as two, and leaves the RE
count untouched for 0 and 1; the shared bring-up path and the kernel decode 1
as one RE4 with no TRAM, which is the model's SI. The CMAP revision check
(`mg_cmaprev`, `0xa800000020824794`) takes bits 2:0 of `0x071308` and of
`0x071708`: it reports an error and returns -1 when either is below 2, and
warns at 5 or above. The inventory warns when that result is 4 or more, which
includes the -1 error return, and continues; so only 2 and 3 pass silently.

## Reset, errors, and coverage

The reset rows below are evidence questions, not required hardware behavior.
The selected target must establish effects and retention before implementation.

| Reset event | Effects to establish | Retention to establish |
| --- | --- | --- |
| Cold reset | queues and active operations; documented display reset state | pixel memory, CMAP, XMAP, VC3 RAM, cursor SRAM, DAC |
| Warm reset | selected command and raster state | native planes and display programming |
| Link reset | transport-visible status | raster planes, display state, pending local work |
| VC3 or XMAP reset | corresponding display controller | native planes and unrelated raster state |
| DMA/readback recovery | faulted transfer and status | completed writes and display state |

Invalid raster state, out-of-range memory, unsupported visual, absent texture,
display-table error, timing loss, and DMA/readback fault are distinct classes.
Until reporting paths are evidenced, stop at the nearest known boundary,
preserve unrelated state, and log the raw event.

| Capability | Required test | Status |
| --- | --- | --- |
| Clear, fill, logic, mask | bounded qtests and native-plane checks | partial |
| Text (FILLMODE-selected bitmap-glyph-row and byte-pixel-transfer modes) | direct-record qtests and the `char-modes` replay fixture | implemented, held pending review (QEMU PR 735) |
| Native pixels, overlay and window IDs | mixed visual and masked-operation tests | display-ID tables, fills that leave IDs alone, overlay draw, erase, XOR and composition are covered; GXnoop and partial-mask effects are missing |
| XMAP and full CMAP | indexed/direct and palette mutation tests | indexed resolution and palette mutation covered through two maps; direct colour is unsupported and hardware mapping remains unknown |
| Cursor live updates | SRAM, palette, movement, clipping tests | position origin and left-edge clipping covered by the `cursor-position-origin` qtest; top-edge clipping is exercised by the same test but a missing check only shows under a sanitizer build, because the stray write lands before the frame buffer; SRAM, palette and movement tests missing |
| Buffer and mode latching | update and retrace ordering tests | missing |
| Block copy | raw capture and negative test only | speculative implementation must be removed |
| DMA and readback | evidence-bound coherence tests | missing |
| Depth, blend, texture, OpenGL paths | later target | deferred |

Complete raster grammar, memory packing, unobserved visuals, texture hardware,
multiple-raster ownership, video I/O, dual-head modes, analog fidelity, and
routed attachment behavior remain outside the first target.
