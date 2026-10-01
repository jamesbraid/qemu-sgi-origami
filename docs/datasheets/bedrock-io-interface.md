# Datasheet: Bedrock I/O interface (II)

References rendered as citation identities name material outside this document
set. Those targets are not included or cleared for redistribution. Review and
runtime statements retain their recorded dates and scope.

**Status:** reviewed 2026-09-03 by an independent second reader
(`docs/reviews/bedrock-io-md-xb-datasheet-review.md`): all 135 registers cleared position for position
against the SN1 header and the three consumers, with the review's seven findings
applied in this revision. The open items stay open. An initial standalone unit is
saved in fork commit `318cf831db`, with nine passing I/O qtest subtests.
Commit `5e2ae23fd1` composes it in the Bedrock parent with explicit DMA and
widget links. Its combined Bedrock/L1 run passed 50 subtests, including three
parent tests. A forced-global-DMA mutation fails the isolation check. These
results do not qualify the full machine or all register side effects.
**Fork file(s):** `hw/misc/sgi-bedrock-io.c`,
`include/hw/misc/sgi-bedrock-io.h`, and `tests/qtest/sn1-bedrock-io-test.c`.
**Evidence cutoff:** 2026-09-03, Origami trunk `506836b9dd`, except the
[BTE timing approximation](#bte-timing-approximation) (2026-09-26), which also
reads the IP35 PROM 6.210 disassembly and traced Origin 300 boots, as recorded
in `docs/provenance.md`.

**Sources used:** the SGI-generated Linux SN1 register headers at tag `v2.5.40`
(commit `7570df54ef8c`): `include/asm-ia64/sn/sn1/hubio.h` (135 registers),
`hubio_next.h`, `sn/io.h`, `sn/bte.h`, `sn/bte_copy.h`, `sn/xtalk/xtalkaddrs.h`,
`sn/xtalk/xwidget.h`, `sn/pci/bridge.h`; the Itanium consumers
`arch/ia64/sn/io/{io.c,sgi_io_init.c,ml_iograph.c,ml_SN_init.c}`,
`arch/ia64/sn/io/sn1/{huberror.c,hubcounters.c,ip37.c,pcibr.c}`,
`arch/ia64/sn/kernel/bte.c`, `arch/ia64/sn/kernel/sn1/error.c`. Linux mainline
`arch/mips/include/asm/xtalk/xwidget.h` for the Bedrock's crosstalk identity. OpenBSD at `2df91d80c1`:
`sys/arch/sgi/xbow/{hub.h,xbow.c,xbridge.c,xbridgereg.h,xbowdevs,widget.h}`,
`sys/arch/sgi/sgi/ip27_machdep.c`. The out-of-tree MIPS port `refs/linux-ip35` at
`9f613a321ea0`: `arch/mips/sgi-ip35/ip35-xtalk.c`, `arch/mips/pci/pci-sgipic.c`,
`include/asm/mach-ip35/{dma-coherence.h,mangle-port.h}`, `include/asm/pci/bridge.h`
for the 64-bit DMA attribute bits. The Origin 3000 System
Architecture 108-0240-002 printed page 2-16 for the unit's role; the Origin 300 sMDK
field diagnostics 108-0264-004 printed pages 2-31 and 2-35 for the vocabulary of the
vendor's own tests. Reading preserved in the SN1 research notebook.

**Facts that are IRIX-derived: none.** **Reset values the header states:** the local
access protection register resets to permit every region; on reset the request
buffer entries have only their valid, priority-count, timeout, timeout-valid and
context-valid fields initialised and software must clear the rest. Everything else
is `unknown`.

## Isolated implementation coverage

The unit provides register, processor PIO and source-qualified inbound apertures.
Sixteen optional `widget-0` through `widget-15` MemoryRegion links select distinct
outbound AddressSpaces. Missing endpoints return a decode error; they do not
fall back to guest DMA memory. Inbound and BTE access use the separately attached
DMA AddressSpace. Property validation bounds region, revision, widget and device
fields before their use in shifts or decode.

Both BTEs copy and zero-fill real memory, one 128-byte line at a time, and
finish the whole transfer inside the write that starts it, with notification,
completion interrupts, data errors, reset and migration of programmed state.
Finishing inside the starting write is an approximation; the
[BTE timing approximation](#bte-timing-approximation) states its limits and
evidence. Tests distinguish widget targets at equal offsets and reject fake
copy completion.

Poison effects, self-port loopback and remote/N-mode routing remain
unsupported. Register retention alone does not implement every
error, timeout, request-buffer or interrupt side effect described in this sheet.
Those gaps remain required for full hardware and diagnostic qualification.

## Role

The I/O interface is the Bedrock's crosstalk port: the unit an XBridge or Crossbow
talks to, and the unit the processors' programmed I/O to widgets leaves through. Per
the Origin 3000 architecture book it translates a device's DMA request into the
crossbar's format and hands it to the internal crossbar, which forwards it to memory.
It also contains the two block transfer engines.

The inspected public MIPS consumers use three identified surfaces: the seven translation entries that turn a big window into a crosstalk address,
which OpenBSD reads and writes; the link status bit that tells the out-of-tree Linux
port the crosstalk link is up; and the widget-control register's low four bits, which
that port reads as the Bedrock's own crossbar port number. The inspected public
MIPS paths do not exercise the block transfer engines. That source coverage does
not bound stock PROM, IRIX 6.5.30 or SGI diagnostic behavior.

## What each public consumer touches

| Register | Offset | OpenBSD | out-of-tree Linux | Itanium port |
|---|---|---|---|---|
| translation entries 0 to 5 | `0x400160` to `0x400188` | read to find a free entry, write, read back (`ip27_machdep.c` lines 533 to 572) | — | write the disabled form at init, then write and read back on demand (`io/io.c` lines 104 to 111, 252 to 254) |
| link control and status | `0x400128` | — | bit 13 must be set or the node's crosstalk is skipped (`ip35-xtalk.c` lines 159 to 163) | same test; clears the enable bit when the link is down (`huberror.c` lines 161 to 171) |
| widget control | `0x400020` | — | bits 3:0 read as the port to skip when walking the crossbar (`ip35-xtalk.c` lines 165 to 179) | bits 3:0 stored per node; bit 17 decides the widget 0 credit count (`ip37.c` lines 24 to 31; `io/io.c` lines 783 to 796) |
| request buffers, error clear, interrupt destination, buffer management, timeouts | `0x400198` to `0x4003c0` | — | — | probe-time sequence in the behavioral contract |
| outbound and inbound widget access, device error mask | `0x400110` to `0x400120` | — | — | outbound access saved, zeroed and restored around request-buffer programming; the others only in a diagnostic dump |
| link log | `0x400130` | — | — | statistics tick reads and zeroes it |
| block transfer engines | `0x410000`, `0x420000` | — | — | descriptors set up per node; no caller of the copy routine at this tag |

Whether a consumer ever reads the Bedrock's own crosstalk identity through the
crossbar depends on the configuration graph. The Itanium walk skips ports whose
graph flag is not I/O-enabled (`ml_iograph.c` line 190) and the out-of-tree port
skips the parent port by number (`ip35-xtalk.c` lines 75 to 79), but OpenBSD walks a
fixed order, `0xf` then 8 to `0xe`, skipping only ports whose graph flag lacks the
enable bit and never excluding the hub master link it records (`xbow.c` lines 246 to
247 and 344 to 353; `sginode.c` line 338). If the PROM's crossbar record flags the
Bedrock's own port enabled, OpenBSD reads that port's identification register
through the crossbar and finds "Bedrock" in its table. The model therefore needs a
defined answer for a PIO aimed at the Bedrock's own crossbar port, which is open: the
XBridge datasheet does not say whether the crossbar routes a port's traffic back to
itself. Settle with the held PROM's crossbar record port flags (the configuration-graph packet owns
the graph) or an Origin 300 console log showing what OpenBSD attached at the hub's port.
No file uses the Bedrock part-number constant the SN1 tree defines.

## Register map

Offsets are hub-relative; the unit starts at `0x400000`. Every register is 64 bits.
Line numbers are `hubio.h` unless named.

| Offset | Name | Fields | Reset | Access | Consumer | Behavior |
|---|---|---|---|---|---|---|
| `0x400000` | widget identification | 0 reserved; 11:1 manufacturer; 27:12 part; 31:28 revision; 63:32 reserved | unknown; derived from the local block's revision register | read | none | Echoes the manufacturer and revision fields of the local block's revision register; the part field is the crosstalk device ID assigned to the Bedrock (lines 26 to 33, 766 to 775). The SN1 tree expects part `0xc110` (`hubio_next.h` line 403); OpenBSD's device table names `0xc110` from manufacturer `0x036` "Bedrock" and `0xc101` "Hub" (`xbowdevs` lines 79 to 80). Also visible from crosstalk at widget offset 0. |
| `0x400008` | widget status | 3:0 pending requests; 4 credit timeout; 5 tail timeout; 9 transmit max-retry; 23:16 transmit retry count; 32 "crazy"; rest reserved | unknown | read; bit 32 cleared through error clear bit 16 | none | Set on error detection (lines 813 to 815). |
| `0x400020` | widget control | 3:0 own widget number; 4 tag mode; 15:13 destination credits; 16 force bad packet; 17 direct connect; 22:18 elasticity threshold | unknown | R/W | see the table above; **no consumer writes it** | Crosstalk flow control (lines 865 to 903). Bit 17 set means a widget is directly connected with no crossbar; the Itanium port then assumes a Bridge at widget 0 with three credits, else a Crossbow with one. |
| `0x400100` | local access protection | 63:0 one enable per region | **all ones** (line 933) | R/W, readable only by permitted regions or through the local alias | none | The II performs a processor PIO only if the requester's region bit is set, else returns an error response; for a widget PIO the outbound access bit must also be set (lines 912 to 933). |
| `0x400108` | protection override | header union gives 8:0; description says a 64-bit key | unknown | write | none | Writing the 64-bit ASCII key the SN1 tree names (`hubio_next.h` line 56) sets the writer's own region bit; a write through the local alias does nothing (lines 952 to 959). |
| `0x400110` | outbound widget access | 0 widget 0; 15:8 widgets 8 to F | unknown; the model resets it to all ones, its choice | R/W | Itanium `io/io.c` lines 780 to 807 | Qualifies every PIO and graphics write the Bedrock launches toward a widget (lines 993 to 994). |
| `0x400118` | inbound widget access | same layout | unknown; the model resets it to all ones, its choice | R/W | Itanium diagnostic dump only | Qualifies every request a widget launches at the Bedrock, including DMA and interrupt writes (lines 1032 to 1034); the device mask's text adds that incoming replies are exempt (lines 1084 to 1086). |
| `0x400120` | inbound device error mask | 8 bits per widget, widget 8 at 7:0, one per DMA master 0 to 7 | unknown; the model resets it to all ones, its choice | read; write-one-to-set | Itanium diagnostic dump only | Cleared by hardware when an error requires the device to be shut out; access = device bit and the widget's inbound bit (lines 1073 to 1086). |
| `0x400128` | link control and status | 5:0 null timeout; 8 warm reset; 10 link enable; 11 8-bit mode; 13:12 link status, **bit 13 = up**; 14 remote power; 25:16 max retry; 27:26 data-available select; 41:32 max burst | unknown | R/W | both Linux ports read bit 13 | Link-level protocol control (lines 1132 to 1183). |
| `0x400130` | link log | 15:0 sequence-number errors; 31:16 check-bit errors | unknown | read; the consumer writes 0 to clear (`hubcounters.c` lines 179 and 216), which the header does not describe | `hubcounters.c` lines 178 to 185 | Error-rate monitor (lines 1192 to 1193); `0xffff` is taken as saturation. |
| `0x400138` | interrupt destination | 6:0 level; 8 PI select; 16:9 node; 24 enable; 28 sent; 32 and 33 forwarded-interrupt pending for PI0 and PI1 | unknown | R/W | Itanium `huberror.c` lines 86 to 87, 186 to 191 | Where every II-detected non-BTE error interrupt goes. **The PI register address is hard-coded**: PI select 0 sends the interrupt packet to offset `0x0180_0090` in PI0's register space on the named node, PI select 1 to `0x01a0_0090` in PI1's (lines 1229 to 1237). |
| `0x400140`, `0x400148` | graphics node-widget map 0, 1 | 3:0 widget; 4 PI; 12:5 node; 16 processor slot | unknown | R/W | IRIX 6.5.30 graphics flow control (disassembly, see the [PI datasheet](bedrock-processor-interface.md#return-routing-the-graphics-node-widget-map)) | Routes a graphics widget's write responses, converted to credits, to the credit counter of the named node, interface and slot (layout established, `hubio_next.h` lines 93 to 109; behavior established, `hubio.h` lines 1290 to 1296). No valid bit. What happens to a response no entry names is unknown. Clarified 2026-09-28: the row previously named no consumer and said only that it routes write responses back. |
| `0x400150`, `0x400158` | scratch 0, 1 | 63:0 | unknown | R/W | the IP35 PROM 6.5.30 listing, confirmed by a live register trace: right after its reset-edge rendezvous, each processor interface's lowest processor sets bits 4 to 7 of scratch 0 for that interface's fitted processors (A sets `0x10` and `0x20`, C sets `0x40` and `0x80`) under the hub lock, and every later barrier reads them as which processors are present; scratch 1 carries the four-phase handshake between the two interfaces' lowest processors (bits 0 and 1) around every barrier | Plain storage. A write to scratch 0 by a node processor also reports to the owning node's bounded join (`sgi_bedrock_presence_published()`, `hw/misc/sgi-bedrock.c`), which parks each interface's publisher, at its next hub-lock release, until the other interface has published. The park halts the processor rather than blocking a host thread, so it applies under multi-threaded TCG, round-robin and `-icount` alike, and its two-second bound runs on the virtual clock; no value changes. See known limitation: the presence-publication join (citation: `docs/known-issues.md#the-presence-publication-join-approximates-same-edge-reset-at-the-first-barrier`). |
| `0x400160` + 8n, n 0 to 6 | translation entries | 4:0 offset; 11:8 widget; 12 I/O space | unknown | R/W; consumers read back after writing | OpenBSD and the Itanium port | In M mode processor address bits 31:29 select the entry and the five offset bits are prepended to bits 28:0 to form crosstalk address bits 33:0, with 47:34 zero; in N mode bits 30:28 select, offset plus 27:0 form 33:0, and the I/O-space bit becomes crosstalk bit 47 (lines 1425 to 1446). Software numbers them 0 to 6. |
| `0x400198`; `0x4001a0` + 8(w−8), w 8 to F | PIO request buffers, widget 0 and widgets 8 to F | 7:0 credits; 21:8 and 37:24 NACK counters A and B; 41:40 mode; 42 fire-and-forget; 47:43 overflow count; 48 error; 49 read timeout; 50 spurious write; 51 spurious read; 63 multiple errors | unknown | R/W | Itanium `huberror.c` lines 63 to 83; `io/io.c` lines 706 to 746 | Per-widget PIO flow control. Writing the credit field re-arms the spurious-write detector, which persists until the register is written again even if error clear is used (lines 1851 to 1869, 1927 to 1945). |
| `0x4001e0` | credit-count timeout | 25:0 threshold | unknown | R/W | none | Counts 128-clock ticks while outbound credits are below one (direct) or two (crossbar); saturates and raises one interrupt (lines 2535 to 2546). |
| `0x4001e8` | miscellaneous error mask | 0 widget 0 enable; 4 and 8 BTE 0 and 1 enable; 12 clear precise | `0x111`, inferred (see [the behavioral contract](#behavioral-contract)) | write-one-to-set | the later SHub's `bte_error.c` (recovery write) and `shuberror.c` (error read); the vendor's System Level Test (diagnostic check) | Qualifies PIO and DMA from widget 0 and from both engines; the hub clears a bit to shut that source down after an error (lines 2579 to 2589). |
| `0x4001f0` | tail and read-response timeouts | 25:0 tail timeout; 54:32 read-response prescaler; 59:55 read-response timeout | unknown | R/W | none | Lines 2635 to 2653. |
| `0x4001f8` | error clear | 0 request buffer 0; 15:8 request buffers 8 to F; 16 crazy; 17 and 18 BTE 0 and 1; 28 internal; 29 spurious read header; 30 and 31 the two forwarded-interrupt bits | unknown | write-one-to-clear | Itanium writes all ones at probe (`huberror.c` line 85) | Clears the named bits elsewhere in the unit (lines 2693 to 2698). |
| `0x400200` | BTE control | 3:0 count of request buffers both engines may use; 8 soft reset | unknown | R/W | Itanium writes count 8 at probe | Lines 2767 to 2770. |
| `0x400208`, `0x400210`, `0x400218`, `0x400220`, `0x400228`, `0x400300` | spurious message, spurious sideband, link channel test, incoming error packet header 1 and 2, request-buffer counter adjust (lines 252 to 289) | per header | unknown | — | none | Not inventoried; retained-write behavior only. |
| `0x400308` + 8n, n 0 to 7 | PIO read address table | 63 valid; 62:58 timeout count; 57:54 widget; 51:44 source; 43:41 initiator; 40:3 address | unknown | — | none | The outstanding-read table the read-response timeout works on (`hubio_next.h` lines 593 to 605). |
| `0x400388`, `0x400390`, `0x400398`, `0x4003a0` | deallocation, FIFO depth, arbitration, alias-port registers | per header | unknown | — | none | Not inventoried. |
| `0x4003a8` | request buffer management | 0 spurious message; 1 read header; 7:4 count; 12 and 13 clear pending; 19:16 flow-control count; 34:20 valid bits; 49:35 marks; 52 precise | unknown | R/W | Itanium writes count `0xf`, rest 0, at probe (`sgi_io_init.c` lines 90 to 92) | Writing while the buffers are not quiescent "can have catastrophic side effects" (lines 3728 to 3758). |
| `0x4003b0` | request buffer control | 3:0 entry; 15:8 command (nop, wake, timeout, eject, flush); 16 pending | unknown | R/W | none | Software control with priority over crosstalk and BTE requests (lines 3789 to 3791). |
| `0x4003b8`, `0x4003c0` | timeout, prescaler | 7:0 timeout; 23:0 prescale | unknown | R/W | Itanium writes `0xff` and `0x1000` at probe | Every valid entry's timer advances when the prescaler counter wraps (lines 3831, 3865 to 3870). |
| `0x400400` to `0x4005d8` | request buffer entries, 15 of 4 words | per header | partial (see the reset statement) | read safe; write only when quiesced | none | Lines 3915 to 3918. |
| `0x400600`, `0x400608`, `0x400610` | request-buffer spurious message low and high, debug submenu select (lines 643 to 660) | per header | unknown | — | none | Not inventoried. |
| `0x410000`, `0x420000` | block transfer engine 0, 1 | six registers each, listed below | unknown | see below | Itanium only, which stores their base addresses per node and never starts a transfer | |
| `0x430000`, `0x430008` | performance control, counter | per header | unknown | — | none | Not inventoried. |

The six registers marked "per header" with no consumer, and the fifteen request-buffer
entries, are retained-write storage in the model.

### Block transfer engine registers

Each engine has the same six registers at its base. The header's protocol: a
legitimate write to length-status sets busy, clears error and loads the length in
cache lines; the hardware decrements it per line and clears busy at the end. Source
and destination are 40-bit physical addresses with the low seven bits zero (128-byte
lines). The notification address receives the completion write-invalidate. **Writing
the control register starts the transfer; reading it terminates any transfer in
progress**, and stopping one engine can disturb the other (lines 4344 to 4353,
4393 to 4397, 4473 to 4479, 4521 to 4522, 4558 to 4560).

| Offset from base | Name | Fields | Consumer |
|---|---|---|---|
| `+0x00` | length and status | 15:0 length in lines; 16 error; 20 busy | written busy plus length, read back as a fallback (`bte_copy.h` lines 185, 254) |
| `+0x08` | source | 39:7 address | `bte_copy.h` line 193 |
| `+0x10` | destination | 39:7 address | line 195 |
| `+0x18` | control and terminate | 0 zero-fill; 4 notify; 8 poison | written notify only (line 213) |
| `+0x20` | notification address | 39:7 address | line 201 |
| `+0x28` | interrupt address | 0 PI select; 8:1 node; 22:16 level | none |

The Itanium consumer sets the notification line to all ones, writes length-status,
source, destination and notification address, then control with notify, and spins
until the notification line's busy bit clears; it therefore expects the completion
write to deposit the final length-status value into that line. It also copies
length-status into the line itself when the expected time passes, a workaround for
notification failures its comment says were seen on real SN0 and SN1 hardware
(`bte_copy.h` lines 174 to 264). No caller of the copy routine exists at this tag, so
the engine is initialised and never used.

The BTE error and completion review (citation: `docs/reviews/bedrock-bte-error-completion-review.md`)
cleared the error and completion behavior below for an engine that ran
asynchronously on a timer. The model now finishes every transfer inside its
starting write, for the reasons in the
[BTE timing approximation](#bte-timing-approximation). The engine still moves
128-byte lines in order and decrements length only after a line is copied or
zero-filled successfully, so the error arithmetic below is unchanged.

A failed source read or destination write stops that engine with busy clear,
error set, and the failed block plus unattempted blocks still counted. Keep
earlier successful blocks. No numeric hardware error code is established, and
the bytes within a failed destination block are not guaranteed unchanged.

With notification enabled, attempt one write of final length-status after
success or a data-transfer error. A failed notification write is logged and
leaves final engine status unchanged, with no retry. It does not itself set
the BTE error bit. On hardware, reading an engine's control register
terminates a transfer in progress. No transfer outlives its starting write in
the model, so the read finds nothing to stop: it returns control and clears the
busy bit of an engine armed but never started. These error details are explicit
emulator choices where hardware behavior remains unspecified. Poison behavior
remains uncleared.

The independent review also clears these explicit model policies for programming
edge cases; they are not observations of hardware behavior:

- A zero-count control start returns the engine to idle with zero length and error
  clear. Log the unsupported start. Touch no data and send
  no notification. This is an unsupported no-start result, not a completed transfer.
- Software first writes busy plus length, then source, destination, notification
  and control; accept that pre-start sequence. The review's rule for writes
  during an active transfer (log and reject them) no longer has an interval to
  apply to, since the transfer ends before the starting write returns. Every
  later programming write is accepted.
- Before moving data, check that the entire destination range and, for copy, the
  entire source range fit below `2^40`, using non-wrapping arithmetic for the
  line count times 128. Zero-fill ignores source. A crossing range stops before
  any data changes, with busy clear, generic error set and the full count still
  remaining. Attempt the normal error notification if enabled. Do not wrap a
  transfer into low memory. A single status write from the aligned, masked
  notification address cannot cross that boundary.

Test zero-count no-start with notification enabled, valid programming before
start and after completion, and both source and destination overflow. Overflow
tests must exclude partial copying and wrapped access at address zero, and
verify error, full remainder and notification. Public address-register readback
remains limited to its defined 40-bit aligned field.

### BTE timing approximation

**What the model does.** A control write that starts an engine moves every line
of the transfer, and writes the notification word when notify is set, before the
write returns. A completion interrupt is raised from a bottom half once the
starting access has returned, so it cannot re-enter the interface. The Origin
200 Hub model (`hw/misc/sgi-hub.c`) already runs its engines inside the
launching store.

**Why.** The IP35 PROM 6.210 hub BTE check (`hub_bte_diag`, at `0x1fc5f950` in
the disassembly of `ip35prom.payload.bin`, SHA-256 `554924380189a03a…`) fills
four 512 KiB buffers at physical `0x1500000`, `0x1580000`, `0x1600000` and
`0x1680000`, programs engine 0 to copy the first to the third and engine 1 the
second to the fourth, 4096 lines each, in interrupt mode at levels `0x3f` and
`0x3e` on the processor interface that bit 1 of the starting processor's
number selects, and starts engine 0 and then
engine 1 with two consecutive stores. It then polls pending register 0 of
processor interface 0 for bit 63, engine 0's level, alone. If bit 62 is also set
it clears both levels. Either way it next compares the first copy and then the
second, and any difference fails the check as `Hub BTE test failed: miscompare.`
(result 159). A poll that runs out of time fails as `hang` (result 160).

The check passes whenever each of engine 1's lines lands before the second
compare reads it. Engine 1 finishing before the first compare ends is
enough. On hardware that is
**inferred**, not measured: the two starting stores issue back to back and the
engines run side by side, so engine 1 should end shortly after engine 0. No
source available here gives the engines' throughput.

The earlier model ran each engine on a virtual-clock timer, one line per
callback in the main loop. Without instruction counting, virtual time is host
time, so the engines' progress had no bound relative to the processor's. All runs
below used two processors and round-robin TCG with
`-accel tcg,thread=single,rr-kick-period-ns=100000`. Run
`prom-20260926T193300-befaea` (QEMU `a31a08af57`) failed the check with result
159. Temporary trace events on the timer engine (QEMU `943006448f`, one-minute
host load 118 to 216 at launch) recorded eleven boots, all of which passed the
check. The two starting stores landed 0.04 to 91 ms of host time apart. In
eight of the eleven, engine 0's interrupt was raised while engine 1 still
owed 2 to 1385 of its 4096 lines, which it finished up to 47 ms later. That those boots passed
because the processor's compare of the first copy took longer is inferred;
the traces do not time the compare.

After a transfer the source and destination registers read one line past the
last line moved, and after a data error they name the failed line. This is the
model's choice; what hardware leaves there is unknown.

**Limitations.** A transfer is instantaneous to the guest: busy is never seen
after the start, a control read never finds a transfer to terminate, and the
engines' bandwidth and their sharing of request buffers are not modelled. The
largest transfer, 65535 lines (8 MiB), runs inside one guest store under the
global lock. Software that polls busy, the notification line or the completion
interrupt is served. Software that needs a transfer still in progress, for
example to terminate it part way, is not; no supported guest is known to. A
transfer whose source or destination is this interface's own registers now
runs inside that interface's register access, so QEMU's re-entrancy guard
refuses it and the engine latches a data error; no known consumer does this.

**Test.** The qtest `sn1/bedrock-io/bte-back-to-back` starts engine 0, advances
virtual time until engine 0's completion interrupt is raised, starts engine 1
and compares engine 1's copy with no further time. The timer engine fails it
at that compare; this model passes.

**What would settle it.** A measurement of engine throughput, and of whether
busy is observable after a start, on an Origin 300. No supported guest needs
either.

## Behavioral contract

For one chassis, with an XBridge on the port:

- **Widget identification** returns part `0xc110` in bits 27:12, manufacturer
  `0x036` in bits 11:1 and the same revision as the local block's revision register,
  at hub offset `0x400000` and in the low 32 bits of crosstalk widget 1 offset 0. The
  manufacturer value is the one two public tables give the Bedrock: OpenBSD's device
  table (`xbowdevs` lines 21 and 80) and mainline Linux's widget table (`xwidget.h`
  lines 96 and 109). No header states what the chip returns, and since this field
  echoes the local block's revision register, that register's manufacturer field is
  the same value in the model; the hardware word is unobserved.
- **Widget control** bits 3:0 read `0xa`, bit 17 reads 0. The port is observed: a
  verbose PROM trace from a real Origin 300 prints `HUB at 0x0 attached as widget
  0xa` and `Check_master: link 10 is master` (`docs/library/boot-logs.md`). Both
  are the model's reset values, since no consumer writes the register; the port must
  equal the hub master link the configuration records name, because the
  out-of-tree port skips that crossbar port and OpenBSD aims interrupts and
  direct-mapped DMA at it.
- **Link control and status** bit 13 reads 1. With it clear both Linux ports skip
  the node's I/O entirely. The PROM prints the whole register early in its boot as
  `Status of local IO: 0x1 0x3fc0fff6403` on a real Origin 300 (`0x3fc03ff6403` on a
  Tezro and an Origin 350), which decodes by the header's layout as link up (bit
  13), remote power (14), link enabled (10), null timeout 3, maximum retry `0x3ff`,
  maximum burst `0x3fc`, and data-available select 3 on the IP45 against 0 on the
  IP53; the model's reset value for the register is that word, labelled as the
  PROM-time observation it is.
- **Translation entries** retain a write and read it back; a fresh entry reads widget
  0 in the model (its reset value, unknown on hardware), which OpenBSD treats as free. The PROM is assumed by OpenBSD to have loaded entry
  6 with widget 0 at offset 0 (the widget-0 window decision packet); the Itanium port instead writes
  every entry's disabled form (widget 3) at init, through a macro whose argument
  order is wrong, so its six writes land on entry 0.
- **Outbound widget access, inbound widget access and the device error mask reset to
  all ones in the model, as its choice.** No MIPS consumer writes any of the three
  (counted: OpenBSD's I/O-interface accesses are the translation entries, the
  out-of-tree port's are two reads), the header makes every request a widget launches
  conditional on its inbound bit and every device request on both bits, and a zero
  reset, the default an implementer reads "unknown" as, would block every XBridge DMA
  and interrupt packet on every MIPS guest. Settle by a trace of the held IP35 PROM's
  writes to `0x400110`, `0x400118` and `0x400120`, or a read of the three after PROM
  hand-off on an Origin 300.
- **Local access protection** reads all ones after reset.
- **Miscellaneous error mask** reads `0x111` after reset: widget 0 and both block
  transfer engines enabled. This value is **inferred**; no source states it. The
  header's description of the register (`hubio.h` lines 2579 to 2589) says the hub
  clears each field when an error requires widget 0 or an engine to be terminated,
  and software sets it by writing 1. The later SHub's BTE recovery
  (`arch/ia64/sn/kernel/bte_error.c` line 108 at Linux `981a2edd19`, code for that
  later chip despite its path through the Itanium tree) re-enables an engine by
  setting its bit, which agrees. The companion header's macro comments instead call
  bits 0, 4 and 8 "shut down due to error" (`hubio_next.h` lines 137 to 139), and the
  SHub-generation error handler that reads them as such (`sn2/shuberror.c` lines 203
  to 214) runs when the widget-status crazy bit is set, not only after a link
  failure; the register description and the recovery write govern.
  The vendor's System Level Test (the Origin 300 sMDK package's `System_Level_Test`,
  SHA-256 `19c5a938a38b6f1e…`) reads the register after each pass and fails the node
  with `BTE II_IMEM hub register indicates errors` when bits 4 and 8 are both clear.
  On its command-line path it does not write the register first; only its engine-reset
  routine, which the individual tests call, writes `0x110`. The stock IP35 PROM 6.210
  has no store addressing the register; the offset appears only in its
  register-name table, and before this fix the diagnostic's check read the register
  as 0 on every topology and test selection tried: two processors with
  `-s 1 -e 1` (run `prom-20260928T092823-90802d`), four processors with a
  `Memory_CC`/`PIO`/`BTE`/`IO_GSN` selection (run `prom-20260928T062907-2bfe7d`), and
  the two-chassis pair with the same selection (run `prom-20260928T063345-6ed7dc`).
  So an engine bit set when the diagnostic starts must come from reset, not from a
  store this model or the stock PROM issues; that cannot rule out a store issued
  only by the PROM's compressed base-I/O segment, which this reading did not reach,
  or by hardware. Bit 0 follows the header's single rule for all three fields. The
  model never shuts a source down, so the bits stay set until reset.

  `System_Level_Test -s 1 -e 1` is the discriminating case: before this fix it failed
  pass 1 at two processors (run `prom-20260928T092823-90802d`); with it, the same
  case passes (run `prom-20260928T092709-d70fbe`). It was not run at four processors
  or on the two-chassis pair; an attempt at four processors with the fixed register
  stopped incomplete before reaching a pass (run `prom-20260928T094344-f0091e`).
  `-s 1 -e 2 -t All`, the field guide's default, passes with both the unfixed and the
  fixed register at two processors (runs `prom-20260928T092432-11b9bc` and
  `-091223-a48f81`) and with the fixed register at four processors and on the pair
  (runs `-091603-3d7501` and `-092115-f23b89`, not paired with an unfixed-register
  run at those topologies); its own engine-reset routine writes `0x110` before the
  check every pass, so it does not discriminate at any topology tried. The runs
  after this fix used a pre-rebase build carrying the same patch as
  `hw/misc/sgi-bedrock-io: reset the miscellaneous error mask enabled`.
  Settle by a read of `0x4001e8` after PROM hand-off on an Origin 300.
- **The Itanium probe sequence is accepted as writes**, in two passes. The per-hub
  pass (`sgi_io_init.c` lines 90 to 115, 172): buffer management (count `0xf`), BTE
  control (count 8), prescaler `0x1000`, timeout `0xff`, then the nine request buffers
  read-modify-written with overflow count 1, NACK counters 0, fire-and-forget 0 and
  credits 3 for all nine including widget 0 (`huberror.c` lines 63 to 83), error clear
  all ones, the interrupt destination read and rewritten without its sent bit, and,
  after the link check, the destination programmed with enable, level, node and PI
  select. The widget-probe pass, later (`sgi_io_init.c` line 219 through `io/io.c`
  lines 604 to 612 and 104 to 112): six translation-entry disable writes that all
  land on entry 0 through the misordered macro, then outbound access saved and
  zeroed, request buffer 0 rewritten with 1 credit for a crossbar at widget 0 (3 if
  directly connected) and buffers 8 to F with 3, and outbound access restored. So
  request buffer 0 is written twice and ends at 1 credit on this machine, and error
  clear, being write-one-to-clear, reads 0 afterwards rather than what was written.
- **An inbound crosstalk write to hub offset `0x1800090`** from a widget whose
  inbound-access bit is set, and whose device bit is set when the source carries a
  device number, is forwarded to processor interface 0's interrupt-pending-modify
  register; `0x1a00090` goes to interface 1. This is the same address the II itself
  uses for its error interrupt, so the external-widget path and the unit's own error
  path converge on one register. No header describes the decode from a widget-1
  address to a hub unit; the model's decode is the address-map datasheet's.
- **Proposed model limitation for unidentified inbound accesses.** A read or
  write through QEMU's shared inbound memory window does not carry its launching
  widget or device number. This includes DMA and inbound interrupt packets.
  Such an access must be refused if any configured candidate is denied by the
  inbound-access or device mask; it may pass when all candidates are allowed.
  This conservative fallback is not the hardware's source-specific protection
  rule. A register test must deny widget 14 while allowing 15, then reverse the
  masks, and confirm that both unidentified accesses are refused; with both
  permitted, they may proceed. A widget-15 SCSI DMA test must refuse a transfer
  when 15 is denied and 14 allowed, catching the earlier unsafe fallback. It
  must also refuse when 15 is allowed and 14 denied: real source qualification
  would allow that transfer, but this model deliberately cannot. With both
  permitted, the same transfer must succeed. Until requester identity reaches
  the II, per-widget inbound protection is unsupported, not hardware-accurate.
- **Block transfer engines** must execute the documented transfer protocol for
  the requested SGI diagnostic coverage. Lack of a public MIPS kernel caller
  does not justify an inert engine. Unknown completion or error behavior remains
  an evidence gap, not permission to report a successful transfer.

## Interrupts

- The II's error interrupt is raised on the PI and level the interrupt-destination
  register names, through the hard-coded address above; the sent bit records it. The
  Itanium bit table expects the PROM to have pointed it at level-1 bit 38, and the
  Bridge error interrupt at 53, both marked as PROM setup (`sn1/intr.h` lines 208,
  228); neither MIPS consumer programs the register.
- Credit and tail timeouts interrupt the same destination.
- BTE completion interrupts go where each engine's interrupt-address register says;
  no public consumer programs one. The IP35 PROM's hub BTE check does, as the
  [BTE timing approximation](#bte-timing-approximation) describes.

What real Origin 300s show of the chain is the bridge half: OpenBSD reports each IO-8
device's interrupt on the bridge pin numbered as its PCI device (SCSI at 1, the IOC3's
super-I/O on 0 and its Ethernet on 4, USB at 5, option cards at 1 and 2) and allocates
Bedrock bank-0 bits 62 down to 59 for them in attach order, its own allocation from bit
63 downward (`ip27_machdep.c` lines 740 to 766)
(the openbsd-sgi list captures in `docs/library/boot-logs.md`). Which processor line each
bit reaches is still unobserved and belongs to the interrupt-chain packet, for James.

## DMA

The unit is the target of every widget's DMA. What the public consumers program on
the bridge side, and what the II must therefore accept:

- **Direct 32-bit window.** OpenBSD constrains DMA to the first 2 GB and writes the
  bridge's direct-map register with offset 0 and the node's hub widget, so every
  OpenBSD DMA targets node 0's low 2 GB (`ip27_machdep.c` lines 216 to 222;
  `xbridge.c` lines 2048 to 2060). The out-of-tree port does the same with the
  master widget (`pci-sgipic.c` lines 253, 351). The Itanium port puts the node's
  memory base shifted right by 31 in the offset field (`pcibr.c` lines 3777 to 3787).
  The crosstalk address a bridge emits is PCI bits 30:0 plus offset bits in 47:31, so
  with the SN1 node at physical bits 39:33 the node number rides in offset bits 8:2.
- **64-bit direct-mapped attributes.** The out-of-tree port builds a bus address as
  the master widget in bits 63:60, the barrier bit 56, the swap bit 55 and the
  precise bit 58 over the physical address (`pci-sgipic.c` lines 226, 317;
  `dma-coherence.h` lines 14 to 27); the PCI address bits 47:0 are the physical
  address with the node field in place.
- **Routing.** The II must route a request with crosstalk bit 47 clear to the memory
  of the node in bits 39:33. No public header describes that decode; the only
  crosstalk memory-address text (`xtalkaddrs.h` lines 21 to 33) is the SN0 form
  with the node at 39:32, stale for SN1 and recorded as open. The inbound-access and
  device-mask registers are the only II registers the headers say qualify such
  requests.

## Remaining implementation and qualification

Credit accounting, request-buffer state, PIO read-table timeouts, graphics maps,
performance counters, spurious messages and Crosstalk link behavior remain
unqualified beyond the specific implemented operations recorded in this sheet.
The implemented BTE path performs real block transfers. Storage-only completion
is not an alternative contract. Poison handling and other unresolved transfer
effects still require implementation and tests.
These gaps remain part of complete machine and SGI diagnostic qualification.

## Behavioral notes

**Two identities for one chip.** The local block's revision register carries part
`0x3012`; the widget identification register carries crosstalk part `0xc110`. Both
are stated by the SN1 tree (`bedrock.h` line 17; `hubio_next.h` line 403) and a model
must return each at its own register.

**The interrupt-destination layout conflicts inside the tree.** The generated struct
puts the node at bits 16:9 and the PI select at bit 8; the companion header's masks
put the node at 15:8 (the SN0 layout) and give the PI select a shift of 8 with a mask
of bit 4 (`hubio_next.h` lines 460 to 465). The Itanium consumer writes through the
struct, so the struct governs; the masks are unused.

**The widget-status and widget-control layouts differ between the II's own registers
and the generic crosstalk widget layout** (`xwidget.h` lines 63 to 82): the generic
status has a receive count at 31:24 and pending at 4:0, the II's has pending at 3:0
and no receive count; the generic control has credits at 15:12, the II's at 15:13.
Which layout a crosstalk read of widget 1 offset 8 or `0x20` returns is open. No
consumer reads either through crosstalk.

**Direct-connect is open.** Whether any IP35 PROM sets bit 17 of widget control, and
whether the II then answers PIO to widget 0 as a bridge, is not stated. With an
XBridge on the port the crossbar case (bit clear) is the one every consumer takes.
Settle by a trace of the held PROM's writes to `0x400020`, or a read after hand-off.
The stale SN0 crosstalk memory-address form under DMA is unobservable on one node
and stays open until a second node exists, as the address map says of the same
question.

**The protection-override field width is a header self-contradiction.** The
description says a 64-bit key, the union gives nine bits. Nothing writes it.

**Header defect.** The little-endian form of BTE 0's length-status union is guarded
by a misspelt macro and never compiles; the consumer uses masks that match the
big-endian struct.

**The vendor's own Origin 300 diagnostics name a BTE test** and note for the
cache-coherency test that PIO and BTE accesses "are not implemented yet"
(108-0264-004 printed pages 2-35 and 2-31). Vocabulary, not behavior.

## qtest checklist

- Widget identification reads part `0xc110` in bits 27:12 and manufacturer `0x036` in bits 11:1 at hub offset `0x400000`, and the same 32 bits at crosstalk widget 1 offset 0; a write does not change it.
- Widget control bits 3:0 read the configured port number; bit 17 reads 0 (the model's reset value); a write to bits 15:13 is retained.
- Link control and status bit 13 reads 1 after reset; a write setting bit 10 reads back set and a write clearing it reads back clear.
- Local access protection reads all ones after reset; a write of the key to the override register with a region bit first cleared sets that requester's bit, any other value changes nothing, and the same write through the local alias changes nothing.
- Outbound access, inbound access and the device error mask are read before any write and the values recorded; the model's values are all ones. The miscellaneous error mask reads `0x111` after reset. The device mask and the miscellaneous error mask accept write-one-to-set: a write of 0 to the miscellaneous mask leaves `0x111`, and a write of `0x1000` then reads back `0x1111`.
- Translation entry 0 reads widget 0 after reset; a write of widget `0x8`, offset `0x2`, I/O-space 0 reads back exactly; entry 6 reads the PROM-loaded widget-0 mapping once the firmware contract fixes it (the widget-0 window packet), else widget 0.
- The Itanium probe sequence, replayed in its two passes register by register, is accepted; each register reads back what was written except error clear, which reads 0, and the interrupt destination, which reads back without its sent bit; request buffer 0 ends with 1 credit.
- Error clear written all ones reads 0. Widget status bit 32 is set only by an error the hardware detects, so its clearing is untested until the model has an error path.
- A request buffer's spurious-write bit, once the model sets it, survives an error-clear write and clears only when the buffer is rewritten.
- The link log reads 0 after a write of 0.
- Scratch 0 and 1 retain any 64-bit value.
- With inbound access written to clear bit 8, a 64-bit write of a level value to widget 8's view of node I/O-space offset `0x1800090` (small window 1, the remote bit, register `0x90`) leaves processor interface 0's pending register unchanged; with bit 8 written set again the same write sets that level.
- Each BTE copies the requested bytes between distinct initialized buffers and writes its final status to the notification line. Test actual data movement, busy/completion state, and documented bounds and errors. An engine that only retains registers or reports completion without copying data must fail.
- Test source-read, destination-write and notification-write failures separately. Assert the generic error flag and remaining-line arithmetic for data errors. A notification error preserves final engine status. A control read after a start finds the transfer complete and changes nothing.
- Start engine 0, let virtual time pass until its completion interrupt is raised, then start engine 1: engine 1's copy is complete with no further time passed (see [the BTE timing approximation](#bte-timing-approximation)).
