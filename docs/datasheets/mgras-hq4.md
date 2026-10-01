# Datasheet: MGRAS HQ4 host interface

References rendered as citation identities name material outside this document
set. Those targets are not included or cleared for redistribution. Review and
runtime statements retain their recorded dates and scope.

**Status:** draft contract. The initial attachment is selected. Exact
textureless-SI revisions, DMA fault and interrupt behavior, and the complete
command grammar remain unresolved.

**Sources used:** SGI OCTANE Technical Report, retained MGRAS PROM and
diagnostic material, IRIX 6.5 and 6.5.30 software holdings, OpenBSD IMPACT at
`7011342f`, X.Org IMPACT 0.2.0, retained caller-path trace and source-contract
metadata, a clean-room decode of the DMA source-mapping and image-transfer
contract from the committed emulator model and its register-contract tests,
and a disassembly reading of the shipped IRIX 6.5.30 IP27 kernel's MGRAS
driver routines for the [context switch and HQ flags](#context-switch-and-hq-flags)
section. Source identity is recorded in `docs/provenance.md` and the MGRAS
research record.

The contract uses original descriptions. It does not reproduce private source
declarations, structure, comments, tables, or program bytes.
The independent SI evidence review (citation: `docs/reviews/graphics-chipset-si-evidence-20260915.md`)
records the restricted evidence boundary. Implementers and implementation
reviewers use these independently worded contracts and their public sources,
not the restricted inputs named in provenance.

## Scope

The initial population is one textureless SI board with one HQ4, one GE11, one
RE4, two PP1s, 12 MiB of pixel memory, no texture daughterboard, and one
display path. Its selected Origin 2000 attachment is a transplant
configuration: BaseIO remains at IO1 and the removable MGRAS XIO board occupies
IO3. The standard four-node Origin 2000 machine
contract (citation: `docs/machines/origin2000.md#what-the-diagram-binds`) assigns Crossbow 0
IO1 to widget 8 and IO3 to widget `0xf`. Those widget numbers describe this
machine configuration, not portable properties of either board.

No shipped configuration carries the board: a stock Origin 2000 has no
graphics. It is fitted at launch into an empty XIO slot, which supplies the
widget: `-device sgi-mgras,slot=io3` (`tools/origami run irix|prom --board mgras@io3`)
keeps the IO3 selection above, and in a GIGAchannel cabinet `slot=io6` puts it
at widget `0xf`. No captured GIGAchannel shows an SI board, so that position
is a model choice.

Public owner records establish the IO1/IO3 composition but expose no raw widget
numbers. The IO3 widget `0xf` value is therefore a selected emulator assignment
from the machine contract, not an observed board fact. Exact
textureless-SI assembly, ASIC revisions, widget revision, PROM bundle, and
software bundle remain unknown. The MOT10 assembly and HQ, GE, RE, PP, VC3,
CMAP, and TRAM revisions observed on an ESI+TRAM board do not transfer to this
population.

This public-evidence selection is ready for independent review. That review may
clear the attachment boundary for implementation, but it cannot clear reset
order, byte lanes, DMA, interrupts, or any revision-dependent behavior.
Direct-XIO and other routed systems require their own reviewed attachment
contracts.

HQ4 accepts host commands, distributes work, moves data, and reports status.
Raster and display behavior belongs to
[the raster/display contract](mgras-raster-display.md).

## Confidence and implementation status

| Area | Confidence | Current prototype | Required disposition |
| --- | --- | --- | --- |
| Widget identity and published windows | established for the published subset | partial | retain published offsets and trace unknown accesses without assigning return values or side effects |
| 32-bit controls and 64-bit FIFO writes | established as software expectation | accepted | add width and partial-write tests |
| FIFO ordering and fences | inferred | approximated | separate acceptance, execution, completion, and empty states |
| Bounded raster commands | inferred for the public console path | partial | preserve raw words; unsupported commands stay inert |
| Block-copy payload and direction | unknown | previously invented | do not dispatch until a reviewed trace fixes the grammar |
| DMA image-transfer format, slot mapping, and source address translation | established for observed node/offset captures; inferred-strong beyond | accepted | extend qtest coverage to a genuine remote-node transfer; DMA fault behavior remains separately unknown |
| Direct-XIO interrupts | unknown | borrowed approximation | do not treat another attachment's allocation as SI behavior |
| Context-switch transaction, general-interrupt sources, HQ flag bits | established as the kernel's software contract; silicon timing unknown | partial: the first switch completes, a later switch's image is fed to the token parser; the query sets flag 17 with an approximated answer of zero | implement the transaction and flag rules in [context switch and HQ flags](#context-switch-and-hq-flags); no closing word |
| Uploaded program protocol | unknown | not interpreted | retain guest-loaded state without embedding or translating it |

## Published register windows

| Function | Offset | Contract |
| --- | ---: | --- |
| HQ status | `0x020000` | Expose only reviewed ready and error fields. Unknown bits remain unspecified and trace-visible. |
| FIFO status | `0x020008` | Track occupancy and published low-water behavior; exact thresholds remain unknown. |
| Host or bus status | `0x020100` | Preserve documented read and write-clear behavior only. |
| DMA status | `0x020200` | Report separately tracked inbound and outbound activity once implemented. |
| Normal command FIFO | `0x020400` | Accept aligned 64-bit writes in guest byte order and preserve arrival order. |
| FIFO controls | `0x040000`–`0x040030` | Store published threshold and delay fields; make transitions observable. |
| XMAP window | `0x071c00` | Forward display state to the raster/display component. |
| VC3 window | `0x072000` | Forward timing and cursor state to the raster/display component. |
| Secondary FIFO (DMA source-mapping port) | `0x130400` | Kernel DMA source-mapping port, FIFO class `0x48`, distinct from the normal command FIFO. Five slots each expose a base-address pair, page count, index width, and control register; see [DMA, interrupts, reset, and retention](#dma-interrupts-reset-and-retention) below. |

Accesses outside this subset are trace-visible but otherwise unspecified. Their
returned value, state changes, and error side effects remain unknown, so no
implementation may choose them until evidence settles them.
Published controls use 32-bit accesses and the normal FIFO uses aligned 64-bit
writes. Byte-wide display accesses belong to the raster/display contract.
Other widths, unaligned accesses, and partial FIFO writes remain unresolved.

## Command and synchronization model

| Event | State change | Guest-visible consequence |
| --- | --- | --- |
| FIFO write accepted | raw word enters the queue | occupancy changes; no raster result is implied |
| Word decoded | state write or execution request is formed | malformed and unknown words remain observable |
| Work completed | HQ, raster, or DMA work reaches its completion point | status or an interrupt may change |
| Fence observed | requested dependency is complete | a guest read or wait may proceed |

FIFO empty is not raster completion, DMA coherence, or display visibility. Save
and restore must retain queued words, decoder position, pending work, status,
interrupt latches, and uploaded program state.

The bounded public console subset may cover masks, colors, rectangle
coordinates, logic operations, and character expansion. It is not the complete
HQ or raster language. Unknown commands leave unrelated state unchanged.

### Observed software wait tail

The retained run is `irix-20260921T062931-35ea4c`, using QEMU
`fdd62121bd44aec1a3b283ee9cf063ac5cbd827c` and the Origin 2000 routed model
with `baseio-preset=io6g`. Its `qemu.trace` SHA-256 is
`db29fe91459163d9cf05c5a83623b87fc55bc5c89f3dfa5543e710967bd2385e`.
The trace has 2,230,603 MGRAS events, including 858,589 writes and 1,372,014
reads. Of those events, 643,138 are writes at `0x120400`. The final sequence
number is 7,404,970.

Near the trace tail, the caller wrote `0x040068 = 1`, read `0x020008` as zero,
and issued no further MGRAS MMIO while the guest waited. This is a trace of the
model and caller, not a hardware observation. It cannot establish direct-XIO
transport or interrupt behavior. The retained caller evidence treats
`0x040068` as a write-only scheduler-state word. It is written on several setup
and switching paths and is not read back. Its value therefore has no settled
meaning here.

The retained `mgras.o` SHA-256 is
`6a65e70d3cdcb9822b1cf5a840431b5d971e39957a350ab52b87cf620ffb6620`.
It is the `mgras.o` member of
`refs/irix-media/6.5.30/usr/cpu/sysgen/IP27boot/mgras.a`. The restricted source
tree is revision `5aa45987657a064aa10df492d02eb4b137f73da0`. These identities
anchor the software evidence and do not turn it into a silicon observation.

The low-water source claim is bound to MGRAS-001 in
`docs/research/graphics/irix-mgras-source-contract.tsv`:
`stand/arcs/lib/libsk/graphics/MGRAS/mgras_init.c`, SHA-256
`a3f579e4a7224bc17c5bac53643c55856ec80e8cf7df233b21bf038dbfc9c74e`, with
anchors `217-372`, `377-500`, `587-722`, `729-772`, `774-831`, `839-1070`,
`1425-1645`, `1688-1772`, `1816-1939`, and `1941-2110`. The caller FIFO-wait
path is MGRAS-002 in the same record:
`stand/arcs/lib/libsk/graphics/MGRAS/mgras_tport.c`, SHA-256
`b23fa13facee6fced9ef390803dd26b4b668688d85a956d3c948ff7c47503f04`, with
anchors `48-103`, `105-146`, `148-191`, `193-268`, and `288-347`.

The same evidence identifies `0x020008` as FIFO status. The relevant FIFO wait
requires its low byte to equal exactly zero. The supplied tail therefore shows
that the observed caller-side FIFO poll had succeeded before MGRAS traffic
stopped. It does not show that the preceding scheduler write caused the wait,
that FIFO empty means raster completion, or that an interrupt woke the caller.
The trace ends before any such wake transition can be observed.

Keep this path separate from the low-water watchdog path. Source and
source-contract metadata identifies its second-level low-water enable at
`0x011050` bit `0x10` and its clear path through `0x011058`. This run has five
total `0x011050` events: three writes and two reads, all with value zero. It
also has three `0x011058` writes: `0xffffffff` once and `0xffffffef` twice. The
run observes clear-register accesses, but neither an arm nor a proven disarm
transition. The timeout path can report a caller timeout or panic if
completion does not arrive. Those are software control-flow and error
outcomes, not settled silicon meanings.

Before implementation assigns any of these transitions, a reduced guest
capture must include the control writes and reads around the first wait,
including `0x011050`, `0x011058`, `0x011020`, and `0x011028`, with strict event
ordering. A discriminating qtest should then run two cases: a scheduler-word
write followed by a successful FIFO-zero poll, and a low-water arm, delivery,
clear, and re-arm sequence. The cases must inspect status and pending state
separately. Passing the model test does not establish physical behavior.

## DMA, interrupts, reset, and retention

Host-memory DMA for image transfers is settled for the observed capture set.
The normal command FIFO at `0x120400` carries image-transfer descriptors; the
secondary FIFO at `0x130400` is the source-mapping port those descriptors draw
guest pages through. The three subsections below record that contract. DMA
fault handling, burst and page behavior beyond the mapping port, cancellation,
and direct-XIO interrupt routing remain separately unspecified, and the
implementation must not claim them settled.

### DMA source-mapping port (`0x130400`)

The secondary FIFO is a kernel DMA source-mapping port, not a second raster
command queue. It shares the normal FIFO's token framing — a 16:8:8-bit
class:register:length header — and answers to FIFO class `0x48`. It exposes
five slots (`k` = 0 through 4), each with four registers: a base-address pair
at registers `0x20 + 2k` and `0x21 + 2k`, a page count at register `0x30 + k`,
an index width at register `0x2b + k`, and a control word at register
`0x4b + k`. For slot 3 these are registers `0x26`/`0x27`, `0x33`, `0x2e`, and
`0x4e`.

The base-address pair addresses a node-qualified guest page list:
`list_pa = (base_hi & 0xff) << 32 | (base_lo & ~0x7)`. The low byte of the high
word selects the guest node; the low word is a byte address, not a
page-aligned one, so the list may begin mid-page (observed low words end in
`0x18`/`0x19`, 24 bytes into the page after three zero header words). Because
the mask clears only the low three bits, `list_pa` already points at entry 0,
and entry `i` follows at `list_pa + i * 8` from `i = 0`; masking `list_pa` to
its page instead would read the page's zero header as page-frame number 0 and
start every transfer 24 bytes short. Each entry is a big-endian 64-bit word
carrying a guest page-frame
number in its low 20 bits (mask `0xfffff`); the committed model masks entries
to that width so a node-shifted address cannot collide with the page-frame
field, which bounds each node to 4 GiB addressed this way.

This is established for the observed capture set: guest nodes 0 and 3, and
page-frame numbers below `0x4000`. Cross-node routing and the entry format
beyond that range are inferred-strong from the same decode and the committed
model's own masking rationale, not independently observed on a genuinely
remote node. The qtest harness cannot program a remote-node memory claim to
exercise that path; a discriminating remote-node qtest is a documented
follow-up, not yet written.

### DMA image-transfer descriptors (`0x120400`)

Three descriptor-class register banks on the normal command FIFO — classes
`0x20000`, `0x28000`, and `0x30000` — carry host-memory image-transfer
descriptors: pitch, a low-3-bit source offset, an aligned source offset, row
count, bytes per row, a destination box (registers `0x046`/`0x047`), and a
kick register. The two source-offset registers are non-overlapping parts of
one offset that the model adds together, not the two halves of a 32-bit
value:

```
va = src_off_low3 + src_off_aligned + row * pitch + byte
```

The transfer walks that virtual byte offset through the mapped slot's page
list: `page_ix = va >> 12` selects the list entry, and
`src_pa = (node << 32) | (pfn << 12) | (va & 0xfff)` is the resulting guest
fabric address the model reads or writes through the normal DMA path, one
byte at a time.

The kick register's low byte selects both the mapped slot and the transfer
direction: `slot = (kick_byte >> 1) & 3`, with bit 3 giving direction
(observed as `0xb1`/`0xb7` for a slot-0/slot-3 write, `0xb9`/`0xbf` for a
slot-0/slot-3 read). A write kick paints the framebuffer from guest memory
through the mapped slot. A read kick stages framebuffer pixels back to guest
memory (screen-to-host) using hardware bottom-up row addressing and needs no
fence. Only the write path sets the completion fence, `0x020010` bit
`0x10000`, that the IRIX X server polls before reusing its source buffer; the
read path completes without touching that bit.

The desktop backdrop is separate host-memory traffic: it is a small
PIO-seeded weave tile that IRIX replicates across the screen by
screen-to-screen doubling copies through the raster engine, not by DMA from a
host buffer. That block-copy grammar remains unknown (see the confidence
table above); resolving it is not part of this contract.

### Outstanding DMA validation

The engine is register-contract-tested:
`tests/qtest/sgi-mgras-pixels-test.c` covers slot-register read/write, the
list-entry byte offset `0x18`, kick-byte slot selection (including an
unmapped slot completing as a safe no-op), a non-zero 8-aligned source
offset, host-to-screen paint, and screen-to-host read-back with an explicit
pre-kick fence-clear assertion. Every case runs at guest node 0; none
exercises a non-zero node end to end. The QEMU repository's held PR #759
carries this engine at commit `ba05b6cdd7`; its required `build-and-qtest`
check passed there.

End-to-end guest validation is incomplete. Retained screen captures from the
same work show the model rendering the greeter's icon panel and, after login,
the desktop's toolchest and icons through this engine. A full post-login
screenshot of the backdrop-stipple weave is still outstanding: graphical
login through the `clogin` greeter has been intermittently unresponsive to
injected input across repeated attempts, a separate input-path issue from the
paint engine documented here. A related `xwininfo`/`xset` hang when a new
client attaches from a shell is also unresolved and needs its own capture.
Neither open item changes the register-contract claims above; both are
tracked follow-ups.

Interrupt state is represented as source, latch, mask, route, acknowledge, and
priority. A four-role allocation seen on a routed system is only a hypothesis
for direct-XIO SI.

The following reset categories identify questions, not required hardware
behavior. The selected target must establish both reset effects and retention
before implementation.

| Reset event | Effects to establish | Retention to establish |
| --- | --- | --- |
| Cold board reset | link state, FIFO, active work, status, device interrupt state | display tables, cursor SRAM, uploaded programs |
| Warm device reset | HQ queue and command state | raster planes, display tables, timing, cursor |
| Link reset | transport credits and link status | HQ program, raster work, DMA, display |
| Local HQ reset | HQ decoder, queue, HQ-owned errors | downstream raster, DMA, display |

The current QEMU model discards all context-handshake progress during a device
cold reset, including its two acknowledgement latches. This is a functional
lifecycle approximation, not a physical HQ4 reset or retention claim. The
qtest observes both latches before and after reset, then verifies that the old
handshake cannot continue. Replace this rule when selected-target evidence
establishes the cold-reset retention contract.

## Context switch and HQ flags

This section records the kernel's side of the pipe: how it hands the board
from one graphics process to another, which interrupt bits it services, and
what the HQ flag bits mean to the software that sets, clears and polls them.
It is a reading of the shipped IRIX 6.5.30 IP27 kernel's disassembly, checked
against one retained model trace. It establishes the software contract the
model must satisfy. It establishes nothing about silicon timing: where the
kernel waits for an interrupt, the board's own condition for raising it is
inferred from what the kernel did just before.

Evidence. Kernel: the installed IRIX 6.5.30 `unix.IP27` (SHA-256
`cbdb02ea5d334ecac949a6524a363e6c945f94baf438d41e8d9248eda0377bdf`,
12,111,512 bytes), read through `tools/origami disasm show` with names from
its own `.symtab`: `MgrasCtxsw` at `0xc0000000003f8b68` (312 instructions),
`MgrasGeneralInterrupt` at `0xc0000000003f9048`, `MgrasSchedSwapBuf` at
`0xc0000000003ff3d4`, `MgrasSchedSwapBufFinish` at `0xc0000000003ff5a8`,
`MgrasSchedSwapTask` at `0xc0000000003fecc4`, `MgrasPcxSwap` at
`0xc0000000003f5010`, `MgrasValidateBanks` at `0xc0000000003f4308`,
`MgrasValidateClip` at `0xc0000000003f4448`, `MgrasFlushHQ3` at
`0xc0000000003f0d4c`, `MgrasFlushHQ3Poll` at `0xc0000000003f0c90`,
`MgrasRetraceHandler` at `0xc0000000003fe4b8`, `MgrasEnableRetraceInterrupts`
at `0xc0000000003efb94`, `mgras_hq4_hiwater_intr` at `0xc0000000003f88e4`,
`MgrasInitialize` at `0xc0000000003f2994`, `MgrasWaitForRSSIdle` at
`0xc0000000004041c8`, `WAIT_FOR_RE_SYNC` at `0xc00000000040318c`, and the
kernel's copies of `__glMgrWaitForDMAWrite` at `0xc0000000004033e0` and
`__Mgr_ReturnMode` at `0xc00000000040424c`. Trace:
`wp7/glxioctl/relink-window.trimmed.trace` under the SI scratch tree, run
`irix-20260922T194153-d244b4`, QEMU `a11951efbd`, sequences 3743493 to
3744853, which hold three complete context switches and the client traffic
that follows them. Labels follow the
SI textureless OpenGL bring-up plan (`docs/plans/si-opengl-bringup.md`, not
yet on trunk): established, inferred, approximated, unknown.

### Interrupt block registers as the kernel uses them

| Offset | Kernel use | Label |
| --- | --- | --- |
| `0x011000` | Per-context control word. Saved by the switch, rewritten twice in its prelude (first with bits `0x88` cleared, then `(value & global_mask) \| 0x1000`, both `0x41005` in the trace), restored from the incoming context in the tail (`0x41085`). | Established as the sequence; bit meanings unknown |
| `0x011008` | Per-context word (`0x765b900f` in the trace). Saved in the prelude; written from the incoming context after the first handshake interrupt and before the 63 image words. | Established as the sequence; meaning unknown |
| `0x011020` | Interrupt status. The handler reads it and masks with `0x011040`. The switch tail writes `0x80` to it, reads it back and saves bit 5 into the outgoing context. | Established; the write's effect unknown |
| `0x011028` | Pending clear, write-1-to-clear: the handler writes exactly the bit it served (`0x10000`, `0x80000`, `0x10000000`, `0x401`, or the error bits), the switch prelude writes `0x80` and the tail `0x70`. | Established as software expectation |
| `0x011040` | Interrupt enable set, readable. A write sets the enable bits written and leaves the others; a read returns the enable word. `MgrasInitialize` writes `0x3008a200` after `MgrasInitHQ` has written all ones to `0x011048`; `MgrasSchedSwapBuf` adds `0x400` with a plain store before a swap; the switch saves the word in its prelude and, in its tail, writes all ones to `0x011048` and then the incoming context's copy. `MgrasGeneralInterrupt` reads it to filter `0x011020`. | Established for the kernel's writes, values and order. The set behavior is Inferred: with a whole-word write the enable word after a swap would be `0x400`, or 0 after the handler's clear, and the next `MgrasCtxsw` would wait at step 4 for source 19, which the handler filters out through this register; SGI's Octane IDE diagnostic names the register an enable set register and pairs it with `0x011048` (see [the IDE evidence](#evidence-from-sgis-octane-ide-diagnostic)). Discriminating qtests: `irq-enable-set-and-clear-ports`, `swap-keeps-context-switch-enable` |
| `0x011048` | Interrupt enable clear, write-1-to-clear on `0x011040`: `MgrasInitHQ` writes `0xffffffff` at initialization, the handler writes each error bit and the swap bit as it serves them, and the switch tail writes `0xffffffff` immediately before restoring `0x011040`. | Inferred: every kernel site fits the set and clear pair; the neighbouring `0x011038 ← 0xffffffff` then `0x011030 ← value` has the same shape in `MgrasInitHQ` and the switch tail; the IDE register test writes all ones here before reading `0x011040`. The trace reads back only `0x011040` |
| `0x011050` | Second-level enable. Bit 4 (`0x10`) is the FIFO low-water enable `MgrasFlushHQ3` sets and `mgras_hq4_hiwater_intr` sets; `MgrasFlushHQ3` clears bit 3 at the same time. The switch saves the word without bits `0x1f8`, and restores the incoming copy together with bits `0x1e8` of the outgoing value, so bit 4 is always zero after a switch. | Established as the sequence; bit 3 and bits `0x1e8` unknown |
| `0x011058` | Written `0xffffffef` in the switch prelude, and with the bits that changed in `0x011050` when `MgrasFlushHQ3` arms the low-water interrupt. | Inferred as the second-level pending clear; consistent with the [wait-tail note](#observed-software-wait-tail) |
| `0x011030`, `0x011038` | Saved and restored per context; `0x011038` is always written `0xffffffff` in the tail. | Established as the sequence; meaning unknown |
| `0x040008` | FIFO low-water threshold: `MgrasFlushHQ3` writes the wanted level plus one before sleeping and `0x14` when woken. | Established |
| `0x040088` | Written `1` in the switch tail. | Established; meaning unknown |
| `0x008000` | Polled `& 0x1eff == 0` before the selection write. | Established; meaning unknown |
| `0x130500` | Selection: the constant `0x4fc`, the only value in the kernel. | Established |

### The context-switch transaction

`MgrasCtxsw(board, incoming, outgoing)` runs entirely on one CPU under two
board semaphores and a 100-tick watchdog whose expiry is reported as a
context-switch timeout. Established, in order:

1. Prelude: `0x011028 <- 0x80`; save `0x011000`, `0x011008`, `0x011040`,
   `0x011030` and `0x011050 & ~0x1f8` into the outgoing context; if a board
   flag allows, `0x011000 <- value & ~0x88`; then
   `0x011000 <- (read & global_mask) | 0x1000`.
2. If the outgoing context is the board's built-in default context, poll
   `0x020200 & 0x200` until zero. Then `0x011058 <- 0xffffffef` and
   `MgrasFlushHQ3(board, base, 0, 0)`: poll `0x020008 & 0xff == 0` for up
   to 1 ms, else arm the low-water interrupt (`0x040008 <- 1`,
   `0x011050 |= 0x10`, `0x011058 <- changed bits`) and sleep on the board's
   low-water semaphore; a failure aborts the switch with an error message
   carrying `0x020008 & 0xff`.
3. Poll `0x008000 & 0x1eff` until zero, then `0x130500 <- 0x4fc`.
4. Sleep until `MgrasGeneralInterrupt` sees status bit 19 (`0x80000`), which
   it acknowledges at `0x011028` and answers by releasing the first
   handshake semaphore. The sleep retries every tick while the board is not
   marked failed.
5. `0x011008 <- incoming->word`, then exactly 63 32-bit words from the
   incoming context's image (loop bound `0x3f`) to `0x120400`, with no
   header, no length word and no trailer. The trace confirms 63 words per
   selection, three times.
6. Sleep until the handler sees status bit 28 (`0x10000000`), acknowledged
   the same way, releasing the second handshake semaphore.
7. Tail: `0x011048 <- 0xffffffff`; `0x040088 <- 1`; `0x011020 <- 0x80`;
   read `0x011020 & 0x20` into the outgoing context; `0x011028 <- 0x70`;
   `0x011000`, `0x011040`, `0x011050` (with the carried bits), `0x011038`
   (`0xffffffff`) and `0x011030` restored; then, only when the incoming
   context is the default context, `0x120400 <- 0x0000f400`.

Consequences for the model, established from the above:

- The transaction is closed by the second interrupt, not by a word. The
  kernel writes nothing after the 63rd word until the handler has run. The
  model must count 63 words after the first acknowledgement and raise bit 28;
  the `0x0000f400` requirement on trunk is wrong for every switch except one
  to the default context, and even there the word arrives after the tail as
  an ordinary class-0 command with no payload.
- `MgrasCtxsw` never reads `0x120400`. The model's readback of `0xf400` at
  that offset has no kernel reader in this routine.
- Between the selection and the first acknowledgement, and between the 63rd
  word and the second acknowledgement, the port carries nothing from this
  CPU. Other CPUs may still touch scheduler words (`0x040068`) and status
  (`0x020008`), as the trace shows interleaved.
- After the tail the next 4-byte writes to the port are command words from
  whichever process now owns the pipe. The
  client command grammar (citation: `docs/datasheets/mgras-gl-commands.md`) establishes that the 1,041
  words after the trace's third switch are the OpenGL client's own context
  setup, ending in a query whose completion the model now signals (see
  [HQ flags](#hq-flags-0x020010-set-and-read-0x020018-clear)). There
  is no kernel second stage.
- What the board does with the 63 words, whether it counts them or its
  program does, and why bit 28 follows the 63rd word, are unknown; the image
  is per-context state (the trace's first and third images differ at index
  1) and the model stores it without interpretation.

### General interrupt sources

`MgrasGeneralInterrupt` reads `0x011020 & 0x011040` and serves one source
per invocation in this order, returning after each. Established:

| Bit | Action |
| --- | --- |
| 16 (`0x10000`) | release the board semaphore at `+0x7c0`; `0x011028 <- 0x10000`. No routine read here enables or waits on it |
| 28 (`0x10000000`) | release the second context-switch semaphore; `0x011028 <- 0x10000000` |
| 10 (`0x400`) | `0x011048 <- 0x400`; `0x011028 <- 0x401`; `MgrasSchedSwapBufFinish` |
| 29 (`0x20000000`) | report a flow-control or command-FIFO overflow error; `0x011048 <- 0x20000000` |
| 9 (`0x200`) | report an HQ program error; `0x011048 <- 0x200` |
| 13 (`0x2000`) | report a DMA address-range error; `0x011048 <- 0x2000` |
| 14 (`0x4000`) | report a command-FIFO privilege violation; `0x011048 <- 0x4000` |
| 15 (`0x8000`) | report a DMA access or stack overflow; `0x011048 <- 0x8000`; mark the board failed through `mgras_error` |
| 19 (`0x80000`) | release the first context-switch semaphore; `0x011028 <- 0x80000` |
| none | with a debug flag set, report a spurious general interrupt |

The default enable `0x3008a200` names bits 9, 13, 15, 19, 28 and 29. Bit 10
is enabled only around a swap. Bits 9, 13, 15 and 29 are error reports: a
model that never raises them is not hiding a completion, and raising one
disables that source until the next context switch restores the enable
word. Because each invocation serves one source and returns, the line must
stay asserted while any enabled source remains pending (inferred; the
alternative, a pulse per event, would lose the second of two simultaneous
sources). The retrace source is separate and is described in
[retrace interrupt](#retrace-interrupt). The low-water source has its own
second-level enable. Its service routine is `mgras_hq4_lowater_intr`
(see [retrace interrupt](#retrace-interrupt)); this reading did not follow how it releases the semaphore at
`+0x758` that `MgrasFlushHQ3` sleeps on.

### Retrace interrupt

Evidence: the IRIX 6.5.30 kernel installed as `/unix` on the SI root disk
(SHA-256 `99040083dd5e6073d2b4a951ac72be130a1dd3dc624d9c9e6477f9b181c96c3f`,
11,101,168 bytes, a different link of the same release than the
`unix.IP27` cited above), read with `llvm-objdump` and `tools/origami
disasm show`. Routine names come from its `.symtab`, except
`rrmValidateMyRetrace`, which only its `.debug_funcnames` names.
Corroboration for the bit layout only: the VC2, the video controller of
the Newport (Indy and Indigo2 XL) graphics, is a sibling chip whose
control register sits at index `0x10`, not `0x1e`. Linux
`include/video/newport.h` (Linux `db2ddb87143519e20a95aa36c60b36107b736a58`)
gives its bit 0 (`VC2_CTRL_EVIRQ`) as the vertical interrupt enable and
bit 2 (`VC2_CTRL_EVIDEO`) as the video timing enable, the same positions
as the display-enable bit the model already takes from VC3 register `0x1e`
bit 2. That is an analogy between chips, which is why the VC3 bit's
gating role below stays inferred.

- `mgras_attach` allocates four crosstalk interrupts and connects, in
  order, `MgrasGeneralInterrupt`, `mgras_hq4_lowater_intr`,
  `MgrasRetraceHandler` and `MgrasHQ4ErrInterrupt`. It packs the first
  three vectors into `0x011070`: low-water in bits 7:0, general in bits
  15:8, retrace in bits 23:16. Runs `irix-20260927T025957-1f8a52` and
  `irix-20260927T035120-7dd191` both write `0x110f10`, so retrace is
  vector 17. Established.
- `MgrasEnableRetraceInterrupts` waits for display-bus room
  (`0x020100 & 0x1f`), sets bit 0 in its copy of VC3 register `0x1e`, and
  writes that register through `0x072038`. Both runs show `0x1e001e00` at
  boot and `0x1e001f00` once the desktop runs.
  Established as the kernel's enable. That the bit gates the board's
  retrace interrupt is inferred from this and the VC2 sibling.
- The kernel never sets bit 0 of `0x011040` (it writes `0x3008a200`), and
  `MgrasRetraceHandler` writes no acknowledge to the board: it reads
  status, reprograms the XMAP buffer selection and wakes waiters. The
  only latch the kernel clears is the Hub's pending bit. Inferred: the
  board sends one interrupt message per retrace while the VC3 bit is set.
- `MgrasRetraceHandler` calls `rrmValidateRetrace` for every rendering
  resource queued for a retrace, and `rrmValidateVideoSync` when a video
  sync waiter is queued. The kernel's graphics fault path
  (`RRM_ValidFault`, through `rrmValidateMyRetrace`) sleeps on the
  resource's retrace wait queue until that runs. Established.

Consequence observed on the model: before the retrace was delivered,
Xsgi slept there for good after a desktop program opened a window
(wait channel `0x14e0dc0`, the retrace queue of the second entry of
`RRMrn`), and no client could connect. Run `irix-20260927T025957-1f8a52`
reproduced it; run `irix-20260927T035120-7dd191` with the retrace
delivered did not. What would settle the inference: a physical SI with
the VC3 bit clear and set, observing Hub pending bit 17 per frame.

### HQ flags (`0x020010` set and read, `0x020018` clear)

A write to `0x020010` sets bits, a write to `0x020018` clears them, a read of
`0x020010` returns them; every software site fits that, and the model already
implements it. Established as software expectation. The bits software uses:

| Bit | Set by | Cleared by | Waited on by | Label |
| --- | --- | --- | --- | --- |
| 16 (`0x10000`) | the board, after the `0x000e0400` fence token (kernel and client copies of `__glMgrWaitForDMAWrite` write the token then poll for the bit) | software before a DMA kick (`__glMgr_WriteDMAHQSetup` writes `0x10000` to `0x020018`) | `__glMgrWaitForDMAWrite`, `MgrasPcxSwap`, `MgrasSchedSwapBuf` reads | Established as software expectation; the model sets it synchronously on the token (approximated) |
| 17 (`0x20000`) | the board, when a class-0 opcode `0xa2` query has its answer at `0x028004` | software immediately before the query (`__Mgr_ReturnMode`, both copies) | `__Mgr_ReturnMode` polls it with no bound | Established as software expectation from both copies and the trace tail. The model sets it once the query's three argument words arrive and answers zero at `0x028004`; both the timing and the value are approximations. Run `irix-20260927T135825-92207b` showed that a zero answer does not block context creation. The true answers are unknown |
| 20 (`0x100000`) | software only: `MgrasValidateBanks` and `MgrasValidateClip` after their register tokens, the client's `glFlush` after its `0x3600` token | `MgrasPcxSwap`, after testing it, before it drains the FIFO | nothing polls it | Inferred: a validation-pending marker between the client and the kernel's swap path; no board writer observed |
| 10 and 0 (`0x401`) | unknown | `MgrasSchedSwapBuf` clears the matching interrupt pending bits (`0x011028 <- 0x401`) when it finds either set before scheduling a swap | none | Inferred: the flag bits mirror interrupt sources 10 and 0 |
| others | unknown | unknown | unknown | unknown |

### Swap scheduling

Established, from `MgrasSchedSwapBuf` and its finish path:

1. Under the swap mutex, `MgrasSchedSwapTaskCheckReturnValue` decides
   whether the window's swap is queued for the retrace path; if it is, the
   routine returns without touching the board.
2. Otherwise `MgrasValidateBanks` (one 64-bit token `0x00009808` with two
   payload words and the flag-20 set), then if `0x020010 & 0x401` is set,
   `0x011028 <- 0x401`.
3. Take the board's swap semaphore, arm a 100-tick watchdog (reported as a
   swap finish timeout), enable source 10 (`0x011040 <- 0x400`, which sets
   bit 10 and leaves the others), then one 32-bit word
   `0x00003700` to `0x120400`: class 0, opcode `0x37`, no payload. Mark the
   client-visible swap-pending bit and return.
4. Interrupt bit 10 arrives; the handler disables it, acknowledges `0x401`
   and calls `MgrasSchedSwapBufFinish`, which runs `MgrasSchedSwapTask`: wait
   until `0x020100 & 0x1f` shows display-bus room, then write XMAP index
   `0x071c88 <- 0` and data `0x071d80 <- per-window swap mask` (windows 32
   and up use a second mask word under index 4, a reading SGI's diagnostic
   contradicts: see
   [the IDE evidence](mgras-raster-display.md#evidence-from-sgis-octane-ide-diagnostic)),
   cancel the watchdog and release the semaphore.
5. `MgrasRetraceHandler`, on the VC3 retrace interrupt, re-programs the XMAP
   (`0x071c88`, `0x071d80`, `0x071e00`, `0x071e80`) and validates the
   swapped buffers through the window-manager callbacks.

Inferred: the `0x3700` word makes the board raise bit 10 once the queue
ahead of it has drained, so the flip is ordered after the rendering; the
XMAP buffer selection latches at the retrace, which the raster/display
contract owns. The model raising bit 10 when it consumes the word, with the
queue empty, satisfies the kernel.

### Other status bits the kernel waits on

Established as software expectations: `MgrasWaitForRSSIdle` and the
client's `glFinish` wait until `0x020000` bit 1 is set (3 s and unbounded
respectively); `WAIT_FOR_RE_SYNC` waits until bit 0 is set;
`mgras_hq4_hiwater_intr` tests bit 4 and arms the low-water interrupt when it
stays clear; `MgrasFlushHQ3Poll` waits for `0x020008 & 0xff` to fall to the
requested level, or for `0x020200 & 0x17` to clear when asked to wait for
DMA; the client's `__glMgrWaitForDMARead` waits for `0x020200 & 0x1eff` to
clear. The model answers `0x020000` with `0x12` and the others with zero,
which satisfies every wait above. The flag-17 query is answered as the
[HQ flags](#hq-flags-0x020010-set-and-read-0x020018-clear) table describes.

### What would settle the unknowns

- Whether bit 19 is raised by the selection alone and bit 28 by the 63rd
  word alone: a kernel-bracketed capture cannot separate them from the
  model's own answers; only a physical HQ4 can. The model's rule stands as
  an approximation of the kernel's expectation.
- The `0x028004` answer for each query index: the model experiment (set
  bit 17 on the query, answer zero) ran in `irix-20260927T135825-92207b`.
  Contexts were created, made current, drawn into and torn down with every
  answer zero, so creation does not depend on the value. The values
  themselves need hardware.
- Bit 16's waiter and enabler, how `mgras_hq4_lowater_intr` (the low-water
  service routine, see [retrace interrupt](#retrace-interrupt)) releases
  `MgrasFlushHQ3`'s semaphore, the `0x401` flag mirror and bit 20's
  board-side meaning: further kernel reading, then the step-0 capture of a
  relinked `clock` run with kernel brackets.
- Whether VC3 register `0x1e` bit 0 gates a board retrace message sent
  once per frame: a physical SI with the bit clear and set, observing Hub
  pending bit 17 per frame.

## Evidence from SGI's Octane IDE diagnostic

SGI's IRIX 6.5.30 Octane standalone diagnostic, `ide.IP30` (SHA-256
`19d7a194cb417330475df9a187379404d413c72433cfc97fc9e238f0fe170d69`), adds the
following software expectations. Addresses are listing addresses in that
object; its listing and names are under `docs/analysis/ide.IP30-stand/` and
`docs/library/ide.IP30-stand.symbols.txt`.

- **Widget identity.** The HQ4 probe (`0xa80000002087ac88`) passes only on
  manufacturer `0x2aa` and part `0xc003`, and warns that a widget revision
  below 2 is a prototype. The production revision is not known; the model
  answers 0.
- **Link reset.** The diagnostic's link reset (`0xa80000002087aed4`) writes 1
  to the Crossbow link-reset register of the board's port and touches nothing
  on the widget. Link liveness is read from the Crossbow link status (widget
  present, bit 5) and auxiliary status (alive, bit 31). Both belong to the
  Crossbow contract.
- **Set and clear pairs.** The HQ4 register test (`0xa800000020886740`)
  names `0x011020` FLAG_SET_PRIV, `0x011030` FLAG_ENAB_SET, `0x011040`
  FLAG_INTR_ENAB_SET and `0x011050` STAT_INTR_ENAB_SET, and before reading
  each writes all ones to the register 8 bytes above it. The strings name
  only the set registers. That access pattern suggests `0x011058` is the
  clear port of the second-level enable rather than a pending clear as
  inferred above, and `0x011038` the clear port of `0x011030`, and
  `0x011048` the clear port of `0x011040`, which the kernel's use of the pair
  agrees with (see the interrupt block table); this is
  inferred from the pattern of a test whose check is compiled out, not
  named. It also
  names widget `0x000028` the request timeout (tested mask `0xfffff`),
  `0x000030`/`0x000038` the interrupt destination high and low words, and
  `0x040078` the flow-control timeout (mask `0x3ff`). The test's check routine
  only prints, so it establishes names and pairings, not reset values.
- **RSS registers through `0x130400`.** The diagnostic writes every raster
  register through the secondary FIFO (`0xa800000020824e3c`): a header
  `((reg | 0x1800) & 0x1fff) << 8 | 0x00400004` and one data word; the execute
  form adds `0x400` to the register. In the 16:8:8 framing that is class
  `0x58`/`0x59` (state) or `0x5c`/`0x5d` (execute), the main port's RSS
  classes with bit 22 set, as the kernel's DMA-mapping class `0x48` is the
  main port's descriptor class `0x08` with bit 22 set. So the port is not
  DMA-mapping only. What bit 22 selects is unknown. The model discards these
  tokens as unimplemented.
- **FIFO and DMA completion.** Initialisation writes `0x060008` = 2 and FIFO
  thresholds `0x040000` = `0x28`, `0x040008` = `0x14`, `0x040010` = `0xa`,
  `0x040020` = `0x28`, `0x040028` = `0x14`, `0x040030` = `0xa`
  (`0xa80000002083f6bc`). DMA completion (`0xa800000020828f68`) polls
  `0x020200 & 0x1f` to zero, then `0x020000` bit 1 (RSS idle) to set, which
  the model's idle answers satisfy.
- **Bring-up.** The shared bring-up path (`0xa8000000208a9750`) writes
  `0x011000` = `0x108d` and `0x011008` = `0x765b900f` as the kernel does, waits
  for FIFO level below `0x70`, writes the 64-bit word `0x65781` to `0x120400`
  and reads the RE4 revision from `0x02c578` bits 7:4.

## Acceptance coverage

| Capability | Required contract test | Status |
| --- | --- | --- |
| Context transaction | selection owns the next 63 main-port words after the first acknowledgement, raises the second, needs no closing word, and returns the port to command parsing; a later switch while ready is not fed to the token parser | missing |
| HQ flags | fence sets bit 16, the `0xa2` query sets bit 17 with an answer at `0x028004`, software set and clear ports, bit 20 untouched by the board | query: `hq-query-sets-flag-after-arguments`, `hq-query-flag-only-from-query`; the rest missing |
| Swap command | `0x3700` raises interrupt bit 10 after the queue drains; the handler's clear and acknowledge sequence | covered (`swap-command-raises-interrupt-10`, `swap-command-needs-enable`) |
| Identity and aperture | reset, status, unsupported access | partial |
| FIFO widths and ordering | controls, 64-bit words, partial writes, full queue | partial |
| Bounded raster submission | known state writes and executes | partial |
| Scheduler write and FIFO wait | `0x040068` write, `0x020008 == 0` poll, and separate low-water arm/disarm sequence | missing |
| Fences | acceptance, raster, DMA, and empty are distinct | missing |
| DMA image-transfer and source-mapping | slot registers, list-entry offset, kick-byte slot/direction, aligned source offset, host-to-screen paint, screen-to-host read-back | covered (`tests/qtest/sgi-mgras-pixels-test.c`); remote-node case missing |
| Interrupts | latch, mask, acknowledge, route, reset | retrace: one message per retrace while VC3 `0x1e` bit 0 is set, re-raised after the Hub clear (`retrace-interrupt-vector`, `retrace-interrupt-needs-vc3-enable`); the rest missing |
| Error handling | malformed command, overflow, underflow, DMA fault | missing |
| Migration | queue, decoder, DMA, interrupt, program state | missing |

The complete command language, uploaded program instruction set, direct-XIO
interrupt topology, DMA grammar, texture variants, multiple-raster variants,
and alternate attachment behavior remain separate work. The selected IO3
attachment may land only after independent evidence review. Exact-head review
must confirm that unknown revisions remain unknown, other functional choices
are identified as approximations, and the attachment qtests pass.
