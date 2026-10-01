# Datasheet: Origin 200 GIGAchannel expansion cabinet (XBOX)

References rendered as citation identities name material outside this document
set. Those targets are not included or cleared for redistribution. Review and
runtime statements retain their recorded dates and scope.

**Status:** reviewed. The draft was evidence-reviewed when it landed. The
cabinet status word, the model status and the approximations below were
added while building the model, and an independent review of that build
confirmed the status word by disassembly of the BASEIO PROM, including the
copy in external flash, and confirmed the model's boots. The Crossbow link
rows' grading by disassembly and the fitted XIO boards in the model status
were added later and await their own independent review. The cabinet's logic
is a Crossbow and a Bridge, both specified in their own datasheets, plus one
register no other datasheet describes: [the status
word](#the-cabinet-system-controller-status-word). What this page adds is the
product identity, the composition and cabling contract, the firmware-visible
identity records, that status word, and the facts still missing before a
byte-faithful model can be built.

**QEMU file(s) on `sgi-origami`:** `hw/mips/sgi/board-gigachannel.c` (the
cabinet's own I/O, type `sgi-gigachannel`), built by `hw/mips/sgi/sn-machine.c`
for the `origin200-gigachannel` and `origin200-gigachannel-loopback`
topologies. The [model status](#model-status) section says what the model does
and does not do.

**Sources used:**

- SGI *Origin200 and Origin200 GIGAchannel Owner's Guide*, `007-3708-002`,
  revision 002.[^owner]
- SGI *Origin200 and Origin200 GIGAchannel Maintenance Guide*, `007-3709-002`,
  revision 002.[^maint]
- SGI *Origin200 GIGAchannel Upgrade Installation Instructions*,
  `108-0187-001`, revision 001.[^upgrade]
- SGI *Tensor Processing Unit (TPU) XIO Board Introduction*, `007-4222-002`,
  for the physical XIO slot numbers.[^tpu]
- Real `hinv -vm` inventories on the Higher Intellect wiki's Origin 200
  page,[^wiki] a 2003 comp.sys.sgi.marketplace owner post,[^pigot] a 2001
  comp.sys.sgi.misc boot log from 1999,[^tivo] and the 2012 *Building an Onyx 200*
  forum thread.[^onyx200]
- The NTC Tech retailer photographs of a CMN A018 module pair and the CMN A019
  cabinet,[^ntc] and the Ordi-Spare photographs of the cabinet's
  `030-1252-001` interface board.[^ordi1252]
- Disassembly of the IP27 PROM 6.156 payload shipped with IRIX 6.5.30 and of
  the IRIX 6.5 base PROM 6.11 payload, with `tools/origami disasm`, and two
  gdb-instrumented runs of the project's Origin 200 model.[^disasm][^trace]
- Disassembly of the widget-to-slot tables in the same PROM 6.156 payload and
  in the IRIX 6.5.30 `unix.IP27` kernel, for the Crossbow link
  rows.[^xtslot]
- `strings` over the same PROM payloads and over the IRIX 6.5.30 `unix.IP27`
  kernel.[^promstrings][^kernelsyms]
- The project's existing identity and topology evidence
  ledger (citation: `docs/library/origin-family-identity-and-topology-evidence.md`), the
  Origin 200 GIGAchannel topology page (citation: `docs/machines/origin200-gigachannel.md`),
  and the Patch 3174 metadata contract (citation: `docs/research/graphics/irix-64-gigachannel-patch3174-contract.md`).

**Clean-room statement.** This author read no IRIX or PROM source, no
source-reader notebook, and no Library document that reasons from vendor
source. Two of the link-table rows below were once settled in the existing
ledger partly from clean-side source-reader contracts; the ledger records that
provenance. Those rows now rest on disassembly of the shipped PROM and kernel
instead, and no longer on the ledger. The PROM listings shown by `tools/origami disasm` carry routine names from the
project's committed analysis files. For the 6.156 payload the output showed
`bringup_fail`, `node_master_p`, `usec_delay`, `usec_now`, `led_set` and
`processor_number`; for the 6.11 payload it showed `printf_equivalent`,
`setfault_equivalent` and `fault_cleanup_equivalent`. Those files
(`docs/analysis/ip27prom-6.5.30/capstone-linear.json` and its 6.11
counterpart) were built alongside the project's source-derived PROM symbol map
(commit `c70c33ddfe`). The author also read the first ten entries of
`docs/library/ip27prom-6.5.30.symbols.txt` (adding `node_arbitrate`,
`node_barrier`, `node_barrier_a` and `alive_bit_withdraw`) to learn how the
PROM listings are annotated. The names are generic, and nothing on this page
uses or depends on them. Public Linux, OpenBSD and NetBSD trees hold no GIGAchannel or
SGI XBOX support: a bounded Library text search for `XBOX` over those roots
returned only the unrelated Sun SBus `xbox` driver and Microsoft identifiers.

**Evidence labels.** Every fact is marked *established* (stated directly by a
cited first-party manual or shown by a real-hardware capture or photograph),
*inferred* (follows from established facts but is not itself printed or
observed), *approximated* (a modeling choice, listed under [model
status](#model-status)) or *unknown*.

## Role

The GIGAchannel is a separate I/O cabinet for the Origin 200. It holds one
Crossbow, one Bridge with four PCI slots, and five XIO slots. Two XIO cables
join it to one Origin 200 CPU module. The upgrade removes the module's passive
XTalk jumper and fits a Crosstown adapter in its place, so after conversion the
module's own Hub reaches its own IP29 Bridge only through the cabinet's
Crossbow. The cabinet therefore sits on the attached node's only I/O path: the
node cannot boot without either the cabinet or a loopback cable.[^owner-xio]

SGI's own names for the pieces are GIGAchannel (the product), XBOX (the
chassis model and board-record prefix) and Crosstown (the cable adapters). This
product is distinct from the PCI shoehorn, a one-slot XIO PCI adapter, and from
the Origin 2000 PCI carrier (shoebox). A shoehorn may be *fitted in* a
GIGAchannel XIO slot, as the NTC specimen shows, but it is not part of the
cabinet.

## Product identity

| Item | Value | Status and evidence |
|---|---|---|
| Product name | Origin 200 GIGAchannel expansion cabinet (also "expansion chassis") | Established.[^owner] |
| Chassis regulatory model number | `CMN A019`, preprinted beside "Model No:" on the rear label | Established, photograph.[^ntc] |
| Printed label values | `XBOX-RM` and `DYP511` | Established that both are printed on the label. Which field each fills is inferred: on the companion CMN A018 module label the marketing code prints near the "Marketing Code:" line and the serial between that line and "Serial No:", which places `XBOX-RM` as the marketing code and `DYP511` as the serial. The print is offset on the photographed label, so this is not settled.[^ntc] |
| Electrical rating, rear label | 100–240 VAC, 6–3 A, 50/60 Hz, 720 W | Established, photograph. The current reads as a range, not "6.3 A". The CMN A018 module label carries the same rating.[^ntc] |
| Published input power | 576 W for a fully loaded cabinet | Established.[^owner-power] |
| Upgrade kits | `HU-CONNECT-XBOX` (module-side hardware), `561-CHAS-XBOX` (cables) | Established.[^upgrade-kits] |
| CPU-side Crosstown board | `030-1264-001`, "PCA XBOX EXTENSION BOARD O200"; board record name `XBOX_EXT` (rev C), `2G_MPLN` in the upgrade guide's rev B example | Established.[^upgrade-kits][^wiki][^pigot] |
| CPU-side flex cable | `030-1273-001`, "PCA IP29 XTALK FLEX CABLE" | Established.[^upgrade-kits] |
| XIO cables | Two `018-0575-001`, "CABLE ASSY DIFF PROTECTED 58", listed as CrayLink cables | Established.[^upgrade-kits] |
| Grounding cable | `018-0649-001` | Established.[^upgrade-kits] |
| Cabinet interface board | `030-1252-001`, silkscreen "XBOX XTALK INTERFACE", fab `034-1252-003`; board record name `XBOX_DUALXTOWN`, rev F in every capture | Established.[^ordi1252][^wiki][^pigot] |
| Cabinet PCI backplane | Visible silkscreen `...0-1250-0?` and "...I BACKPLANE": the leading `03` and the last digit are hidden. Read as `030-1250-00?` | The visible digits are established from the photograph; the leading `030-` is inferred, corroborated by the IP27 PROM's `030-1250-` part prefix.[^ntc][^disasm] |
| Cabinet midplane | `030-1380-002`, listed as "PCA XBOX MIDPLANE O200" | Established as a part identity by two seller listings; no image.[^ledger] |
| Cabinet Bridge revision | Widget revision 4 (Bridge rev D) | Established by three inventories.[^upgrade-hinv][^onyx200][^wiki] |

**Correction to an earlier description.** The [PCI shoehorn
datasheet](sgi-pci-shoehorn.md) describes the NTC photographs as showing a
"four-XIO-slot, CrayLink-port" chassis with a three-slot backplane, and the
evidence ledger describes the lot as a CMN A018 master and a "CMN A019 slave"
module. Read directly, the photographs show otherwise: the rear of the
CMN A019 chassis has five XIO slots labeled 15, 14, 13, 12 and 11 from left to
right, the Crosstown panel with connectors labeled XIO A (upper) and XIO B
(lower), each with a pair of LEDs labeled `RMT` and `LINK`, and four PCI openings labeled 0 to 3. The word "CRAYLINK" is a plain
paper site label of the same kind as the "UNITY", "SLAVE" and "IFMX4" labels on
the two CPU modules, not an SGI port marking. CMN A019 is the cabinet; both CPU
modules carry CMN A018. The backplane photograph shows three PCI connectors
clearly; the fourth is out of view.[^ntc]

## Physical composition

| Assembly | What it carries | Status |
|---|---|---|
| Crosstown board `030-1264-001` in the CPU module | Replaces the XTalk jumper; plugs into the jumper's microstrip socket through flex cable `030-1273-001`; carries connectors XIO A and XIO B. | Established.[^owner-xio][^upgrade-jumper] |
| Interface board `030-1252-001` in the cabinet | Two external XIO connectors on one bracket, one card-edge connector to the midplane, two heatsinked ASICs whose markings are not legible. No Crossbow- or Bridge-sized package is on it. Link and data status LEDs for each of XIO A and XIO B. | Established, photographs and manual.[^ordi1252][^maint-led] |
| Midplane `030-1380-002` | The XIO slots, the interface board and the PCI backplane all plug into it; it carries a voltage regulator whose failure lights a diagnostic LED. | Established.[^maint-fru][^maint-led] |
| Crossbow | In the cabinet. | Established: the owner's block diagram places it there; a forum participant who fitted the adapter says the base I/O feed goes "through the Gigachannel's XBOW and back". That it sits on the midplane is inferred, from the TPU guide's general statement that XIO slots reach a crossbar on the system midplane.[^owner-block][^onyx200][^tpu] |
| Cabinet Bridge | In the cabinet, drawn between the midplane and the PCI backplane. | Established as present.[^owner-block][^upgrade-hinv] Its board is inferred to be the midplane: no photograph shows it on the interface board or on the visible part of the PCI backplane. |
| PCI backplane `030-1250-00?` | Four PCI slots: three full-length and one half-length (the one nearest the power switch). | Established.[^owner-xio][^maint-pci] |
| XIO slots | Five, physical numbers 11 to 15. Customer-installable. | Established.[^owner][^tpu][^maint-xio] |
| Power supply, blower, three internal diagnostic LEDs, a front status LED | One power supply (a redundant version needs module controller firmware 3.3S or later); a single constant-speed blower. | Established.[^owner-blower][^maint-fru] |

## Topology

### Cabling and supported configurations

- Two XIO cables join the module's XIO A to the cabinet's XIO A and the
  module's XIO B to the cabinet's XIO B. Both cables are required. The tower
  label places A above B; the upgrade guide calls A the top or rightmost
  connector. *Established.*[^owner-xio][^upgrade-cable]
- Loopback mode: one XIO cable joins the module's own XIO A to its own XIO B,
  so the module runs without a cabinet. *Established.*[^owner-loop]
- The cabinet may attach to the master or the slave module. A two-module
  system may attach one cabinet to each module. One cabinet cannot be shared by
  two modules, and cabinets cannot be daisy-chained. *Established.*[^owner-xio]
- Each cable carries one complete Crosstalk link: one joins the module's Hub
  to the cabinet, the other joins the module's IP29 Bridge to the cabinet.
  *Inferred:* the Crosstown board takes the place of a jumper that joined Hub to
  Bridge, both cables are needed, and a loopback cable joining A to B restores a
  working node with the base I/O in its pre-upgrade position. Which cable
  carries which link is *unknown*.

### Crossbow link assignment

| Crossbow link | Endpoint | Software location | Status |
|---|---|---|---|
| 8 | Cabinet Bridge, four PCI slots | `io7`, board `xbox_dualxtown` | Established: `PCI_XIO ... Slot io7`, `ASIC BRIDGE Rev 4, (widget 8)`.[^onyx200][^upgrade-hinv] |
| 9 | Attached module's IP29 Bridge (base I/O) | `io1`, board `MotherBoard` | Established: `BASEIO ... Slot io1`, `ASIC BRIDGE Rev 4, (widget 9)`.[^onyx200] |
| A | Attached module's Hub | The node's own slot (`n1`, `MotherBoard`) | Inferred that the Hub's port is link A: no capture prints the Hub's port number. The inference rests on disassembly, which establishes that the PROM and the IRIX kernel both name link A the node's own slot, the name the Origin 2000 table in the same PROM gives its Hub links.[^xtslot] |
| B | Optional XIO board | `io2` | Established: a Fibre Channel XIO board at `io2` reports `(widget 11)`.[^onyx200] |
| C, D, E, F | Optional XIO boards | `io3`, `io4`, `io5`, `io6` | Established by disassembly: the PROM 6.156 and IRIX 6.5.30 tables for an Origin 200 with a Crossbow name widgets C to F `io3` to `io6`, and the same tables give the captured 8/`io7`, 9/`io1` and B/`io2`.[^xtslot] Real boards sit at these names (MENET at `io3`, MSCSI at `io4`, an InfiniteReality `kona` at `io6`).[^wiki] |

Physical XIO slots 11 to 15 correspond, as a set, to links B to F and positions
`io2` to `io6`. The one-to-one mapping is *unknown*. The NTC specimen has
shoehorn PCI adapters in physical slots 11 and 12, but no inventory from that
machine exists.[^ntc][^ledger]

The four cabinet PCI slots are labeled 0 to 3 on the chassis, and IRIX
inventories on the cabinet Bridge use PCI slot numbers from 0.[^ntc][^owner-hw]
That the four slots are Bridge PCI devices 0 to 3 is *inferred*: two
hardware-graph paths in the corpus show `io7/xbox_dualxtown/pci/0`, the second
with a Cad Duo (`PCI_ENET`, `030-1155-002` rev D) whose Ethernet IRIX reports
as `slot io7, pci 0`,[^twocab] and IRIX's
per-board adapter lists in the wiki inventory are interleaved in a way that
does not bind them reliably to a board.[^admin][^wiki] The owner's block
diagram labels the same slots 1 to 4, a drawing convention that no software
output uses.[^owner-block]

### Which node owns the cabinet

The Hub of the module the cables attach to is the cabinet's only host. In a
two-module system with one cabinet on module 1, module 1's base I/O moves to
`io1` behind the Crossbow and module 2's base I/O stays at
`MotherBoard/node/xtalk/8`, a direct Hub-to-Bridge path.[^wiki][^onyx200]
*Established* for the module-1 case. The PROM records the owning node in a
per-node variable, `XboxNasid`, that it recomputes every boot from whichever
node sees a Crossbow, so its side does not depend on master or slave role (see
[below](#how-the-ip27-prom-finds-the-cabinet-and-loads-its-io-prom)).

A two-module system with a cabinet on each module is also captured. Each module
reports the same layout as a one-cabinet module 1: an `XBOX_DUALXTOWN` record
at `/hw/module/N/slot/io7/xbox_dualxtown`, and `IP29` and `XBOX_EXT` records at
`/hw/module/N/slot/io1/MotherBoard`, for N = 1 and 2. IRIX prints `Origin PCI
XIO board, module N slot 7: Revision 4` for both cabinets. It names the base
I/O `module 1 slot 1` and `module 2 slot 2`, and the PROM line `IO6prom on
Global Master Baseio in Module 1/Slot io1: Revision 6.156` appears only once.
The second module's cabinet carries its own XIO boards: `XTALK_PCI` shoehorns
`030-1275-004` at `io2`, `io3` and `io4`, and `MSCSI` boards at `io5` and
`io6`. *Established* from that inventory.[^twocab] The poster notes that some
hardware in the top cabinet was moved or removed before the capture, so an
empty XIO position in it is not evidence. The capture does not print widget
numbers, so it does not settle the link rows above. How IRIX treats a single
cabinet on the slave module alone is still *unknown*.

## Firmware and IRIX view

Each item is real-hardware output unless it names another source. None is
emulator output.

- **Board records.** `hinv -vm` lists the cabinet under
  `/hw/module/1/slot/io7/xbox_dualxtown` as `XBOX_DUALXTOWN Board: ... part
  030-1252-001 rev F`, and lists `XBOX_EXT Board: ... part 030-1264-001 rev C`
  beside `IP29 Board` under `/hw/module/1/slot/io1/MotherBoard`. The owner post
  adds `Group ff Capability ffffffff Variety ff` and a Laser value for each
  record. *Established* in three specimens.[^wiki][^pigot] These are human-
  readable fields, not raw one-wire bytes.
- **Base I/O location.** With a cabinet, the base I/O is `/hw/module/1/slot/io1`
  in PROM and IRIX paths; a plain Origin 200 reports `slot/MotherBoard`.
  *Established.*[^tivo][^onyx200]
- **Bridge inventory.** The PROM's `hinv -v` prints `PCI_XIO Origin 200 IO
  Board, Module 1, Slot io7` with `ASIC BRIDGE Rev 4, (widget 8)`; IRIX prints
  `Origin PCI XIO board, module 1 slot 7: Revision 4`. The Crossbow itself does
  not appear as an inventory line in any capture. *Established.*[^onyx200][^wiki]
- **I/O PROM load message.** On two GIGAchannel systems the IP27 PROM prints
  `Loading BASEIO prom from XBOX Flashprom....` followed directly by `DONE`;
  Origin 2000 systems and the project's plain-Origin-200 model runs print
  `Loading BASEIO prom ....`. In the Onyx 200 capture the BASEIO PROM that
  follows reports `built 11:26:28 AM Nov 18, 2003`, the build that ships in
  external flash, not the IP27 PROM's internal copy (`11:05:48`).
  *Established.*[^tivo][^onyx200][^bridge-flash] Why the XBOX form appears and
  where it reads from are in the next section.
- **Part-number recognition.** The IP27 PROM classifies a board whose
  identity-record part number starts `030-1250-` or `030-1252-` as board type
  `0x26`, the value public Linux names `KLTYPE_HAROLD` and IRIX's inventory
  prints as `PCI_XIO`. *Established* by disassembly of PROM 6.156 (the
  classifier at image address `0x1fc41538` compares nine-character prefixes and
  returns `0x26` on either match) and the public Linux header.[^disasm][^linux-kl]
  `030-1264-` sits in a separate string list in the same routine; its use was
  not traced.
- **IRIX kernel cabinet support.** The IRIX 6.5.30 `unix.IP27` kernel has its
  own routines for the cabinet's system controller and redundant power
  supply. What they do is answered in the GIGAchannel system controller
  datasheet (citation: `docs/datasheets/sgi-gigachannel-system-controller.md`).
- **IRIX support.** IRIX 6.4 needs Patch 3174 installed before the hardware;
  installing the hardware first prevents boot. The patch describes
  GIGAchannel system-controller changes and "kernel I/O PROM support for
  flashing the XBOX". IRIX 6.5 needs no such patch. *Established* from release
  metadata; the patch payload was not executed.[^patch3174]
- **Loopback inventory.** In loopback mode the PROM inventory is identical to
  the pre-upgrade one except for the added `030-1264-001` record: base I/O
  `Slot MotherBoard`, `ASIC BRIDGE Rev 3, (widget 8)`. *Established* from the
  upgrade guide's example.[^upgrade-loop]

## How the IP27 PROM finds the cabinet and loads its I/O PROM

Everything in this section is read from the IP27 PROM 6.156 image shipped with
IRIX 6.5.30, by disassembly, unless it names another source.[^disasm] The
IRIX 6.5 base image, PROM 6.11, has the same structure; the one difference
found is noted. Addresses are image addresses at the project's usual base
`0x1fc00000`; the PROM runs a copy at `0xc000_0000_1fcx_xxxx`, which is where
the gdb breakpoints below were set.

1. **Only on an Origin 200.** Every step is gated on the 32-bit machine-type
   field the PROM reads at physical `0x1000_0090`.[^cpu-reset-mode] It must be
   nonzero on a real Origin 200: the real GIGAchannel boots print the XBOX load
   line, which only a nonzero value reaches.[^tivo][^onyx200] The project's
   model sets it to 1.[^model-mtype] With the value zero, as on an Origin 2000,
   the XBOX path is never taken.
2. **Detection, every boot.** Early in the node's main sequence (the call at
   `0x1fc527c0` to `0x1fc484c0`) the PROM walks the nodes of its partition.
   For each it first clears a per-node variable named `XboxNasid`, then reads
   the 32-bit identification word of **widget 0** through that node's Hub
   (Hub I/O space, widget 0, offset 4), with a bus-error guard. If the part
   field, bits 27:12, is `0x0000` or `0xd000`, that node has an XBOX. PROM
   6.11 accepts `0x0000` only. `0x0000` is the Crossbow's part number (see the
   [Crossbow datasheet](xbow.md)); `0xd000` is not identified here. If any node
   qualifies, the PROM then writes `XboxNasid` = that node's number on every
   node of the partition. With two cabinets the last node found wins. No board
   identity record takes part in the decision.
3. **Where `XboxNasid` lives.** It is read and written through the same routine
   that handles the PROM's other named per-node variables, keyed by node
   number and name; the kernel's `ip27log_xbox_nasid_*` names place it in the
   IP27 PROM log. *Inferred* location; the lookup routine itself was not traced
   to the flash sector.
4. **The load.** Once the console is found, the main sequence reads
   `XboxNasid`. If it holds a node number, it prints `Loading %s from XBOX
   Flashprom....` and calls the BASEIO PROM loader (image `0x1fc47c9c`) with
   flags `5`; otherwise it prints the plain `Loading %s ....` and, on an Origin
   200, calls it with flags `4`. Flag bit 0 selects the external flash and bit
   2 allows the fallback below. With bit 0 set the loader reads `XboxNasid`
   again and builds, at `0x1fc47ce8` to `0x1fc47d1c`,

   `address = (own << 32) | ((xbox << 32) + 0x9200_0000_08c0_0000)`

   where `xbox` is `XboxNasid` and `own` is the executing node's number from the
   Hub network-interface status register (`0x1fc5ac1c`), each sign-extended from
   16 bits. The constant selects **widget 8, offset `0xc0_0000`**, the
   external-flash window of the cabinet Bridge on Crossbow link 8. When the two
   node numbers are equal, as in the one-module case and the model trace
   below, the address is that node's widget 8 window. When they differ the OR
   merges two node numbers, so what a second module's node actually fetches
   on a two-module system is *unknown*. The loader then copies the image through the Bridge
   flash-port sequence already described in the [Bridge
   datasheet](bridge.md#external-flash-port) (write enable, AMD autoselect,
   header magic, per-segment checksums). With bit 0 clear it copies the reduced
   BASEIO PROM stored inside its own image. A plain Origin 200 therefore never
   reads its onboard Bridge's flash window.

   The cabinet's flash gets exactly the check an Origin 2000 BaseIO card's
   does. When `XboxNasid` holds no node, the same loader with bit 0 set reads
   a widget number through the node's Hub (`0x1fc48118` to `0x1fc481d8`,
   *inferred* to be the BaseIO's entry in the node's configuration record) and
   forms `0x9200_0000_00c0_0000` plus that widget. Both addresses then join
   one path at `0x1fc47d24`. It sets the Bridge control register's flash write
   enable (bit 31 of the register at widget offset `0x24`), then probes the
   part (`0x1fc2ad8c`, calling `0x1fc0a888`), trying a second port setting if
   the first fails. The probe writes the AMD unlock sequence at word
   addresses `0x5555` and `0x2aaa`, reads the manufacturer and device as
   halfwords at offsets 0 and 2, and marks three identities as sixteen-bit
   parts: AMD `0x01`/`0x22da`, ST `0x20`/`0xd7` and Fujitsu `0x04`/`0x22da`.
   Four more pass without that mark: `0x01` with `0xd5` or `0xad`,
   `0x20`/`0xf1` and `0xad`/`0xd5`. No step depends on which path formed the
   address, and no size limit, window, byte-lane or strap difference
   separates the two. *Established* by disassembly of 6.156.[^disasm]
5. **Fallback.** A bus error in the window prints `Bus error trying to access
   BASEIO PROM at 0x...`; an unrecognized part, bad magic or bad segment sum
   fails the same way. With flags `5` the loader then prints `Retrying using the
   IP27 PROM internal copy of BASEIO PROM` and loads the internal copy. A
   GIGAchannel whose cabinet flash is missing or blank still boots, with that
   message on the console.
6. **Other effects.** Ten other call sites test whether a node's I/O is
   reached through a Crossbow. At `0x1fc410e4` the test is exactly "machine
   type zero, or a Crossbow at widget 0". The one in the main sequence,
   `0x1fc51734` to `0x1fc5175c`, has a second input: with machine type zero it
   also reads bit 0 of the 32-bit word at physical `0x1000_00a8`, and when that
   bit is set an Origin 2000 is sent down the same path (`0x1fc5338c`) as an
   Origin 200 without a cabinet. Otherwise an Origin 200 with a cabinet takes
   the Origin 2000's path (`0x1fc51764`). *Inferred* that the latter is the
   Crossbow link walk and the former the direct-Bridge path; neither path,
   the meaning of the `0x1000_00a8` bit, nor the other eight sites was read
   in detail. No hard-coded link number other than widget 8 for
   the flash was found.

**Emulator trace.** On the project's Origin 200 model (QEMU `sgi-origami`
integration build `4686d60de8`, stock PROM 6.156, run
`prom-20260925T071352-a493fe`), a breakpoint after the widget-0 read in the
detection routine saw `0x4c00206d`: revision 4, part `0xc002`, manufacturer
`0x036`, the model's direct Bridge answering at widget 0, so the routine
returned "no XBOX". In a second run, `prom-20260925T072641-d4f5bf`, gdb forced
the two `XboxNasid` reads to return node 0 and changed nothing else in the
model or the PROM. The loader was entered with flags `5`, composed
`0x9200000008c00000`, and the console printed:

```text
Loading BASEIO prom from XBOX Flashprom....		Bus error trying to access BASEIO PROM at 0x9200000008c00000
Retrying using the IP27 PROM internal copy of BASEIO PROM
DONE
BASEIO PROM Monitor SGI Version 6.156  built 11:05:48 AM Nov 18, 2003 (BE64)
```

The bus error is the model's IP29 Bridge fitting no flash part. This run shows
the model took the branch the disassembly describes. It says nothing about
real hardware beyond that.[^trace]

## The cabinet flash part

- **Established: the image.** The real Onyx 200 cabinet's BASEIO PROM reports
  `6.156 built 11:26:28 AM Nov 18, 2003`.[^onyx200] That is the banner of the
  `io6prom.img` the IRIX 6.5.30 media ship beside the node PROM, the image the
  Origin 2000 BaseIO card's flash holds.[^bridge-flash] The model boots that
  image from the cabinet and prints the same banner.[^flash-boot] The match is
  by banner, not by comparing the bytes.
- **Established: the loader's contract.** The PROM reads the part through the
  same probe and copy as the BaseIO card's, with the same accepted identities
  (step 4 above).
- **Approximated: the part.** The model fits the BaseIO card's part: 1 MiB,
  512K x16 top boot, AMD `0x01`/`0x22da`, which Linux's JEDEC table names
  AM29LV800BT ([BaseIO datasheet](baseio.md#the-flash-part)). The loader
  accepts that identity, and the image fits in it. Which part SGI fitted in the
  cabinet is not known. The accepted parts return different identity bytes to
  an autoselect read; the loader accepts all three the same way. Whether they
  also differ in sector layout or command-set detail was not checked.
- **Unknown.** The physical part, its placement, and the strap behind the
  cabinet Bridge's `FLASH_SELECT` pin. The model reads the strap as zero, so
  the part answers the base chip select, where the loader reads.

## The cabinet system-controller status word

Both the BASEIO PROM and the IRIX 6.5.30 kernel read a 16-bit word at the
cabinet Bridge's widget offset `0xe0_0000` on a GIGAchannel system. That is the
start of the Bridge's second external-flash chip select, 2 MB above the
`0xc0_0000` window the IP27 PROM loads its BASEIO PROM from (the [Bridge
datasheet](bridge.md#external-flash-port) gives the port two 2 MB chip
selects). Neither program writes it. Everything in this section is read from
shipped binaries by disassembly, and from model runs, unless it names another
source.[^disasm][^kernelsyms][^status-trace]

- **Who reads it.** The BASEIO PROM 6.156 internal copy, as `hinv` prints the
  node's inventory: after it asks whether the node is the cabinet's node, it
  reads the halfword at `(nasid << 32) | 0x9200_0000_08e0_0000` and tests bits
  `0x40` and `0x10`. The IRIX kernel reads the same halfword at start and
  then polls it. *Established* that software reads this word here;
  *inferred* that the hardware answers it without a bus error, since real
  GIGAchannel systems boot IRIX and print `hinv`.
- **What IRIX does with it.** The kernel's reading of each bit, its polling
  and its reactions are answered in the GIGAchannel system controller
  datasheet (citation: `docs/datasheets/sgi-gigachannel-system-controller.md`). The fixed zero the model
  answers is what the kernel expects from a healthy cabinet without a
  redundant supply.
- **What is not known.** The part that answers, where it sits (interface board
  or midplane), how many address lines it decodes, what the upper byte holds,
  whether the word changes on its own, and what a write would do. The
  Maintenance Guide's blower and redundant-supply behavior matches the event
  names but does not describe this interface.[^owner-blower][^maint-fru]

Without an answer at that address the model's cabinet made the BASEIO PROM's
`hinv` take a bus error and drop to POD mode, and made IRIX 6.5.30 panic with
`Fatal Craylink error` right after the PROM's `Starting up the system...`: the
kernel's bus-error handler found the Origin 200's unconnected CrayLink port
down and blamed it.[^status-trace]

## Power, reset and link behavior

- **Power sequencing.** The cabinet's rear switch arms it; it turns on only
  when the attached module turns on, and the module's front power switch also
  turns it off. The Crosstown LEDs by each connector light when the link is up.
  *Established.*[^upgrade-power][^maint-power] The signal path that carries
  this power control is *inferred* to be the XIO cables, since only the two XIO
  cables and a ground strap join the chassis.[^upgrade-kits]
- **Order.** SGI's order is Origin Vault, then cabinet, then slave module, then
  master module. Turning on either chassis with the XIO cables not properly
  connected "may not function correctly". *Established.*[^owner-power-on]
- **Blower failure.** A blower failure shuts down the whole server, cabinet and
  CPU modules, immediately; the cabinet's only blower status is its front LED.
  *Established.*[^owner-blower] How the module learns of it is *unknown*.
- **Missing cable.** A GIGAchannel module "cannot boot" without the cabinet or
  a loopback cable. *Established.*[^owner-loop][^upgrade-cable] What the
  PROM prints and where it stops are *unknown*. PROM 6.156 contains `HubIO Link
  is down. Never came out of reset`, a plausible but unconfirmed candidate.[^promstrings]
- **Hot service.** XIO cables must not be removed with either chassis powered;
  XIO boards are installed with power removed. *Established.*[^owner-loop][^maint-xio]
- **Link training.** No GIGAchannel-specific training rule was found. The
  Crossbow link reset, width negotiation and alive state in the [Crossbow
  datasheet](xbow.md#reset-presence-and-width) apply to the cabinet's links.
  Whether the Crosstown adapters add latency, retiming or any state of their own
  is *unknown*.
- **Loose XIO board.** A loose board lever lets the server boot but the board
  "may not be recognized". *Established* as documented behavior; it is a
  missing widget, not a separate state.[^owner-check]

## What a model needs

The cabinet itself introduces no register block that any source describes. A
faithful model is a composition of existing devices with product-specific
wiring and identity records.

### Composition

1. A Crossbow instance for the cabinet, using the reviewed `sgi-xbow` register
   contract, with the attached node's Hub on link A. Its widget-0
   identification must carry part `0x0000`: that is the whole of the PROM's
   cabinet detection.
2. The attached module's IP29 Bridge moved from the direct Hub path to
   Crossbow link 9. The IP29 board keeps every other part of its contract.
3. A cabinet Bridge on link 8, widget revision 4, with four PCI slots, and the
   `XBOX_DUALXTOWN` board record reported at `io7`. How the firmware reaches
   that record is unknown (see below), so the model must present it without
   claiming a wiring.
4. An external flash part behind that Bridge's `0xc0_0000` window, answering
   an autoselect identity the loader accepts and holding a BASEIO PROM image
   with a valid header and segment sums. Without it the machine still boots,
   but prints the bus-error and retry lines a real cabinet does not. See [the
   cabinet flash part](#the-cabinet-flash-part) for what is established.
5. The `XBOX_EXT` record (`030-1264-001`) reported beside the IP29 board
   record under `io1/MotherBoard`, and still present in loopback.
6. Five XIO positions on links B to F that accept existing XIO boards; an empty
   position must read as an absent link, not as a present widget.
7. A loopback variant: the IP29 Bridge back on the direct Hub path at widget 8,
   with the `XBOX_EXT` record still present and no Crossbow. The only record of
   that record in loopback is the upgrade guide's example, which shows it as
   `2G_MPLN`, rev B; a rev C board's name in loopback is not captured. With no
   Crossbow, widget 0 should answer with the Bridge's part `0xc002` and the PROM
   should take the plain load, as the guide's identical-inventory example
   requires. That widget-0 value is *inferred*: it is what the project's model
   returns (the first trace run below), and no real plain Origin 200 PROM boot
   in the corpus shows the load line.
8. An unconnected variant (a converted module with neither cabinet nor
   loopback). Until its behavior is known it should be a configuration error
   at machine creation, not an invented firmware result.
9. Physical labels (slots 11 to 15, XIO A/B) kept as naming only; no model
   behavior should depend on the unknown physical-to-link rows.
10. The cabinet's system-controller status word on the cabinet Bridge's
    second external-flash chip select, widget offset `0xe0_0000`, reading
    without a bus error. With no source for its hardware, a model answers
    the value a healthy cabinet without a redundant supply would give by the
    kernel's own reading of the bits: zero. See [the status
    word](#the-cabinet-system-controller-status-word).
11. Nothing else for the firmware's sake. The PROM keeps `XboxNasid` in its own
    per-node log, and the model's flash already provides a log area; the model
    should not preset the variable, because the PROM rewrites it every boot.

### Discriminating tests

These separate a GIGAchannel model from a base Origin 200 and from an Origin
2000 Crossbow. The expected values come from the captures above.

- The PROM prints `Loading BASEIO prom from XBOX Flashprom....` then `DONE`,
  with no bus-error or retry line, and the BASEIO PROM banner is the flash
  build, not the internal `11:05:48` build. Without a cabinet, or in loopback,
  it prints `Loading BASEIO prom ....` and the internal build.
- After a boot with a cabinet, every node's PROM log holds `XboxNasid` equal to
  the attached node's number; after a boot without one, it has been
  cleared.
- PROM `hinv -v` shows `PCI_XIO ... Slot io7` with `ASIC BRIDGE Rev 4, (widget
  8)` and `BASEIO ... Slot io1` with `(widget 9)`.
- XIO boards at `io2`, `io3` and `io4` report widgets 11, 12 and 13.
- IRIX `hinv -vm` lists `io7/xbox_dualxtown` with part `030-1252-001` and lists
  `XBOX_EXT` under `io1/MotherBoard`.
- In loopback, the inventory matches the base machine's apart from the added
  `030-1264-001` record (named `2G_MPLN`, rev B, in the upgrade guide's only
  example), and the PROM prints the plain load line. The load-line
  expectation is inferred from the disassembly and the model, not from a real
  capture.
- A PCI device fitted in cabinet slot 0 appears under
  `/hw/module/1/slot/io7/xbox_dualxtown/pci/0`.
- In a two-module machine with one cabinet, the other module's base I/O stays
  at `MotherBoard/node/xtalk/8`.
- Reading an empty XIO position's widget identity through the Crossbow follows
  the Crossbow datasheet's missing-destination behavior.

### Register-contract tests (qtest)

- Crossbow widget-zero identity and per-link status for links 8, 9, A and B–F,
  with the expected alive/absent pattern for an empty cabinet.
- Hub-originated PIO to widget 8 reaches the cabinet Bridge; to widget 9
  reaches the IP29 Bridge; to an empty link raises the Crossbow's
  missing-destination error.
- Cabinet Bridge DMA and interrupt writes reach the Hub through link A.
- The board-identity chains return the two XBOX records with the observed
  part, revision and barcode formats.
- The cabinet status word reads at widget 8 offset `0xe0_0000`, and the IP29
  Bridge's second chip select stays empty. With the flash image supplied, the
  base chip select at `0xc0_0000` returns the image and, with the flash write
  enable set, the autoselect identity. Without it, that chip select stays
  empty.

## Out of scope

- The module system controller's power, blower and status-LED protocol to the
  cabinet. No guest-visible register is known, and the evidence stops at
  documented behavior.
- The Crosstown adapters' own electrical behavior. No software-visible state is
  known.
- Redundant power supply status and the cabinet's events, beyond the status
  word's fixed value.
- Physical cable and slot labels beyond naming.

## Unknowns and what would settle them

| Unknown | Why it matters | What would settle it |
|---|---|---|
| The cabinet flash part SGI fitted, and the strap behind the cabinet Bridge's `FLASH_SELECT` pin | A guest that reads the part's identity sees which part is fitted; the loader accepts all three sixteen-bit identities the same way | A photograph of the part on the midplane or interface board, or a PROM capture that prints the part type |
| Where the flash sits physically | Board placement only; software sees it at widget 8 either way | Midplane or interface-board photographs |
| What the `0xd000` part number accepted by PROM 6.156 identifies | Whether another crossbar also counts as an XBOX | A public header or specification naming part `0xd000` |
| Raw one-wire contents of `XBOX_DUALXTOWN` and `XBOX_EXT` | Needed to present byte-accurate identity records | A NIC dump from a real board, or the PROM's parser read by disassembly plus the captured human-readable fields |
| That the Hub's port is Crossbow link A | Settles the one link row still inferred; software names link A the node's own slot | A GIGAchannel `xbow` register dump, or a capture that prints the Hub's port |
| Which XIO cable carries which link | Only failure behavior would show it | Continuity from each connector to the Crossbow, or a capture with one cable deliberately removed |
| Physical slot 11–15 to link B–F | Labeling only | A backplane drawing, or an inventory from a machine with known physical placement |
| Cabinet PCI slot to Bridge device and interrupt pin | PCI enumeration and interrupt routing for cabinet cards | A PROM `hinv -v` with cards in several cabinet slots, or IRIX `/hw` paths per slot |
| Location of the cabinet Bridge and its board-identity line | Whether the `XBOX_DUALXTOWN` record is on the Bridge's own line | Midplane `030-1380-002` photographs or a drawing |
| IRIX behavior with a single cabinet on the slave module and none on the master | Two-module configurations. The PROM side is covered: it detects the cabinet on whichever node sees a Crossbow and writes that node's number on every node. A cabinet on each module is captured[^twocab] | A capture from a system with one cabinet, attached to the slave module |
| Behavior with the converted module neither cabled nor looped | The unconnected variant | A capture of that boot |
| The hardware behind the status word at widget 8 offset `0xe0_0000`: its part, decode, upper byte and dynamic behavior | Whether a model can report a real cabinet's supply and blower state, and whether the fixed value hides anything IRIX checks | A photograph or drawing of the part on that chip select, or a real `hinv` from a cabinet with a redundant supply |

A physical test cannot answer any of these on the project's available SGI
systems, an Origin 350 and an Octane2: neither is an Origin 200.

## Is the evidence enough to build a model?

Enough for a structurally faithful composition that the stock PROM will treat
as a GIGAchannel. The chips, the link used by each of the three populated
positions, the board records and their placement, the widget revision,
loopback and the software-visible names come from first-party manuals and real
captures. The PROM's detection rule (a Crossbow part number at widget 0 on an
Origin 200) and its load address (cabinet Bridge, widget 8, flash window
`0xc0_0000`) come from disassembly and are confirmed in the model by a trace.
The link numbers of `io3` to `io6` come from the same PROM's and the IRIX
kernel's widget-to-slot tables, read by disassembly. What remains before a
byte-faithful model: the cabinet's physical flash part and the raw
identity-record bytes. Neither blocks a first implementation. The cabinet status word, found while building
the model, blocks IRIX and the PROM's `hinv` unless it answers; its fixed
value is an approximation grounded only in how software reads it.

## Model status

The `origin200-gigachannel` topology (QEMU `-M
origin200,topology=origin200-gigachannel`) builds the cabinet Crossbow with the
Hub on link A, the IP29 Bridge on link 9 with the `XBOX_EXT` record beside its
own, and an `sgi-gigachannel` on link 8: a Bridge revision 4 with the
`XBOX_DUALXTOWN` record and four PCI openings, devices 0 to 3, on bus
`io7.0` (the board is named by its IRIX slot, `io7`). Links B to F are the
XIO positions `io2` to `io6`, widgets 11 to 15, each controlled by the
attached Hub; they are empty until a board is fitted there at launch by slot
name. `-device sgi-xio-baseio,preset=mscsi,slot=io2` and its siblings, or
`--board mscsi@io2 --board menet@io3 --board mscsi@io4`, give the XIO
population of the wiki's one-module inventory: an [MSCSI](sgi-mscsi.md) at
`io2`, a MENET at `io3` and a second MSCSI at `io4`. (The capture below was
taken when a catalogue topology, `origin200-gigachannel-xio`, placed those
three boards.) The
`origin200-gigachannel-loopback` topology is the plain Origin 200 with the
`XBOX_EXT` record added. The harness supplies the shipped BASEIO PROM image as
the cabinet's flash, and stock PROM 6.156 then prints the XBOX load line
followed by `DONE`, the flash build's banner (`built 11:26:28`), and ALL DIAGS
PASSED, as the real cabinet does.[^flash-boot] Its `hinv -v` and IRIX
6.5.30's `hinv -vm` show the records and slots the real captures
do.[^model-boot] With the XIO boards fitted, the PROM's `hinv -v`
prints `MSCSI Origin 200 IO Board, Module 1, Slot io2` at `(widget 11)`, the
MENET at `io3` `(widget 12)` and the second MSCSI at `io4` `(widget 13)`, and
IRIX lists `io2/mscsi`, `io3/menet` and `io4/mscsi`, the MENET's four
Ethernets as `ef1` to `ef4` at `slot io3, pci 0` to `3`, and eight more
QL1040B controllers, as the wiki's inventory does.[^model-xio]

Approximations and limitations, each stated where it is built:

- The cabinet flash is the Origin 2000 BaseIO card's approximated part, an
  AM29LV800BT identity, because SGI's own part is not known. QEMU fits it only
  when a drive named `sgi-gigachannel-flash0` supplies the image. Without one
  the base chip select is empty and the PROM announces its fallback, as it
  would for a cabinet with no part.
- The cabinet status word reads a fixed zero, set by the
  `sgi-gigachannel.system-controller-status` property. Writes are logged and
  dropped. It repeats across its chip select because the port model repeats
  any part smaller than a chip select.
- Both XBOX records ride Bridge one-wire lines chosen from where inventories
  list them, and their barcodes and serials are placeholders.
- The Crossbow's revision is the Origin 2000 module's selected Crossbow 1.1.
- The cabinet's PCI openings take Bridge devices 0 to 3, inferred from two
  hardware-graph paths.
- The loopback topology fits the rev C `XBOX_EXT` record, where the guide's
  only loopback example shows a rev B board named `2G_MPLN`.
- Two-module systems, a cabinet on the slave module and the unconnected
  converted module are not built.
- The fitted boards' records are the MSCSI and MENET models' own, part
  `030-1243-001` rev F and `030-0873-003` rev J. The wiki's system has
  `030-1281-001` rev D at `io2` and a rev G MENET with a revision 3 Bridge.
- Every fitted SCSI controller reports single-ended; the wiki's system has
  differential controllers too (see the [MSCSI datasheet](sgi-mscsi.md)).
- The MSCSI fits any XIO widget. The Origin 2000's configuration rules keep
  it out of IO2, and the model does not enforce that.

## References

[^owner]: SGI, *Origin200 and Origin200 GIGAchannel Owner's Guide*,
    `007-3708-002`, revision 002, Table 1-1 printed page 5/PDF 31 (five XIO
    slots, four extra PCI slots). SHA-256
    `ae6857707a113bd63b77a3a8187c81beed76cea1406592a38a735c4aa2c462dc`.
[^owner-block]: The same Owner's Guide, Figure 1-5, printed page 7/PDF 33:
    Crosstown boards and cables A/B, Crossbow, midplane, Bridge, XIO slots 11–15
    and PCI slots 1–4 in the cabinet.
[^owner-xio]: The same Owner's Guide, printed pages 19–24/PDF 45–50: XIO
    connectors on the Crosstown board replacing the jumper, the cabinet's four
    PCI (three full, one half size) and five XIO boards, both cables required,
    supported and forbidden configurations.
[^owner-loop]: The same Owner's Guide, printed page 63/PDF 89 (no boot without
    the cabinet or a loopback cable) and printed pages 109–110/PDF 135–136
    (loopback mode; power off both chassis before removing XIO cables).
[^owner-power]: The same Owner's Guide, Table 1-4, printed page 25/PDF 51.
[^owner-blower]: The same Owner's Guide, printed pages 30–32/PDF 56–58: one
    constant-speed blower; its failure shuts down the cabinet and the CPU
    modules; printed page 134/PDF 160: its only status is the front LED.
[^owner-check]: The same Owner's Guide, printed page 79/PDF 105.
[^owner-power-on]: The same Owner's Guide, printed page 81/PDF 107.
[^owner-hw]: The same Owner's Guide, printed page 135/PDF 161: "PCI slots are
    numbered starting with 0" in the hardware graph.
[^maint]: SGI, *Origin200 and Origin200 GIGAchannel Maintenance Guide*,
    `007-3709-002`, revision 002. SHA-256
    `9059f188e6df8445b3a5e03d833685b6ec322a1d041f30ac95354f5015c31c33`.
[^maint-power]: The same Maintenance Guide, printed page 2/PDF 18: the front
    power switch also turns off the cabinet.
[^maint-pci]: The same Maintenance Guide, printed page 23/PDF 39: the slot
    nearest the power switch takes only a half-length board.
[^maint-xio]: The same Maintenance Guide, printed pages 33–41/PDF 49–57: XIO
    board installation with power cords removed, retaining plate, blanking
    cards.
[^maint-led]: The same Maintenance Guide, printed pages 60–65/PDF 76–81:
    Tables 4-2 and 4-3 (status and diagnostic LEDs, midplane voltage
    regulator) and Figure 4-4 (per-port link and data LEDs on the cabinet's
    Crosstown board).
[^maint-fru]: The same Maintenance Guide, printed pages 107–123/PDF 123–139:
    power supply (redundant version needs module controller firmware 3.3S),
    PCI backplane plugging into the midplane, midplane, and Crosstown board
    replacement.
[^upgrade]: SGI, *Origin200 GIGAchannel Upgrade Installation Instructions*,
    `108-0187-001`, revision 001. SHA-256
    `ec04eb39af14f2c500653a5ba923b8274221899b6fbd8ba1125c60692f227d80`.
[^upgrade-kits]: The same Upgrade Instructions, Tables 1-1 and 1-2, section
    page 1-2/PDF 14.
[^upgrade-jumper]: The same Upgrade Instructions, Figure 2-3, section page
    2-4/PDF 18.
[^upgrade-loop]: The same Upgrade Instructions, section pages 2-9 to 2-11/PDF
    23–25: loopback test, with the `2G_MPLN` `030-1264-001` rev B record and the
    base I/O Bridge at widget 8.
[^upgrade-cable]: The same Upgrade Instructions, section page 3-4/PDF 30.
[^upgrade-power]: The same Upgrade Instructions, section page 3-7/PDF 33.
[^upgrade-hinv]: The same Upgrade Instructions, section page 3-8/PDF 34:
    `PCI_XIO IO Board, Module 1, Slot io7`, `ASIC BRIDGE Rev 4, (widget 8)`. The
    same example also prints module 1's base I/O as `Slot MotherBoard` at widget
    8. That cannot hold beside a second widget-8 Bridge under one Crossbow, and
    it matches the guide's loopback example line for line, so this page takes
    the base I/O position from the real capture in the Onyx 200 thread instead.
[^tpu]: SGI, *Tensor Processing Unit (TPU) XIO Board Introduction*,
    `007-4222-002`, "Slot Selection Rules for Origin 200 Gigachannel Servers"
    and Figure 4 (five XIO slots numbered 11–15), and page 1 (a general statement, for
    every system the guide covers, that XIO slots reach a crossbar on the
    system midplane).
[^wiki]: Higher Intellect wiki, *SGI Origin 200*, Library root `origami-docs`,
    path `library/cache/wiki/preterhuman-sgi-origin-200.html`, SHA-256
    `7e952adb35a1a913c08d8671192ac3f3ff7b7d8de27c1a86d87ba84913323fb4`. Two
    `hinv -vm` inventories: a one-module system (`io2` and `io4` MSCSI, `io3`
    MENET) and a two-module system with an InfiniteReality `kona` at `io6`.
[^pigot]: Simon Pigot, "Re: Have 3 Origin 200 XBOX Gigachannel Exp. NEW
    FACTORY BOXES", comp.sys.sgi.marketplace, 3 June 2003, Message-ID
    `<3EDC9A71.CE2535AA@delm.tas.gov.au>`, Library `usenet-comp` corpus.
[^tivo]: John Rasey, "Inst rbaud weirdness", comp.sys.sgi.misc, 6 December
    1999, Message-ID `<384C6090.A4854432@tivo.com>`, Library `usenet-comp`
    corpus. IP27 PROM 6.25 boot log from a one-module system: IP29 board
    `030-1389-001`, base I/O at `slot/io1`, and the XBOX form of the I/O PROM
    load message.
[^admin]: rabi, "ln -s by user crashing orgin200", comp.sys.sgi.admin, 19 May
    2004, Message-ID `<40ABA53A.76027067@larc.nasa.gov>`, Library `usenet-comp`
    corpus: disk paths under
    `/hw/module/1/slot/io7/xbox_dualxtown/pci/0/scsi_ctlr/`.
[^onyx200]: *Building an Onyx 200. Success*, Nekonomicon forum mirror, first
    post by `bjornl`, 17 June 2012, Library path
    `graphics/onyx/nekonomicon-building-onyx200.html`, SHA-256
    `2bb21cea6c6b6f4838d021ae874aed1657eb63260c71ff0126b0ef210ee3119f`. PROM
    `hinv -v` with widgets 8 (`io7`), 9 (`io1`) and 11 (`io2`); PROM 6.156 boot
    log; a later participant's statement, posted 17 June 2012, that the base
    I/O feed passes through the cabinet's Crossbow.
[^twocab]: `johnnym`, IRIX `hinv -vm` of a two-module Origin 200 with a
    GIGAchannel cabinet on each module, IP27 PROM 6.156 on both nodes,
    Nekonomicon forum mirror thread 16731033, 9 September 2016,
    <https://nekonomicon.irixnet.org/forum/14/16731033/1.html>, page SHA-256
    `201c2518d5c09981a91630449b853dd45e871daa2c17647169e92a08dfcd1d29`
    (retrieved 2026-09-22). No flash part or widget number appears in it.
[^ntc]: NTC Tech, *Silicon Graphics Origin 200 Server CMN A018 Master and
    Slave, CMN A019 Expansion*, retrieved 2026-08-30; images 03 (CMN A019 rear
    label), 05 and 09 (CMN A018 labels), 06 (rear of all three chassis), 02
    (cabinet interior and PCI backplane) and 12 (a `030-1275-004` shoehorn from
    the lot). Paths and hashes are in the acquisition
    ledger (citation: `docs/library/origin-family-public-reference-acquisition.md#ntc-tech-master-and-slave-module-images`).
    Retailer and photographer retain rights.
[^ordi1252]: Ordi-Spare/EURINCO, `030-1252-001` XBOX XTalk interface board,
    images 17221–17226, retrieved 2026-08-15; hashes in the evidence
    ledger (citation: `docs/library/origin-family-identity-and-topology-evidence.md#public-physical-evidence-register`).
[^ledger]: Origin-family identity and topology
    evidence (citation: `docs/library/origin-family-identity-and-topology-evidence.md`),
    "Origin 200 GIGAchannel identities" and "Evidence boundary".
[^promstrings]: `strings -t x` over `assets/firmware/6.5.30/ip27prom.payload.bin`
    (IP27 PROM 6.156, SHA-256
    `c2c69bf33c375b43ec7ac511fab41ddf595be1b6838ae717e58a9a1273437fdd`) at file
    offsets `0x60308`, `0x60330`, `0x6ccd8`, `0x70fe8`, `0x70ff8` and
    `0x710f8`, and over `assets/firmware/6.5-base/ip27prom.payload.bin`. The code
    that uses them is described under the disassembly reference.
[^patch3174]: IRIX 6.4 GIGAchannel Patch 3174
    contract (citation: `docs/research/graphics/irix-64-gigachannel-patch3174-contract.md`),
    summarizing SGI note `007-3907-001` and the `812-0788-001` release metadata.
[^disasm]: Disassembly with `tools/origami disasm` (listings keyed by image
    hash) of `assets/firmware/6.5.30/ip27prom.payload.bin`, IP27 PROM 6.156,
    SHA-256 `c2c69bf33c375b43ec7ac511fab41ddf595be1b6838ae717e58a9a1273437fdd`,
    and `assets/firmware/6.5-base/ip27prom.payload.bin`, PROM 6.11, SHA-256
    prefix `14e0d0a77b92`. Routines in 6.156: cabinet detection `0x1fc484c0`
    (called from `0x1fc527c0`), per-node Crossbow test `0x1fc50fc0` and
    `0x1fc3722c`, widget-identity read `0x1fc157fc`, `XboxNasid` get
    `0x1fc478d0`, set `0x1fc394bc`, clear `0x1fc39508`, BASEIO PROM loader
    `0x1fc47c9c` (flags `5` from the XBOX branch at `0x1fc53348` and from
    the Origin 2000 branch at `0x1fc53654`, flags `4` at `0x1fc535d8`), the
    loader's shared probe path from `0x1fc47d24` (write enable at
    `0x1fc48080`, probe at `0x1fc480a0` and `0x1fc48224`), the part probe
    `0x1fc2ad8c` and `0x1fc0a888` (identity table `0x1fc0a918` to
    `0x1fc0a970`), load-message selection `0x1fc532f4` to `0x1fc535dc`, board
    classifier `0x1fc41538`. In 6.11: the Crossbow test `0x1fc49a40`
    (part `0x0000` only), the detection loop near `0x1fc0e2f4`, and the XBOX
    load address loaded from a constant at `0x1fc07dac`. A shipped binary read
    by disassembly, not vendor source.
[^xtslot]: The IP27 PROM 6.156 payload (SHA-256 prefix `c2c69bf33c375b43`)
    routine at image address `0x1fc419cc`, whose error message names it
    `xbwidget_to_xtslot` (string at `0x1fc6cc78`). For widgets 8 to F it
    returns the low byte of a 32-bit word from one of three tables, each
    given here by its widget-8 entry: the Origin 2000 Crossbow 0 table
    (`0x1fcd1df0`), the Crossbow 1 table (`0x1fcd1e10`), and, when the
    machine-type word at physical `0x1000_0090` is nonzero, the Origin 200
    table at `0x1fcd1e30`: `0x27`,
    `0x21`, `0x01`, `0x22`, `0x23`, `0x24`, `0x25`, `0x26`, that is `io7`,
    `io1`, node slot 1, `io2`, `io3`, `io4`, `io5`, `io6`. The encoding
    (`0x2N` is `ioN`, `0x0N` node slot N) is checked by the Origin 2000
    tables, which give the published maps: widgets 8 to F are `io1`, `n1`,
    `n3`, `io4`, `io2`, `io6`, `io5`, `io3` on Crossbow 0 and `io11`, `n4`,
    `n2`, `io8`, `io10`, `io12`, `io7`, `io9` on Crossbow 1. The IRIX 6.5.30
    `unix.IP27` kernel (SHA-256 prefix `cbdb02ea5d334eca`) has its own
    `xbwidget_to_xtslot` at `0xc00000000003ca18`, table at
    `0xc000000001487140`, with `get_slotname` at `0xc00000000003cc9c`, and
    names the Origin 200 widgets the same way; its reading was done
    separately and agrees. Shipped binaries read by disassembly, not
    vendor source.
[^model-xio]: Boot capture (citation: `docs/boot-logs/irix.boot.2026-09-26T20:44:38-07:00`)
    of the `origin200-gigachannel-xio` topology on QEMU
    `sgi-origami-gigachannel-xio`: the PROM's `hinv -v`, ALL DIAGS PASSED
    and IRIX 6.5.30 to `login:` and `hinv -vm`. Model run, not hardware
    evidence.
[^trace]: Origin 200 model runs `prom-20260925T071352-a493fe` and
    `prom-20260925T072641-d4f5bf`, QEMU integration build `4686d60de8`
    (build-fast), stock PROM 6.156, gdb hardware breakpoints at
    `0xc00000001fc37244`, `0xc00000001fc5332c`, `0xc00000001fc47cc8` and
    `0xc00000001fc47d44`. The second run's console was kept in the project
    scratch store; it is an experiment, not a boot-archive capture. It shows
    what the model does with the branch forced, not what real hardware does.
[^flash-boot]: Boot capture (citation: `docs/boot-logs/firmware.boot.2026-09-26T18:23:48-07:00`),
    run `prom-20260927T012348-065b37`, QEMU `sgi-origami-gigachannel-flash` at
    `a7edd8a4e5` (build-fast), stock PROM 6.156, with `io6prom.img`
    (SHA-256 prefix `024d2f28b526933e`) padded with `0xff` to 1 MiB as
    `sgi-gigachannel-flash0`. The QEMU functional test
    `tests/functional/mips64/test_origin200_gigachannel_prom.py` repeats it.
    With the same flash, IRIX 6.5.30 boots to `login:` (run
    `irix-20260927T013825-90d216` (citation: `docs/boot-logs/irix.boot.2026-09-26T18:38:25-07:00`)).
    Model runs, not hardware evidence.
[^bridge-flash]: [Bridge datasheet, external flash
    port](bridge.md#external-flash-port): the internal copy's banner
    `11:05:48` and the flash image's `11:26:28`.
[^cpu-reset-mode]: [Origin 2000 CPU reset-mode
    datasheet](origin2000-cpu-reset-mode.md), which describes the 32-bit
    machine-type field at physical `0x10000090`.
[^model-mtype]: QEMU `sgi-origami`, `hw/mips/sgi/sn-machine.c`,
    `origin200_class_init`, `smc->firmware_machine_type = 1` (integration tree
    at `4686d60de8`). Model source, not hardware evidence.
[^linux-kl]: Linux `arch/mips/include/asm/sn/klconfig.h` (public, GPL):
    `KLCLASS_IO` is `0x20` and `KLTYPE_HAROLD` is `KLCLASS_IO | 0x6`.
[^kernelsyms]: `strings` over `assets/boot/6.5.30/unix.IP27`, the IRIX 6.5.30
    IP27 kernel (SHA-256 prefix `cbdb02ea5d33`), symbol table names only. The
    status-word findings add a disassembly of `xbox_sysctlr_rps_query`
    (`0xc00000000003ffd8`), `xbox_sysctlr_init` (`0xc00000000004006c`), the
    poller between them, and the `xbox_event` table at `0xc000000001487590`,
    whose six 16-byte entries each name a string and a flags word (`0x2`
    marks a shutdown event).
[^status-trace]: Origin 200 GIGAchannel model runs on QEMU `sgi-origami`
    before the status word existed. Run `irix-20260925T163541-8c80cd`: the
    BASEIO PROM's `hinv -v` stopped after the power-supply line, the CPU's
    BadVAddr was `0x9200000008e00000`, and the ELSC console printed a general
    exception at EPC `0xc000000011c445e0` and entered POD. Run
    `irix-20260925T170512-9c1de6`: the QEMU log recorded `load of 2 bytes at
    0x9200000008e00000 ... raising a bus error` from IRIX, followed at once by
    `PANIC: Fatal Craylink error` and a restart. With a zero answering there,
    run `irix-20260925T171801-62cc88` reached `login:`. Model runs, not
    hardware evidence.
[^model-boot]: Boot captures on QEMU `sgi-origami-gigachannel` `7370dc4f5e`,
    taken before that branch was rebased for review; the rebased branch's
    `tests/functional/mips64/test_origin200_gigachannel_prom.py` repeats the
    PROM part of them:
    the cabinet (citation: `docs/boot-logs/irix.boot.2026-09-25T11:17:41-07:00`), the
    cabinet with cards in a PCI opening and an XIO
    slot (citation: `docs/boot-logs/irix.boot.2026-09-25T11:54:25-07:00`), and the
    loopback form (citation: `docs/boot-logs/irix.boot.2026-09-25T11:58:47-07:00`). Model
    runs, not hardware evidence.
