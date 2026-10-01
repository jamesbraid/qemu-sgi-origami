# Odyssey/VPro direct-XIO interface

References rendered as citation identities name material outside this document
set. Those targets are not included or cleared for redistribution. Review and
runtime statements retain their recorded dates and scope.

## Status

This is an evidence-blocked contract for the selected acquisition target,
direct-IP30 V12 assembly `030-1611-001 rev C`. Device implementation must not
begin until a known-good specimen of that revision supplies one atomic
software/firmware manifest and passes the named experiments in the research
note.

The readiness review (citation: `docs/reviews/sgi-graphics-evidence-readiness.md`) confirms
that no Odyssey implementation plan is cleared.

## Applicability

The contract applies to one V12-class Odyssey endpoint attached directly to IP30 XIO.
V6, V8, V10, dual-board IP30, dual-channel display, Fuel, and IP35 V-brick
configurations are separate scopes until explicitly added.

## Compatibility path

Acceptance requires one atomic stock IP30 PROM, IRIX base/overlay/kernel/patch
set and `gfxinit`, Xsgi with
`odyssey.so` and `glx.so`, the matching IP30 OpenGL product, and compatible
diagnostics. Replacement guest drivers, X servers, GL libraries, uploaded
programs, and OpenGL interception are not acceptance paths.

## Selection gate

The physical acquisition target is selected from Usenet Message-ID
`<1154846834.587752.145330@m79g2000cwm.googlegroups.com>`. Its literal owner
inventory is `ODY128VERSIONB`, `030-1611-001 rev C`, BUZZ B.1, PB&J 1, 128 MB,
four banks, CAS latency 3, and a Dual Channel Display option on direct IP30 XIO.
The record also
names IP30 system board `030-1467-001 rev D`, Crossbow 1.4, Heart F, and mode
`2@1600x1024_83_ds`.

That specimen froze its X server after logout, so it is not a known-good oracle.
Implementation requires a known-good specimen of the same board revision and
this complete evidence bundle:

- both-side photographs, assembly suffix and revision, NIC data, and every
  readable ASIC and SDRAM marking.
- Xtalk vendor, product and revision, IP30 Crossbow revision, output hardware,
  machine PROM, IRIX release, overlays and patches.
- installed-product inventory and SHA-256 values for the loader, Xsgi modules,
  ABI-specific OpenGL library, display data, and uploaded program.
- ordered cold, warm, link and any local reset observations, MMIO widths and
  byte lanes, command/data traffic, DMA, interrupts, completion, readback,
  display, and errors.
- known-good PROM text, Xsgi desktop, native OpenGL render/readback, and
  compatible diagnostic results from that board.

The NIC, widget revision, readable package and SDRAM markings, exact PROM and
IRIX bundle, and all behavior traces remain unresolved. Neither the owner mode,
a V-brick inventory, nor an isolated ASIC revision clears the implementation
gate. The executable procedures are in
the Odyssey research note (citation: `docs/research/graphics/odyssey.md#blocking-acquisitions-and-executable-experiments`).

## Source and rights boundary

Public numeric facts below come from SGI's published VPro manuals and OpenBSD's
permissively licensed pre-retirement driver. Held IRIX source and binaries
establish stock initialization, loader, display, and diagnostic behavior under
D-0002. Those requirements are restated independently. No private declaration,
source structure, table, payload byte, or diagnostic vector belongs here.

The selected capture outranks a family-level or secondary claim for revision
details. A disagreement must remain recorded until reconciled. MGRAS and KONA
interfaces supply no fallback values.

## Configuration identity

The endpoint reports Xtalk vendor `0x0023` and product `0xc013`. The widget
revision is read from the selected board. This tuple identifies Odyssey/VPro.
It does not select V6, V8, V10, V12, memory size, output option, ASIC stepping,
firmware, or platform software.

Configuration must retain these independent values:

| Field | First-candidate requirement |
| --- | --- |
| Machine and attachment | IP30, direct XIO, exact widget and Crossbow route |
| Marketed option | V12, owner-reported for the selected acquisition target |
| Physical identity | `030-1611-001 rev C`; NIC, widget revision, photographs, and labels still required |
| ASICs and memory | Owner-reported BUZZ B.1, PB&J 1, 128 MB, four banks, CAS latency 3; package and SDRAM markings unresolved |
| Output | Owner-reported base 13W3 plus DCD; neither output/mode is accepted until the display experiment passes |
| Software | One atomic PROM, IRIX base/overlay/kernel/patch, DDX, GL, VFO, uploaded-program, and diagnostic set |

Unknown configuration is an attachment failure for stock acceptance. It must
not silently become a generic 128 MiB V12.

## Attachment and topology

The first endpoint is one Odyssey widget on the IP30 Crossbow. It owns only its
widget-relative registers, board memory, command processor, uploaded execution,
raster state, and display state. The IP30 machine owns XIO routing, interrupt
delivery, DMA access to host memory, hardware graph, and inventory projection.

An IP35 V-brick is not an alternate placement switch. It adds a midplane, L1
controller, brick inventory, distinct reset and routing, the IP35 graphics
software product, and one or two independent V12 pipes. That configuration
requires a separate machine contract after IP35 fabric work. A second V12 is a
second pipe, not another raster unit inside this endpoint.

The first-party `108-0314-003` example is bounded to IRIX64 6.5.10m build
`07171440` on `IP35 (shad)`. Its one-board `odydiags -getinfo` result reports Buzz
revision 10, PB&J version 1, 128 MB, four banks, CAS latency 3, and board version
1. Physical system subtype, assembly, widget route, and output are unknown.
These diagnostic observations are not defaults, expected values, or validation
evidence for this direct-IP30 endpoint.

## Public PIO baseline

The public console driver maps `0x00410000` bytes at widget-relative offset
zero. The only stable locations available before the selected-board trace are:

| Offset | Width | Baseline behavior |
| ---: | ---: | --- |
| `0x00001064` | 32 bits, read | Command-FIFO status. Public masks `0x00008000` and `0x00020000` expose threshold states. Other bits and reset values are unspecified. |
| `0x0000106c` | 32 bits, read | Data-engine status. The public console waits until the low seven bits are zero before another bounded data burst. Other fields are unspecified. |
| `0x00110000` | 32 bits, write | Ordered command-FIFO port for the public setup and console stream. |
| `0x00400000` | 64 bits, write | Ordered data-FIFO port used by the public setup stream. |

Only aligned accesses at those widths enter the public baseline. Reads or
writes at other widths, unaligned offsets, aliases, byte swapping, unmapped
behavior, and the rest of the aperture remain selection-capture questions. The
frontend must preserve the guest's raw value, width, byte enables, bus byte
order, address, and total order in traces.

The mapped range is not evidence for a linear framebuffer. Direct framebuffer
mapping remains unsupported until the selected stock path or hardware capture
establishes one.

## Reset and initialization

No reset-value table is approved. The selection trace must distinguish the
reset domains actually visible on the IP30 system and record:

- assertion and release order.
- widget identity and link state before and after reset.
- every readable public and newly established register value before PROM setup.
- command/data queue contents, pending DMA, interrupt state, and error state.
- uploaded instruction/data memory and processor state.
- SDRAM contents or invalidation rules.
- timing, scanout, cursor, palette/color, gamma, overlay, and blanking effects.

Firmware initialization may supply state that a later driver assumes. A model
must not manufacture that state at reset or rely on OpenBSD's refusal to attach
before firmware setup. Warm, link, and board-local reset behavior cannot be
derived from a cold boot.

## Command and data submission

The early public PIO subset consists of ordered 32-bit command words and
separate 64-bit data records. On a firmware-initialized console it can set
bounded state, fill rectangles, copy rectangles, apply logical raster
operations, and expand monochrome glyphs. A qtest may use the exact raw words
from a rights-cleared synthetic fixture after comparing them with the public
source.

That subset does not define stock Xsgi or OpenGL submission. Before desktop or
OpenGL work, the selected trace must establish:

- command and data record boundaries, lengths, packing, and byte order.
- command and data FIFO capacity, occupancy, thresholds, wrap, and
  back-pressure.
- context identifiers, interleaving, state restoration, and scheduling.
- malformed, privileged, stalled, and overlong input behavior.
- command acceptance versus downstream execution.
- explicit flush, fence, completion, readback, and error records.

Unknown records stop deterministic execution with a stable diagnostic. They are
never skipped, guessed, or translated through a MGRAS/KONA grammar.

## DMA and interrupts

DMA is required for stock acceptance but has no approved direct-IP30 descriptor
or routing contract. The selection capture must establish descriptor bytes,
direction, address width and reach, alignment, chunking, cache/coherence rules,
short and faulted transfer behavior, completion visibility, and interaction
with both FIFOs and board memory.

The same capture must establish interrupt source count, device latches, masks,
target/vector programming, IP30 route, level or pulse behavior, assertion and
deassertion order, acknowledgement, coalescing, retrace behavior, and error
interrupts. No IP35 V-brick or MGRAS allocation may fill these fields.

DMA walkers and queues require finite descriptor, byte, and work budgets. A
cycle, invalid address, or exhausted budget reports an error without dropping
or reordering accepted work.

## Board memory

Published first-generation VPro allocates commands, framebuffer, textures,
pbuffers, accumulation buffers, and scratch storage from one SDRAM pool. The
selected V12 must confirm that allocation model and its exact 128 MiB marketing
lead before the emulator exposes it. Required capture fields include:

- physical SDRAM population, banks, interleave, refresh, ECC or parity, and
  address map.
- command-storage reservation and allocation rules.
- framebuffer, depth/stencil, accumulation, overlay, pbuffer, texture, and
  scratch layouts.
- tiling, swizzle, compression, fast-clear, and cache state.
- CPU, DMA, raster, display, and readback visibility.
- behavior at allocation failure and out-of-range addresses.

The published 4K by 4K maximum drawable and shared-pool allocation are
application-visible limits. They do not establish the physical V12 address map.
No board memory may be represented as a flat linear framebuffer solely for host
convenience.

## Uploaded program and firmware loading

IRIX 6.5.30 `gfxinit` selects and uploads
`/usr/gfx/ucode/ODSY/buzz_xform.bin`. The reviewed file is 33,528 bytes with
SHA-256 `a0201d88cf23752cd35ac60c25f2c1b05cfab3253e5991db09006bd1b85b0375`.
It is proprietary guest software. A normal guest supplies it from its installed
filesystem. The emulator does not embed it or require a second host copy.

The loader trace must establish the container, board predicate, transfer path,
destination, entry point, initialization state, and completion protocol. Only
then may an interpreter define instruction memory, data memory, registers,
program counter, stalls, exceptions, single-step state, and a watchdog. The
filename, destination class, and hash do not establish an instruction set.

No board-resident EEPROM or programmable-logic dump is held for a selected V12.
If the board needs one, acquire exact authorized bytes and bind them to the
physical revision. Do not substitute an IP30 PROM, VFO file, or clean-room
program.

## Raster and rendering behavior

The public console subset is the first deterministic raster oracle: solid
rectangles, copies, logical operations, and glyph expansion. It runs after
firmware setup and does not prove native 3D state, exact packet encoding beyond
that stream, or pixel arithmetic.

The stock OpenGL gate requires revision-bound traces and expected results for:

- points, lines, triangles, rectangles, and clipping.
- coordinate conversion, viewport, culling, and provoking-vertex rules.
- color, depth, stencil, accumulation, overlay, and fast-clear state.
- blending, logical operations, masks, dithering, and write ordering.
- 1D, 2D, and 3D texture upload, borders, filtering, lookup tables, and
  perspective correction.
- copies among buffers and textures.
- single, double, stereo, and pbuffer behavior supported by the selected mode.
- readback formats and conversions.
- buffer swap, retrace, and error behavior.

Published OpenGL feature support guides probe selection. It does not define the
hardware packets or exact fixed-point and rounding rules. Hardware results settle
those rules.

## Display, color, and cursor

The first display target is one native mode selected by the exact 6.5.30
`odyssey.so` and matching VFO data. Capture it from reset through DDC/EDID,
timing selection, scanout base and format, color mode, palette, gamma, overlay,
cursor, blank/unblank, retrace, and stable presentation.

OpenBSD's firmware-assisted path uses 1280 by 1024 at 32 bits per pixel and
paints the text cursor with an XOR rectangle. It does not program a native mode
or prove a hardware cursor. Its software colormap also does not establish a
device palette interface. Native cursor, palette, gamma, overlay, and mode
registers remain capture gates.

The direct-IP30 board's base output and optional dual-channel display must be
identified independently. V-brick output is not part of this contract. The
published V-brick has two DVI-I outputs plus swap-ready, genlock, and stereo
connectors per pipe. Those enter only the later IP35 configuration.

## Completion and readback ordering

Keep these observable points separate:

1. command-FIFO space becomes available.
2. data-FIFO or data-engine state drains.
3. DMA input becomes device-visible or output becomes host-visible.
4. uploaded execution reaches an observed completion.
5. raster work makes memory visible.
6. readback returns the completed result.
7. display state changes at the observed scan boundary.
8. an interrupt or fence becomes visible and is acknowledged.

The public console proves only two queue waits. The published approximately
one-millisecond deep-FIFO latency explains why readbacks can wait. It is not a
completion rule. The implementation must publish results only at a completion
point established by the selected stock trace. A synchronous test mode may run
work immediately but must preserve the same visible ordering.

The graphics trace format records raw reads and writes, FIFO words, DMA objects,
interrupts, resets, fences, asynchronous completion, display changes, errors,
and checkpoints. Proprietary program bytes and arbitrary guest memory remain
hash-identified external assets.

## Stock IP30 software set

The first selected run uses one atomic stock bundle containing:

- a compatible IRIX 6.5 base.
- `eoe_6530m:eoe.sw.base`, `eoe.sw.gfx`, and `eoe.sw64.gfx`.
- matching `x_eoe_6530m`.
- the installed IP30 kernel object and build, the running PROM component, and
  complete patch inventory.
- the objects below and their ABI-matched siblings.
- one exact VFO and one compatible IST package/version/hash.

| Role | Reviewed object | SHA-256 or boundary |
| --- | --- | --- |
| PROM candidate | `refs/irix-media/6.5.30/usr/cpu/firmware/IP30prom.bin`; tracked byte-identical `assets/firmware/6.5.30/IP30prom.bin` | 879,312 bytes; `ee4722ca6f9b821ffadf156346c4d69bf5be71fc4b911cee75e36a1f51a49972`; embedded IP30 SGI 6.5 revisions 4.9 and 4.17, 20 January 2004; not yet paired |
| Kernel candidate | `eoe_6530m:eoe.sw.base` `/usr/cpu/sysgen/IP30boot/kernel.o` | Installed `/unix`, component build, and hash must be captured with the compatible base and complete patch set |
| Loader | `/usr/gfx/gfxinit` | `add7430a5ae4cc7f5eda546d9288e9505a32a8cabdafa37dec7e85d903a9c4a` |
| X server | `/usr/gfx/arch/GENERIC/Xsgi` | `68792b0377bde4be868d03618c34b0dbaa7696cb8b1b5fd9cf40bb09ebbc7166` |
| GLX module | `/usr/lib/X11/dyDDX/glx.so` | `ba330e08d1e52cb2d55fe3bdb2c4d23ae42b1e8295ea28341bdbcf504c1c7f2b` |
| Odyssey DDX | `/usr/lib/X11/dyDDX/odyssey.so` | `f49996d4ba3ee203eb752eb374d86605311ef943ae126fb770748b7828a743f0` |
| IP30 64-bit OpenGL example | `/usr/gfx/arch/IP30ODSY64/libGLcore.so` | `30dc6c8e51d7b67b36dcfa5e6447ae3abe6e3ebe06082f15ddec763b1c387526` |
| Uploaded program | `/usr/gfx/ucode/ODSY/buzz_xform.bin` | `a0201d88cf23752cd35ac60c25f2c1b05cfab3253e5991db09006bd1b85b0375` |
| Display data | 71 reviewed `ODSY/vof/*.vfo` objects; owner lead `2@1600x1024_83_ds.vfo` | Bind one exact file and hash to a passing output/mode; the owner lead is not accepted yet |
| Field diagnostics | Compatible IST 2.7 `field_diags_odyssey` | Exact `812-0640-011` media and both 211-file variants are held and hashed; the selected variant still requires a demonstrated compatible kernel and known-good run |

The IP35 64-bit OpenGL example has the same byte length as IP30 and SHA-256
`4e78f11a45b361840c3ee762c4746a7b3f39050c732fb649a7941b2723d70b2b`.
It is evidence that platform selection matters, not a substitute. Likewise, the
held PROM and 6.5.30 objects are candidates, not a reconstructed copy of the
owner's unknown software configuration.

## Acceptance gates

1. **Target evidence:** one `030-1611-001 rev C` specimen supplies the frozen
   atomic manifest and physical evidence above.
2. **Host interface:** `ODSY-IP30-ID`, `ODSY-IP30-CMD`, `ODSY-IP30-DMA`,
   `ODSY-IP30-IRQ`, and `ODSY-IP30-RST` pass for identity/MMIO, queues, DMA,
   interrupts, and reset.
3. **Memory and execution:** `ODSY-IP30-MEM`, `ODSY-IP30-LOAD`, and
   `ODSY-IP30-EXEC` pass for SDRAM, the stock loader, and uploaded execution.
4. **Raster and presentation:** `ODSY-IP30-RASTER`, `ODSY-IP30-DISP`, and
   `ODSY-IP30-ORDER` pass for primitives/pixels, one exact output/VFO,
   cursor/color/scanout, and readback ordering.
5. **Stock acceptance:** `ODSY-IP30-ACCEPT` passes three cold boots, ten
   login/logout cycles, the native OpenGL suite, and every compatible diagnostic
   on the same atomic bundle. An unproved IST 2.7 package/kernel pairing blocks this gate.
6. **Follow-ons:** add V10 differences, IP30 dual-board or dual-channel output,
   and then the IP35 V-brick only as separately reviewed contracts.

No earlier gate is waived because the public console renders. No later topology
may supply a missing direct-IP30 fact.

## Sources and provenance

The evidence record is Odyssey and VPro research (citation: `docs/research/graphics/odyssey.md`).
Exact holdings, rights, hashes, and source ancestry are in the
graphics source map (citation: `docs/library/graphics-source-map.md`),
stock software stack (citation: `docs/library/graphics-software-stack.md`),
firmware index (citation: `docs/library/graphics-firmware-index.md`), and
payload ledger (citation: `docs/library/graphics-payload-ledger.tsv`). This datasheet contains
IRIX-derived interface facts, independently stated. It contains no proprietary
source text, declarations, program bytes, or diagnostic vectors.
