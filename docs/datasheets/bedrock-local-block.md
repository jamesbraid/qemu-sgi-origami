# Datasheet: Bedrock local block (LB)

References rendered as citation identities name material outside this document
set. Those targets are not included or cleared for redistribution. Review and
runtime statements retain their recorded dates and scope.

The 2026-09-05 identity correction (citation: `docs/reviews/origin300-identity-correction.md`)
withdraws unsupported IP45 register assignments and scopes inherited controller
names. The original clearance remains recorded for the unchanged contract.

**Status:** reviewed 2026-09-03 by an independent second reader
(`docs/reviews/bedrock-pi-lb-ni-datasheet-review.md`): all 34 registers cleared position for position
against the SN1 header, with the review's five findings applied in this revision.
The open items stay open. An isolated register implementation and its qtests are
recorded in the integration notebook (citation: `docs/notebooks/origin300-integration.md`).
This does not establish a complete local block or Origin 300 integration.
Independent evidence review of the MicroLAN behavior added on 2026-09-08
cleared the header-derived register facts and bounded controller model choices.
Endpoint selection remains blocked pending IP45 evidence.
The 2026-09-24 two-node delta (node number assignment, scratch 1's assignment
state, and the vector engine's route, status-clear and write behavior) replaces
the one-node self-route policy with what the stock PROM does. It is pending
independent evidence review.

**Fork file(s):** `hw/misc/sgi-bedrock-lb.c`, with its public header
`include/hw/misc/sgi-bedrock-lb.h`. The future Bedrock parent owns physical
decode and processor-interface interrupt routing.
**Evidence cutoff:** 2026-09-03, Origami trunk `506836b9dd`; the two-node
delta 2026-09-24, Origami trunk `e09edbef40`.

**Sources used:** the SGI-generated Linux SN1 register headers at tag `v2.5.40`
(commit `7570df54ef8c`): `include/asm-ia64/sn/sn1/hublb.h` (34 registers),
`hublb_next.h`, `sn1/bedrock.h`, `sn/vector.h`; the Itanium consumers
`arch/ia64/sn/io/sn1/{ip37.c,huberror.c}`, `arch/ia64/sn/io/{l1.c,klgraph_hack.c,stubs.c}`,
`arch/ia64/sn/kernel/setup.c`, `include/asm-ia64/sn/pio_flush.h`. OpenBSD at
`2df91d80c1`: `sys/arch/sgi/xbow/hub.h`, `sys/arch/sgi/sgi/ip27_machdep.c`,
`sys/arch/sgi/include/mnode.h`. The Origin 3000 System Architecture 108-0240-002
printed page 2-17 for the unit's role. The MicroLAN extension also uses a scan
of SGI's XBridge ASIC specification revision 0.5, printed pages 79 to 80. The
scan is publicly indexed by Library, but its pages are marked proprietary and
confidential. The Library catalogue (citation: `docs/library/docs.tsv`) records it with
SHA-256
`a6a462d72148a3ce545a0ea3d82174f10b1a6cb5c9e42cc49506536193475d8d`.
The out-of-tree MIPS port carries no local-block header and touches no
local-block register. Reading preserved in the SN1 research notebook. For the
real-time clock registers' reset value: a repository-wide search of the pinned
Linux SN1 tree and the pinned OpenBSD tree above, beyond the specific files
already listed, turned up no further reader; the *Origin200/2000 IP27 PROM
Technical Reference Manual* (108-0170-001), `docs/library/cache/prom/108-0170-001-ip27-prom-technical-reference.pdf`
printed page 50, which names `PI_RT_LOCAL_CTRL` as a recognized debugger
register with no value stated — a different, SN0-generation manual and
register, cited to show it was checked and came up empty rather than
unchecked; and [the Hub processor interface datasheet](hub-processor-interface.md)
for the sibling fact recorded below.

The two-node delta adds the stock IP35 PROM 6.210 (payload SHA-256
`554924380189a03a512156af2faa38344902f251b49fd5c80c253c23ecaff27b`), read by
disassembly with `tools/origami disasm build`, and one instrumented one-node
emulator run of it that recorded every local-block and network-interface
access with the issuing PC (run `prom-20260924T052035-55abd1`, QEMU
`sgi-origami` `ca021b99c6`); both are preserved in the
PROM trace notebook (citation: `docs/notebooks/origin300-numalink-prom-trace-20260924.md`).
PROM addresses below are listing addresses without the `0xc000000000000000`
prefix, and `func_` names are the project disassembler's labels. The product
facts come from the Origin 300 User's Guide 007-4365-002, printed pages 1, 8
and 38.

The QEMU reuse source, rather than hardware evidence, is `fj/sgi-origami` at
`12d7ce7830b6daf7de086b6426349b5790081969`. Its generic 1-Wire bus and ROM
state machine are in `hw/1wire/core.c`. Its SGI slot decoder is
`include/hw/misc/sgi-onewire-slot.h`, and the current Hub master in
`hw/misc/sgi-hub.c` supplies the controller timer and migration pattern.

**Facts that are IRIX-derived: none.** **Reset values the header states:** scratch
register 0 and its write-if-zero twin reset to zero; scratch registers 1 and 2 are not
reset; the error bits survive reset. Everything else is `unknown`.

## Role

The local block is the unit the Origin 200's Hub does not have. Per the Origin 3000
architecture book it services processor register accesses to the network, crossbar
and memory-directory units, holds its own registers, generates the real-time clock
signal the two processor interfaces count, replies to programmed-I/O requests,
initiates vector operations across the fabric during system initialisation, and
services local invalidates. For a one-chassis Origin 300 three of its registers are on
the boot path: the revision register that carries the chip identity, node number and
region size; scratch register 2, which OpenBSD uses as the reboot-request mailbox;
and scratch register 3's read-set-if-zero twin, which the Itanium port uses as a
hub-wide lock around the L1 console. The clock-control registers decide what the
processor interfaces' real-time counter counts, and no public source records what the
firmware programs there.

## Register map

Offsets are hub-relative; the unit starts at `0x600000`. Every register is 64 bits.

### Identity and access control

| Offset | Name | Fields | Reset | Access | Consumer | Behavior |
|---|---|---|---|---|---|---|
| `0x600000` | revision and identity | 0 reserved; 11:1 manufacturer; 27:12 part number; 31:28 revision; 39:32 node number; 45:40 reserved; 47:46 region size; 63:48 reserved | unknown | read; the stock PROM writes the node number and region size with one 64-bit store of the whole word (see "Node number assignment") | the stock IP35 PROM reads bits 39:32 as its own node number wherever it needs one; the Itanium port reads it as a fence before every posted-write flush and defines a region-size test (`pio_flush.h` lines 43 to 54; `ip37.c` lines 33 to 39); no MIPS consumer reads it | Part number `0x3012`, revision 1 for Bedrock 1.0 and 2 for 1.1 (`bedrock.h` lines 17 to 21; `hublb_next.h` line 24); region size 0 one node per region, 1 two, 2 four (N mode), 3 invalid (`hublb_next.h` lines 28 to 38). No header states the manufacturer value; the header says IBM assigns the revision, part and manufacturer values under IEEE 1149.1 (lines 257 to 263). The I/O interface's widget identification echoes this field, and two public tables give the Bedrock's crosstalk manufacturer as `0x036` (OpenBSD `xbowdevs` lines 21 and 80; mainline Linux `xwidget.h` lines 96 and 109), so the model returns `0x036` here; the hardware word is unobserved. |
| `0x604000` | CPU permission | 63:0 one bit per region | unknown | R/W | none | Permits processor access to the local-block, network, crossbar and memory-directory registers per region; a denied write, or a denied read with a side effect, gets an error reply (lines 310 to 319). |
| `0x604008` | CPU permission override | 63:0 pattern | unknown | write | none | A write of `0x53474972756c6573` sets the writer's region bit in CPU permission (lines 338 to 340; the value is `bedrock.h` line 15's hub password). |
| `0x604010` | I/O permission | 63:0 per region | unknown | R/W | the stock IP35 PROM writes all ones at hub initialisation | The same vector for I/O-interface-initiated accesses (lines 359 to 368). |
| `0x604018` | soft reset | 0 reset; 63:1 reserved | unknown | write | none; both IP35 consumers reset through the network interface instead | A write resets the Bedrock with a soft reset (line 387); what a soft reset preserves is known only from the registers that say they survive it. |
| `0x604020` | region present | 63:0 | unknown | R/W | the stock IP35 PROM writes 1 after node-number assignment on a one-node boot (`func_1fc5be80`); OpenBSD writes the processor interface's register of the same name, not this one | Which regions may receive an invalidate; for an absent region the block acknowledges on the interface's behalf (lines 420 to 429). |
| `0x604028` | nodes absent | four 16-bit entries, each 7:0 node, 15 valid | unknown | R/W | the stock IP35 PROM writes 0 beside region present | Nodes that cannot receive an invalidate (lines 448 to 454). |
| `0x604030` | MicroLAN control | 0 sampled data; 1 completion; 9:2 sample offset; 19:10 low-pulse duration; 26:20 phase-0 divider; 33:27 phase-1 divider | unknown | model choice: one aligned 64-bit read or write; narrower hardware accesses are open | the public Linux and OpenBSD IP35 consumers do not access it | The generated SN1 header names the offset and fields, calls it access to the node board's add-only serial identity part, and says data arrives least-significant bit first (`hublb.h` lines 76 to 79 and 507 to 549). The register type is `u64` (`include/asm-ia64/sn/arch.h` line 28), and bit 33 makes a 32-bit implementation incomplete. |

## Node number assignment

**The firmware programs the node number by storing the whole revision word
[established, disassembly and trace].** The stock IP35 PROM 6.210 reads its
node number only from bits 39:32 of this register (`func_1fc6b5d0`, 311
direct callers; `func_1fc04438` takes the 7-bit M-mode field early in reset). It
writes the register at two points:

- At hub initialisation `func_1fc676c8` stores `0` to the whole register
  (pc `0x1fc67718`), before the memory test. The same routine clears the
  vector route, data, status, return-route and read-data registers.
- After `Changing node ID to %d`, `func_1fc5c32c` reads the word, replaces
  bits 39:32 with the assigned number and, when its region argument is not
  negative, bits 47:46 with the region size, then stores **old, old, new**
  (the store is at `0x1fc5c384`, in the delay slot of a call), each store
  between two `sync`s and followed by a short delay loop. It then re-reads the register (`0x1fc61294`, `0x1fc612b0`) and
  builds every later node-relative address from the value read back, not from
  the value it meant to write. On the traced one-node run all stores carried
  `0x2301206c` (node 0, region size 0), which the model logged as a write to a
  read-only register and ignored.

**The identity fields are read-only [inferred].** The early store of `0`
would clear the manufacturer, part and revision if they were writable, yet
discovery later identifies a hub by reading the part field (`0x3012`) of the
neighbour's word, and `func_1fc5c32c` writes the identity back unchanged. The
model keeps manufacturer, part and revision as the device's identity.

**How a remote hub gets its number [established, disassembly; not executed on
one node].** The master assigns a number to every discovered hub through the
vector engine, never through the remote register window. For a hub with no
processor (`> Headless Hub:` debug text) it vector-reads that hub's revision
word, replaces bits 39:32 and 47:46 and vector-writes it back. For a hub with
processors (`> Notifying Hub:`) it vector-reads the hub's scratch 1, replaces
bits 11:0 with the number and vector-writes it back; that hub's own processor
polls its own scratch 1 every 250 ms until bits 11:0 are no longer `0xfff`,
then runs the same local sequence on its own revision register. The POD
`nasid` command (`func_1fc27c20`) is the headless path alone; a second POD
routine (`func_1fc28b04`, which warns that changing the node ID is very
dangerous) either runs the local sequence or rewrites a remote revision word
through the POD vector primitive. On a directly cabled pair both
hubs have processors, so the only cross-node writes on this path are the
vector writes to scratch 1 (inferred from the two paths).

**Decode follows the programmed number [inferred].** After the store the
firmware addresses its own node at `programmed << 33`: cached memory at
`0xa800000002000000 | n << 33` and its own registers through the remote form
`0x9200000001800000 + (n << 33) + offset` (the vector-engine primitive and the
reset-enable clear both do so). The firmware's use is established; that the
hardware re-derives decode from the written number is inferred from it. On one
node the number is 0 and the question cannot be distinguished from a strap.
The captures do not settle how numbers fall on a directly cabled pair. The
nekonomicon two-brick thread (catalogued in `docs/library/boot-logs.md`, cached
as `boot-logs/nekonomicon-thread-16724335-origin300-two-brick-prom-boot.html`)
holds two inventories of the same two modules. In its 2012 posts the modules
are joined through a NUMAlink router, and the PROM's `hinv -v` shows module
001c01 at `nasid 1` and 001c02 at `nasid 0`, so there numbers come from
discovery, not from the module number. In its November 2010 post the two
modules are, by inference, cabled directly: the poster writes "since I
NUMAlinked the two modules" and only hooks up the router he "bought a while
back" in March 2012; no router appears in that inventory. There the
BASEIO PROM's `hinv -v` prints `(nasid 0)` for both Bedrocks while listing
processors 4 to 7 on 001c02 [unexplained; a print artefact is plausible but
not shown]. The OpenBSD list capture of another pair ("two Origin 300
systems and a NUMA cable", no router in its inventory; `docs/library/boot-logs.md`,
"Origin 300 with NUMA link", cached as
`boot-logs/openbsd-sgi-list/marc-139223952727384-origin-300-numalink-1.html`)
prints `(nasid 0)` on 001c01 and `(nasid 1)` on 001c02.

**Unknown:** why the firmware stores three times; whether decode changes on
the store or on the next access; the node number's hardware reset value; and
what the PROM flag-word bit 24 that selects region size 1 (and scratch 1 bit
59) means. The traced run wrote region size 0, which is what a stock pair runs.

**Model contract.** An aligned 64-bit write latches bits 39:32 as the node
number and bits 47:46 as the region size; the identity fields ignore the
write. A write whose region size is 3, which the header calls invalid, is refused
whole with a guest-error log: neither field changes (model choice). Every reset of the
block restores the realized node-number and region-size properties (model
choice: the hardware reset value is unknown, and the firmware rewrites both
before use). Within one Bedrock the latched number is the node number every
unit decodes by: the I/O interface's crosstalk register-packet and BTE
completion node match read it. That is the model's choice of one node number
per chip; no examined source says how the I/O interface matches inbound
packets. Relocating the processors' node windows
and a peer's view to the programmed number belongs to the cross-node address
decode, which this datasheet does not yet specify.

## MicroLAN discovery contract

The register definition establishes four facts without help from a guest or an
emulator:

- `0x604030` is the local block's MicroLAN controller.
- It is a 64-bit register. Bits 33:0 have defined positions and bits 63:34 are
  reserved.
- Bit 0 is received data, bit 1 is completion, and the timing inputs occupy
  bits 33:2.
- The attached identity part describes the node board, and bytes travel on the
  wire least-significant bit first.

The header establishes the register's numeric width but does not state which
bus transfer sizes or byte lanes the hardware accepts. The public IP35 Linux
configuration is big-endian (`arch/mips/configs/ip35_defconfig` lines 67 to
68). Accepting one aligned eight-byte, big-endian access and rejecting narrower
or misaligned requests is an explicit model choice, not a hardware finding. It
also prevents QEMU from splitting one guest access into multiple register
writes and starting more than one wire slot.

Linux `v2.5.40` contains no use of `LB_MICROLAN_CTL` or
`lb_microlan_ctl` outside `hublb.h`. The public IP35 MIPS port at
`9f613a321ea0` contains neither name nor the numeric offset. OpenBSD at
`2df91d80c1` uses a different MicroLAN controller on Heart and does not access
this register. These source trees therefore provide no public guest sequence
for Bedrock.

### Start, completion and sampling

The publicly indexed XBridge scan describes another SGI MicroLAN register
whose low 20 bits match this register field for field. For XBridge, a write
starts one of four primitives. Software then polls bit 1 until the operation
completes and, for a read, takes the result from bit 0. Its sample field is an
offset after the controller releases the wire. Printed pages 79 to 80 give
these examples in microseconds: reset and presence uses pulse 500 and sample
65, write zero uses pulse 80 and sample 1, and read uses pulse 5 and sample 5.
Write one uses the same short pulse, a sample value of 80, and ignores the
sampled result. These statements describe XBridge only.

The matching field positions do not establish the Bedrock transaction
semantics. The Bedrock header adds two seven-bit phase dividers and does not say
how they turn field values into time. It also gives no completion latency.

The bounded model choice is to reuse `sgi_onewire_slot()` for pulse
classification, reset and presence, and sampling on the shared 1-Wire bus. One
write starts one slot, clears completion, latches the bus sample, and publishes
completion and data after a positive virtual delay. Reads have no bus side
effect, and writes to bits 1:0 do not set the device-owned result fields. The
helper's 400 and 40 pulse thresholds are existing QEMU policy rather than
Bedrock values (`sgi-onewire-slot.h` lines 14 to 42).

Reusing the Hub master's 1 microsecond virtual timer avoids adding a second
arbitrary delay and makes the request state observable to a register test
(`sgi-hub.c` lines 146 to 170, 1263 to 1269, and 1938 to 1952). Start,
completion, presence handling and that delay are all model choices. None is a
claim about Bedrock hardware timing or transaction semantics. The model
preserves all six defined fields on readback.

The behavior of a second write while a slot is pending is not published. Keep
it observable as an unsupported case until a public consumer or matching
capture settles it.

### Endpoint and identity boundary

The endpoint is not established as a DS2505. The Origin 200 Hub uses a
DS2505-compatible manufacturing record, but that is another node controller
and another board generation. Importing its family byte or page image would be
an unsupported topology transfer.

The Bedrock header points to a “Dallas 1990A” identity part while also calling
the device an add-only serial PROM that carries serial-number and configuration
information (`hublb.h` lines 507 to 514). The XBridge scan uses the same Dallas
reference and then defines a 64-bit, identity-only reply: family `0x01`, a
48-bit unique value and an eight-bit CRC, returned by Read ROM `0x33`
least-significant bit first (printed page 80). This makes a
DS1990A-compatible serial endpoint a candidate model choice. It does not prove
which part or family byte was fitted to IP45, whether IP45 uses Read ROM, or
whether an identity-only endpoint is sufficient. The “add-only” description
also leaves open whether the production part exposes memory pages beyond its
ROM identity.

Endpoint implementation is blocked until a public slot trace, public consumer,
or matching IP45 component document establishes the required family, commands
and identity content. If independent evidence later clears a synthetic
identity, it must not copy a real ROM serial, an IP45 barcode, an SGI part
number or an observed manufacturing record. Expose no invented page content.

### Reset and migration

No public Bedrock source states the register's power-on value, which fields a
local reset preserves, or the reset state of completion and sampled data. The
model still needs deterministic lifecycle behavior: a machine or Bedrock-local
reset cancels a pending completion, prevents a late timer callback, and resets
the child endpoint's protocol phase. Do not label any chosen register value as
hardware reset state. A long low pulse is a 1-Wire bus reset and presence
operation under the controller model choice above. It is distinct from
resetting Bedrock.

Migration must preserve the visible register, a pending sampled level and the
controller's completion timer. The shared 1-Wire base already migrates the ROM
phase, bit index, receive shift and transmit byte, and validates them after
load (`hw/1wire/core.c` lines 245 to 317). The Hub precedent migrates its
register and timer (`hw/misc/sgi-hub.c` lines 3153 to 3156). Family and
synthetic identity are realized machine properties, so they need not be copied
into a migration stream between identical machine definitions. If evidence
later requires a memory part, that part owns and migrates its page and
memory-function state.

### Errors

| Offset | Name | Fields | Reset | Access | Consumer | Behavior |
|---|---|---|---|---|---|---|
| `0x604040` | error bits | 0 to 9 request and reply bad command, short, long, bad data, bad address, bad invalidate; 10 global clock dropout; 63:11 reserved | survive reset | read non-destructive; a write sets bits | the Itanium debug file reads and zeroes it, against the header | Any set bit asserts the block's error request to the processor interfaces, which is pending 1 bit 55 there and cannot be cleared through the interface (lines 563 to 569; `hubpi.h` lines 1107 to 1109). |
| `0x604048` | error mask clear | same positions | unknown | write-one-to-clear | the Itanium port writes all ones at I/O init (`huberror.c` lines 92 to 93) | The documented way to clear error bits: any subset, atomically (lines 624 to 632). |
| `0x604050` | error header 1 | 6:0 command; 18:8 supplemental; 30:20 source; 35:32 error type; 40 overrun; 44 valid | unknown | R/W | the Itanium port writes 0 at I/O init | First erroring header is captured while valid is clear; a later error sets overrun (lines 686 to 699). |
| `0x604058` | error header 2 | 37:0 address | unknown | R/W | the Itanium debug file zeroes it (`klgraph_hack.c` lines 133 to 134); the I/O-init path writes header 1 only | The address of the first erroring header (lines 753 to 755). |
| `0x604060` | error data | 63:0 | unknown | R/W | none | The data flit of the first erroring packet for the data-bearing error kinds (lines 789 to 821). |
| `0x604100`, `0x604108` | debug select, debug pins | 5:0; 31:0 | unknown | R/W; read | none | Debug pin routing. |

### The real-time clock source

| Offset | Name | Fields | Reset | Access | Consumer | Behavior |
|---|---|---|---|---|---|---|
| `0x604200` | real-time clock local control | 0 global clock enable; 13:4 maximum count; 25:16 global clock counter; 28 global clock; 32 use internal | unknown | R/W | none | The local block supplies both processor interfaces with the clock their real-time counters count. With use-internal set it generates the clock from the Bedrock's system clock with a period set by the maximum count; with it clear it filters an incoming global clock and passes it on (lines 919 to 927). No public source states the resulting frequency or the firmware's setting; every MIPS consumer assumes the counter runs at 1,250,000 Hz. |
| `0x604208` | real-time clock filter control | 4:0 offset; 19:8 mask counter; 20 mask enable; 33:24 dropout counter; 45:36 dropout threshold; 57:48 error counter | unknown | R/W | none | With an external clock, simple filtering drops single-cycle glitches; complex filtering tracks missing or unexpected edges and synthesises a missing one; the clock handed to the interfaces is high for a computed number of cycles after each edge (lines 977 to 995). Clock loss is error bit 10 here and pending 1 bit 58 in the processor interface. |

**Neither register's reset value is established, and this is now the tracked defect
`jamesb/qemu#581` (private issue identity).** The stock PROM
converts a real-time counter reading to microseconds by reading local control's
maximum-count field back and computing `counter * (((control & 0x3ff0) >> 3) + 2) /
200`, where 200 is the Bedrock clock in megahertz. That relationship is stated by
the firmware's own arithmetic, not inferred from a guest constant. For the field to
agree with the 1,250,000 Hz every MIPS consumer assumes for the counter, hardware
would have to reset it to 79. **The model resets both registers to all zero instead,
which is a model choice asserted by a qtest, not a measured hardware value** — with
the field at zero the PROM's own conversion comes out about eighty times short of
real time. This is the third register on this project found in that shape: a
model-chosen reset value nothing writes before reading it, where a wrong guess
produces wrong arithmetic rather than a visible failure.

The search this rests on, all within the sources already listed above: neither
`hublb.h` nor its `hublb_next.h` mask/shift companion states a reset for
`LB_RT_LOCAL_CTRL` or `LB_RT_FILTER_CTRL`, in contrast to the explicit "reset to
0x0" and "not reset" wording `hublb.h` itself carries for the scratch registers a
few hundred lines away — the silence is the header choosing not to say, not a gap
in what was read. No file in the pinned Linux SN1 tree
(`linux-history`, tag `v2.5.40`, commit `7570df54ef8c`) reads or writes either
register outside that header; a repository-wide search for `RT_LOCAL_CTRL` and
`RT_FILTER_CTRL` returns only the two declarations. The pinned OpenBSD tree
(`openbsd-sgi`, commit `2df91d80c1`) touches neither name. The *Origin 3000 System
Architecture* book (108-0240-002) names the local block as the RTC signal's
generator on its printed page 2-17, already cited above for the unit's role, and
says nothing about the register's contents at any printed page mentioning a
real-time clock. The IP27 PROM Technical Reference Manual (108-0170-001, a
different, SN0-generation manual) lists `PI_RT_LOCAL_CTRL` only as a recognized POD
debugger register name, with no value attached, on its printed page 50.

**A related register on the Origin 200/2000 Hub is documented, and it is evidence
about that chip, not this one.** The Hub folds the local-block function into its
processor interface as `PI_RT_LOCAL_CTRL` at offset `0x160`
([Hub processor interface datasheet](hub-processor-interface.md)), a 17-bit
register against this one's 33, with its own internal-clock-select bit reset set —
an IRIX-register-table fact for that sibling chip, sourced the way that datasheet's
own provenance section states, and corroborated there by a firmware capture writing
that exact value. It does not transfer to Bedrock: the two registers differ in
width, field layout and which chip owns them, and nothing established for this part
matches it independently. It especially does not supply a maximum-count value —
that datasheet records its own maximum-count field's declared reset as inconsistent
with the field's declared width, and the model discards it, resetting that field to
zero on the Hub as well. No source in hand puts a working number on either chip's
maximum-count field; inference from the sibling is not a documented fact for this
part, per this project's sourcing rule, so none is carried over.

### Scratch registers

| Offset | Name | Fields | Reset | Access | Consumer | Behavior |
|---|---|---|---|---|---|---|
| `0x608000` | scratch 0 | 63:0 | **0** (line 1047) | R/W | the PROM writes `0x0000000052c5878c` after printing `Hub NIC: 0x52c5878c` in the Origin 300 verbose trace; the trace does not identify the value's source or relate it to MicroLAN control | Plain storage. |
| `0x608008` | scratch 1 | 63:0 | not reset (line 1069) | R/W | the PROM writes a flags word here at power-on: `0x0000080690349000` on an Origin 300, `0x6000081690349000` on an Origin 350, `0x6000081690341000` on a Tezro (the three verbose traces); what the bits mean is open. Earlier in the same boot, before that flags word, this register is also the node's inter-processor rendezvous mailbox: every processor clears it on reset, then one processor (the boot master) posts a go-flag (`0x07f80000`) that every other processor polls for with no timeout, observed at `0x9200000001608008` in the IP35 PROM 6.5.30 listing. Later in the boot the same register carries the node-number assignment state (established, disassembly and trace, see the PROM trace notebook (citation: `docs/notebooks/origin300-numalink-prom-trace-20260924.md`)): bits 11:0 hold the assigned number, `0xfff` meaning not yet assigned (`scratch1 \|= 0x4000000fff` at `0x1fc60c24` before discovery); the master writes its own number there at `0x1fc5ccd0`, and writes each peer's through a vector read-modify-write; bit 17 is set when discovery ends (`0x1fc5f3cc`); bit 13 is set and cleared again by `func_1fc5ea08` each time it runs (ten callers; meaning unknown); bits 60:59 are cleared and bit 59 set from the same flag as the region size | Plain storage, with a bounded halting park at this offset (`sgi_bedrock_hold_scratch1_write()`, `hw/misc/sgi-bedrock.c`) approximating same-edge reset and continuous execution on real hardware, under every TCG scheduling mode. See "Reset-barrier park" below and known limitation: the SMP boot rendezvous hold (citation: `docs/known-issues.md#the-smp-boot-rendezvous-hold-approximates-same-edge-reset`). |
| `0x608010` | scratch 2 | 63:0 | not reset (line 1091) | R/W | OpenBSD writes its reboot request here on IP35: magic `0x0ead0000` with a command (halt `0x10`, power down `0x20`, restart `0x30`, reboot `0x40`, enter the interactive monitor `0x50`, which OpenBSD abbreviates EIM and Linux IMODE) and options (no diagnostics `0x100`, no memory init `0x200`) (`ip27_machdep.c` lines 680 to 704; `mnode.h` lines 456 to 466) | Plain storage that survives a reset, consistent with a mailbox the firmware reads after the node-local reset that follows the write; no public source says the firmware reads it. On the Origin 200 the same request goes to the processor interface's error-stack-address register; the Linux SN1 tree still names that register for the purpose and never writes it. The hand-off register decision packet carries it. |
| `0x608018` | scratch 3 | 0 bit | unknown | R/W | the Itanium port writes 0 to release its console lock (`l1.c` lines 214 to 215, 237 to 240) | One-bit storage. |
| `0x608020` | scratch 4 | 0 bit | unknown | R/W | none | One-bit storage. |
| `0x608040`, `0x608048`, `0x608050` | scratch 0, 1, 2 write-if-zero twins | as their registers | twin 0 resets to 0 | write accepted only while the current value is zero (lines 1049 to 1050) | none | |
| `0x608058`, `0x608060` | scratch 3, 4 read-set-if-zero twins | 0 bit | unknown | a read returns the old value and sets the bit if it was zero (lines 1115 to 1116) | the Itanium port takes its hub-wide lock by reading scratch 3's twin until it returns 0 | A test-and-set primitive. The IP35 headers say vector exchange is unsupported because scratch registers do locks directly (`hublb_next.h` lines 99 to 103). |

### Reset-barrier park (scratch 1)

**Observed firmware usage.** In the IP35 PROM 6.5.30 listing every processor of a
node writes `0` to scratch 1 shortly after leaving reset (the disassembly's own
reset-path clear), then one processor -- the node's boot master -- writes
`0x07f80000` there. Every other processor polls scratch 1 for that value with no
timeout. Real processors of one node leave reset on the same clock edge and run
continuously, so on hardware the poller always reads the flag before the
master's next unrelated write could disturb it. A host thread does not run
continuously: under multi-threaded TCG the vCPU implementing a slower
processor's own reset-path clear can be descheduled long enough that it runs
*after* the master's flag write, storing `0` back over the flag. The polling
processor then finds nothing and the firmware proceeds as if that processor
were absent.

**Model behavior.** `sgi_bedrock_hold_scratch1_write()` (`hw/misc/sgi-bedrock.c`)
tracks, per physical processor of the node, whether that processor has written
scratch 1 since reset. A processor's first write always lands immediately and
re-checks whether any processor already parked on the barrier can now be
released. A second-or-later write -- the master's flag post, in the observed
sequence -- is withheld (not yet stored) until every other present,
currently-running processor of the node has made its own first write, or
until 2000 ms of virtual time have passed, whichever comes first: the writer
parks, halting rather than blocking its host thread
(`bedrock_park_start()`), the same mechanism the presence join below uses.
The withheld value lands when the park ends, before that processor's next
instruction, so it can never observe a post it has already taken back. This
restores, for a bounded time, the ordering real hardware's same-edge reset
already guarantees. The park changes *when* the master's second write takes
visible effect, never its value. Because a park halts the processor rather
than blocking a host thread, it applies the same way under every TCG
scheduling mode, and it still requires at least two of the node's processors
present and running. Reaching the 2000 ms bound without every processor's
first write ends only this park: the withheld value still lands on release,
and unlike the publisher and partner parks below, whose own timeout
abandons the whole presence join, a timed-out reset-barrier park does not
(`bedrock_park_expire()`, `bedrock_park_release()`). An access this device
cannot attribute to an owning node's processor -- and a standalone
`sgi-bedrock-lb` under a register-contract qtest, which has no owning
Bedrock node at all (`SGIBedrockLBState.bedrock` is only set by
`bedrock_realize()`) -- sees the same plain read/write contract as before. A
second write there still round-trips immediately, which
`tests/qtest/sn1-bedrock-lb-test.c`'s `sn1/bedrock-lb/scratch1-second-write-no-hold`
names directly. See
known limitation: the SMP boot rendezvous hold (citation: `docs/known-issues.md#the-smp-boot-rendezvous-hold-approximates-same-edge-reset`)
for what is and is not established about this approximation, including this
park's own deviation from the SN0 Hub hold its predecessor mirrored.

**The presence join reads the same writes.** Once a second-or-later write has
reached the register, stored at once or landed from this hold, the model
treats the reset barrier as released, and each processor's next scratch 1
write counts as its presence join. The release itself, the master's
`0x07f80000` stored at `0x1fc01278`, is never a join. In PROM 6.210 every
processor's join is then its write in main at `0x1fc69a00` (established from
disassembly), which comes before its pair liveness post. The join decides
which real-time compare write the liveness park takes for that post; see
the real-time compare row in [the processor interface
datasheet](bedrock-processor-interface.md) and known limitation: the
presence-publication join (citation: `docs/known-issues.md#the-presence-publication-join-approximates-same-edge-reset-at-the-first-barrier`).
The join is model bookkeeping and changes no value.

**Accesses from outside the node while a value is withheld (2026-09-25,
independently reviewed).** On a two-chassis Origin 300 scratch 1 is also
reached from the other node: the global master reads a peer's scratch 1 and
writes the assigned number into its bits 11:0 through the vector engine, and
the peer's processors poll their own scratch 1 for it (see [node number
assignment](#node-number-assignment)). A withheld local post and that remote
write can meet. Landing the post when its hold ends, after the remote write,
would overwrite the node's NASID with the post it had been holding, and the
peer would never see its number. The model instead treats any scratch-1
access it cannot attribute to one of the node's own processors (the peer's
vector engine, a qtest) as ordered after every withheld post: before such a
write or read, every withheld value lands, in the order its processor wrote
it, and the external access follows. The held processors stay parked until
their own join completes; only the values move.

- *Limitation.* This is a model rule about an approximation's side effect,
  not a hardware fact. On hardware no value is ever withheld, so a local post
  always precedes a remote access that arrives after it; the rule restores
  that order. Whether the hardware orders a vector write against a local
  store racing it on the same clock is not established by any source read,
  and the rule does not model such a race.
- *Evidence.* The global master's number delivery is a vector
  read-modify-write of the peer's scratch 1, established from disassembly
  of the stock IP35 PROM in the
  NUMAlink PROM trace notebook (citation: `docs/notebooks/origin300-numalink-prom-trace-20260924.md#15-how-a-remote-hub-is-told-its-nasid-established-from-disassembly-not-exercised-on-one-node`)
  ("How a remote hub is told its NASID"); the peer's own processors use
  the same register as their rendezvous mailbox (the scratch 1 row above),
  so a withheld post and the vector write can meet there.
- *Discriminating test.* `sn1/bedrock-link/peer-scratch1-write-follows-withheld-posts`
  (`tests/qtest/sn1-bedrock-link-test.c`) places withheld posts through the
  test-only `x-test-withhold-scratch1` property, reads and writes the peer's
  scratch 1 through the vector engine, then ends the hold with
  `x-test-release-scratch1`; it fails without the rule (the read returns the
  pre-post value). A qtest cannot be one of the node's processors, and a guest
  program that provokes a real hold races the hold's own wall-clock bound, so
  the state is placed directly. The two properties are test-only and no
  machine sets them.

### Vector operations

| Offset | Name | Fields | Reset | Access | Consumer | Behavior |
|---|---|---|---|---|---|---|
| `0x60c000` | vector parameters | 0 type (a one-bit field in the union, a two-bit mask in the companion header); 23:3 address; 39:32 write id; 50:40 request id; 63 busy | unknown | R/W; a write launches the request | the stock IP35 PROM launches reads with type 0 and writes with type 1 (see "Vector engine on the boot path"); the Itanium port's vector routines are stubs (`stubs.c` lines 163 to 176) | A vector operation sends a read or write along an explicit port-by-port route to a router or another node, used during system initialisation. Sequence: wait for busy to clear, write route and data, write parameters last; the block replies to the write at once (lines 1327 to 1339). |
| `0x60c008`, `0x60c010` | vector route, vector data | 63:0 | unknown | R/W | the stock IP35 PROM writes 0 to both at hub initialisation and then one route (and, for a write, data) per launch | The outgoing route and the write data (lines 1385 to 1408). |
| `0x60c020`, `0x60c028`, `0x60c030` | vector status, return route, read data | status: 2:0 type or error (4 address, 5 command, 6 protection, 7 unknown), 23:3 address, 39:32 write id, 50:40 request id, 61:51 source, 62 overrun, 63 valid | unknown | read; the stock PROM writes `0` to all three at hub initialisation (pcs `0x1fc67764`, `0x1fc67778`, `0x1fc67784`), and seeds read data with `0xdeadbeefdeadbeef` before each launch | the stock IP35 PROM | Software clears the status before launching; a good reply with valid set marks overrun; otherwise the fields load and return route and read data update (lines 1427 to 1518). What a write does on hardware is unknown. Model choice: a write stores its value into read data and return route, and a write of 0 clears status; a nonzero write to status is logged as unimplemented and ignored, so no write can fabricate a reply. |
| `0x60c038` | vector status clear | as status | unknown | read-to-clear [inferred from established firmware use]: the stock PROM reads this register before every launch and never writes it, then treats a status still valid as a failed operation (`func_1fc1663c`, `func_1fc163a4`, `func_1fc29054`); if the read did not clear, every operation after the first would fail. The header is silent on read versus write (lines 244 to 247, 1535 to 1551); the Origin 200's sibling is read-and-clear (mainline `sn0/hubni.h` line 41) | the stock IP35 PROM | A read returns the status and clears it (return value per the header's "as status"; the firmware discards it). A write also clears: the model's retained choice, with no consumer that writes it. |

### Vector engine on the boot path

**The launch, poll and result sequence was independently reviewed on
2026-09-14; the rest of this section is the 2026-09-24 delta.** It replaces
the earlier one-node self-route policy, which the firmware contradicts.

**The primitive [established, disassembly].** The boot-path read and write
primitives (`func_1fc1663c` and `func_1fc163a4`, called through
`func_1fc5875c` and `func_1fc58714` with the node's own number) address the
engine through the remote form of the node's own registers,
`0x9200000001800000 + (n << 33) + 0x60c0xx`. Per attempt, up to eight:

- read status clear (`0x60c038`), then read status; if valid is still set,
  give up on the whole operation;
- poll parameters bit 63 (busy) up to 256 times with no delay, then wait
  100 us if it never cleared;
- for a read, seed read data with `0xdeadbeefdeadbeef` and write the route;
  for a write, write the data and then the route; then parameters last:
  address in 23:3, write id in 39:32, request id in 50:40 (`0x1c8` for reads,
  `0x7b` for writes), type 0 read or 1 write;
- poll status up to 256 times, 100 us apart, for valid.

**The reply echoes the request type [established, disassembly].** A reply is
accepted only with overrun clear and the launched request id, write id and
address echoed in their fields, and with bits 2:0 equal to the launched type:
the read primitive requires 0 (`0x1fc16858`), the write primitive requires 1
and returns a failure for anything else, 0 included (`0x1fc165ac` to
`0x1fc165d8`). Values 4, 5 and 6 are reported as the address, command and
protection errors. So the field the header calls "type or error" carries the
type on success. For a read, read data is then the result. The whole budget is about 205 ms per operation. The
POD primitive `func_1fc29054` is the same shape through the local window with
request id `0x123` and a 10,000-iteration poll, and it also seeds the return
route with `0xdeadbeefdeadbeef`.

**Route encoding [established as firmware behavior].** A route is a string of
four-bit port numbers, consumed first-hop-first from the least significant
nybble: discovery extends a route by `port << (4 * hops)` (`func_1fc58268`).
Route 0 reaches the device at the other end of the node's own cable. The link
test (`func_1fc5f798`) makes 32 vector reads of `0x600000` with route 0, then
checks the port errors register's check-bit count (bits 15:8, six or more is a
bad link); discovery (`func_1fc272dc` through `func_1fc26f70` and
`func_1fc26ca4`) reads `0x600000` with route 0 and dispatches on the part field
(bits 27:12): `0x3012` adds a hub, `0x3017` a router whose ports it walks,
anything else is reported and dropped. This matches the Origin 200 rule that
vector 0 is the immediately connected device.

**No launch without a link [established, trace].** Both routines first test
network-interface port status bit 1 and skip the engine when it is clear. The
traced one-node boot launched no vector operation at all, so one-node
discovery does not read its own hub through the engine; the "self route" the
earlier revision required is not something this firmware uses.

**Writes [established, disassembly; not executed on one node].** The master
vector-writes a peer's revision word (headless hubs) and a peer's scratch 1
(hubs with processors), each after a vector read of the same register (see
"Node number assignment"). The router-register writes named by POD strings are
outside a direct pair.

**Unknown:** the value hardware puts in the return-route register (the
firmware stores it and the traced code does not test it); the source field
(61:51), which the firmware does not test; what a nonzero route does on a
direct pair with no router; what a launch does with no link; and the reset
values of busy and valid.

**Model contract.**

- Route 0 is delivered across the node's cable to the peer Bedrock, which
  performs the read or write on its own hub register block at the address
  field and answers. The launcher's status gets valid with the launch's
  request id, write id and address echoed, and bits 2:0 equal to the
  launched type (0 read, 1 write); a read also loads read data. Completion is immediate, well inside the firmware's budget.
- A peer that refuses the address answers with the address error (4) in the
  type-or-error field. The error code for a refused register is not observed;
  4 is the header's address-error value and the Origin 200 precedent, recorded
  as the model's choice.
- No cable, a cable marked down, or a nonzero route on a direct pair: no reply
  is latched (status stays not valid, so the firmware times out and reports
  the failure) and the launch is logged as unimplemented. What hardware
  returns there is unknown; answering from the local hub, as the self-route
  policy did, would present the node as its own neighbour and is removed.
- A launch while status is still valid sets overrun and does nothing else, as
  before.
- Return route and source read 0 after a reply.

## Behavioral contract

For a one-chassis boot the block must: answer the revision register with part
`0x3012`, revision 1 or 2, the node's number and region size 0, latch a written
node number and region size and ignore writes to its identity fields; clear scratch 0 and keep scratch 1 and 2 across the reset the
header describes, with scratch 3 and 4 unknown; implement the write-if-zero and
read-set-if-zero twins so the Itanium port's lock works and a plain latch is
rejected; hold the error bits across reset and clear them through the mask-clear
register; and drive the processor interfaces' real-time counters at 1,250,000 Hz,
the SN1 header's default (`sn1/ip27config.h` line 50), which the firmware's
configuration block also carries and every consumer assumes. Which reset the scratch
narrative means is not named: this block has its own soft-reset register, the
processor-interface header speaks of a soft reset, and the network interface's
local-reset bit is a third thing. The model equates the network interface's local
reset with that reset; that is the model's equation, settled by the PROM trace
(scratch values across the PROM's own early local reset) or a hardware read. The
permission registers should read permissive; no public consumer writes them and
every consumer's first access assumes its region is allowed. With no link the
stock PROM launches no vector operation; over a cable it reads and writes the
peer with route 0 and clears status by reading the status-clear register
(read-to-clear is inferred from that use), so the engine must deliver route 0
to the peer, echo the request type, and clear on that read. The real-time
clock local and filter control registers reset to all zero; no permitted source
states either register's power-on contents, so this is a model choice rather than
a hardware fact, and it is known to make the stock PROM's own real-time conversion
arithmetic run about eighty times short (see the real-time clock source section
above and jamesb/qemu#581).

## Interrupts

Error bits assert the block's error line into both processor interfaces as pending 1
bit 55. No other interrupt originates here.

## DMA

None.

## Remaining implementation and qualification

Vector execution is required for a cabled pair: the stock PROM reads and
writes the peer with route 0. Multi-hop routes through a router, the return
route and the source field remain open. Debug behavior and
clock-loss filtering remain unqualified beyond the implemented register behavior.
The MicroLAN register layout is
bounded, while the access and controller transaction behavior above are model
choices. Endpoint family, commands and identity content remain open and block
endpoint implementation. A public slot trace, a public firmware implementation
or a matching IP45 component document would settle them.
Diagnostic coverage remains required for the other unresolved functions.

## Behavioral notes

**Who decodes the L1 UART is not this unit by any public statement.** The UART sits
in Hub special space at the register window plus `0x80`; the header for this unit
names no UART, and the Origin 200's equivalent port was in the memory-directory unit.
Open; the L1 datasheet carries the address contract.

The OpenBSD board-identification read is not assigned to this unit. Its product
code is consumer evidence, while the decoder and full register behavior remain
unestablished. See the independent board-identification evidence (citation: `docs/library/origin300-identity-evidence.md`).

**Conflicts with settling observations.** The manufacturer field is `0x036` in the
model, from the two public widget tables; read the whole word on an Origin 300 to
confirm it. Whether the firmware reads scratch 2 after reset:
trace the held IP35 PROM on warm reset. The vector-parameters type width: not
boot-critical. What the firmware writes into the clock-control registers: read them
at PROM hand-off, read the configuration block's frequency field from the held PROM,
and time the counter against the processor's count. The vector status clears on a read
of the status-clear register, inferred from the stock PROM's use (see the row);
a Bedrock specification or a hardware read would confirm it.

## qtest checklist

- The revision register reads part `0x3012` in bits 27:12, revision 1 or 2 in 31:28, the node number in 39:32 and region size 0 in 47:46. A 64-bit write of the word with a different node number and region size 1 reads back with both changed and the identity fields unchanged; a write of `0` leaves the identity fields intact; a write with region size 3 is refused; a reset restores the realized node number.
- After a write of node number `n`, a crosstalk register packet addressed to node `n` reaches this Bedrock's registers through the I/O interface and one addressed to the old number does not (a test of the model's one-number-per-chip choice).
- Permission registers retain writes; with the writer's region bit first cleared and read back clear (the test states which region its accesses belong to), the override with the right pattern sets it and a wrong pattern does not.
- After a node-local reset through the network interface, which the model equates with the header's reset, scratch 0 reads 0 and scratch 1 keeps its value.
- Scratch 0 to 4 retain writes; a write to a write-if-zero twin takes effect only when the register is zero; a read of a read-set-if-zero twin returns the old value and sets the bit, and a write of 0 to the register releases it.
- A write of a set bit to the error-bits register asserts pending 1 bit 55 in both processor interfaces, which a pending-modify write cannot clear; a write of the same bit to the mask-clear register clears it; header 1's valid bit follows a write.
- Region present and nodes absent retain writes with the sub-register widths stated.
- The controller model-choice test writes all six MicroLAN fields in one 64-bit
  access. Immediately after the write completion is clear. After the positive
  virtual slot delay completion is set and sampled data is the level returned
  by the bus. A 32-bit handler would lose phase-divider bits 33:32, so the test
  writes a nonzero value there.
- After evidence review clears an endpoint contract, one MicroLAN register test
  performs its reset-and-presence sequence, sends the cleared ROM command, and
  reads the configured identity. Repeat the test with two synthetic identities
  that differ in several identity and check bits. Each reply must equal its
  configured value and include both zero and one bits. Together with the
  completion transition above, these cases fail an empty bus, a controller
  whose completion bit is stuck, and a controller whose sampled value is fixed.
  Do not assert Read ROM `0x33`, family `0x01`, or Dallas CRC-8 until independent
  review clears those XBridge-derived endpoint choices for IP45.
- Reset while a MicroLAN slot is pending cancels its timer. Completion must not
  appear after reset. A later 1-Wire reset starts a fresh ROM-command sequence.
- A migration test saves once between start and completion. After an endpoint
  contract is cleared, it also saves partway through the cleared identity
  command. After load the slot completes exactly once and the command resumes
  at the same identity bit.
- The clock-control registers retain writes to their defined fields; reserved bits read 0.
- Both clock-control registers read zero at power-on and after the block's own soft
  reset, asserting the model's chosen value rather than leaving it unstated.
- Vector route and data retain writes; writes of 0 to status, return route and read data are accepted without a guest error and clear them.
- A read of status clear returns the status and clears valid; a write clears it too.
- With no cable, a launched read latches nothing: status stays not valid and read data keeps its sentinel.
- Two Bedrocks joined by one cable (the acceptance case discovery depends on): each side sets a distinct value in its own scratch 0; on side A, seed read data with a sentinel, write route 0, write parameters with a read of `0x608000`. Status reads valid with bits 2:0 equal to 0 and the request id, write id and address echoed, and read data reads B's scratch 0, not A's. A second launch after reading status clear succeeds; a second launch without it sets overrun.
- Over the same cable a vector write of `0x608008` with route 0 replies valid with bits 2:0 equal to 1 and changes B's scratch 1 and not A's, and a read-modify-write of B's revision word through the engine changes B's node number.
- A vector read of an offset B's register window does not decode replies valid with bits 2:0 equal to 4 (the model's choice of error code) and leaves read data at its sentinel; return route and source read 0 after every reply.
- A nonzero route over the direct cable, and any route with the cable marked down, latches nothing.
