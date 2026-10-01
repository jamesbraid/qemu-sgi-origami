# Datasheet: SGI PCI-XTALK ADAPTER (the "PCI shoehorn")

References rendered as citation identities name material outside this document
set. Those targets are not included or cleared for redistribution. Review and
runtime statements retain their recorded dates and scope.

**Status:** draft — PENDING independent review. Grounds the product identity
and its Bridge chip in ingested SGI product photographs and public kernel
board-type namespaces; wires a fitted Cad Duo card's keyboard/mouse interrupt
as a reviewed approximation (see the [Cad Duo
datasheet](sgi-cad-duo-pci-card.md), "Interrupts"). It proposes no new
register behavior of its own: the board is a Bridge plus one PCI slot, both
already modeled.

**QEMU file(s) on `sgi-origami`:** `hw/mips/sgi/board-pci-shoehorn.c` (the
`sgi-pci-shoehorn` board, an `sgi-xio-board` subclass). The Bridge is the
existing `sgi-bridge`; a fitted card is the existing `sgi-ioc3`.

**Sources used:**
- Ingested SGI product photographs (Library `docs/library/cache`, from the
  ntc-tech and eBay listings; Origami PR #1547 and pre-existing ntc-tech
  rows): the board silkscreen "PCI-XTALK ADAPTER"; SGI assembly
  **030-1275-002 REV B** (eBay) and **030-1275-004** rev A (ntc-tech),
  (c) 1997, made in USA, tested at Celestica; the **LSI L2A0710 "Bridge Rev
  D"** ASIC marked SGI part **099-0125-007**; a Burndy "COMPUTERBUS"
  card-edge connector carrying one PCI daughter card.
- OpenBSD `sys/arch/sgi/include/mnode.h` (`IP27_BRD_PCI3 ... /* PCI
  shoehorn */`) and Linux `include/asm-ia64/sn/klconfig.h`
  (`KLTYPE_SHOEHORN`) — the IP27/SN board-type namespace.
- The project's own `docs/library/origin200-nic-inventory.md`,
  `docs/library/what-a-populated-board-identity-looks-like.md` (the real IP27
  PROM capture) and the parts index
  `docs/library/graphics-missing-acquisition-20260813-metadata/kona-missing-4dwm-root-disposition.tsv`
  (product/platform attribution; see the reconciliation below).
- The reviewed Origin 2000 topology
  `docs/machines/layers/modules/origin2000-module.yaml` (the XIO-slot
  XIO widget map).

## Role

The PCI-XTALK ADAPTER is a single XIO slot board that converts
XIO to PCI and carries **one** PCI card. On it is the XIO-to-PCI **Bridge**
ASIC (LSI L2A0710, "Bridge Rev D", SGI 099-0125-007) — the same Bridge D
revision (widget revision 4) `sgi-bridge` already models — and a single PCI
daughter-card position on a Burndy "COMPUTERBUS" connector. It plugs into one
XIO slot and presents its Bridge at that slot's XIO widget.

SGI's kernel board-type namespace names it distinctly from the multi-slot
expansion products (`IP27_BRD_PCI3` "PCI shoehorn" against `IP27_BRD_PCI1`
"PCI shoebox"; Linux `KLTYPE_SHOEHORN`). The photographed unit carried a
QLogic Fibre Channel HBA (ISP2200); the adapter is generic and holds any
single PCI card. This model fits an SGI Cad Duo input card (an `sgi-ioc3`) in
that position for the keyboard/mouse-through-XIO scenario.

## Platform attribution: a cross-platform XIO PCI adapter (three sources, reconciled)

The 030-1275 board's platform is recorded three ways in our own Library.
Reconciled, they say it is a cross-platform XIO-to-PCI adapter used on both
Octane (IP30) and Origin 2000 (IP27), with the reseller "Origin 200"
attribution the weakest of the three. Components transfer across machines,
topology does not (`docs/component-taxonomy.md`), so one adapter part
appearing on more than one platform is exactly what the evidence should look
like.

1. **Octane (IP30), an index record.** The parts index catalogues
   030-1275-003, -004 and -005 as "Octane 1 Slot PCI Shoehorn", type
   "IP30 | Option Board"
   (`docs/library/graphics-missing-acquisition-20260813-metadata/kona-missing-4dwm-root-disposition.tsv`,
   records 102-104). A web-catalogue index record, not a hardware capture.
2. **Origin 2000 (IP27), real PROM and IRIX captures -- the strongest
   evidence.** A real Origin 2000 running IP27 firmware (PROM 6.156) prints
   "Found an io Board with nic 030-1275-002" and "...030-1275-004"
   (`docs/library/what-a-populated-board-identity-looks-like.md`, from the
   cached forum PROM transcript). The same transcript's PROM `hinv` names the
   boards: `XTALK_PCI IO Board` at io5 and io6, each on a Bridge Rev 4, and a
   separate `PCI_XIO IO Board` at io2 on a Bridge Rev 3 (Library
   `forum/nekonomicon-thread-16727716-origin-is-sick-prom-pod.html`). So the
   030-1275 nics are the XTALK_PCI shoehorns, and the "Origin PCI XIO board,
   module 1 slot 2: Revision 3" line belongs to the other board, a PCI_XIO
   cardcage whose part is 030-1062-002 in another real capture (see the
   [carrier datasheet](sgi-pci-shoebox.md)). An Origin 2000 owner's `hinv -mv`
   (mimgfx.homeunix.net, held in the research notes, not yet in the Library)
   lists two shoehorns as `XTALK_PCI Board: ... part 030-1275-004 rev A` at
   `/hw/module/1/slot/io9/xtalk_pci` and `... part 030-1275-003 rev A` at
   `.../io3/xtalk_pci`.
3. **Origin 200, a reseller listing -- the weakest.** The eBay listing
   (277925514107) that photographs a 030-1275-002 REV B "PCI-XTALK ADAPTER"
   is labelled Origin 200 by the seller (`docs/library/docs.tsv`). Reseller
   provenance is not a hardware capture, and our own inventory notes 030-1275
   is not the Origin 200's own board -- a real Origin 200's Bridge reports
   030-1244-001 (`docs/library/origin200-nic-inventory.md`).

The dash-revision 030-1275-004 appears in BOTH the Octane index (record 103)
and the real IP27 capture -- the same board revision on both platforms, which
is what a cross-platform adapter looks like in the evidence. The user's
target is Origin 2000, where the strongest (real-PROM) evidence places the
board, so this models an Origin 2000 XIO adapter. Which platform a given
physical unit was pulled from, and the exact nic-to-board mapping in the IP27
transcript, stay open pending a fuller capture.

## Evidence

| Property | Status | Evidence |
|---|---|---|
| A single-XIO-slot XIO→PCI adapter carrying one PCI card exists | Established | Ingested photos: silkscreen "PCI-XTALK ADAPTER"; one COMPUTERBUS PCI-daughter position. |
| SGI assembly 030-1275-002 (rev B) / 030-1275-004 (rev A), (c)1997 | Established | Ingested photos (eBay / ntc-tech). |
| Its converter is a Bridge D ASIC, LSI L2A0710, SGI 099-0125-007 | Established | Ingested photos; matches `sgi-bridge`'s Bridge D (widget revision 4). |
| Distinct board type from the multi-slot "shoebox" | Established | OpenBSD `mnode.h`: `IP27_BRD_PCI3 /* PCI shoehorn */` vs `IP27_BRD_PCI1 /* PCI shoebox */`. |
| On Origin 2000, the reserved PCI-expansion XIO slot (IO2) is XIO widget 12 | Established | `origin2000-rack.yaml` pci-expansion attachment (`widget: 12, physical_port: 4`), cited there to the Origin/Onyx2 Hardware Quick Reference Booklet and `108-0175-001`. |
| The card behind the adapter sits at PCI device 1 | Established | Two real Origin 2000 hardware graphs: a Cad Duo's `pckb` and `pcms` at `/hw/module/1/slot/io5/xtalk_pci/pci/1/` (Library `graphics/mgras-ip27/nekonomicon-thread-5749-page1.html`), and a SCSI card at `/hw/module/1/slot/io5/xtalk_pci/pci/1/scsi_ctlr/0` behind another adapter (`forum/nekonomicon-thread-16727716-origin-is-sick-prom-pod.html`). A third, an owner's Origin 2000 listing showing the Cad Duo at `io3/xtalk_pci/pci/1` as `ef1 ... slot io3, pci 1`, was reported in review but is not located in the Library. |
| Real shoehorns sat in XIO slot 5 (and, per the unlocated listing, slot 3) | Established for io5 | The two captures above. IO2, where the removed `origin2000-pci-shoehorn` topology placed it and where `--board pci-shoehorn@io2` fits it now, is a composition choice, not a captured position; the board can be fitted in any empty XIO slot. |
| IRIX names a real adapter's board component `xtalk_pci` | Established | Both captures above. The model matches since the board-identity record below. |
| The board's identity record names it `XTALK_PCI`, part `030-1275-00x` | Established | The owner's `hinv -mv` above prints `XTALK_PCI Board` with parts 030-1275-003 and -004; the PROM `hinv` in thread 16727716 prints `XTALK_PCI IO Board`. |
| The modelled unit's record revision is `B` | Inferred | The photographed 030-1275-002's label reads REV B. A label is not the record, and the captured records are other dash numbers (rev A). |
| The PROM prints no hinv board line for a 030-1275 board | Established | The PROM's table of board part-number prefixes has no 030-1275 entry, so it types the board as unknown; both real captures list no "Origin ... board" line for their shoehorns. |
| A fitted add-in IOC3's SuperIO interrupt reaches Bridge pin `slot ^ 4` | Inferred | OpenBSD `sys/arch/sgi/pci/ioc.c` puts an add-in IOC3's SuperIO on PCI INTB. A slot's INTB reaches Bridge pin `slot ^ 4`: OpenBSD `sys/arch/sgi/xbow/xbridge.c` (`xbridge_intr_map()`, even pins to `device ^ 4`), Linux `arch/mips/pci/pci-xtalk-bridge.c` (`int_mapping[slot][1] = slot ^ 4` on IP27) and the Linux 2.5.0 SN Bridge driver (`arch/ia64/sn/io/pcibr.c`). IRIX 6.5.30 enables Bridge pins 1 and 5 for a Cad Duo at device 1. Drivers, not a hardware capture. |

## Behavioral contract

`sgi-pci-shoehorn` builds an `sgi-bridge` behind its XIO widget, at Bridge
widget revision 4, with the board-identity record below, and publishes the
Bridge's PCI bus.
A `-device ...,bus=<name>` populates it, `cad-duo-slot` fits a Cad Duo
whose one-wire identity serial `cad-duo-onewire-serial` sets, or `sio-slot`
fits SGI's dual-serial PCI card, whose record-part serial `sio-onewire-serial`
sets ([the IOC3 boards datasheet](sgi-ioc3-boards.md#the-dual-serial-pci-card)).
The adapter has one PCI position, so setting both slot properties is refused. It attaches in
one of two ways:

- **At a named slot.** `-device sgi-pci-shoehorn,slot=io2,cad-duo-slot=1`,
  or `--board pci-shoehorn@io2`, fits the board into the IO2 connector
  (Crossbow 0 widget 12) with the Cad Duo at PCI device 1; QEMU attaches it
  once every Crossbow has realized and brings its link alive. Until the
  one-off board topologies were removed, the `origin2000-pci-shoehorn`
  topology placed it there instead, alive at reset like the BaseIO's
  widget 8; the boots below come from that topology.
- **At another slot.** The qtests below also fit it at `slot=io4`
  (widget 11). The widget answers a processor for config space, MMIO, DMA
  and interrupts once the machine is up.

## Board-identity record

The Bridge's one-wire part holds the fixed-offset record the
[Bridge datasheet](bridge.md#the-record-layout-and-every-constraint-on-building-it)
lays out: part `030-1275-002` (established, the photographed label), revision
`B` (inferred, the same label), name `XTALK_PCI` (established, real
inventories) and the placeholder barcode `OXTPCI0001`, since a real barcode
names one physical unit.

```text
01 4f 58 54 50 43 49 30 30 30 31 20 20 20 20 20 20 20 20 20 20 30 33 30 2d 31 32 37 35 2d ef bf
20 20 20 30 30 32 42 20 20 20 ff ff ff ff ff ff 58 54 41 4c 4b 5f 50 43 49 20 20 20 20 20 a8 7e
```

Observed on the model (IRIX 6.5.30,
irix.boot.2026-09-24T23:55:07-07:00 (citation: `docs/boot-logs/irix.boot.2026-09-24T23:55:07-07:00.terminal.txt`)):

- the PROM prints `Found an io Board with nic 030-1275-002.` during local IO
  discovery, where it printed `030-1275-002? .` before;
- the PROM's `hinv -v` names the board `XTALK_PCI IO Board, Module 1, Slot io2`
  on `ASIC BRIDGE Rev 4, (widget 12)`, the same form as the real transcript;
- IRIX names the slot `/hw/module/1/slot/io2/xtalk_pci`, where it showed
  `______________` before, and `hinv -mvvv` prints
  `Part:030-1275-002;Name:XTALK_PCI;Serial:OXTPCI0001;Revision:B;Group:ff;Capability:ffffffff;Variety:ff;`;
- `hinv` prints no board line for io2, as on real shoehorns.

The fitted Cad Duo's own record (`PCI_ENET`, part 030-1155-002 rev D on a real
card) sits on the card's IOC3 one-wire bus, not on this Bridge's line; the
[Cad Duo datasheet](sgi-cad-duo-pci-card.md#board-identity-record-on-the-ioc3-one-wire-bus)
owns it. Without it IRIX lists `/hw/module/1/slot/io2/xtalk_pci/pci/1` in
`hinv -mvvv` with no record under it, where the owner's listing shows the
`PCI_ENET` line.

## Discovery status (what is and is not demonstrated)

- **Catalogue placement is discovered by the stock PROM and IRIX.** A 4-node
  Origin 2000 boot of the `origin2000-pci-shoehorn` topology (stock IP27 PROM
  6.156, IRIX 6.5.30 disk, launched with `--topology origin2000-pci-shoehorn`)
  printed `Found an io Board with nic 030-1275-002? .` during local IO
  discovery before the board-identity record (now `030-1275-002.`; see
  "Board-identity record") and lists no "IO link bad" for io2. IRIX boots to login and
  reports `Fast Ethernet: ef1, version 1, module 1, slot io2, pci 1` in hinv.
  The hardware graph has
  `/hw/module/1/slot/io2/______________/pci/1/{ioc3,ef,ei,pckb,pcms,tty/1,tty/2}`,
  and `/hw/input1/keyboard` and `/hw/input1/mouse` are the card's `pckb` and
  `pcms` nodes (device numbers 0,181 and 0,182 on both). At the shell the
  shoehorn Bridge's interrupt-enable register reads `0x6ffffe22` (pins 1 and
  5) with nothing pending (`docs/boot-logs/irix.boot.2026-09-24T10:16:40-07:00`).
  The io4 and io11 "IO link bad" lines in the same boot also appear on the
  plain four-node module; they are the spare-widget positions, not this board.
- **The identity defects that boot showed are fixed.** It printed a stray
  `?` after the part number and named the board component `______________`,
  both because the Bridge served an older text layout of its one-wire page.
  The board-identity record above replaces it.
- **No hinv board line is correct.** hinv prints no "PCI XIO board" line for
  io2. The PROM's table of board part-number prefixes has no entry for
  030-1275, so the PROM types a 030-1275 board as unknown and hinv lists it
  only through its PCI functions. The real "Origin PCI XIO board, module 1
  slot 2: Revision 3" line belongs to the PCI_XIO cardcage (030-1062), not to
  a shoehorn: the same transcript's PROM `hinv` shows both kinds side by side.
- **The card's two serial ports are an identity discrepancy.** hinv lists
  tty3 and tty4 and the hardware graph has `pci/1/tty/1` and `tty/2`, but a
  Cad Duo has no serial ports ([Cad Duo datasheet](sgi-cad-duo-pci-card.md);
  OpenBSD's CADDuo subdevice mask enables only the keyboard, mouse and
  Ethernet). The model gives the card no serial ports: its IOC3 population
  for this card is Ethernet, keyboard/mouse and one-wire only. IRIX creates
  those nodes from its own classification of the card, most likely keyed on
  the subsystem ID, which the model sets to the uncaptured `0xffff`
  placeholder. Open until a real Cad Duo's subsystem ID is captured.
- **A board fitted at launch has not been booted here.** Before slot
  fitting, a `-device` board landed on the spare widget, which the stock PROM
  reported as `/hw/module/1/slot/io4: IO link bad`. A shoehorn fitted at IO2
  by name has not been booted under the stock PROM since; the qtest above
  shows its link alive, which is what that PROM checks.
- **Live keystrokes through the card are not yet exercised from a guest.**
  The qtests prove the interrupt path; the boot proves IRIX attaches the
  input devices.

### Fitted dual-serial card

`sio-slot=N` fits SGI's dual-serial PCI card at PCI device N: an `sgi-ioc3`
with serial ports A and B only, subsystem id `0xffff`, and its PCI_SIO_UFC
record on the chip's own one-wire bus. Its SuperIO interrupt is wired to Bridge
pin `N ^ 4`, the line-B pin IRIX 6.5.30 takes for this card. The
[IOC3 boards datasheet](sgi-ioc3-boards.md#the-dual-serial-pci-card) owns its
evidence and placeholders.

### Fitted Cad Duo and its interrupt

`cad-duo-slot=N` fits an SGI Cad Duo card (an `sgi-ioc3`, subsystem id the
uncaptured `0xffff` placeholder) at PCI device N and wires the chip's SuperIO
interrupt output to Bridge pin `N ^ 4`, the pin the Bridge routes the card's
INTB to. The ethernet keeps INTA, pin N. The first placement wired SuperIO to
pin N. IRIX 6.5.30, with the card then at device 0, held Bridge pin 0
asserted, looped reading and clearing the ethernet interrupt status (always
0), and never finished its startup scripts, because nothing on that pin
services a SuperIO source. With SuperIO on pin `N ^ 4`, IRIX boots, enables
pins N and `N ^ 4`, and both read clear at the shell. The
wire is inferred from drivers, not captured; the stated limitation and the
discriminating test are in the [Cad Duo datasheet](sgi-cad-duo-pci-card.md),
"Interrupts".

## Related, distinct products (not modeled here, kept separate)

Several related XIO-PCI products are easily conflated; this landing models
only the shoehorn (the single-slot adapter above).

- **Shoebox — a 3-slot PCI-to-XIO expansion module.** A distinct product,
  confirmed by the preterhuman "Shoebox" wiki text and an interior photo
  showing three PCI slots (Origami PR #1577): a small shoebox-shaped card
  cage with an XIO compression connector and knurled captive thumbscrews at
  its midplane end, which registers in hinv as "XIO PCI adapter". It exists
  in two physically distinct variants (incompatible connector/screw placement
  per the wiki):
  - an **Octane (IP30)** variant -- the reseller listing gives SKU
    013-2304-001, but that number is not legible on the hardware in any
    photo, so it is a reseller SKU not visually confirmed on the unit, not an
    established SGI part number; the Octane variant is out of the current
    `sgi-sn` family regardless;
  - an **Origin 2000/Onyx2** variant -- its own SGI assembly part number is
    not established from these sources (the only part number present,
    040-1358-001, is a bottom-lid bracket, not the assembly's own number).
  The Origin 2000 shoebox is, by **well-evidenced inference rather than a
  settled naming fact**, the in-chassis "PCI Carrier Assembly" / "PCI module"
  (007-3456-003 printed pages 34 and 98: three PCI slots -- two full, one
  half -- in Origin 2000 XIO slot 2, 75 W): the function matches closely, but
  the word "shoebox" never appears in that manual and it names no part number
  for the carrier, so the equivalence is inferred, not proven. It is modeled
  separately, as the `sgi-pci-carrier` board; see
  [its datasheet](sgi-pci-shoebox.md).
- **XBOX / GIGAchannel**: the Origin 200 external XIO expansion enclosure, an
  external Crossbow reached over Crosstown cables (007-3708-002). The
  photographed cabinet's preprinted "Model No:" reads CMN A019 (XBOX-RM is
  probably the marketing code). It has five XIO slots silkscreened 15 to 11,
  a Crosstown panel labelled XIO A and XIO B, four PCI openings labelled 0 to
  3, a 100-240 VAC, 6-3 A, 720 W rating, and backplane 030-1250-00x (last
  digit illegible). "CRAYLINK" there is a paper site label, not a port. It is
  **not** a shoebox and is not modeled here; the draft
  [GIGAchannel datasheet](sgi-gigachannel.md) covers it.

## Out of scope / unknown (not modeled, not fabricated)

- Which XIO slot a shoehorn occupied in a given system, and its PCI
  device-number map — not captured; a fitted card's device number is chosen
  by `cad-duo-slot`.
- Which platform a given physical unit was pulled from, and the exact
  nic-to-board mapping in the IP27 capture (see the platform-attribution
  reconciliation above).
- Physical connector detail, bracket hardware, and bill of materials beyond
  the part numbers cited.

## qtest checklist

Covered in `tests/qtest/sn0-pci-shoehorn-test.c`:

- `sn0/pci-shoehorn/cad-duo-enumerates` — a Cad Duo fitted in the shoehorn on
  Origin 2000 answers its PCI vendor/device and subsystem identity through
  the adapter's XIO widget window.
- `sn0/pci-shoehorn/cad-duo-ps2-interrupt-reaches-hub` — driving the fitted
  card's PS/2 keyboard latches the programmed interrupt number in node 0's
  Hub `PI_INT_PEND0`.
- `sn0/pci-shoehorn/cad-duo-ps2-interrupt-dead-bare` — the same card behind a
  plain `sgi-pci-expansion`, with no SuperIO wire, leaves the Hub bit clear
  (the RED baseline the shoehorn's wiring turns GREEN).
- The keyboard-interrupt case also checks that a keystroke leaves the card's
  INTA pin (its own slot's pin) low; with SuperIO on that pin it failed.
- `sn0/bridge-identity/pci-shoehorn-record`
  (`tests/qtest/sn0-bridge-identity-test.c`) — the catalogue-placed board's
  Bridge one-wire part holds the record above byte for byte, with memory past
  it unprogrammed, no `:`, `;` or NUL in its text fields, `0xff` flag bytes,
  both pages leaving the CRC residue `0xb001`, a formatted part of
  `030-1275-002` and name `XTALK_PCI`, and Bridge widget revision 4. It
  failed against the older text layout.
- `sn0/pci-shoehorn/placed/widget-alive`,
  `sn0/pci-shoehorn/placed/cad-duo-enumerates` and
  `sn0/pci-shoehorn/placed/cad-duo-interrupt-reaches-hub` — the same checks
  on a board fitted at IO2 (widget 12) with the Cad Duo at device 1; the
  first now checks the widget reads present and alive once the machine is
  up.
