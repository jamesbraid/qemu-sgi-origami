# Datasheet: InfiniteReality host interface

References rendered as citation identities name material outside this document
set. Those targets are not included or cleared for redistribution. Review and
runtime statements retain their recorded dates and scope.

**Status:** reviewed 2026-09-22 — the
independent review (citation: `docs/reviews/infinitereality-host-interface-datasheet-review-20260922.md`)
cleared revision 9e0e48765d as a document. It clears nothing for
implementation on its own; the host-command-boundary approximation
track (citation: `docs/decisions/D-0028-infinitereality-host-command-approximation.md`) and its
executable plan (citation: `docs/plans/infinitereality-host-command-path.md`) name it as one
input among several that stage 1's device contracts must incorporate and that
those contracts still need their own independent review.

**QEMU file(s) on `sgi-origami`:** none yet. This is a stage-0 evidence document
under the host-command-path plan; the widget and device model are stage 2 and later.

**Coverage:** this document now holds both halves of the InfiniteReality host interface.
The header-derived half — the mapped apertures, the DMA descriptor, the full
private ioctl surface, the failure-code catalogue, the board-inventory
structure, the event vocabularies, and the flow-control/credit model — carries
the review noted above. The disassembly-derived register-map half, added in
this revision — widget identity and the board-type classifier, the aperture's
internal windows, the full register table, the credit register's two read
polarities, the three interrupt vectors and cause-bit dispatch, the mailbox
protocol, reset sequences, the EEPROM fallback ports, the DMA packet, and the
inventory the guest derives from the pipe — is **not yet reviewed** and does
not carry the clearance noted above. See **Role** below for the full boundary.

**Sources used:** held IRIX 6.5.5 kernel interface headers for this driver — three
headers read in full by a source reader: the driver's own private interface header
(ioctl numbers and argument shapes, the aperture layout, the DMA descriptor, the
failure-code catalogue, the board-inventory structure), a second header devoted
entirely to the board-inventory structure and the two event vocabularies, and a
third header devoted to two diagnostic-only ioctls, the DMA address-mapping modes,
and the flow-control/credit model. Cross-checked against two authored
disassembly records, cited inline by section:
the driver disassembly note (citation: `docs/research/graphics/ir-feasibility-kona-driver-disasm-20260922.md`)
sections 9 through 13, and
the userspace-path note (citation: `docs/research/graphics/ir-feasibility-kona-userspace-path-20260922.md`)'s
follow-up items 2 through 4. No path, filename, or identifier from the restricted
tree is reproduced anywhere in this document; see **Provenance and rights** below.

The register-map half added in this revision is built entirely from two further
authored disassembly records, cited inline by section throughout: the
same driver disassembly note (citation: `docs/research/graphics/ir-feasibility-kona-driver-disasm-20260922.md`)'s
sections 1 through 4 and 11 through 17, and
the PROM InfiniteReality-path note (citation: `docs/research/graphics/ir-feasibility-prom-kona-path-20260922.md`)'s
sections 7 through 9. Neither source is a source-reader product; both are
Origami's own capstone disassembly of the kernel driver's compiled object files
and of the IO6 and CPU PROM firmware images, so this half carries no source-reader
exposure and no restricted-tree identifier at all — see **Provenance and rights**
below for the boundary this implies for who may act on it.

## Role

This document is the InfiniteReality graphics pipe's host-facing software
contract, in two halves that come from different evidence classes.

The first half is header-derived: the shape of the interface the kernel driver
presents to user space, as read directly from the driver's own interface headers
rather than inferred from compiled code. It covers the mapped user apertures, the
strided DMA descriptor and the two ioctls that use it, the two DMA address-mapping
modes, the complete private ioctl numbering and argument shapes, the memory-class
allocation taxonomy, the buffer-sharing handle, the health report and failure-code
catalogue, the board-inventory structure the driver assembles per pipe, the two
event vocabularies exchanged with the pipe's on-board controller, the flow-control
register roles and credit model, and the multi-pipe network ioctl group.

The second half is disassembly-derived: everything the header-derived half
deliberately left out because it comes from compiled code and firmware images
rather than source headers, so that the two evidence classes were never mixed
inside one paragraph. It covers widget identity and the CPU PROM's board-type
classifier, the register-level MMIO map inside the widget's aperture, the credit
register's two read polarities, the three interrupt vectors and cause-bit
dispatch, the mailbox protocol's wire format, reset sequences, the EEPROM
fallback ports, the DMA packet's on-the-wire encoding, and the inventory the
guest derives from the pipe. It does not cover the host command grammar the
pipe's co-processor executes once a command reaches it, or the display-generator
programming model; those are sibling documents under the same plan, kept
separate so that this document's register-level scope stays bounded to what the
host-facing aperture itself exposes. Two further headers referenced only in
passing from the three read for this document were not opened: a generic
per-board identity-record format shared across more than one SGI graphics family
(used for every "board inventory" entry below; its own internal layout is out of
scope here), and a generic, non-InfiniteReality-specific graphics ioctl family that supplies
board information and other cross-pipe queries to user-space callers alongside
this driver's own private range (mentioned below only where it borders the private
range). A byte-level identity comparison the plan's inventory-synthesis item asks
about, found in authored disassembly of a user-space caller of that generic family
(see the cross-check note under **The private ioctl family shares its range with
two other numbering families**), is not resolved by anything in the three headers
read for this document and stays an open question for that later synthesis step.

Every statement below is marked **established** (stated directly by a header) or
**inferred** (a reasonable reading the header does not state outright). Numeric
values a guest passes across this interface — ioctl numbers, offsets, sizes, and
codes — are recorded because a datasheet is this project's sanctioned place for
them; no such number is a restricted identifier.

## The four mapped aperture sub-windows

**Established.** A calling process maps a fixed set of four sub-windows out of the
pipe's aperture, always in the same relative order and at the same offsets:

| Window | Offset range | Size | Conditional | Role |
|---|---|---|---|---|
| 1 | `0x0000`–`0x3fff` | 16 KiB | No | Graphics command/data FIFO: the host posts commands and immediate data here. |
| 2 | `0x4000`–`0x7fff` | 16 KiB | No | The pipe's host-interface register window. |
| 3 | `0x8000`–`0xbfff` | 16 KiB | No | The return-data ("rdata") buffer: readback and query results land here. |
| 4 | `0xc000`–onward | 16 KiB or 2 MiB | Yes | A diagnostics-only chip register window, not used by ordinary operation. |

The fourth window's size is conditional on which generation of the pipe's host
interface chip is present: exactly 16 KiB (`0x4000` bytes) on the first
generation, exactly 2 MiB (`0x200000` bytes) on the second. The header states the
offsets themselves as independent of which CPU board the pipe is attached to, but
the second and fourth windows' sizes as generation-labeled — the second window's
16 KiB size is explicitly tied to the first-generation host-interface chip even
though its offset does not move. Two total-size figures are given: one covering
the always-present first three windows together (`0xc000` bytes), and one covering
the full aperture including the generation-dependent fourth window; a header
comment ties both figures to the windows staying in this fixed order.

**Cross-check (established, agreement).** The driver disassembly independently
found four aperture-mapping calls at the same four offsets — `0x0`, `0x4000`,
`0x8000`, and `0xc000` — each mapping a 16 KiB-aligned region (driver disassembly
note, section 9). This confirms the window count and the three fixed boundaries
from an independent, public source. Role correlation to the underlying
widget-side offsets the disassembly was working from remains largely
unconfirmed by disassembly, with only a plausible, unestablished pairing for
two of the four windows and no established match at all for the other two;
that correlation stays an open question the disassembly note itself flags, not
settled by the header, since the header states offsets *within the mapped
result*, not the widget-side offsets the disassembly examined.

## The two-dimensional strided DMA descriptor

**Established.** A single descriptor has exactly four fields, all byte-valued per
the header's own framing: a user virtual base address, a total byte length, a
per-line byte length, and an end-of-line byte stride (padding between the end of
one line and the start of the next). A second, mirror form of the same
four-field descriptor exists for 32-bit-mode user processes running under the
64-bit kernel; its fields carry the same roles.

Two ioctls use this descriptor in different shapes:

- **Single-descriptor form**, used by the DMA-read request (see the ioctl table
  below): one embedded descriptor, plus a fixed 32-word command buffer and a
  command count, both destined for the host-interface hardware rather than the
  descriptor mechanism itself.
- **Multi-descriptor form**, used by the DMA-write request: a pointer to a table
  of descriptors plus an entry count, plus the same 32-word command buffer and
  command count fields as the read form.

**Cross-check (established, agreement on direction and use).** Public
disassembly of two IRIX 6.5.30 user-space objects (an X-server driver and the
OpenGL runtime) independently exercises the DMA-read request code for
image-read and framebuffer-block-read calls, and the DMA-write request code for
framebuffer sub-block writes and image draws (userspace-path note, follow-up
item 2) — consistent with "read" meaning device-to-host and "write" meaning
host-to-device.

## Two DMA address-mapping modes

**Established.** A separate diagnostics-only request programs address mapping
for DMA in one of two modes, selected by a flag field alongside a
create-or-destroy flag (four valid flag combinations: create-physical,
destroy-physical, create-iovirtual, destroy-iovirtual — the header requires
exactly one value from each pair):

- **Physical mode.** Returns the physical address corresponding to a supplied
  virtual address and length. Limited to one page; the header states plainly
  that no scatter/gather-style operation is performed in this mode.
- **I/O-virtual mode.** Performs the same physical-address translation as
  physical mode, and additionally programs the virtual-to-physical mapping into
  dedicated address-translation hardware, so that the device itself can walk a
  virtual range without the driver pre-translating every page. *Inferred:*
  this is plausibly the same translation hardware whose register window is the
  fourth aperture sub-window's first-generation identity described under **The
  four mapped aperture sub-windows** above — the header names the mapping
  target only informally, without stating that link outright.

The argument structure's two output fields are a page number and a combined
page-offset-and-attributes value; the header's own descriptive prose uses
different informal short names for these two outputs than the structure's actual
field names, a minor internal inconsistency worth noting since it means the
prose and the structure do not read as a perfectly matched pair. Destroying a
mapping requires the caller to resupply the exact original address, length, and
mode flags, because — as the header states — the driver keeps no record of the
resource once created. Specific error codes for this request are not enumerated
in the header; a source comment marks this an open item in the vendor's own
development, not something this datasheet is withholding.

## The private ioctl family shares its range with two other numbering families

**Established.** The driver's own request numbers form two related families that
the header organizes as separate groups: a "regular" family starting at decimal
10000 (`0x2710`) and documented as intended to stay at offset 99 (`0x2773`) or
below, and a "diagnostic" family starting at offset 100 (`0x2774`), in a
separate section of header material specifically so that, in the header's own
framing, day-to-day request numbers are not cluttered by diagnostic-only ones.

**Cross-check (established, a genuine disagreement worth carrying forward).**
The compiled dispatch function the driver disassembly recovered validates and
dispatches a single, unified, contiguous range of 103 request-code slots —
`0x2710` through `0x2776` inclusive — through one dispatch table (driver
disassembly note, section 10). That upper bound, `0x2776`, is exactly this
header pair's last diagnostic request code, so the two sources agree on the
outer boundary of the whole mechanism. What they do not obviously agree on is
organization: the header presents "regular" and "diagnostic" as two separate,
purpose-motivated groups in two different places, while the compiled dispatch
table treats the entire 103-slot span as one seamless mechanism regardless of
that grouping. Of the 103 slots, this pair of headers names 26 requests: 23 in
the regular range (two internal gaps, at `0x2714` and `0x271d`, are not defined
by either header) and 3 in the diagnostic range. The remaining 77 slots —
the two internal gaps plus the entire span from `0x2729` through `0x2773` between
the regular and diagnostic ranges — are not named by anything read for this
document; what the compiled table does at those slots (a stub, an error return,
or something else) is not established here.

A separate, non-InfiniteReality-specific ioctl family sits outside this whole 103-slot
range and is not covered by anything read for this document beyond its
existence: user-space disassembly (userspace-path note, follow-up items 2 and 3)
found two low request numbers, decimal 101 and 102, used by both the
X-server driver and the OpenGL runtime for board-information and hyperpipe-network
queries against a generic graphics-device interface shared across pipe types, and
a further three-member family at decimal 1011, 1012, and 1017 used for clip
validation, swap-buffer validation, and display-mode loading — the same note
flags this third family as numerically distinct from both this driver's private range
and the board-information pair, with nothing in either source establishing what
separates it. The board-information request (102 decimal) is where the
disassembly traced a single-byte identity comparison against the literal value
`0x49`, the plan's inventory-synthesis item asks about; nothing in the three
headers read for this document names or explains that byte, so it remains an
open question for whoever does that synthesis, not answered here.

## The full private ioctl surface

**Established**, field-by-field, from the driver's own interface header. Each
request is given a descriptive label here, not the header's own name.

### Regular range (base `0x2710`)

| Offset | Code | Label | Direction | Argument fields |
|---|---|---|---|---|
| 0 | `0x2710` | DMA read | in: descriptor + command buffer; out: via descriptor | One embedded DMA descriptor, a 32-word host-interface command buffer, a command count. |
| 1 | `0x2711` | DMA write | in | A pointer to a table of DMA descriptors, an entry count, the same 32-word command buffer and command count. |
| 2 | `0x2712` | Start select-feed | in | One embedded DMA descriptor, plus a snapshot flag. |
| 3 | `0x2713` | Finish select-feed | out | A word-count result and an overflow flag (two 4-byte fields, 8 bytes total). |
| 4 | `0x2714` | *(not defined in the header read)* | — | — |
| 5 | `0x2715` | Select cursor | in | A 3-value mode field (off / crosshair / glyph), a cursor address, and hot-spot X/Y coordinates; address and hot spot are only meaningful in glyph mode. |
| 6 | `0x2716` | Set configuration info | — | Argument shape not detailed in the header read. |
| 7 | `0x2717` | Register DMA pool (older form) | in | Buffer address, length, a pool-type field (one type currently defined: the descriptor-table pool), and a shared/not-shared flag. |
| 8 | `0x2718` | Debug hook | in | A fixed-size routine-name string, a fixed-size action-name string, and a numeric argument. The header itself calls this mechanism a hack, in its own commentary, not reproduced here. |
| 9 | `0x2719` | Set channel rectangle | in | Channel number, a 3-value validity state (inactive / crosshair-cursor / DAC-cursor), a signed priority in the header's own stated range of -100 to 100, a footprint rectangle, and, DAC-cursor mode only, a hot spot and an output resolution. |
| 10 | `0x271a` | Allocate memory | in/out | A memory-class type byte, a per-class allocation-purpose key byte, a small flag field, a requested size, and an alignment requirement. |
| 11 | `0x271b` | Free memory | in | Same shape as allocate. |
| 12 | `0x271c` | Get channel by position | in/out | Input X/Y position; output channel number and its footprint rectangle — the reverse lookup of "set channel rectangle." |
| 13 | `0x271d` | *(not defined in the header read)* | — | — |
| 14 | `0x271e` | Reallocate memory | in/out | Not separately defined; presumed to reuse the allocate/free shape given the naming pattern, but no distinct structure was found. |
| 15 | `0x271f` | Share buffer | in | A single opaque 64-bit sharing identifier. |
| 16 | `0x2720` | Health report | out | A death-reason code (see the failure catalogue below) plus two saved hardware-register snapshots, captured before the pipe's failure handler overwrote them: an interrupt-info-class register and an internal transaction-bus status register. |
| 17 | `0x2721` | Check pipe | — | No argument structure is defined in the header read; consistent with the disassembly's own finding, discussed by name in the ioctl cross-check paragraph immediately below the diagnostic-range table. |
| 18 | `0x2722` | Register DMA pool (current form) | in | Same field shape as the older-form request at offset 7; what distinguishes the two request numbers in practice is not stated. |
| 19 | `0x2723` | Query multi-pipe topology | out | Up to 32 index/network-identifier pairs. |
| 20 | `0x2724` | Configure multi-pipe network | in | A pipe count, a network identifier, a "hyperpipe" identifier, and up to 32 per-pipe entries, each carrying flags, a channel number, a time-slice value, and a count. |
| 21 | `0x2725` | Query multi-pipe configuration | out | Presumed to share the configure request's structure; no separate structure was found. |
| 22 | `0x2726` | Bind hyperpipe | in | A single hyperpipe-identifier field. |
| 23 | `0x2727` | Unbind hyperpipe | in | Presumed to share the bind request's single-field shape; no separate structure was found. |
| 24 | `0x2728` | Destroy multi-pipe configuration | — | Argument shape not shown in the header read. |

### Diagnostic range (base `0x2774`)

| Offset | Code | Label | Direction | Argument fields |
|---|---|---|---|---|
| 100 | `0x2774` | Address-mapping-mode control | in/out | See **Two DMA address-mapping modes** above. |
| 101 | `0x2775` | Flow control on/off | in | A single value: off or on. Off is the default; on hands the pipe's flow control to the kernel on behalf of a diagnostic caller (see **Flow-control register roles and credit model** below). Requires that the pipe already be through its general initialization; documented error returns are "not initialized" and "invalid value." |
| 102 | `0x2776` | Credit-count query | out | See **Flow-control register roles and credit model** below. |

**Cross-check (established, mixed).** Authored disassembly independently exercises
request codes `0x2710`, `0x2711`, `0x2715`, `0x2719`, `0x271c`, `0x2721`, `0x2723`,
`0x2724`, `0x2725`, and `0x2728` from user space, in each case for a purpose
consistent with the label above (userspace-path note, follow-up item 2). The
`0x2721` ("check pipe") call sites are especially precise corroboration: the
disassembly finds this code used as a poll-fallback after a spin-wait for
return data, with no argument buffer built or read around the call (userspace-path
note, follow-up item 4) — matching the header defining no argument structure for
this request at all. Bind and unbind (`0x2726`/`0x2727`) were not observed at any
user-space call site examined; their single-field shapes above rest on the
header's declared field layout and the naming symmetry with each other, not on
independent confirmation.

**Cross-check (open, sizes established on both sides).** The 32-bit-mode
mirror of the start-select-feed request's argument structure (offset 2,
`0x2712`) computes to 20 bytes from the header's own field list: four 4-byte
descriptor fields plus a 4-byte snapshot flag, needing no padding since every
field shares the same 4-byte alignment. The driver disassembly independently
records the compiled dispatch code for this same request performing a
`copyin_xlate` translation of a 32-byte userspace structure (driver
disassembly note, section 10). The two figures disagree, and this document
does not resolve why. One candidate explanation: the disassembly's cited size
may name the size `copyin_xlate` writes out to — the native (64-bit) form of
the same structure, whose embedded descriptor carries an 8-byte pointer field
and would, under ordinary MIPS N64 structure-alignment rules, pad to 32 bytes
overall — rather than the 20-byte 32-bit source structure it reads in. This
document has not verified that padding computation against the compiled
object, so it stays a candidate, not a finding. What would settle it: a live
PIO/qtest capture of the actual argument-buffer size crossing this ioctl, or a
deeper re-read of the dispatch code past the point already examined, to see
which structure's size the `copyin_xlate` call's own size operand names. The
same disassembly note records a second, similarly-shaped translation
elsewhere in the same dispatch function, of a 160-byte structure; that second
call site is not tied to any specific request code in the cited note, so this
document does not attempt to match it to any row in the table above.

## Memory-class taxonomy

**Established.** Memory is allocated (and freed) against exactly four classes,
each with its own count of allocation-purpose keys and its own set of five
limit/behavior fields (a maximum key value, a maximum alignment requirement, a
size multiplier applied when the allocation is swapped out, a shared-or-not
flag, and a copy-down-supported flag), plus a page size and a total available
page count that apply across all classes:

| Class | Purpose keys | Notes |
|---|---|---|
| Co-processor-only | 1 | A single allocation purpose for memory used only by the pipe's on-board co-processor. |
| Shared | 1 | A single allocation purpose, tied to the same "display list" concept that a separate, currently single-valued DMA-pool-type field also names. |
| Geometry-engine | 11 | Eleven allocation purposes: four pixel-conversion variants, three color-table variants, a histogram purpose, a purpose the header describes as unique per engine instance, a deactivation-related purpose, and one more — eleven keys numbered 0 through 10 inclusive. |
| Texture | 1 | A single allocation purpose for texture memory. |

## Buffer-sharing handle

**Established.** Sharing a buffer between processes (or between the driver and
a cooperating process) is a single opaque 64-bit identifier and nothing else —
one field, no flags, no size, no type.

## Health report and failure-code catalogue

**Established.** The health-report request (offset 16, `0x2720`, see the ioctl
table) returns a death-reason code plus two hardware-register snapshots taken
immediately before the pipe's failure handler overwrote the live registers: one
from an interrupt-info-class register (the same register role that also holds
the global interrupt-enable bit described under flow control below) and one
from an internal transaction-bus status register.

The death-reason code space has two sentinel values and 23 numbered failure
causes:

| Code | Meaning |
|---|---|
| 0 | Normal, working state — not a failure. |
| 1 | Never initialized — the sentinel value set before the pipe is ever brought up. |
| 2 | A context-deactivation request timed out. |
| 3 | A context-verification check failed. |
| 4 | An input DMA operation timed out. |
| 5 | A select-feed (readback) operation timed out. |
| 6 | The graphics FIFO stayed above its high watermark past a timeout. |
| 7 | The graphics FIFO clogged with no bus-I/O activity in flight. |
| 8 | The graphics FIFO clogged while bus-I/O activity was in flight. |
| 9 | Setting a timeout itself failed. |
| 10 | The graphics FIFO overflowed. |
| 11 | The output DMA engine overflowed. |
| 12 | An error was reported by dedicated address-translation/diagnostic hardware, distinct from the ordinary DMA and FIFO path. |
| 13 | A programmed-I/O overflow. |
| 14 | An input DMA overflow. |
| 15 | A memory swap-in operation timed out. |
| 16 | A memory swap-out operation timed out. |
| 17 | A mop-up (cleanup) operation timed out. |
| 18 | A check-pipe request timed out. |
| 19 | A death deliberately triggered through the debug request, for testing. |
| 20 | A context-activation request timed out. |
| 21 | A map-RAM verification check failed. |
| 22 | A parity error was detected. |
| 23 | The graphics FIFO remained above its high watermark (distinct from code 6's timeout framing; the header gives both a timeout-flavored and a persistent-condition-flavored code for the same watermark). |
| 24 | The graphics credit count got stuck below its bias floor and did not recover. |

## Board-inventory structure

**Established**, from the header devoted to this structure. The driver
assembles one inventory record per pipe, delivered to user space through a
generic, non-InfiniteReality-specific board-information request (out of scope here; see
above). Its fields:

- **Board-set/generation code.** One of five values: the original board
  generation (paired with the earlier of two host CPU-board families this
  driver supports), and four variants of a second-generation board set (paired
  with the later host CPU-board family) — a base second-generation variant, a
  reduced variant, a variant built entirely from one implementation-chip
  revision, and a variant with wider geometry-engine-to-backend interfaces
  paired with a different raster-manager chip revision.
- **Geometry-engine and raster-manager counts.**
- **A FIFO RAM size field.**
- **A video-cable-presence flag.**
- **A logical video-channel count**, including the channels contributed by an
  option board on the display generator (see the display-generator
  sub-structure bullet below).
- **A video-option-board-type flag field.**
- **Two color-map RAM size fields** (a primary one and a second, external/gamma
  one).
- **A four-entry raster-manager array**, one entry per possible raster-manager
  board, each carrying a presence flag and a texture-memory size.
- **A display-generator sub-structure**: a channel count, a channel revision,
  and — since the display generator supports an optional second, option-board
  channel — that second channel's type (five named type values, three of which
  the header treats as aliases for the same underlying type) and its revision.
- **A software/usage sub-structure**, carrying the memory-class table above
  plus: active geometry-engine and raster-manager counts, an occupancy mask,
  a flag distinguishing a narrow from a wide engine-to-backend interface width,
  up to eight logical-to-physical engine-identifier mappings, screen width and
  height, and a four-valued pixel-depth enumeration — with an explicit header
  note that one hardware variant reuses the enumeration's "extra-large" value to
  mean "extra-small" instead, since that variant supports neither large nor
  extra-large pixels.
- **A per-chip identity/revision sub-structure.** Each of three chip families —
  geometry engine, raster manager (across up to three distinct chip roles per
  board and up to four raster-manager boards), and display generator (an
  addressing chip plus two more chip roles) — has one or more raw identity
  values read from the hardware, and the header defines revision-detection
  logic as bit-pattern tests against those raw values rather than as separate
  fields. This document does not enumerate every detection rule; the pattern
  is consistent across all three chip families.
- **Seven per-board identity/revision records**, each in the generic,
  out-of-scope "identity-record" format named above: the geometry-engine board;
  up to four raster-manager boards; one board shared across the whole backplane
  that the header does not otherwise functionally describe beyond its identity
  record — this document calls it only "the shared backplane"; up to four
  texture-memory boards; a further board whose header comment gives only a
  place-name-style label with no functional description — this document
  records its existence and its identity-record slot as established, but
  cannot confirm from the header alone that it corresponds to any particular
  functional role such as a compositing accessory, and flags that
  characterization as unconfirmed; the display-generator board; and the
  display generator's option board.
- **An eight-flag mode/strapping word**, evidently controlling: a two-raster-manager
  configuration, texture-memory presence, a forced-FIFO mode, forced-present or
  forced-absent memory variants, round-robin engine dispatch, automatic
  raster-manager selection, and a "bare" (unadorned) rendering mode. The header
  gives this word an informal internal name this document does not reproduce;
  each flag's meaning above is this document's own reading of the header's
  descriptive comments, not a quotation of them.

## Event vocabularies

**Established**, both defined as enumerations in the board-inventory header.

**Geometry-engine to kernel**, delivered as the geometry engine encounters
conditions worth reporting to the host: one unconditional print/debug value
(code 0), plus six numbered event codes in a reserved range: select-feed
completion, context deactivation, a memory swap-out event, a mop-up
(cleanup) event, a memory-suspend event, and a check-pipe event.

**Co-processor to kernel**, delivered through a mailbox register: one
unconditional print/debug value (code 0), plus three numbered event codes in a
separate reserved range at offsets 0, 2, and 3 from that range's base — an
input-DMA-complete event, a memory swap-in event, and a context-activation
event. Offset 1 in that same range is explicitly not defined by the header; it
is a genuine gap in the vocabulary, not an omission by this document.

**Cross-check (established, exact agreement).** The mailbox-dispatch function
the driver disassembly recovered dispatches on exactly the same four values —
the print code and the three offsets 0, 2, and 3 — routing every other value,
including the undefined offset 1, into its generic "unexpected value" error
path (driver disassembly note, section 13). Header and disassembly agree
exactly here, including on the gap, which is the strongest single piece of
corroboration in this document.

## Flow-control register roles and credit model

**Established**, from the diagnostic header's descriptive prose. Turning
kernel-managed flow control on (see the flow-control-on/off request above)
programs seven distinct host-hardware register roles:

1. An interrupt-enable register, of which three bits matter here: FIFO
   high-watermark, FIFO low-watermark, and FIFO overflow.
2. An interrupt-info register, holding a global interrupt-enable bit (this is
   the same register role the health report's first saved snapshot names
   above).
3. A global-interrupt-info register, holding an interrupt-vector selector and
   a target-CPU-identifier mask.
4. A FIFO high-watermark threshold register.
5. A FIFO low-watermark threshold register.
6. An interrupt-status register.
7. A matching interrupt-acknowledge register.

With flow control off (the default), the pipe simply discards data that
overflows the FIFO rather than interrupting the host; turning it on lets a
diagnostic caller write into the pipe without risking silent data loss.

The credit-count query (offset 102, `0x2776`) returns four fields, each with
a stated meaning in the header's own prose:

- **Total.** Fixed total credit capacity — the size of the graphics FIFO
  between the host and the pipe's host-interface hardware. Zero on a platform
  that does not use credit-based flow control at all.
- **Bias.** A fixed per-CPU value equal to that CPU's "skid" — a floor below
  which the host holds off sending further data beyond the skid amount already
  in flight. Does not change once set.
- **Current.** The live credit count presently available to the calling CPU —
  the value a diagnostic caller checking flow-control health cares about most.
- **Interrupt-delay.** A value in nanoseconds: how long the current count may
  stay below the bias floor before the CPU is interrupted.

## Multi-pipe network ioctl group

**Established**, consolidating the five requests already listed in the ioctl
table above (offsets 19 through 24, minus the query-configuration and
destroy-configuration requests whose structures were not separately located):
a maximum of 32 pipes; a topology-query result of up to 32 index/network-identifier
pairs; a per-pipe configuration record of flags, channel, time-slice, and a
count field; a configure request bundling a pipe count, a network identifier, a
"hyperpipe" identifier, and up to 32 of those per-pipe records; and bind/unbind
requests each apparently taking a single hyperpipe-identifier field. Four of
the five request codes in this group (topology query, configure, bind, and
destroy) are independently exercised by user-space disassembly for hyperpipe
network queries and configuration (userspace-path note, follow-up item 2),
corroborating that this group is live, guest-facing functionality and not
vestigial.

## Widget identity, revision and the board-type classifier

**Established**, from CPU PROM disassembly (PROM InfiniteReality-path note,
sections 8.1-8.2), cross-checked against the kernel driver's own attach path
(driver disassembly note, section 1). The pipe identifies itself the way any
Crosstalk/XIO widget does, through the widget's own configuration registers,
before any of the pipe-specific windows described below come into play:

- **`+0x4`, widget ID register.** A 32-bit read splits into a presence bit
  (bit 0), a manufacturer field (bits 1-11), a part-number field (bits 12-27),
  and a revision field (bits 28-31). The CPU PROM extracts the part-number
  field with a right-shift by 12 then a 16-bit mask at more than twenty
  sites; the manufacturer field is extracted the same way at twelve further
  sites, for inventory text only.
- **`+0x24`, `WIDGET_CONTROL`.** A generic Crosstalk register, not specific to
  this pipe. The CPU PROM's classifier rewrites it once, unconditionally, as
  part of recognizing the board: `(old & ~0xf00f) | ((aperture >> 24) & 0xf) |
  0x3000`, planting the value 3 into bits 12-15 — the same bit field the
  platform's separate link-credit table (see **The inventory the guest
  derives from the pipe** below) assigns to this part number.
- **`+0x64`.** A 3-bit field, read masked with 7. Both the CPU PROM's
  board-type classifier and the kernel driver's own attach routine require
  this field to read `0` before treating the widget as present and of a
  known type — two independent implementations of the same check, on two
  different processors, converging on the same bit test. The driver reads it
  from the widget's own pre-piomap computed Crosstalk address, before its
  softc-cached aperture pointer even exists (driver disassembly note, section
  1); the CPU PROM reads it as part of the same instruction sequence that
  classifies the board (PROM InfiniteReality-path note, section 8.1). Neither
  source establishes what a nonzero value means beyond "not present, or not
  of a recognized type," and neither establishes the field's real reset
  value — only that zero is what admits the board. **Approximation: a model
  presents `0` unconditionally**, the minimum needed to pass both checks, not
  a hardware fact about the field's actual reset behavior.

**The classifier itself** is CPU PROM code at `0x1fc4291c` (PROM
InfiniteReality-path note, section 8.1), dispatching purely on the widget ID
register's part-number field:

| Part number | Board-type byte | Name written into the board record |
|---|---|---|
| `0xc102` | `0x51` | `kona` |
| `0xc003` | `0x53` | `mgras` |
| `0xc013` | `0x54` | `odsy` |
| anything else | `0x50` | `unknown_gfx` |

Type `0x51` is the published Linux `KLTYPE_GFX_KONA` (`KLCLASS_GFX | 1`, from
the GPL `arch/mips/include/asm/sn/klconfig.h` header); type `0x53` is the
published `KLTYPE_GFX_MGRA`. Type `0x54` has no published name — it postdates
the header this cross-check used. This board-type byte is what the IO6
firmware's own install routine dispatches on to run the install path
described below (PROM InfiniteReality-path note, section 7.9); it is written
into the board record at offset `+0x5` and read back from there by that
installer.

**Negative results, established.** The manufacturer field (`0x2aa`, the same
value the driver passes to its own widget-registration call at attach)
appears as no instruction immediate anywhere in the CPU PROM classifier — it
plays no part in board-type assignment. No revision-field check exists
anywhere in either source read for this document. Classification is by part
number and the `+0x64` presence check alone.

## Aperture layout: the widget's internal windows

**Established** (PROM InfiniteReality-path note, sections 7.2-7.3). This is a
different coordinate scheme from the four mapped user aperture sub-windows
described above — those are offsets within the result of a user-space
`mmap()` call; the windows below are the widget's own aperture-relative
offsets, read directly out of disassembled firmware and driver code, and the
correspondence between the two schemes remains only partly established (see
the cross-check under **The four mapped aperture sub-windows** above).

The aperture base is computed as `0x9200000000000000 + (nasid << 32) +
(widget << 24)` — the standard IP27/SN0 node-plus-widget compressed physical
address, 16 MiB per widget, matching the kernel driver's own independently
computed aperture size (a `0x1000000`-byte allocation, driver disassembly
note, section 1). Three windows are used from this base:

| Window | Offset | Use |
|---|---|---|
| Widget low window | `+0x0` | One 32-bit write of `0x23` at install (see below); a separately cited FIFO kick/doorbell register also sits at this window's own `+0x0` (see the register table below). |
| Register window | `+0x800000` | Status, mailbox, interrupt, reset and EEPROM-fallback ports — the bulk of the register table below. |
| Command port | `+0xc10000` | Every FIFO command word this pipe accepts (see **The DMA packet** and the FIFO command grammar under **The mailbox protocol** below). Always written at this window's own offset `0`. |

**Inferred**: a fourth, flow-control register window sits at `+0xc00000`,
because the driver's second "kick" register lands exactly `0x108` into it
(`+0xc00108`, driver disassembly note, sections 2 and 4). The IO6 firmware
never touches this window at all — an established negative result (PROM
InfiniteReality-path note, section 7.3).

**The register window is what reconciles the two sources' addressing.** The
driver's own base pointer for its register accesses is the aperture plus
`0x800000`; its citations of offsets `0x208`, `0x220`, `0x230`, `0x238`,
`0x240` and `0x300` are this document's `+0x800208`, `+0x800220`,
`+0x800230`, `+0x800238`, `+0x800240` and `+0x800300` (PROM
InfiniteReality-path note, section 7.3). Where both sources reach the same
register this way, this datasheet cites both; where only one source's addend
has been independently confirmed against its own object code, that is stated
in the register table below.

**A standalone register outside this scheme**: `+0x98`, the one-wire
identity-bus accessor (PROM InfiniteReality-path note, section 9), sits
directly off the aperture base — neither inside the low window nor inside
the `+0x800000` register window. See **The inventory the guest derives from
the pipe** below.

**Unreconciled: `+0x7c`.** The IO6 firmware writes `0x23` to `+0x7c` in the
widget low window exactly once, at install, before its first
synchronization probe (PROM InfiniteReality-path note, section 7.4). The
driver separately names an offset it also calls `+0x7c` the single
heaviest-used register in the entire driver — a FIFO/command-data port
written from every subsystem (driver disassembly note, section 1). The
firmware's own evidence places its register window `0x800000` higher than
the widget low window it wrote `0x7c` into, so the two `+0x7c` citations are
not obviously the same register; the driver's may in fact be `+0x80007c`,
distinct from the firmware's `+0x7c`. Neither source resolves this, and this
datasheet does not attempt to.

**Unreconciled: a driver-internal field-numbering discrepancy.** The driver
caches the address it reaches through both the flow-control kick register
and the command port in a softc-private field the disassembly cites as
`0x938` in more than one place, but with two different, seemingly
incompatible roles across citations — a word count in one reading of the DMA
kick path, and an aperture-relative base pointer in the readings of the
command-port and mmap-window-selection code (driver disassembly note,
sections 4, 5 and 17). This is a discrepancy in the underlying evidence, not
introduced here; it is recorded rather than resolved.

## The full register table

The table below combines every register either source establishes, grouped
by where it sits. **Approximation: reads zero** marks a register with no
established reset or idle value from either source, where a model must
present something because a supported guest reads it.

**Widget-level Crosstalk configuration registers.** Standard
widget-configuration space, not specific to this pipe; see **Widget
identity, revision and the board-type classifier** above for `+0x4`, `+0x24`
and `+0x64`.

**Widget low window:**

| Offset | Width | Direction | Value / behavior | Evidence |
|---|---|---|---|---|
| `+0x0` | 32 | W | FIFO kick/doorbell, paired with a computed credit value | driver disassembly note, section 1; addend not independently audited against the citing object's own instructions the way the offsets below were |
| `+0x7c` | 32 | W | `0x23`, written once at install, before the first synchronization probe | PROM InfiniteReality-path note, section 7.4 — see the unreconciled note above against the driver's own, differently addended `+0x7c` citation |
| `+0x9c` | 32 | W then R | A single write precedes an unbounded byte-stream read of an eight-field, colon-delimited ASCII identity record (see **The inventory the guest derives from the pipe** below); the write's own meaning on real hardware is not established | driver disassembly note, section 15; write observed in `ir-stage3-attach-capture-20260926.md`; modeled in `ir-stage3-increment1-kona-identity-register-20260927.md` |
| `+0x98` | — | R/W | One-wire identity-bus accessor — its own protocol, not a plain register read | PROM InfiniteReality-path note, section 9; see below |

**Register window (`+0x800000`), confirmed with the addend checked directly
against the citing object's own instructions** (driver disassembly note,
section 16) or established directly by the firmware at this address (PROM
InfiniteReality-path note, section 7.4):

| Offset | Width | Direction | Value / behavior | Evidence |
|---|---|---|---|---|
| `+0x800208` | 32 | R | Credit register, mask `0x7fe` — see **The credit register and its two polarities** below | Both sources; driver addend confirmed (driver disassembly note, sections 5, 11, 16) |
| `+0x800210` | 32 | W | Initial FIFO credit/threshold configuration, written once with `0x700` | Driver only, addend confirmed (driver disassembly note, section 16) |
| `+0x800220` | 32 | W | `0`, written first in the firmware's recovery reset sequence (see **Reset sequences** below) | PROM InfiniteReality-path note, section 7.4 |
| `+0x800228` | 32 | W | Fixed sentinel `0xbadc0ffe`, written unconditionally after the driver's general interrupt handler finishes, whether or not anything was pending | Driver only, addend confirmed (driver disassembly note, sections 3, 16) |
| `+0x800230` | — | — | **Inferred existence only.** No literal `0x230` register-offset access was found in the object this fact was originally attributed to; it rests entirely on a softc-shadowed field the driver treats as an interrupt enable/mask, inferred from how that shadow field is used in the interrupt dispatch path, not from any directly observed device access. **Approximation: reads zero.** | driver disassembly note, section 16 |
| `+0x800238` | 32 | R/W | Interrupt cause/status register — see **The three interrupt vectors and cause-bit dispatch** below | Both sources; driver addend confirmed (driver disassembly note, section 16) |
| `+0x800240` | 32 | R/W | ARM mailbox data register — see **The mailbox protocol** below | Both sources; driver addend confirmed (driver disassembly note, section 13) |
| `+0x800300` | 32 | R/W | Reset strobe — two distinct sequences, see **Reset sequences** below | PROM only; the driver's own `0x300` citation is not used here (see the note following this table) |
| `+0x800308` | 32 | W | `1`, enable — EEPROM-check path only | PROM only (PROM InfiniteReality-path note, section 7.4) |
| `+0x8003e8` | 32 | W | EEPROM address port | PROM only (PROM InfiniteReality-path note, sections 7.4, 7.8) |
| `+0x8003f0` | 32 | W/R | EEPROM data port: write `0` to prime, read the addressed word | PROM only (PROM InfiniteReality-path note, sections 7.4, 7.8) |
| `+0x8003c0` | 32 | W | `0x8800`, written once in the attach path | Driver only, addend confirmed (driver disassembly note, section 16) |
| `+0x8003d0` | 32 | R | Tested against bits `0x100` and `0x200000` in the attach path | Driver only, addend confirmed (driver disassembly note, section 16) |

Two offsets the driver cites as `0x300` and `0x220` are recorded above under
the firmware's own confirmed `+0x800300`/`+0x800220`, but the driver's own
citations for what it calls the same numeric offsets could not be verified:
no literal `0x300` or `0x220` register-offset access exists anywhere in the
object the citation was originally attributed to, and no function by the
cited name exists in that object either (driver disassembly note, section
16). The driver almost certainly does touch these registers, from a
different, unread object; until that is retraced, the driver-side claims
that offset `0x220` is "written once with `0x8000` at init, shadow-cached in
a softc field" and that offset `0x300` carries a reset strobe sequence of
its own remain **unknown** as to their real address, not refuted as facts
about the driver's behavior.

**Cross-check (inferred correspondence).** The header-derived half's **Flow-control
register roles and credit model** above lists seven abstract host-hardware
register roles from the diagnostic header's own prose, with no addresses.
Roles 6 and 7 there ("an interrupt-status register" and "a matching
interrupt-acknowledge register") plausibly correspond to `+0x800238` and
`+0x800228` here, and the credit-count query's "Current" field plausibly
reads back `+0x800208`. Neither disassembly source states this
correspondence directly; it is this document's own inference, offered
because the two halves otherwise never refer to each other's register
citations at all.

**Registers with an unaudited addend.** Established to exist, with the role
and width shown, but neither source has confirmed whether these sit inside
the `+0x800000` window, the widget low window, or somewhere else; each also
lacks an established reset or idle value. **No established behavior for any
of these: approximate each as inert** — a read returns zero, a write is
logged and otherwise has no effect on modeled state:

| Offset (as cited) | Width | Direction | Role | Evidence |
|---|---|---|---|---|
| `0x6c` | 32 | R | Error/status bitmask, one bit per named fault, dumped by the fatal widget-error interrupt handler | driver disassembly note, sections 1, 3 |
| `0x248` | 32 | R | Read once from the video/display-generator object. The disassembly note does not identify the reading function or trace what the value feeds into — a read site exists, but nothing further about its use was found. | driver disassembly note, section 1 |
| `0x260`, `0x268`, `0x278`, `0x2a0`, `0x2a8`, `0x2b8` | 32 | R | Geometry-engine fault/diagnostic registers, dumped on a GE error | driver disassembly note, sections 1, 3 |
| `0x3a8` | 32 | R/W | Touched from both a timer/watchdog-handling object and the video/display-generator object. The disassembly note does not name a specific function and traces no downstream consumer of the value beyond that timer/watchdog association. | driver disassembly note, section 1 |
| `0xb00` | 32 | W | Write target for a 3-bit field shifted out of the `0x64` read above | driver disassembly note, section 1 |

**Flow-control window and command port:**

| Offset | Width | Direction | Role | Evidence |
|---|---|---|---|---|
| `+0xc00108` | 32 | W | Flow-control "kick" register: a word count, written immediately before a payload burst to the FIFO port or command port | driver disassembly note, sections 1, 2, 4, 6 |
| `+0xc10000` | 32 | W | FIFO command port — every command word (see **The DMA packet** and **The mailbox protocol** below) | Both sources; PROM establishes 49 write sites and the complete command grammar (PROM InfiniteReality-path note, sections 7.3, 7.5) |

## The credit register and its two polarities

**Established**, and this is the sharpest disagreement between the two
sources on the same bits, not a contradiction this datasheet resolves.

Register `+0x800208`, masked with `0x7fe`, is read by both the IO6 firmware
and the kernel driver before every FIFO command or DMA push, but interpreted
oppositely:

- **The kernel driver** treats the masked value as a **signed** remaining-capacity
  counter (its own health-check and setup paths, driver disassembly note,
  sections 5, 11). Its wait loop retries while the value is **greater than
  zero**, with a 2-microsecond delay between attempts, and proceeds once the
  value is **less than or equal to zero** — or once a fixed cap of 100,001
  iterations is reached regardless of value.
- **The IO6 firmware** treats the same masked value as an unsigned equality
  check (PROM InfiniteReality-path note, sections 7.4, 7.5, 7.7): every
  command wait loop polls until the value is **exactly zero**, with no delay
  between reads, capped at 5,000 reads.

Both readings agree that zero means "go"; they disagree on what a nonzero
value means, on how patient to be waiting for it, and on whether any delay
belongs between polls. Neither source states the register's true reset
value or its behavior above the two readings' shared "zero" case.
**Approximation: reads zero**, satisfying both readings' proceed condition
unconditionally — the minimum needed to avoid either caller spinning to its
cap, not a hardware fact about how credits are actually tracked.

## The three interrupt vectors and cause-bit dispatch

**Established**, entirely from the kernel driver (driver disassembly note,
section 3); the IO6 firmware programs no interrupt mask at all and polls
`+0x800238` directly instead (PROM InfiniteReality-path note, section 7.4).

Three separate Crosstalk/XIO interrupt vectors are wired, one entry point
per vector:

1. **The widget/board-error vector.** Always fatal: dumps register `0x6c`'s
   fault bitmask, then unconditionally aborts the pipe. No recoverable path.
2. **The general status vector.** Reads the cause register at `+0x800238`,
   masks it against a softc-shadowed enable/mask value (inferred to be
   register `+0x800230`, above) with bits `0x1` and `0x20` excluded (`raw &
   mask & ~0x21`), then dispatches by bit:

   | Bit | Action |
   |---|---|
   | `0x1` | Fatal abort |
   | `0x2` and `0x4` together | FIFO low-watermark handler |
   | `0x4` alone (without `0x2`) | Fatal abort |
   | `0x8` | Fatal abort |
   | `0x10` | Video interrupt handler |
   | `0x40` | ARM mailbox handler (see **The mailbox protocol** below) |
   | `0x100` | Logged only, not otherwise acted on |
   | `0x200` | Genlock/VOC interrupt handler |
   | `0x400` | Fatal abort |
   | `0x800` | Fatal abort |

   Handled bits are acknowledged by writing the same masked value back to
   `+0x800238` (write-to-clear). Then, **unconditionally, regardless of
   whether anything was pending**, the fixed sentinel `0xbadc0ffe` is written
   to `+0x800228` — established as this exact value and address; its role as
   a re-arm or end-of-interrupt write, distinct from the cause register
   itself, is **inferred**.
3. **The geometry-engine vector.** A separate entry point, GE-only: dumps the
   six GE fault/diagnostic registers listed in the register table above,
   then acknowledges by writing `0x20` to `+0x800238` — its own ack path,
   distinct from the general vector's write-back.

No interrupt-vector-select register was found inside the pipe's own
aperture; vector routing is entirely through the platform's generic
Crosstalk interrupt-allocation calls, outside this driver's own register
interface.

## The mailbox protocol

**Established**, from both sources, and in close agreement (driver
disassembly note, sections 11, 13; PROM InfiniteReality-path note, sections
7.6, 7.7).

The pipe's ARM co-processor answers commands the host pushes through the
FIFO command port (`+0xc10000`) by posting reply words through the mailbox
pair `+0x800240` (data) and `+0x800238` (cause, bit `0x10` = reply ready):

- **Reading a reply word.** Poll `+0x800238` masked with `0x10`; once set,
  read `+0x800240`, write `0` back to `+0x800240` to clear it, then write
  `0x10` back to `+0x800238` to acknowledge and re-arm before polling for the
  next word. The driver delays 100 microseconds between poll attempts,
  default cap 50,000 iterations, overridable by the caller; the IO6 firmware
  delays a shorter, firmware-internal unit between attempts, default cap
  5,000. Both sides implement the identical read-clear-acknowledge sequence.
- **The liveness/synchronization probe.** Both sources send the same fixed
  command triplet, `1, 9, 0`, through the command port, then expect exactly
  one reply word equal to the fixed constant `0x1234abcd` within a
  caller-supplied timeout — the driver's own liveness check and the
  firmware's own synchronization probe are, independently, the identical
  check.
- **The graphics-info query.** Both sides send `1, 0xc, 0` and read back five
  reply words. Here the two sources diverge on which words carry the answer:
  the driver consumes reply words 1 and 2 (checking each is non-negative,
  otherwise returning fixed error codes `0x500`/`0x400`); the IO6 firmware
  consumes reply words 3 and 4, established there as width and height,
  substituting the defaults 1280 and 1024 when negative. This document does
  not reconcile the two word-index readings — they may reflect two different
  firmware revisions' reply layouts, or one of the two disassembly passes
  mis-indexing a stack buffer; either is possible and neither is confirmed.

**Dispatch on an unsolicited mailbox value** (the driver's own
interrupt-driven path, distinct from the two request/reply exchanges above,
driver disassembly note, section 13): the driver reads `+0x800240` and
dispatches on its value —

| Value | Action |
|---|---|
| `0` | Drains an ARM-originated diagnostic string a byte at a time from `+0x800240`, terminated by a NUL byte or a 511-byte cap, logged once complete. Acknowledges `+0x800238` once at loop entry, not per byte. |
| `0x100` | Wakes a waiter (semaphore post, pending timeout cancelled), no lock held around the wake. |
| `0x102` | Hands off to the driver's memory swap-in completion path; not itself a wake operation. |
| `0x103` | Wakes a different waiter, the same way as `0x100` but under a spinlock. |
| anything else, including `0x101` | Falls into the driver's generic "unexpected mailbox value" error path. |

## Reset sequences

**Established**, and the two sequences are distinct, not one sequence with
an alternate encoding (PROM InfiniteReality-path note, section 7.7).

The IO6 firmware's pipe initialization first runs the synchronization probe
(above) with a reduced 100-iteration cap. Only if that fails does it run a
**recovery sequence**: write `0` to `+0x800220`, write `3` to `+0x800300`,
read `+0x800300` back, write `2` to `+0x800300`, delay, then re-run
graphics-info and a second synchronization probe at the default cap.

A separate **EEPROM-check sequence** (used during the install path described
below, not during ordinary reinitialization) writes `1` to `+0x800300`,
reads it back, writes `0`, delays, then writes `1` to `+0x800308` before
reading the EEPROM fallback ports.

**Inferred**: bit 0 of `+0x800300` is a reset strobe and bit 1 a run/enable
that the recovery sequence leaves asserted (write `3` then `2`, versus the
EEPROM path's write `1` then `0` with no bit-1 assertion at all). Neither
bit's real name is established from either source, and nothing in either
source identifies any register here as specifically starting the ARM
co-processor beyond this strobe pair.

The driver's own citation of a `0x300`-offset reset strobe (write `1`, read,
write `0` — structurally close to the firmware's EEPROM-path sequence, not
its recovery sequence) could not be confirmed against the object it was
attributed to (see the register table above); it is not used as independent
corroboration here.

## EEPROM fallback ports

**Established**, IO6 firmware only (PROM InfiniteReality-path note, sections
7.4, 7.8); no driver-side analog was found in anything read for this
document.

The address port `+0x8003e8` and data port `+0x8003f0` read twelve
consecutive words from an address range the firmware's own log messages call
`0x87ff00` through `0x87ff2c` (step 4): write the target address to
`+0x8003e8`, read the word back from `+0x8003f0`, delay 2 units, repeat.
These `0x87ffxx` values are contents of the address port, not further
aperture offsets; that they name an EEPROM address space at all is
**inferred** from the firmware's own diagnostic messages, not confirmed
against any EEPROM datasheet.

The full check sequence: print a diagnostic message, run the EEPROM-path
reset sequence above, delay, write `1` to `+0x800308`, write a fixed address
to `+0x8003e8`, write `0` to `+0x8003f0`, then read the twelve words. The
install routine (below) accepts only the eighth word (from address
`0x87ff1c`) equal to exactly `2`; any other value abandons the pipe for
graphics-console purposes — the installer returns `0`, prints a warning
naming the pipe as having a bad EEPROM, and the pipe gets no graphics
console. This is the only way a widget that is otherwise present and
correctly typed ends up with no console: a failed synchronization probe
**and** a failing EEPROM check.

## The DMA packet

**Established**, three distinct mechanisms, each driven by its own private
ioctl code (driver disassembly note, sections 2, 4, 14):

1. **PIO streaming** (ioctl `0x2710`, "DMA read" in the ioctl table above).
   No DMA engine at all: the driver loops over a caller-supplied array of
   `(value, run-length)` pairs and writes each value that many times, one
   32-bit programmed I/O write at a time, to a single fixed per-channel
   control-block register.
2. **The six-word FIFO descriptor** (ioctl `0x2711`, "DMA write" above). The
   driver pins the caller's buffer, writes the flow-control word count to
   the kick register at `+0xc00108`, then pushes six 32-bit words to the
   FIFO port in sequence:

   | Word | Content |
   |---|---|
   | 1 | Transfer address, right-shifted 12 bits |
   | 2 | Low 12 bits of the address, OR'd with the fixed tag `0x9000` |
   | 3 | Caller-supplied argument, passed through unchanged |
   | 4 | Caller-supplied argument (the same transfer address), passed through unchanged |
   | 5 | Caller-supplied argument, passed through unchanged |
   | 6 | Caller-supplied argument (a flag deciding whether a timeout is armed), passed through unchanged |

   The `>>12`/`&0xfff` split **infers** a 4 KiB page granularity for
   whatever address space this addresses; nothing in either source confirms
   this against a datasheet. No explicit completion poll was found in the
   function that pushes these words — completion is presumably signaled
   through the interrupt path above, but the exact signal is **unknown**.
3. **The 16 KiB page-table/mapram load** (ioctl `0x2722`). Walks a physical-page
   scatter list in 16 KiB chunks and writes one 32-bit word per chunk — the
   chunk's physical frame number — to a device address that itself encodes a
   destination channel. This looks structurally like loading a
   page-table/TLB-style mapping unit, not a literal content copy, matching
   the function's own name; **inferred**, not confirmed by any string or
   comment.

None of the three is individually confirmed as the path that carries a
microcode image into the pipe; nothing in the driver corpus read for this
document references a microcode image filename directly. The six-word FIFO
mechanism (`0x2711`) is the strongest candidate, since it is the only one of
the three preceded by variable-size buffer handling — **inferred**, not
established by any direct evidence naming it as a microcode carrier. The IO6
firmware, separately, downloads no program into the pipe at all during its
own install path (PROM InfiniteReality-path note, section 7.10): the only
bulk transfer it makes is a display-logo bitmap, sent as FIFO command
`0xa`'s argument, copied host-side into RAM rather than to the pipe.

## The inventory the guest derives from the pipe

**Established**, from both sources, covering the one-wire identity path and
the board-type assignment that together supply per-pipe inventory text and
typing (driver disassembly note, sections 1, 11, 15; PROM
InfiniteReality-path note, sections 8, 9).

**The one-wire identity register at `+0x98`.** A standalone register outside
both the widget low window's other offsets and the `+0x800000` register
window (see **Aperture layout** above), implementing the same bit-banged
one-wire protocol design already documented for the
[Bridge widget's own one-wire register](bridge.md#the-board-identity-one-wire-line-which-mainline-linux-must-be-able-to-read)
(offset `+0x00b0`) and the [Hub's one-wire register](hub-onewire.md#register-map)
(offset `+0x2000a8`) — a third documented instance of the identical design,
differing only in access width and aperture offset. The full bit contract,
established from the firmware's accessor (PROM InfiniteReality-path note,
section 9.2):

| Bits | Direction | Meaning |
|---|---|---|
| 19:10 | W | Pulse width — how long the master drives the wire low to open a slot |
| 9:2 | W | Sample offset — how long after the pulse the master samples |
| 1 | R | Completion — **polled with no iteration cap, no deadline and no error exit; the only unbounded wait in the entire firmware path** |
| 0 | R | The level sampled during the slot just finished |

Firmware-observed timings, **inferred** to be microseconds from the values
themselves (a textbook one-wire reset/presence window and standard-speed
write slots), not from any source naming the unit:

| Slot | Pulse | Sample offset | Post-slot delay |
|---|---|---|---|
| Reset / presence | 520 | 65 | 500 |
| Write-1 / read | 6 | 110 | 0 |
| Write-0 | 80 | 30 | 0 |

The full command sequence: bus reset, then a search command (`0xf0`) to
discover attached parts (a 64-iteration bit-shift search), then a match
command (`0x55`) followed by the target's eight identity bytes to address a
specific part, then a family-specific read dispatched through a seven-entry
table. Failure paths are bounded and return numbered codes (no presence
pulse, a null buffer, a propagated search/write failure); the completion
poll on bit 1 is the sole exception. An undriven wire samples as `1` ("no
part attached"), which the firmware treats as a clean, handled case rather
than a failure.

**Approximation, for a minimal model**: presenting bit 1 set after every
write (turning the unbounded wait into an ordinary, immediate poll success)
is enough to remove the hang described above; additionally presenting bit 0
set reports "no part fitted" cleanly, since the board type is already fixed
from the widget ID register's part number by the time this read happens —
this asserts nothing about any real InfiniteReality one-wire contents,
which this evidence does not establish.

**The eight-field identity record.** Both sources independently confirm the
same colon-delimited field labels — Revision, Capability, Variety, Laser,
Part, Name, Serial, Group (driver disassembly note, section 11; PROM
InfiniteReality-path note, section 8.3, which finds seven of the eight
literal field-separator strings verbatim in the CPU PROM). The driver reads
this record as plain ASCII through ordinary byte loads from `+0x9c` off its
own aperture pointer (driver disassembly note, section 15), with no protocol
of its own — a plain memory-style read, not a bit-banged one-wire exchange.
**Unreconciled**: this is a different offset from the firmware's bit-banged
`+0x98` register, and a different access mechanism entirely (plain byte
reads versus a pulse/sample protocol) for what both sources otherwise treat
as the same underlying identity content. Neither source explains how a
4-byte-higher offset produces already-decoded ASCII from what is, at
`+0x98`, a raw one-wire bus accessor; this datasheet records both citations
without merging them into one register.

**A single write precedes the read loop, established from a guest capture,
correcting this section's earlier read-only claim.** A two-boot capture
against a guest running the kernel driver
(`docs/research/graphics/ir-stage3-attach-capture-20260926.md`) shows the
driver writing once (`0x82104`) before it begins reading `+0x9c`, with no
further writes interleaved among the reads that follow — the driver
disassembly's `nic_xg_vertex_info` call site was not traced far enough by
itself to show this write; the capture supplies the missing half. Both
sources' disassembly sections describe the read side only, so the write's
own role — priming the identity-record cursor, addressing a specific
one-wire part the way the `+0x98` protocol's match command does, or
something else entirely — is not established by either source or this
capture. One further fact the capture adds without resolving it: the
write's value is the same 32-bit word the same boot's earlier `+0x98`
one-wire handshake had already produced, which may indicate a real board
derives `+0x9c`'s answer from the same one-wire data `+0x98` exposes,
encoded differently, or may be coincidental to this guest's driver and this
model's fixed one-wire fixture. This datasheet records the observation
only; docs/designs/infinitereality-host-command-approximation.md owns the
model's own approximated response (a write-resets, read-streams design,
built and qtest-covered in
`docs/research/graphics/ir-stage3-increment1-kona-identity-register-20260927.md`)
as an approximation, not a hardware fact.

**The board-type assignment.** The CPU PROM's classifier (see **Widget
identity, revision and the board-type classifier** above) writes the
board-type byte and, separately, the one-wire record's parsed text into the
platform's board-configuration record — established as inventory text only,
not part of the type decision itself: the type byte is fixed from the widget
ID register's part number before the one-wire read ever happens (PROM
InfiniteReality-path note, section 8.3).

**The link-credit table, background only.** A fourteen-record table (part
number, credit count, mask, shift) programs how many Crosstalk link credits
each widget type gets, consulted by generic platform link-management code,
not by the graphics-console install path (PROM InfiniteReality-path note,
section 8.4). This pipe's part number (`0xc102`) shares its table row with
the MGRAS and Odyssey graphics parts: 4 credits, in a 4-bit field at bit
position 12. It plays no part in board-type classification and is recorded
here only because it shares the same widget ID register this datasheet
already documents.

## Provenance and rights

Held IRIX 6.5.5 kernel interface headers for this driver, read in full by a
source reader — the identical path is absent from this Library's IRIX 6.5.7
holding, which retains only cross-reference and manifest entries for these
three headers, not their content; no code or identifier reproduced; readers of
the restricted tree are excluded from this family's implementation and review.

The register-map half added in this revision carries none of the above
exposure. It is built entirely from Origami's own disassembly of the kernel
driver's compiled object files and of the IO6 and CPU PROM firmware images —
authored binary-analysis research, not a source-reader product. No restricted-tree
path, filename, or identifier appears anywhere in the two disassembly notes
it cites or in this half of the document itself, so this half carries no
source-reader exposure and imposes no implementation or review restriction of
its own; see **Coverage** above for what implementation still needs before it
can rely on this half — its own independent review, not yet done.
