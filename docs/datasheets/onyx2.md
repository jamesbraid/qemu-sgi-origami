# Datasheet: Onyx2 deskside and rack, the product contract

References rendered as citation identities name material outside this document
set. Those targets are not included or cleared for redistribution. Review and
runtime statements retain their recorded dates and scope.

**Status:** draft, awaiting independent evidence review. Nothing here clears
a model change on its own.

**QEMU file(s) on `sgi-origami`:** `hw/mips/sgi/sn-machine.c` builds both
chassis from `hw/mips/sgi/sn-catalogue.json`. The InfiniteReality widget is
described in the
[host interface](infinitereality-host-interface.md) and
kernel attach (citation: `docs/datasheets/infinitereality-kernel-attach.md`) datasheets.

**Sources used:**

- SGI *Onyx2 Deskside Workstation Owner's Guide*, `007-3454-005`, revision
  005.[^desk]
- SGI *Onyx2 Rackmount Owner's Guide*, `007-3457-005`, revision 005.[^rack]
- SGI *Origin2000 Deskside Owner's Guide*, `007-3453-002`, revision 002.[^o2k-desk]
- SGI *Origin and Onyx2 Theory of Operations Manual*, `007-3439-002`,
  revision 002.[^theory]
- SGI *Origin and Onyx2 Hardware Quick-reference Booklet*, `HMQ-380-C`.[^hmq]
- SGI *Onyx2 Reality, Onyx2 InfiniteReality and Onyx2 InfiniteReality2
  Technical Report*, August 1998.[^techrep]
- Five real-hardware inventories: an Onyx2 deskside PROM `hinv -v`
  (humble-kiwi),[^kiwi] two Onyx2 deskside IRIX inventories with `gfxinfo`
  and board lists (whole-tetra, fancy-akita),[^tetra][^akita] an Onyx2
  rack IRIX inventory with `gfxinfo` and board lists (true-wombat),[^wombat]
  and an Origin 2200 board list (model-toad).[^toad]
- Disassembly of the IP27 PROM 6.156 payload shipped with IRIX 6.5.30 and of
  the IRIX 6.5.30 `unix.IP27` kernel, with `tools/origami disasm`.[^disasm]
- Linux `arch/mips/include/asm/xtalk/xwidget.h` and OpenBSD `xbowdevs` for
  the widget part and manufacturer numbers.[^linux][^openbsd]

**Clean-room statement.** The author read no IRIX or PROM source, no
source-reader notebook and no Library document that reasons from vendor
source. Kernel routine names come from the kernel's own symbol table. PROM
routine names on this page are borrowed from the kernel routines they match
instruction for instruction. One sibling datasheet read for context,
[the Hub memory/directory datasheet](hub-memory-directory.md), cites vendor
register-table field names in its slot-identity section. This page uses none
of them.

**Evidence labels.** Every fact is marked *established* (printed in a cited
manual, shown by a real-hardware capture, or read directly from disassembly),
*inferred* (follows from established facts but is not itself printed or
observed), or *unknown*.

## Role

An Onyx2 is Origin 2000 silicon with a graphics pipe. The node boards, Hub,
Crossbow, Router and Bridge are the Origin 2000 parts. The differences are
physical: a graphics BaseIO (IO6G) in XIO slot 1 and one or more
InfiniteReality pipes. The deskside and the rack attach the pipe in two
different ways, and firmware and IRIX name the slots of the two with
different tables.

- **Deskside** (CMN A015): its own midplane, `4P1G5_MPLN`, with two node
  slots, a Null Router, five XIO slots and four graphics slots. The pipe's
  geometry board sits on the midplane and takes XIO slot 4 directly. There is
  no Ktown.
- **Rack** (CMN A017): an ordinary eight-processor compute module on the
  `8P12_MPLN` midplane, plus a separate graphics module holding up to two
  pipes. Each pipe reaches the compute module over a Crosstown cable from an
  Xtown board in a compute-module XIO slot to a Ktown board in the graphics
  module.

This page owns the chassis, slot and widget contract. The pipe's register
interface belongs to the InfiniteReality datasheets, the BaseIO-G board to
[the BaseIO reference](baseio.md), and the crossbar to [the Crossbow
datasheet](xbow.md).

## Product identity

| Item | Deskside | Rack | Status and evidence |
|---|---|---|---|
| Regulatory model | CMN A015, shared with the Origin 2000 deskside enclosure | CMN A017 | Established.[^desk-intro][^o2k-desk][^rack-intro] The number names the enclosure, not the Onyx2. |
| Midplane board record | `4P1G5_MPLN`, part `013-1839-001` rev F | `8P12_MPLN`, parts `030-1535-001` rev B, `030-0762-006` rev K and `013-1547-003` rev D in one three-module system | Established from IRIX board lists.[^tetra][^akita][^wombat] |
| Node boards | One or two, up to four processors | Up to four per compute module, 2 to 8 processors in a single rack, up to 64 across racks | Established.[^desk-intro][^rack-intro] |
| Node interconnect | Null Router, no external ports | Star and Rack Routers | Established.[^theory-router][^rack-router] |
| Graphics | One pipe, one or two RMs | One or two pipes per graphics module, up to 16 pipes across racks | Established.[^desk-intro][^desk-gfx][^rack-intro][^rack-gfxmod] |
| Base I/O | BaseIO-G (IO6G) in XIO slot 1 | BaseIO-G (IO6G) in XIO slot 1 of the compute module | Established.[^desk-xio][^rack-xio] |
| Graphics BaseIO records | `BASEIO` `030-0734-002` with an `MIO` `030-0880-003` daughterboard | The same two records | Established.[^tetra][^wombat] |

The graphics boards are specific to Onyx2 midplanes and may not be moved to
any other SGI system. *Established.*[^desk-gfx-note]

## Deskside

### Slots

The deskside midplane carries, from the rear: node slots 2 and 1, four
graphics slots (GE, RM/TM, RM/TM, DG5), and five XIO slots numbered 1, 2, 3,
5 and 6. XIO slot 4 is the graphics board set. A Null Router joins the two
node boards and has no external connectors. *Established.*[^desk-slots][^desk-null][^rack-router]

XIO slot rules, all *established*:[^desk-xio]

- The BaseIO-G stays in XIO slot 1.
- The optional three-slot PCI module goes in XIO slot 2, and a SCSI board
  never does.
- Slots 3 and 5 fill first.

The same guide says "there is no functional XIO connection below XIO slot 3".
Its Figure 1-4 draws the position under XIO 3, where an XIO 4 slot would sit,
as a solid blank. The reading is that this one physical position has no
connector because the graphics set uses XIO 4, not that `io2` or `io6` are
dead. A real deskside runs boards in `io2` and `io6`, which fits that
reading. *Inferred.*[^desk-slots][^tetra]

### Widget-to-slot map

A deskside has one Crossbow. Firmware and IRIX map its widgets to slots with
a table that exists only for this chassis:

| Widget | Slot | Holds |
|---|---|---|
| 8 | `io4` | The graphics pipe (GE board) |
| 9 | `n2` | Node board 2 |
| A | `n1` | Node board 1 |
| B | `io2` | XIO slot 2 (PCI module) |
| C | `io6` | XIO slot 6 |
| D | `io5` | XIO slot 5 |
| E | `io3` | XIO slot 3 |
| F | `io1` | BaseIO-G |

*Established* by disassembly: the IP27 PROM table at `0x1fcd1dd0` and the
kernel's byte-identical copy at `0xc0000000014870e0`.[^disasm] A real deskside
PROM `hinv -v` prints `BASEIO IO Board, Module 1, Slot io1` at `(widget 15)`,
an `XTALK_PCI` board in `io5` at `(widget 13)`, and `KONA Graphics Board,
Module 1, Slot io4`, matching the table.[^kiwi]

The node rows come from disassembly only, but two independent kernel tables
agree on them. `hub_slot_to_widget`, which maps a node slot to its Crossbow
port, has its own deskside table at kernel table base +0xd0: `n1` at widget
A and `n2` at widget 9. `slot_to_widget`, the inverse for `io` slots, has a
deskside table at +0x1b0: `io1` to `io6` at F, B, E, 8, D, C. *Established*
by disassembly.[^disasm] That node board 2 is physically cabled to widget 9
is *inferred* from those tables.

The pipe therefore attaches at widget 8. This differs from every other SN0
map: on an eight-processor module's Crossbow 0, widget 8 is `io1`.

### Node slot codes

The deskside's two node slots have their own slot codes, `0x51` for `n1` and
`0x52` for `n2`, and both firmware and kernel name them `n1` and `n2`. The
PROM's decoder at `0x1fc461fc` and the kernel's `hub_slotbits_to_slot` turn
the low three bits of the Hub slot-identity register into those codes: raw 3
gives `n1` and raw 2 gives `n2`, where an eight-processor module uses raw 7
to 4 for `n1` to `n4`. *Established* by disassembly.[^disasm] A real deskside
IRIX `hinv` prints node memory as `Module 1/Slot 81` and `Slot 82`, the two
codes in decimal.[^tetra] The [Hub memory/directory
datasheet](hub-memory-directory.md#origin-2000-slot-and-processor-identity)
already records raw 2 and 3 as "a different node class". This page names the
class: the Onyx2 deskside.

Only the low three bits decode. Raw 0 and 1 decode as invalid (`0xf0`).
What a deskside midplane drives on the upper bits is *unknown*.[^disasm]

The deskside codes need bit 0 of the word at physical `0x100000a8` clear.
That word is the configuration-flags field of the boot flash's
[firmware configuration block](address-map.md#the-firmware-configuration-block),
at flash offset `0xa8`, beside the machine-type field at `0x90`. With the bit
set, raw 3 and 2 decode as ordinary node slots 5 and 6 instead. A deskside's
flash must therefore carry the bit clear, and the real deskside's
`Slot 81/82` shows it clear. *Established* by disassembly and
capture.[^disasm][^tetra] What the bit means otherwise is *unknown*.

### The two-node deskside

No inventory lists the Null Router. Neither the deskside PROM
`hinv -v` nor the IRIX inventory lists a Router, where the rack lists `r1`
and `r2`. *Established.*[^kiwi][^tetra][^wombat]

PROM routine `0x1fc470a0`, called at `0x1fc4ff88`, probes the partner node.
For a deskside slot code it tries an empty route, then a one-hop route out
of port 4 or 5 chosen by slot parity. Otherwise it routes through Router
port 6. *Established* by disassembly.[^disasm] That this models a direct
Hub-to-Hub link through the Null Router is *inferred*.

In both deskside captures `n2` holds NASID 0: humble-kiwi prints `n2` with
NASID 0 and `n1` with NASID 1, and whole-tetra puts CPUs 0 and 1 on `n2`.
*Established.* What makes `n2` NASID 0 is *unknown*.[^kiwi][^tetra]
whole-tetra's IRIX `hinv` also prints `IO6prom on Global Master Baseio in
Module 1/Slot io2` while the BaseIO's hardware-graph path is `io1/baseio`.
Why the line says `io2` is *unknown*.[^tetra]

## Rack

### Compute module

The compute module is the eight-processor module with four node slots, two
Router slots, twelve XIO slots and two Crossbows. Node 1 activates XIO slots
1 to 6 and node 2 activates XIO slots 7 to 12. Nodes 1 and 3 reach slots 1 to
6 through Crossbow 0, and nodes 2 and 4 reach slots 7 to 12 through
Crossbow 1. The BaseIO-G stays in XIO slot 1 and the PCI option in slot 2.
*Established.*[^rack-nodes][^rack-xio][^hmq-xtown]

| Widget | Crossbow 0 | Crossbow 1 |
|---|---|---|
| 8 | `io1` (BaseIO-G) | `io11` |
| 9 | `n1` | `n4` |
| A | `n3` | `n2` |
| B | `io4` | `io8` |
| C | `io2` | `io10` |
| D | `io6` | `io12` |
| E | `io5` | `io7` |
| F | `io3` | `io9` |

*Established* by disassembly: PROM tables at `0x1fcd1df0` (Crossbow 0) and
`0x1fcd1e10` (Crossbow 1), and the kernel copy.[^disasm] The booklet's xbstat
table agrees on rows B to F. It labels widget 8 "BaseIO" only, with no
Crossbow 1 entry.[^hmq-xbstat] It prints the node rows as
"Node 1 or 2" at widget 9 and "Node 3 or 4" at widget A, where the PROM puts
`n4` at 9 and `n2` at A on Crossbow 1. The booklet's pairing reads as loose
and the PROM's as the operative naming. That reading is *inferred*.

`io9` is Crossbow 1 widget F. *Established.*

### Graphics module

The graphics module holds up to two pipes. Each pipe's boards plug into the
rear of the graphics-module midplane. The Ktown board sits between the two
board sets. The right-hand pipe (seen from the rear) takes one, two or four
RM/TM assemblies and uses the Ktown's upper connector. The left-hand pipe
takes one or two and uses the lower connector. *Established.*[^rack-gfxmod][^rack-midplane]

The booklet names a "KCAR midplane" in the graphics troubleshooting chain,
between the Ktown board and the GE14.[^hmq-trouble] That this is the
graphics-module midplane is *inferred* from its place in that chain. No
capture shows a graphics-module midplane record.

### How a pipe reaches the compute module

Each pipe needs its own Crosstown cable from a Crosstown XIO board ("Xtown")
in a compute-module XIO slot to the Ktown board. *Established.*[^rack-xtown]
The Crosstown board converts the XIO link's on-board signalling to
differential levels for a cable of up to 3 meters. *Established.*[^theory-xtown]
That manual calls the XIO-slot board "the Crosstown (KTOWN) conversion
board". The rack guide and the captures use KTOWN for the graphics-module
board, so this page calls the XIO-slot board Xtown and keeps KTOWN for the
graphics-module end.
The host's path ends at the GE board's XIO-to-graphics ASIC (XG). The booklet
calls it the "XBOW-to-GE14 path" and lists the midplane, the Xtalk XIO board,
the Crosstown cable, the Ktown board, the KCAR midplane and the "GE14
(XGASIC)" as the parts on it. *Established.*[^hmq-trouble][^desk-ge]

The Xtown and Ktown are therefore link extenders, and the XIO widget at
the end of the link is the XG on the GE board, as on the deskside. That the
Xtown and Ktown present no widget identity of their own is *inferred*: nothing
in the captures or the disassembly reads one.

### Xtown placement and pipe limits

A node controls at most one pipe. *Established.*[^hmq-xtown] The booklet's
recommended Xtown slots:

| Nodes | Pipes | Xtown slots | Crossbow and widget |
|---|---|---|---|
| 1 | 1 | Any of `io2`–`io6` | Crossbow 0, widgets C, F, B, E, D |
| 2–4 | 1 | `io7` | Crossbow 1 widget E |
| 2–4 | 2 | `io2`, `io7` | Crossbow 0 widget C, Crossbow 1 widget E |
| 3 | 3 | `io2`, `io7`, `io8` | adds Crossbow 1 widget B |
| 4 | 3 | `io2`, `io7`, `io8` | as above |
| 4 | 4 | `io2`, `io3`, `io7`, `io8` | adds Crossbow 0 widget F |

The slot columns are *established*.[^hmq-xtown] The widget column follows from
the map above. The recommended second pipe sits at `io7` (Crossbow 1
widget E), not at `io9`.

The real rack capture has its one pipe at `io3`, Crossbow 0 widget F, in a
four-node module.[^wombat] So the table is a recommendation, and the pipe
takes whatever XIO slot its Xtown occupies. The rack guide's block diagram
draws pipe 0 on the first Crossbow and pipe 1 on the second.[^rack-block]

Multirack systems support up to 16 pipes. The rack guide's RealityMonster
example has up to eight pipes across racks, and each pipe needs one node
board. *Established.*[^rack-intro][^rack-config]

## Node-board populations

Both chassis ship with fewer node boards than they hold, filled from `n1`.

**Deskside: one or two node boards.** The standard deskside has one node
board, and the entry level one processor; a second node board is an option.
The first node board always sits in the right-hand slot, `n1`.
*Established.*[^desk-intro][^desk-first] With one node board `n1` is the only
Hub on the one Crossbow (widget A), so it controls every widget: the BaseIO-G
at F, the pipe at 8 and XIO slots 2, 3, 5 and 6. That is *inferred* from its
being the only Hub. Widget 9, where `n2` would be, is an empty port. The Null
Router joins two node boards and carries nothing firmware reads, so with one
node board the model fits no link at all. The rack guide says the Null Router
is used in desksides with one or two node boards, so a real one-node deskside
probably carries one, its far end empty; that the two are firmware-equivalent
is *inferred*, and so the model's one-node row is.[^rack-router2]

**Rack: one to four node boards.** A single rack holds 2 to 8 processors, and
the booklet lists a one-node, one-pipe system with N1 controlling the IO6G and
the Xtown in IO2 to IO6. *Established.*[^rack-intro][^hmq-xtown] The compute
module's rows are the Origin 2000 rack module's: no Router with one node
board, both Standard Routers from two
([the module's populations](origin2000-module.md#node-board-populations)).
The rack guide names the Null Router for one or two node boards and the Star
for a single-rack Onyx2 with three or four; true-wombat's module 1 carries
Standard Routers, with two node boards and later with four.
*Established.*[^rack-router2][^wombat] The Star fit is documented but not
modelled, for the reason the
[module datasheet](origin2000-module.md#the-star-router-and-why-the-model-does-not-fit-it)
gives. With one node board the model fits no Router; the guide says none is
required, and no capture shows one, so that row is *inferred*. The pipe's Xtown in IO3 is Crossbow 0
widget F, which `n1` controls at every count, and one pipe needs only one node
board. *Established.*[^hmq-xtown]

## How firmware and IRIX choose the widget map

There is no machine-type word for Onyx2. The PROM routine at `0x1fc419cc`
and the kernel's `xbwidget_to_xtslot` at `0xc00000000003ca18` pick a table
from the hardware, in this order. *Established* by disassembly.[^disasm]

1. Widgets outside 8 to 15 give `0xf0`, an invalid slot.
2. The routine reads the local node's slot code (`get_my_slotid`). A code of
   class `0x5`, an Onyx2 deskside node slot, selects the deskside table. Only
   Crossbow 0 is legal on a deskside. Any other Crossbow prints an error and
   gives `0xf0`.
3. Otherwise a nonzero machine-type word at physical `0x10000090`, the
   [Origin 200 case](origin2000-cpu-reset-mode.md), selects the Origin 200
   GIGAchannel table.
4. Otherwise the Crossbow number picks the Crossbow 0 or Crossbow 1 table of
   the eight-processor module. An Onyx2 rack takes this path and is named
   exactly as an Origin 2000.

The kernel's `hub_slot_to_crossbow` returns Crossbow 0 for every deskside
node slot and alternates Crossbows for `n1`–`n4` otherwise. *Established*.[^disasm]

## Pipe identity

### Widget part number

The pipe presents XIO manufacturer `0x2aa` and part `0xc102`, named KONA in
Linux and OpenBSD. *Established.*[^linux][^openbsd]

- The PROM's graphics classifier at `0x1fc4291c` gives part `0xc102` board
  type `0x51` and name `kona`. It gives MGRAS `0xc003` and Odyssey `0xc013`
  their own types and every other part `unknown_gfx`.
  [Host interface datasheet](infinitereality-host-interface.md#widget-identity-revision-and-the-board-type-classifier).
- The kernel's `kona_init` registers the `kona` driver for exactly one
  identity, part `0xc102` with manufacturer `0x2aa`, at
  `0xc000000000415d58`. *Established*.[^disasm]

The deskside pipe presents the same identity. Its PROM prints `KONA Graphics
Board, Module 1, Slot io4`, and the classifier names a board `kona` only for
part `0xc102`. The same deskside's IRIX `gfxinfo -v` reports
`Graphics board 0 is "KONAL" graphics` with its GE, RM, TM and DG records,
which only the `kona` driver, registered for `0xc102`/`0x2aa`, produces
once attached. The `/hw/module/1/slot/io4/kona` path alone could come from
the board type. *Established* by capture and disassembly.[^kiwi][^tetra][^disasm] Both chassis end the host
path at the same XG ASIC on the GE board, which accounts for this.
*Inferred.*

### What IRIX accepts as a pipe

`kona_attach` checks the widget's `+0x64` field as the
kernel attach datasheet (citation: `docs/datasheets/infinitereality-kernel-attach.md#attach`) records.
It then looks the widget up in the table `kona_init` built. *Established* by
disassembly:[^disasm]

- The kernel builds its own graphics-pipe list from firmware-written
  KLCONFIG records. The list head is zero in the image. On first use
  `find_gfxpipe` (`0xc000000000406c6c`, build at
  `0xc000000000406ccc`–`0xc000000000406f34`) scans each node's KLCONFIG
  graphics-class (`0x50`) boards that carry a graphics component. It keys
  each entry on module and `io` slot and inserts in ascending order of
  module, then slot.
- `kona_init` (`0xc000000000415cd4`) asks `find_konapipe` for each list
  index up to `GfxBoardLimit` (128 in the image). `find_konapipe` requires
  board type `0x51` and takes the widget number and node from the board's
  graphics component, as firmware wrote them.
- `kona_attach` matches the attaching widget against that table on widget
  number, then the module and Crossbow of the entry's node. The check starts
  at `0xc000000000416c70`.
- With no match it prints `unexpected graphics pipe: node 0x%x, widget 0x%x`
  (format at `0xc000000000472e28`, call at `0xc000000000416fcc`) and returns
  without attaching.

The kernel has no deskside or rack test in this path. The (node, widget)
pairs it accepts are the ones firmware wrote into KLCONFIG. On a deskside those pairs come from
the deskside table, so the pipe must be widget 8 for firmware to name its
board `io4` and for the kernel to find it.

### One-wire records

IRIX reads the pipe's board records through the pipe itself.

| Record | Deskside | Rack |
|---|---|---|
| GE | `GE14-4` `030-1129-002`, or `GE14-2` | `GE16-4` `030-1398-001` |
| KT (Ktown) | Absent. `gfxinfo -v` prints `KT: No NIC serial number available.` | `KTOWN` `030-1067-001` rev F, under the pipe's slot |
| RM, TM | RM7 `030-1054-001`, TM7 `030-1053-001` per assembly | RM9 `030-1402-001` |
| DG | DG5-2 `030-1087-001` | DG5-2 `030-1242-001` |
| BP | `No NIC serial number available.` | `No NIC serial number available.` |
| Xtown | not applicable | No record in the board list |

*Established* from the captures.[^tetra][^akita][^wombat] IRIX lists the rack's
Ktown record at `/hw/module/1/slot/io3/kona`, the Xtown's XIO slot, beside the
GE record. It lists only the GE record at the deskside's `io4/kona`. The
[host interface datasheet](infinitereality-host-interface.md#the-inventory-the-guest-derives-from-the-pipe)
owns how the records are read.

## Telling an Onyx2 from an Origin 2000

No firmware or kernel word says "Onyx2". Recognition is by slot code.
*Established* by disassembly.[^disasm] These routines test for slot class
`0x5`, each in the kernel and in the PROM:

- `hub_slotbits_to_slot` (PROM `0x1fc461fc`) produces the class.
- `xbwidget_to_xtslot` (PROM `0x1fc419cc`) picks the deskside widget table.
- `get_slotclass` (PROM `0x1fc06d70`) names class `0x5` slots `n`.
- `hub_slot_to_widget` (PROM `0x1fc450f4`, table `0x1fcd1ea8`) maps `n1` to
  widget A and `n2` to widget 9.
- `hub_slot_to_crossbow` (PROM `0x1fc2702c`) always answers Crossbow 0.
- `slot_to_widget` (PROM `0x1fc06790`, table `0x1fcd1f88`) maps `io` slots,
  and its deskside path skips the bounds check.
- PROM `0x1fc470a0` changes the partner-node probe, as
  [above](#the-two-node-deskside).

The Origin 2000 deskside is not the Onyx2 deskside. It is the
eight-processor module in a deskside enclosure: four node slots, two Router
slots taking a Null, Star or Standard Router, two Crossbows and twelve XIO
slots. It uses the Crossbow 0 and 1 tables, never the deskside one, and a
two-node Origin 2000 deskside with a Null Router is still that module.
*Established.* Its nodes report codes `0x01` and `0x02` in the captures;
that they read raw slot values 7 to 4 (codes `0x01`–`0x04`) is *inferred*
from the decoder.[^o2k-desk][^o2k-null][^theory-desk][^hmq-mix][^toad]

- An Onyx2 deskside is recognized by its node slot codes, `0x51` and `0x52`.
  Only its `4P1G5_MPLN` midplane, part `013-1839-001` or
  `030-1066-002` or later,[^hmq-mix] gives class `0x5`. That the midplane drives raw
  slot values 3 and 2 is *inferred* from the decoder and the captured
  `Slot 81/82`.
- A rack compute module is an Origin 2000 module. What makes it an Onyx2 is
  the hardware in it: a BaseIO-G with MIO, one or more Xtown boards, and KONA
  widgets behind them.

A model therefore builds an Onyx2 from components: the deskside by its
midplane's slot values and a KONA widget at widget 8, and the rack by an
Origin 2000 module with BaseIO-G and KONA widgets on Xtown slots.

## The Technical Report's deskside Ktown label

The Technical Report's deskside figures (PDF pages 12 and 13) draw a chain
from the XIO slots through a block labelled KTOWN to the GE.[^techrep] The
same report says on PDF page 74 that desksides use the XIO protocol and racks
the Crosstown protocol.

Every other source says the deskside has no Ktown:

- The deskside guide lists four graphics slots (GE, RM/TM, RM/TM, DG5) and
  says XIO slot 4 is used by the graphics board set.[^desk-slots][^desk-gfx]
- The booklet's deskside diagrams show GE14, RM7/TM7 or RM8 and DG5. Ktown
  appears only on its rack diagrams.[^hmq-desk]
- A real deskside prints `KT: No NIC` and has no Ktown record. A real rack
  has one.[^tetra][^wombat]

The figures are a stylised signal chain, not a board layout. The KTOWN block
stands where the deskside midplane joins XIO slot 4 to the GE. *Inferred.*
Nothing in the widget or slot contract depends on it: the kernel sees the XG
either way.

## Unknowns and what would settle them

| Unknown | Why it matters | What would settle it |
|---|---|---|
| What configuration-flags bit 0 means beyond the deskside decode | A model's flash must carry it clear on a deskside, and its other effects are unread | Disassembly of the other PROM readers of physical `0x100000a8` |
| Which upper Hub slot-identity bits the `4P1G5_MPLN` midplane drives | A model must present the upper bits too | A deskside `hinv` that prints the register, or a midplane pinout |
| Why `n2` takes NASID 0 on a two-node deskside | Node numbering in a model | Disassembly of the PROM's NASID assignment for class `0x5` slots |
| Whether `gfxinfo` and X pipe numbers follow the kernel's pipe-list order | The kernel fixes `find_konapipe`'s index order as module, then slot. Whether user-visible pipe numbers use that index is open | Disassembly of how `kona_attach` or the graphics layer assigns the board number, or a two-pipe rack `gfxinfo -v` |
| Which node board is physically wired to Crossbow 1 widgets 9 and A | HMQ-380-C prints "Node 1 or 2" at 9 and "Node 3 or 4" at A, the PROM table names `n4` and `n2` | A midplane drawing or wiring table. A rack PROM `hinv -v` with nodes 2 and 4 fitted helps only if it prints each Hub's widget, which the one held PROM `hinv -v` (humble-kiwi) does not |
| Whether the Xtown or Ktown has any register a host can read | Whether the model needs anything between Crossbow and the XG | A rack PROM `hinv -v`, which prints widget numbers, or an `xbstat` capture on a pipe port |
| Node-to-widget wiring on the deskside beyond the table | Only the table names it | A deskside midplane drawing, or a capture that prints each Hub's widget. The held PROM `hinv -v` prints `ASIC HUB ... (nasid N)` without one |
| The graphics-module midplane's board record | Rack inventory fidelity | A rack IRIX board list that includes the graphics module |

Neither of the project's physical SGI systems, an Origin 350 and an Octane2,
is an Onyx2. A physical test cannot answer any of these.

## References

[^desk]: SGI, *Onyx2 Deskside Workstation Owner's Guide*, `007-3454-005`,
    revision 005. SHA-256
    `7284b14d16eb7f094c84a149502b9b21f847aa34fbc26e1f3dfe3313a318e408`.
[^desk-intro]: The same guide, printed pages 1–2/PDF 19–20: model CMN A015,
    one or two node boards and up to four processors, BaseIO-G (IO6G), one
    pipe, up to four optional XIO boards, the PCI module on the second XIO
    slot.
[^desk-first]: The same guide, printed page 21/PDF 39: the first Node board
    is always installed in the right-hand slot.
[^desk-gfx]: The same guide, printed page 7/PDF 25 (one or two RM assemblies
    per board set) and printed page 28/PDF 46 (GE, RM and DG boards).
[^desk-gfx-note]: The same guide, printed page 7/PDF 25.
[^desk-ge]: The same guide, printed page 28/PDF 46: the GE board's HIP
    communicates with the host through an XIO-to-graphics interface, the XG
    ASIC.
[^desk-slots]: The same guide, printed pages 8–9/PDF 26–27 (XIO4 is used by
    the graphics board set, Figure 1-4 board locations) and Figure 2-2,
    printed page 16/PDF 34.
[^desk-null]: The same guide, printed page 19/PDF 37 and Figure 2-6: the
    Null-Router board.
[^desk-xio]: The same guide, printed page 24/PDF 42: five XIO slots and the
    XIO rules.
[^rack]: SGI, *Onyx2 Rackmount Owner's Guide*, `007-3457-005`, revision 005.
    SHA-256
    `92e872585b7e1ae8fc8b6826b6ea4ac6dca515856ac8b250f347b90a18210ff5`.
[^rack-intro]: The same guide, printed page 1/PDF 25: model CMN A017, 2 to 8
    processors and one or two pipes per rack, up to 64 processors and 16
    pipes multirack.
[^rack-block]: The same guide, Figure 1-2, printed page 5/PDF 29.
[^rack-midplane]: The same guide, printed pages 20–22/PDF 44–46: compute and
    graphics module midplanes, Figures 1-11 and 1-12.
[^rack-gfxmod]: The same guide, printed page 14/PDF 38 and printed page
    31/PDF 55: two pipes per graphics module, RM counts, Ktown connectors,
    Figure 2-3.
[^rack-nodes]: The same guide, printed pages 33–35/PDF 57–59: node-to-XIO
    slot activation, Figures 2-5 and 2-6.
[^rack-xio]: The same guide, printed page 36/PDF 60: XIO rules.
[^rack-router]: The same guide, printed page 46/PDF 70: Null, Star and Rack
    Router boards.
[^rack-router2]: The same guide, printed pages 45-47/PDF 69-71: the Null
    Router in deskside systems with one or two node boards, the Star Router in
    a single-rack Onyx2 with three or four, the Rack Router in multirack
    systems.
[^rack-xtown]: The same guide, printed page 48/PDF 72: each pipe needs its
    own crosstown cable from a crosstown XIO board.
[^rack-config]: The same guide, printed page 51/PDF 75: each graphics pipe
    requires one node board.
[^o2k-desk]: SGI, *Origin2000 Deskside Owner's Guide*, `007-3453-002`,
    revision 002, SHA-256
    `db75a435d8f0fe4e5591fde00a110ba25cd8803eb1c170af5131afb10cb7f3d7`:
    printed page 1/PDF 19 (model CMN A015, up to four node boards and twelve
    XIO boards) and printed pages 19–24/PDF 37–42 (Router and node
    combinations).
[^o2k-null]: The same guide, Figure 2-9, printed page 22/PDF 40: a two-node
    Origin 2000 deskside with a Null Router.
[^hmq-mix]: SGI booklet `HMQ-380-C`, printed page 95/PDF 97: rule 7 gives the
    Onyx2 deskside midplane as part `013-1839-001` or `030-1066-002` or later, and rule
    9 names Origin 2000 and Onyx2 desksides that scale to four processors on a
    Null Router.
[^toad]: Origin 2200 IRIX inventory credited to `julesr`,
    https://nekonomicon.irixnet.org/forum/14/16729560/1.html, board list with
    `8P12_MPLN` `030-1535-001` and nodes `n1` and `n2`. Held as capture
    `model-toad`.
[^theory]: SGI, *Origin and Onyx2 Theory of Operations Manual*,
    `007-3439-002`, revision 002. SHA-256
    `f6f30191b662aeef6c9888f109c4997d58ac9c4a77acd24263937c6392113309`.
[^theory-router]: The same manual, printed page 57/PDF 71: the Null Router
    joins two node boards in one deskside and cannot be expanded.
[^theory-xtown]: The same manual, printed page 73/PDF 87: the Crosstown
    (KTOWN) conversion board and Figure 2-28. Its name for the XIO-slot board
    clashes with the rack guide's use of KTOWN for the graphics-module board.
[^theory-desk]: The same manual, printed page 20/PDF 34 (the Origin2000
    deskside module holds one to eight processors) and printed pages
    63–64/PDF 77–78 (the deskside midplane and node placement).
[^hmq]: SGI, *Origin and Onyx2 Hardware Quick-reference Booklet*,
    `HMQ-380-C`, held as
    `docs/library/cache/sgi-hw/origin-onyx2-hardware-quick-reference-booklet.pdf`,
    SHA-256
    `75f82fa4123bec59e81b2e4f1330ee03e2a714e7d5b6e9216c919648a704fb04`.
[^hmq-xtown]: The same booklet, Tables 41 and 42, printed pages 130–131/PDF
    132–133: node-to-XIO control per Crossbow, one pipe per node, and Xtown
    placement.
[^hmq-xbstat]: The same booklet, printed page 250/PDF 252: xbstat port,
    widget and eight-processor-module device table.
[^hmq-trouble]: The same booklet, printed page 251/PDF 253: graphics
    troubleshooting chain and the XBOW-to-GE14 path.
[^hmq-desk]: The same booklet, Figures 96–98, printed pages 277–279/PDF
    279–281 (deskside), against Figures 99, 101 and 105, printed pages 280,
    282 and 286 (rack).
[^techrep]: SGI, *Onyx2 Reality, Onyx2 InfiniteReality and Onyx2
    InfiniteReality2 Technical Report*, August 1998, held as
    `docs/library/cache/sgi-hw/onyx2-technical-report.pdf`, SHA-256
    `0b2be85d655ae996d7e39bf3f22248060e3c7c493a852a2358e2e0afdc2d6305`:
    Figures 1 and 2 (PDF pages 12–13) and section 8.3.3 (PDF page 74).
[^kiwi]: Onyx2 deskside PROM `hinv -v`, posted 2010-04-19 by `dexter1`,
    https://nekonomicon.irixnet.org/forum/3/16723250/1.html. Held read-only as
    capture `humble-kiwi` in the project's hinv capture store.
[^tetra]: Onyx2 deskside IRIX `hinv`, board list and `gfxinfo -v`,
    https://forums.irixnet.org/showthread.php?mode=linear&tid=4352&pid=32192.
    Held as capture `whole-tetra`.
[^akita]: Onyx2 deskside inventory and board list credited to Gerhard
    Lenerz, http://www.sgistuff.net/collection/systems/2200.html. Held as
    capture `fancy-akita`.
[^wombat]: Onyx2 rack IRIX `hinv -v -m`, board list and `gfxinfo -v`, posted
    2007-12-03 by `regan_russell`,
    https://nekonomicon.irixnet.org/forum/14/16285/1.html. Held as capture
    `true-wombat`.
[^disasm]: IP27 PROM 6.156 payload `assets/firmware/6.5.30/ip27prom.payload.bin`,
    SHA-256 `c2c69bf33c375b43ec7ac511fab41ddf595be1b6838ae717e58a9a1273437fdd`,
    and IRIX 6.5.30 kernel `assets/boot/6.5.30/unix.IP27`, SHA-256
    `cbdb02ea5d334ecac949a6524a363e6c945f94baf438d41e8d9248eda0377bdf`, read
    with `tools/origami disasm`. The addresses are recorded in
    the PROM note table (citation: `docs/library/ip27prom-6.5.30.symbols.txt`) and
    the kernel note table (citation: `docs/library/unix.IP27-6.5.30.symbols.txt`).
[^linux]: Linux `arch/mips/include/asm/xtalk/xwidget.h`, mainline:
    `WIDGET_KONA_PART_NUM 0xc102` and `WIDGET_KONA_MFGR_NUM 0x2aa`.
[^openbsd]: OpenBSD `sys/arch/sgi/xbow/xbowdevs` revision 1.6, from the
    retired SGI port at `the-machine-hall/openbsd-sgi` commit `2df91d80c1`:
    vendor `0x02aa`, product `KONA 0xc102`.
