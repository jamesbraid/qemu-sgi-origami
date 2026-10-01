# MGRAS family architecture: review and design

References rendered as citation identities name material outside this document
set. Those targets are not included or cleared for redistribution. Review and
runtime statements retain their recorded dates and scope.

**Status:** Proposed design, held for independent review. No model code
changes accompany it.

**Date:** 2026-09-24

**Reviewed revision:** QEMU `sgi-origami` at `0ed18fce2d` (`hw/misc/sgi-mgras.c`,
`hw/misc/sgi-mgras-raster.c`, `hw/misc/sgi-mgras-display.c`,
`include/hw/misc/sgi-mgras.h`, `tests/qtest/sn0-origin2000-mgras-test.c`,
`tests/qtest/sgi-mgras-replay-test.c`, `tests/data/mgras/`), Origami `main` at
`ea66f076bb`, and the unlanded colour-source branch
`sgi-origami-si-ci8-colour-20260924` at `5ed10fd7d8`.

## Summary

The merged MGRAS model emulates one board, a textureless Octane-generation
Solid IMPACT (SI) transplanted into an Origin 2000, and its code says so:
every function is `si_*`, the planes are sized by two compile-time
constants, and the board-version, raster-status and inventory registers
answer fixed numbers. The published record shows that almost none of that
behaviour is SI-specific. The Octane Technical Report describes four
configurations built from the same ASICs behind one HQ4 host interface and
one display back end; the public drivers (OpenBSD, X.Org, Linux, GXemul)
use one register map and one XIO identity for all of them and tell them
apart only by a board-version byte. What differs between SI, SI with
texture, SSI and MXI is the count of geometry and raster engines, the
pixel-memory size, the texture option, and the resolutions and visuals the
larger boards unlock.

So the family-ready structure is not a set of sibling devices. It is one
core with a board descriptor that carries the counts, sizes and identity
bytes; an attachment head that owns the XIO widget registers, interrupt
packets and fabric DMA so that a later GIO attachment has somewhere to go;
and a display pipeline whose geometry comes from state rather than macros.
The refactor sequence below reaches that shape in seven small steps that
start once the in-flight colour-source work has landed: one mechanical
rename of everything, then the descriptor, board-version and geometry
changes before any 3D code reaches trunk, then the remaining structure.

## Scope and non-goals

The design covers the XIO-attached Octane/Origin family: SI, SI with
texture, SSI and MXI, and the later E-series aliases (SE, SSE, MXE, also
written ESI, ESSI, EMXI). Indigo2 IMPACT (GIO64, HQ3) appears in the
matrix so that the structure does not preclude it, but no GIO work is
scheduled and nothing here designs around it.

Not decided here: the hardware contract for a second raster engine, for
texture, or for any resolution other than 1280x1024. Those need captures
the project does not have (see the open questions at the end). This
document changes no datasheet claim; where it names a fact it also names
the source and the label the owning contract would give it.

## Sources and their weight

Labels used in the matrix: **established** for a published SGI manual,
**software expectation** for a public driver (what software assumes, not a
silicon observation), **third-party** for a collector wiki, **model** for
what the committed emulator does, **inferred** and **unknown** as in the
datasheets.

| Source | Where it is held | Weight |
| --- | --- | --- |
| SGI OCTANE Technical Report (1997), section 4 "Graphics Architecture" and section 7.3 "Graphics" | `docs/library/cache/graphics/mgras-ip27/octane-technical-report.pdf`, SHA-256 `248e73ee…` in `docs/library/docs.tsv` | established |
| Higher Intellect Vintage Wiki, "Octane Graphics Boards" | `docs/library/cache/wiki/preterhuman-octane-graphics-boards.html`, SHA-256 `5adcb085d843…`, acquired 2026-09-24 through `tools/library-fetch` | third-party |
| Higher Intellect Vintage Wiki, "IMPACT", "SGI Solid IMPACT", "SGI High IMPACT", "SGI Maximum IMPACT" (linked from the page above) | `docs/library/cache/wiki/preterhuman-impact.html` (`dfeab485ba90…`), `preterhuman-sgi-solid-impact.html` (`7aaeceffa554…`), `preterhuman-sgi-high-impact.html` (`a39c10efd7c8…`), `preterhuman-sgi-maximum-impact.html` (`1b600a2af08e…`), same acquisition | third-party |
| OpenBSD `sys/arch/sgi/dev/impact.c`, `impactreg.h`, `impactvar.h`, `sys/arch/sgi/xbow/impact_xbow.c`, `xbowdevs`, `sys/arch/sgi/gio/impact_gio.c`, `giodevs` | Library root `openbsd-sgi`, branch `origin/sgi-is-alive-at-7.9` | software expectation |
| X.Org `xf86-video-impact` 0.2.0 (`impact_regs.h`, `impact_driver.c`) | `docs/library/cache/graphics/mgras-ip27/xf86-video-impact-0.2.0.tar.bz2`, SHA-256 `1528a123…` | software expectation |
| Linux "Impact video driver for SGI Indigo2" patch, Peter Fuerst, linux-mips list, 2011-09-11, `<Pine.LNX.4.64.1109111200400.4146@Indigo2.Peter>` (resubmitted as a four-part series 2011-09-15) | Library mail root `linux-mips-list` | software expectation |
| GXemul `src/include/thirdparty/impactsr-bsd.h` | Library root `gxemul` | software expectation (derived from the Linux header) |
| MGRAS and IMPACT research note (citation: `docs/research/graphics/mgras.md`), [HQ4 datasheet](../datasheets/mgras-hq4.md), [raster and display datasheet](../datasheets/mgras-raster-display.md), SI Viz emulator architecture (citation: `docs/designs/2026-09-19-si-viz-emulator-architecture.md`) | Origami `main` | the owning contracts |

The wiki pages were not in the Library before this work: a federated search
for "Octane Graphics Boards" over the public roots returned no hit from
either backend with both reporting `ok`, and `docs.tsv` had no row for any
of the five URLs. All five were acquired through `tools/library-fetch`
(rows appended as `want`, fetched, hashed and flipped to `have`); the
Wayback save request failed with an HTTP error for each and should be
retried. Nothing in this design rests on the wiki alone: every board fact
it contributes is either corroborated by the technical report or marked
third-party.

## Family matrix

### Board populations

Counts from Table 4.1 of the technical report unless labelled otherwise.
"Texture" means one TE1 texture engine and one 4 MB TRAM ASIC on a
daughterboard per raster engine (report, sections 4.2.4 and 4.2.5).

| Product | Host, bus, slots | HQ | GE11 | RE4 | PP1 | TE1 / TRAM | CMAP | Pixel memory | Label |
| --- | --- | ---: | ---: | ---: | ---: | --- | ---: | ---: | --- |
| Octane SI | Octane, XIO, one slot | HQ4 | 1 | 1 | 2 | none | 2 | 12 MB RDRAM | established |
| Octane SI with texture (SI+T, "High" equivalent) | Octane, XIO, one slot; daughterboard adds no slot | HQ4 | 1 | 1 | 2 | 1 / 4 MB | 2 | 12 MB | established |
| Octane SSI | Octane, XIO, two slots | HQ4 | 2 | 2 | 4 | none | 2 | 24 MB | established |
| Octane MXI | Octane, XIO, two slots | HQ4 | 2 | 2 | 4 | 2 / 8 MB (Table 4.1); section 7.3.3 says "4 MB of dedicated texture memory and two Texture Engines", which reads as 4 MB per engine | 2 | 24 MB | established, with the report's own inconsistency noted |
| Octane SE, SSE, MXE (ESI, ESSI, EMXI) | Octane, XIO | "updated ASIC chips"; MXE described as the "speed bumped MXI, 40% average performance improvement" | as SI / SSI / MXI | as SI / SSI / MXI | as SI / SSI / MXI | as SI / SSI / MXI | | | third-party; the research note leaves every E-series population unbound |
| Origin 2000 "SI Viz" and owner transplants; Origin 200 GIGAchannel | Origin 2000 IO slot via Crossbow; Origin 200 via Crosstown and the GIGAchannel Crossbow | HQ4 | as the Octane board fitted | | | | | | research note: software route proven, board unbound |
| Indigo2 Solid IMPACT (MG10) | Indigo2, GIO64 | HQ3 | 1 | 1 | 2 | none | | 12 MiB class | research note (field-upgrade manual) |
| Indigo2 High IMPACT (MG11, MG14) | Indigo2, GIO64 | HQ3 | 1 | 1 | 2 | 1 / 1 MiB base, 4 MiB with the option | | 12 MiB class | research note |
| Indigo2 Maximum IMPACT (MG21, MG24) | Indigo2, GIO64 | HQ3 | 2 | 2 | 4 | 2 / 2 MiB base, 8 MiB with options (research note); the wiki says "1 MB … expandable to 4 MB through optional Mezzanine card" | | 24 MiB class | research note; wiki conflicts on TRAM |

The wiki gives 13.5 MB for SI and 27 MB for SSI/SSE/MXI/EMXI framebuffer
memory where the report gives 12 MB and 24 MB; the report's figures are
used. The wiki's "1RSS"/"2RSS" shorthand (one or two raster subsystems)
matches the Linux driver's `num_rss`.

### What software can see

This is the part of the matrix that decides the model's structure: what a
guest reads to learn which board it has.

| Observable | Value | Distinguishes | Source and label |
| --- | --- | --- | --- |
| XIO widget identity (register `0x000004`) | manufacturer `0x02aa`, part `0xc003`, "ImpactSR" | HQ4 boards from every other widget; **not** SI from SSI from MXI | OpenBSD `xbowdevs`, Linux `IMPACTSR_XTALK_MFGR/PART`: software expectation. OpenBSD prints the widget revision field but does not act on it. |
| XIO aperture | 2 MiB | | OpenBSD `IMPACTSR_REG_SIZE`, Linux `ioremap(…, 0x200000)`, X.Org `mmap(…, 0x200000)`: software expectation; model: `SGI_MGRAS_MMIO_SIZE` |
| GIO identity | 32-bit GIO product ID `0x10` "Impact" | HQ3 boards | OpenBSD `giodevs`, `impact_gio_match()`: software expectation |
| GIO aperture | `0x80000` | | OpenBSD `IMPACT_REG_SIZE`: software expectation |
| Board version byte 1 (`VC3 window + 0x488`, i.e. `0x072488` on HQ4) | bits 1:0 = number of geometry engines; Linux then sets the raster-subsystem count equal to it and names the device "ImpactSR *n*RSS" | 1-GE/1-RE boards from 2-GE/2-RE boards | Linux `IMPACT_BDVERS1`, byte read: software expectation. The driver reads the byte on both buses but trusts it only on its Octane/HQ4 path; on its Indigo2/HQ3 path it overrides the count to 1, with a comment that the rule fits Solid IMPACT and could not be checked on High or Maximum IMPACT. That caveat belongs to the HQ3 boards, not to this row. The model answers a fixed `0x21000000` to a 32-bit read (its `SGI_MGRAS_BOARD_INVENTORY`), whose most significant byte `0x21` gives 1 under the Linux rule, and rejects a byte read at that offset. |
| Board version byte 0 (`VC3 window + 0x408`, `0x072408`) | read by Linux; no field decoded | | Linux `IMPACT_BDVERS0`: software expectation. Model: fixed `0x0f000000` on read, stored on write, named `SGI_MGRAS_RESET_CONTROL`. |
| Texture presence | no public register | | unknown. The Origin 200 owner record in the research note shows `gfxinfo` reporting four TRAM inventory units on an SI+TRAM board, so IRIX has a way to see it; none of the public drivers does. |
| ASIC revisions (HQ, GE, RE, PP, VC3, CMAP, XMAP) | X.Org's `ImpactProbeCardInfo()` stubs every revision to `'0'`; OpenBSD prints only the widget revision | | unknown to public software. The research note records one owner's `hinv` (HQ B, GE11 B, RE4 C, PP1 A, VC3 A, CMAP E) without the registers that produced it. Model: VC3 register `0x1f` has "revision bits" masked to zero; `0x071708`, `0x028000`, `0x028004` answer zero. |
| Raster-engine status (`RSS window + 0x578`, `0x02c578`) | polled by Linux/GXemul as `RESTATUS` | | software expectation; model answers a fixed `0x100`. Whether an SSI exposes a second word is unknown. |
| Command-FIFO depth | 128 entries on HQ4, 64 on HQ3; the FIFO-status count mask is `0xff` on HQ4 and `0x7f` on HQ3 | HQ4 from HQ3 | Linux `IMPACT_CFIFO_MAX`, `MSK_CFIFO_CNT`: software expectation |
| XMAP `PP1SELECT` (`0x071c08`) | Linux writes 1 before programming XMAP "into 24-bpp mode" | possibly which pixel processor's XMAP path a write reaches on a four-PP1 board | X.Org and OpenBSD name it; only Linux writes it: software expectation for the name, unknown for the meaning. Model stores it as `repp_status[0]`; the colour branch renames it. |

Nothing a guest reads on an HQ4 board, in the public record, distinguishes
an E-series board from its predecessor, or an SI with texture from an SI.
That is not evidence that the hardware cannot be told apart, only that the
project has no public register for it; the stock IRIX inventory clearly
does tell them apart, through registers this design must leave to a
capture.

### Register windows by bus

The two public register maps differ only in where the HQ's windows sit and
how wide its FIFO is; the XMAP, VC3 and RSS register files are the same
inside their windows (OpenBSD `impactreg.h`, X.Org `impact_regs.h`, Linux
`impactsr.h`/`impact.h`: software expectation).

| Window | HQ4 (ImpactSR, XIO) | HQ3 (Impact, GIO) |
| --- | ---: | ---: |
| HQ status | `0x020000` | `0x070000` |
| FIFO status | `0x020008` | `0x070004` |
| bus (GIO) status | `0x020100` | `0x070100` |
| DMA busy | `0x020200` | `0x070104` |
| normal command FIFO | `0x020400` | `0x070080` |
| privileged (kernel) command FIFO | `0x130400` | `0x050080` |
| command/DMA FIFO thresholds | `0x040000`…`0x040030`, 8-byte stride | `0x050020`…`0x050034`, 4-byte stride |
| raster (RSS) register window | `0x02c000` | `0x07c000` |
| XMAP window | `0x071c00` | `0x061c00` |
| VC3 window | `0x072000` | `0x062000` |
| XIO widget configuration | `0x000000`…`0x00ffff` | none |

The model hard-codes the HQ4 column throughout `sgi-mgras.c`; that is
correct for every board in scope and only becomes a problem for a GIO
attachment, which is why the design asks for named window bases rather
than an offset table now.

### Display capabilities

| Capability | SI | SI+T | SSI | MXI | Source and label |
| --- | --- | --- | --- | --- | --- |
| Display back end | VC3, two VIO1, two CMAP, one RAMDAC, one 13W3 | same | same | same | report section 4.2.6: established; the back end is family-constant |
| 1280x1024 | yes | yes | yes | yes | X.Org ("only an 1280x1024 mode"), OpenBSD `IMPACT_WIDTH/HEIGHT`, Linux virtual 1280x1024: software expectation; model: the only geometry |
| up to 1600x1200 and HDTV | no | no | no | yes | report: "only supported on the OCTANE/MXI graphics configuration": established |
| quad-buffered stereo in a window | optional | optional | | standard | report: established. The wiki: "The MXI/MXE could switch back and forth between double-buffered 1280x1024 @ 76Hz and quad-buffered 1024x768 @ 96Hz but it wasn't as clean (the Solid/High IMPACT couldn't even do this)": third-party |
| pixel clock ceiling | 160 MHz | | | | wiki: third-party |
| framebuffer formats | 36-bit (12 per component) and 32-bit RGBA colour, 12-bit colour index, 24-bit depth, 8-bit stencil, 8-bit overlay; "not all features are available on all graphics configurations" without saying which | | | | report section 4: established as a family list, unknown per board. X.Org accepts 8 or 24 bitplanes; OpenBSD uses depth 32; Linux 24 bpp. |
| colour maps | two CMAP ICs; the guest addresses 32 maps of 256 entries through the selector | same | same | same | report: established count; addressing: inferred (raster and display datasheet) |
| raster precision | 8-bit subpixel x/y, 12-bit RGBA and 24-bit Z iteration; the second RE4 "handles alternate spans" | | | | report section 4.2.3: established |

## Review of the current model

### Naming inventory

Every identifier below is family-wide in behaviour: the FIFO framing, the
raster register file, the XMAP/VC3/CMAP/DAC programming and the DMA
descriptors are HQ4-family contracts that the public drivers use unchanged
on every ImpactSR board. Only the values in the last row are SI's.

| Kind | Today | Where | Family-wide? |
| --- | --- | --- | --- |
| QOM type and C types | `TYPE_SGI_MGRAS` "sgi-mgras", `SGIMgrasState`, `SGIMgrasClass`, `SGIMgrasRasterSubmission` | `sgi-mgras.h`, `sgi-mgras-raster.c` | yes, already |
| trace events, region names | `sgi_mgras_mmio`, `sgi_mgras_control_mmio`, "sgi-mgras.regs", "sgi-mgras.fabric" | `trace-events`, `sgi-mgras.c` | yes, already |
| exported functions | `si_compose_xtalk`, `si_raster_command`, `si_raster_command_token`, `si_dma_map_write`, `si_dma_map_slot_*`, `si_mgras_display_ops`, `si_mgras_display_ensure_surface` | the three headers | yes; the display pair even carries both prefixes |
| file-static functions | about 75 `si_*` definitions across the three sources (35, 37 and 5): `si_read`, `si_write`, `si_realize`, `si_reset_*`, `si_update_irq`, `si_ge_*`, `si_cmap_*`, `si_command_fifo_token`, `si_draw_*`, `si_put_pixel`, `si_gfx_update`, `si_gamma_apply`, … | all three sources | yes |
| file-private macros | `SI_PENDING_*`, `SI_HQ_*`, `SI_INT_DEST_*`, `SI_MGRAS_CONTEXT_COMMAND`, `SI_CMD_*`, `SI_REG_*` (RSS registers), `SI_FILLMODE_*`, `SI_DESC_*`, `SI_DMA_*`, `SI_CURSOR_CMAP_INDEX`, `SI_BYTE_XFER_*` | `sgi-mgras.c`, `sgi-mgras-raster.c`, `sgi-mgras-display.c` | yes; `SI_REG_*` are the public drivers' RSS names |
| public header macros | `SGI_MGRAS_*` register offsets and sizes | `sgi-mgras.h` | the prefix is right; `SGI_MGRAS_WIDTH`, `SGI_MGRAS_HEIGHT` carry SI-only values |
| machine wiring | child "si-viz", regions "si-viz-packet-proxy", "si-viz-packet-ingress", field `s->mgras`, slot "xio-slot-3" | `hw/mips/sgi/sn-machine.c` | the child name is SI's; the machine also decides the board (there is only one) |
| tests | `SI_WIDGET`, `SI_BASE`, `SI_REG`, `SI_VIZ_QOM_PATH`, `SI_*` constants, `SI_NATIVE_WIDTH/HEIGHT`, `MACHINE_ARGS "-M origin2000"` | both qtests | the constants are family values with SI names; the widget and machine are one attachment's |
| device description | "SGI MGRAS SI functional endpoint" | `sgi-mgras.c` | SI's |
| fixed answers | `0x072408` → `0x0f000000`, `0x072488` → `0x21000000`, `0x02c574` → `0x8`, `0x02c578` → `0x100`, `0x011098` → `0x2`, `0x020000` → `0x12`, `0x071208` → `0x08`, `0x071f00` → `1`, `0x071f80` → `0x101`, VC3 `0x1f` revision bits cleared | `sgi-mgras.c` `si_read_internal()` | the *offsets* are family; the *values* are the model's SI approximations and belong in a descriptor |

The colour-source branch already renames the XMAP window's registers to
the public header's names (`SGI_MGRAS_XMAP_PP1SELECT`, `_CONFIG`,
`_CONFIGB`, `_BUF_SELECT`, `_OVERLAY_MODE`, `_DIB`, `_DIB_DW`, `_RE_RAC`),
replacing `REPP_STATUS_1/3/4`, `RE_ARG` and `REPP_SYNC`. That is the
direction this design asks for everywhere: family names from the public
record, board values from a descriptor.

### Assumptions baked into the model

Listed by how far they reach. File references are to the reviewed
revision.

1. **Geometry is two macros.** `SGI_MGRAS_WIDTH`/`SGI_MGRAS_HEIGHT` size
   the front, back and WID planes and the display buffer (`si_realize`),
   clip every draw (`si_rect_bounds`, `si_draw_glyph_row`,
   `si_draw_byte_row*`, `si_draw_point`, `si_draw_line`, both DMA paths),
   flip the screen-to-host read box (`si_raster_dma_read_image`), bound the
   test pixel getters, validate migration (`si_front_count_valid`,
   `si_back_count_valid`, `si_post_load`), and create the console surface
   (`si_mgras_display_ensure_surface`). MXI reaches 1600x1200; every board
   has lower modes; VC3 timing is stored but never decoded into a
   geometry. The retrace timer is a fixed 60 Hz.
2. **One raster engine, one pixel-processor pair.** `rss[0x200]` is a
   single register file, `0x02c578` is one fixed status word, and every
   paint goes to one flat plane. SSI and MXI have two RE4s that "handle
   alternate spans". Nothing in the model could even log a second engine
   being addressed, because nothing in the public record says how one is.
3. **Plane layout.** Two `uint32_t` colour planes plus a `uint8_t` WID
   plane (the colour branch adds a `uint8_t` format plane), allocated at
   the fixed geometry. Depth, stencil and overlay storage do not exist;
   the OpenGL plan adds depth. Physical packing is unknown for every
   board; what the descriptor can carry is the pixel-memory size (12 or
   24 MB) so that a later mode change or a 24 MB board is bounded by the
   right number.
4. **Board identity is fixed reads.** The board-version bytes, the
   raster-status word, the readiness words and the cleared revision bits
   (the "fixed answers" row above) are literals in one switch. A 32-bit
   read of `0x072488` yields `0x21000000`; a byte read there is rejected
   as "unsupported byte read", although the Linux driver reads that byte.
   The widget part, manufacturer and revision are already properties
   (`part` `0xc003`, `manufacturer` `0x02aa`, `revision` 0) with SI
   defaults set by the device rather than by a board table.
5. **Uploaded-program storage is sized to one capture.** `hq[5120]` and
   `SGI_MGRAS_GE_WORDS` (59392 x 3) fit the IRIX 6.5.30 upload on SI;
   another release or board overflows into `si_error("GE upload
   overflow")`. Two GE upload slots already exist (ports A and B at
   `0x058040`–`0x05804c`) because SI traffic uses both; whether port B is
   a second engine on SSI or something else on SI is unknown.
6. **Texture is absent by omission**, not by declaration. No field, no
   log line, no refusal.
7. **XIO is in the core.** The widget identity word, widget control and
   interrupt-destination registers (`0x000004`, `0x000024`,
   `0x000030`–`0x00003c`), packet composition (`si_compose_xtalk`,
   `si_send_packet`), the four-role delivery (`si_update_irq`) and the DMA
   address space (`fabric_as`, widget from the mapping slot's control word)
   live in `sgi-mgras.c` beside the HQ4 decoder. The machine finds the
   board through a product-specific "xio-slot-3" lookup and names the child
   "si-viz". A GIO head would need a level interrupt line, GIO64 physical
   DMA, no widget window and the HQ3 offsets; none of that has a
   boundary to attach to today.
8. **Display constants that are actually family-constant.**
   `SGI_MGRAS_CMAP_ENTRIES` (8192 = 32 x 256) follows the selector's field
   widths, `SGI_MGRAS_XMAP_ENTRIES` (32) follows the XMAP index register,
   the 32x32 VC3 cursor and the 3 x 256 x 8-bit gamma table follow the
   parts every board carries. These need no descriptor field. The gamma
   table's 8-bit width would not serve a 12-bit-per-component visual, which
   the report lists without saying which boards offer it: unknown, not an
   SI assumption.

### Attachment coupling

The device is a `SysBusDevice` whose one MMIO region is the whole 2 MiB
aperture, with the XIO widget configuration space (`0x000000`–`0x00ffff`)
decoded in the same `switch` as the HQ4 windows. Interrupts are not
`qemu_irq` lines: they are 64-bit stores to a fabric address the guest
programmed, composed from a `fabric-widget-shift` property or a
`fabric-base`, and delivered through an `AddressSpace` over a link
property the machine supplies. DMA reads and writes go through the same
address space with the target widget taken from the mapping slot's control
word bits 19:16.

That is the right shape for an XIO widget and should stay. What is wrong
is only its location: the widget window, the packet composer and the
fabric DMA belong to an attachment head, so the HQ4 core never has to know
what a widget is. The machine side has the same problem in miniature: the
lookup of "xio-slot-3" is guarded by `sgi_machine_topology_matches_product(t,
"origin2000")`, so a second product (an Onyx2, an Octane) or a second
board choice needs its own branch there.

### Tests and fixtures

`tests/qtest/sn0-origin2000-mgras-test.c` (98 registered cases, one
machine, widget `0xf`, QOM path `/machine/si-viz`) mixes three kinds of
case that the family split needs to tell apart:

- **attachment cases**: widget identity, interrupt packets and their
  destinations, DMA through the Crossbow, migration of the packet state;
- **family cases**: FIFO framing and widths, context handshake, RSS state
  and executes, XMAP/CMAP/VC3/DAC programming, DMA descriptor banks,
  reset and migration of device state;
- **board cases**: the fixed board-version and inventory answers.

`tests/qtest/sgi-mgras-replay-test.c` and `tests/data/mgras/*.replay` are
already device-relative ("replay files contain no machine addresses or
topology"), which is the right property; only the harness constants
(`SI_BASE`, `SI_NATIVE_WIDTH/HEIGHT`, `SI_VIZ_QOM_PATH`) are attachment- and
board-specific. Every fixture is an SI fixture and none says so.

### Risks in the in-flight work

**Colour-source branch** (`sgi-origami-si-ci8-colour-20260924`,
`5ed10fd7d8`; 340 insertions across the three sources, the header and the
replay test). It adds a per-pixel `format` plane, per-visual
`overlay_mode[32]` and `dib[32]` arrays, a stored `buf_select`, and a
scanout rule keyed on the main mode's bits 4:0 that resolves RGB-tagged
pixels directly and CI8-tagged pixels through a second-level map from
`OVERLAY_MODE`. None of that is SI-specific; the per-visual arrays follow
the family's XMAP index width and the format plane is a property of a
pixel, not a board. Three things to watch when it lands:

- the new plane is allocated with the two geometry macros, so it is one
  more site for the geometry step below to convert; better if the branch allocates it
  beside the WID plane in one place so the conversion is one edit;
- `si_resolve_pixel_rgb()` keeps a `static bool logged[32]` latch; with a
  second board instance (or a second head) the latch is shared across
  devices. It should move into `SGIMgrasState` or become a per-device
  bitmask;
- it names the XMAP registers from the public header (good) but keeps
  `si_` and `SI_` prefixes on the new code. The rename step below lands
  straight after this branch does, so neither side rebases over the
  other's rename.

**OpenGL bring-up plan** (unmerged `docs/plans/si-opengl-bringup.md`). Its
steps 3–8 add a context transaction in `sgi-mgras.c`, a decoder in new
`sgi-mgras-gl.c`, a depth plane, and buffer selection in the display
module. The risks of baking SI in deeper:

- the decoder is HQ4 grammar and the renderer is RE4 arithmetic; both are
  family-wide and should carry `mgras_` names from the start and take
  their geometry and planes from `SGIMgrasState` fields rather than macros
  (the plan predates this design and cites the macros);
- the report ties 36-bit colour, 12-bit colour index and 1600x1200 to MXI.
  A renderer that assumes the SI visual set is fine; a renderer that
  hard-codes it in a way a descriptor cannot later widen is not. Keep the
  visual's component widths as renderer parameters;
- the HQ4 "load balancing based on the filling of the GE11 command FIFOs"
  means a two-GE board splits one command stream across engines. A decoder
  that consumes the stream in order on the host does not need to model
  that, but it must not assume a single GE upload slot when it reads
  program state.

## Proposed structure

### One core, a board descriptor, an attachment head

Keep one QOM type, `sgi-mgras`, one `SGIMgrasState`, synchronous
execution and the existing private-module split (the SI Viz architecture's
"one board with private modules"). Add two things and move one.

```text
sgi-mgras.c          HQ4 core: aperture decode, FIFO framing, context
                     handshake, uploads, flags, status, reset, migration
sgi-mgras-xio.c      attachment head (moved out of sgi-mgras.c): widget
                     configuration registers, interrupt packets, fabric DMA
sgi-mgras-raster.c   RSS state, bounded 2D operations, DMA image paths
sgi-mgras-display.c  XMAP/CMAP/VC3/DAC conversion, cursor, host scanout
sgi-mgras-gl.c       (per the OpenGL plan) decoder and CPU renderer
sgi-mgras.h          shared state, family register names, the descriptor
```

A later GIO head would be `sgi-mgras-gio.c` with a level `qemu_irq`, a GIO
DMA address space and the HQ3 window bases; the core would not change. No
ops table is introduced until that second head exists, because the
project's rule is that a new abstraction needs an existing caller.

### Board descriptor

A `const SGIMgrasBoard` selected by a `board` string property (default
`"si"`), resolved at realize into `s->board`, following the machine's
existing `baseio-preset` idiom rather than QOM subclasses. Subclasses
(`sgi-mgras-si`, `sgi-mgras-ssi`) become worthwhile only if a board needs
class-level divergence, which nothing in the matrix predicts for the XIO
family.

| Field | SI value | What reads it |
| --- | --- | --- |
| `name`, `product` | `"si"`, "Octane SI" | QOM description, error messages, `x-test-board` |
| `ge_count`, `re_count`, `pp_per_re` | 1, 1, 2 | inventory answers; a realize refusal while `re_count > 1` is unimplemented |
| `te_count`, `tram_bytes` | 0, 0 | nothing yet; documents the option explicitly |
| `pixel_memory_bytes` | 12 MiB | the upper bound on plane allocation once geometry is state |
| `cmap_count` | 2 | nothing; recorded because the report gives it |
| `max_width`, `max_height` | 1280, 1024 | the default geometry and the bound on any later mode change |
| `bdvers0`, `bdvers1` | `0x0f`, `0x21` | the board-version bytes, answered at byte and word widths |
| `widget_part`, `widget_manufacturer` | `0xc003`, `0x02aa` | defaults for the existing properties |
| `hq_words`, `ge_words` | 5120, 59392 x 3 | upload storage bounds (today's constants) |
| `implemented` | true | realize refuses an unimplemented board by name instead of running it as an SI |

Descriptors for `si-tram`, `ssi`, `mxi`, `se`, `sse` and `mxe` can be
declared with `implemented = false` and the report's counts as soon as the
table exists; they cost nothing and make "not modeled" an explicit,
testable answer. E-series rows stay identical to their predecessors until
a capture says otherwise, with a comment saying so.

### Naming convention

- QOM: `sgi-mgras` for the family. Board names are property values, not
  type names, until class-level divergence appears.
- Public header macros: `SGI_MGRAS_<WINDOW>_<PUBLICNAME>` for registers,
  using the names the public drivers use (`SGI_MGRAS_XMAP_OVERLAY_MODE`,
  `SGI_MGRAS_VC3_INDEXDATA`, `SGI_MGRAS_HQ_FIFO_STATUS`). A register the
  public record does not name keeps a descriptive project name with its
  "approximation" comment, as today. No macro carries a board value.
- File-private macros: `MGRAS_*`. Raster registers: `MGRAS_RSS_<NAME>`
  with the OpenBSD/X.Org names (`MGRAS_RSS_BLOCKXYSTARTI`,
  `MGRAS_RSS_PP1FILLMODE`), replacing `SI_REG_*`.
- Functions: file-static `mgras_<verb>` (`mgras_put_pixel`,
  `mgras_update_irq`); cross-file `sgi_mgras_<module>_<verb>`
  (`sgi_mgras_raster_command`, `sgi_mgras_display_ensure_surface`,
  `sgi_mgras_xio_compose`). The `si_mgras_display_*` pair loses its `si_`.
- Board-specific identifiers exist only where behaviour differs by board
  and are prefixed by the board (`mgras_mxi_*`). Nothing qualifies today.
- Trace events stay `sgi_mgras_*`. The machine child becomes `mgras` and
  the packet regions `mgras-packet-*`; the test macro becomes
  `MGRAS_QOM_PATH`. No Origami tool references `/machine/si-viz`, so the
  rename touches only the QEMU tree.
- Datasheets and prose keep "SI" for the board and "MGRAS" for the family,
  as the component taxonomy already does.

### Raster-engine multiplicity and texture

Where they slot in, and why the sequence does not implement them:

- **Second RE4.** The command stream stays one stream (the HQ4 owns the
  FIFO and the report says the second engine takes alternate spans), so
  `rss[]` stays one register file at the HQ level and `mgras_put_pixel`
  stays the sink. If a capture shows guest-visible per-engine state (a
  second status word at the RSS window, an engine-select field in
  `PP1SELECT` or `RE_TOGGLECNTX`), it becomes a small per-engine struct
  array sized by `re_count`, and the sink gains an owner function
  `mgras_re_for_span(y)`; the alternate-span rule would be an inferred
  approximation with its own discriminating fixture. Until then
  `re_count > 1` refuses realize.
- **TE1 and TRAM.** No public register touches texture; the report says
  texture data enters through HQ4 DMA. Texture would be a
  `sgi-mgras-texture.c` fed by the GL decoder, with storage sized by
  `tram_bytes` and no register surface until a capture provides one. The
  descriptor fields exist so that "SI with texture" is a named, refused
  board rather than an SI that silently ignores texture traffic.
- **GE count.** Two upload slots already exist. `ge_count` is recorded and
  answered in `bdvers1`; it does not change which ports accept uploads,
  because SI traffic already uses both and the meaning of port B is
  unknown.

### Display pipeline

The back end is the same on every board in scope, so the generalisation
is about mode, not board:

- geometry becomes `s->width`, `s->height` (from the descriptor's maxima
  today; from decoded VC3 timing when a capture of a mode change settles
  the safe update boundary), migrated, with the console surface recreated
  when they change and the retrace period derived from them later;
- planes are allocated by one helper from `s->width * s->height`, bounded
  by `pixel_memory_bytes`, and the migration validators compare against the
  migrated geometry instead of macros;
- the XMAP per-visual register set (main mode, overlay mode, DIB, and the
  in-flight format plane's scanout rule) is family-wide and stays as the
  colour branch shapes it;
- the CMAP address space (32 x 256), the VC3 cursor and the 8-bit gamma
  table stay constants with a comment naming the family part that fixes
  them; a 12-bit-per-component visual, if a board offers one, is an
  unknown to record in the raster and display datasheet, not a descriptor
  field to invent now.

### Tests and fixtures

- Split `sn0-origin2000-mgras-test.c` into `sgi-mgras-test.c` (family and
  board cases, driven by a small table of `{machine args, aperture base,
  QOM path, board name, expected identity}` with one SI row) and
  `sn0-origin2000-mgras-xio-test.c` (Crossbow packets, widget identity,
  fabric DMA, their migration). A second board or attachment adds a row or
  a file, not a copy of 170 KB.
- Replay fixtures gain an optional `board <name>` header line (default
  `si`) that the harness passes as the device property and uses to pick
  the expected geometry; board-specific fixtures live under
  `tests/data/mgras/<board>/` only when they exist. Today's fixtures stay
  where they are.
- `tools/check-qtest-registered` and the `tests/qtest/meson.build` timeouts
  follow the split.

## Ordered refactor sequence

Each step is one QEMU change with its tests, mechanical where it says so,
independently reviewable, and landable on its own. The order is decided:

1. The colour-source branch lands first, unchanged by this design.
2. Straight after it, every rename lands as one mechanical change: core,
   raster, display, the headers, the tests and the machine's child name,
   with the same 98 qtest cases and every replay fixture producing the
   same results. An earlier draft split the renames into a core half
   before the colour branch and a raster/display half after it; that
   split existed only to stay out of the colour branch's files, and once
   that branch has landed there is nothing to avoid. One rename is also
   the easier review: a single diff that must change no behaviour.
3. Then the board descriptor with the board-version bytes, and geometry
   as state, both before any 3D code (the OpenGL plan's context
   transaction and decoder) reaches trunk, so that code is written to the
   new names, reads its planes and geometry from `SGIMgrasState` fields,
   and never cites the macros the geometry step deletes.
4. The remaining structural steps follow in order. They can be scheduled
   around the 3D work under the usual one-writer-per-file rule.

| Step | Change | Files | Test | Behaviour change |
| ---: | --- | --- | --- | --- |
| 1 | Rename everything, after the colour branch: file-static `si_*`/`SI_*` become `mgras_*`/`MGRAS_*` in all three sources; `si_compose_xtalk` becomes `sgi_mgras_xio_compose` at its definition and its three raster DMA call sites; the exported display pair becomes `sgi_mgras_display_*`; `SI_REG_*` become `MGRAS_RSS_*`; the device description becomes "SGI MGRAS functional endpoint"; the machine child `si-viz` becomes `mgras` and the packet regions `mgras-packet-*`; the test macros become `MGRAS_QOM_PATH` and `MGRAS_*`. | `hw/misc/sgi-mgras.c`, `hw/misc/sgi-mgras-raster.c`, `hw/misc/sgi-mgras-raster.h`, `hw/misc/sgi-mgras-display.c`, `hw/misc/sgi-mgras-display.h`, `include/hw/misc/sgi-mgras.h`, `hw/mips/sgi/sn-machine.c`, both qtests | the existing 98 cases and every replay fixture, unchanged results | none |
| 2 | Add `SGIMgrasBoard`, the SI descriptor, the device's `board` property and `s->board`; answer `0x072408`/`0x072488` from `bdvers0`/`bdvers1` at both byte and 32-bit widths; default `part`/`manufacturer` from the descriptor; refuse unknown or unimplemented board names at realize; declare the other six XIO boards `implemented = false`; add `x-test-board`; forward a `mgras-board` machine property (default `si`) to the device. | `sgi-mgras.h`, `sgi-mgras.c`, `hw/mips/sgi/sn-machine.c`, `sn0-origin2000-mgras-test.c` | new: `board-si-version-bytes` (byte and word reads agree), `board-unimplemented-refuses-realize`; the existing `REG_BOARD_INVENTORY` case keeps its value | byte reads of the two version registers stop being errors (they were guest-error logs; Linux reads them as bytes) |
| 3 | Geometry as state: `width`/`height` fields set from the descriptor, one plane-allocation helper (front, back, WID, format), migration of the two fields, validators against them, surface creation from them, every clip and bound in the raster and display modules through them. Delete `SGI_MGRAS_WIDTH/HEIGHT`. Bump the vmstate version. | `sgi-mgras.h`, `sgi-mgras.c`, `sgi-mgras-raster.c`, `sgi-mgras-display.c`, replay test constants | new: `geometry-migrates`; all replay fixtures unchanged | none at 1280x1024 |
| 4 | Move the XIO head: widget registers `0x000004`/`0x000024`/`0x000030`–`0x00003c`, packet composition and delivery, and the fabric DMA helpers into `hw/misc/sgi-mgras-xio.c`, dispatched from the core's read/write for the widget window; register in `meson.build`. | new `sgi-mgras-xio.c`, `sgi-mgras.c`, `sgi-mgras-raster.c` (DMA calls), `sgi-mgras.h`, `hw/misc/meson.build` | existing attachment cases | none |
| 5 | Split the qtest into family and XIO files with the board table; add the `board` fixture header to the replay harness. | `tests/qtest/sgi-mgras-test.c` (new), `sn0-origin2000-mgras-xio-test.c` (renamed), `sgi-mgras-replay-test.c`, `tests/qtest/meson.build` | same 98 cases, re-homed; `tools/check-qtest-registered` green | none |
| 6 | Upload storage from the descriptor: allocate `hq[]` and the GE slots from `hq_words`/`ge_words`, migrate the counts, validators against them. | `sgi-mgras.h`, `sgi-mgras.c` | existing upload and migration cases | none for SI |
| 7 | Name the HQ4 window bases once (`SGI_MGRAS_HQ_STATUS`, `SGI_MGRAS_HQ_FIFO_STATUS`, `SGI_MGRAS_HQ_CFIFO`, `SGI_MGRAS_HQ_PRIV_CFIFO`, `SGI_MGRAS_HQ_THRESHOLD_BASE`, `SGI_MGRAS_RSS_BASE`, `SGI_MGRAS_XMAP_BASE`, `SGI_MGRAS_VC3_BASE`) and express the core's cases through them, so a GIO head can substitute the HQ3 column without editing the decoder. | `sgi-mgras.h`, `sgi-mgras.c` | existing | none |

Not in the sequence, because each needs evidence first: a second raster
engine, texture, any mode other than 1280x1024, E-series identity, a GIO
head, an Octane machine.

## Open questions the sequence does not decide

What would settle each, in the form the datasheets use:

| Question | Settles it |
| --- | --- |
| Which registers IRIX reads to report HQ/GE/RE/PP/VC3/CMAP revisions and TRAM inventory (`hinv`, `gfxinfo`)? | A control-register trace of `gfxinit` and `hinv` on the SI model with the fixed-answer offsets instrumented, then the same on any texture-equipped board capture the project obtains. Owner: HQ4 datasheet. |
| Does a two-RE board expose per-engine state (a second `RESTATUS`, an engine select), and are spans interleaved by row? | An SSI or MXI register-contract capture of a clear and a fill with native readback. Owner: raster and display datasheet. |
| What do `bdvers0` and the other bits of `bdvers1` carry? | The same capture across two boards; until then the Linux rule (`bdvers1 & 3` = GE count) is a software expectation. |
| How does a mode change reach the model (VC3 timing to geometry, retrace period, safe update boundary)? | A capture of the stock `impact.so` selecting a second `vof/*.sdb` timing record. Owner: raster and display datasheet. |
| Is `PP1SELECT` a pixel-processor select, and must XMAP programming be repeated per PP1 on a four-PP1 board? | The SSI capture above, comparing XMAP writes against the SI capture. |
| Do E-series boards answer the same identity and version bytes as their predecessors? | Any E-series `hinv` with the register trace that produced it. |

## Owning contracts

- [HQ4 host interface](../datasheets/mgras-hq4.md)
- [Raster, memory, and display](../datasheets/mgras-raster-display.md)
- MGRAS and IMPACT research note (citation: `docs/research/graphics/mgras.md`)
- SI Viz emulator architecture (citation: `docs/designs/2026-09-19-si-viz-emulator-architecture.md`)
- Graphics roadmap (citation: `docs/graphics-roadmap.md`)
- Component taxonomy (citation: `docs/component-taxonomy.md`)
