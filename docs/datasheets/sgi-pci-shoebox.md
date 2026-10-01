# Datasheet: Origin 2000 PCI Carrier Assembly (the PCI "shoebox")

References rendered as citation identities name material outside this document
set. Those targets are not included or cleared for redistribution. Review and
runtime statements retain their recorded dates and scope.

**Status:** draft, pending independent review. It adds no register behavior.
The carrier is a Bridge and a PCI bus, and the model reuses both. The
board-identity record and Bridge revision below are a hardware-visible
identity change, also pending independent review.

**QEMU file(s) on `sgi-origami`:** `hw/mips/sgi/board-pci-expansion.c`
(`sgi-pci-carrier`, the `sgi-pci-expansion` board with three slots and the
identity below as class defaults), fitted with
`-device sgi-pci-carrier,slot=io2`.

**Origami model:** `docs/machines/layers/modules/pci-carrier.yaml` (citation: `docs/machines/layers/modules/pci-carrier.yaml`),
fitted at launch into Origin 2000 IO2 (`--board pci-carrier@io2`).

**Sources used:**
- SGI 007-3456-003, *Origin2000 Rackmount Owner's Guide*: "PCI Carrier
  Assembly" and Figure 2-11 (printed page 34, PDF page 54), and "XIO Board
  Slots" (printed page 98, PDF page 118).
- OpenBSD `sys/arch/sgi/include/mnode.h`, whose public constants
  `IP27_BRD_PCI1` and `IP27_BRD_PCI3` distinguish the shoebox and shoehorn
  products, for the product name.
- The [PCI shoehorn datasheet](sgi-pci-shoehorn.md), "Related, distinct
  products", for the shoebox photographs and reseller material already
  reconciled there.
- Real IP27 inventories: IRIXnet forum post 32252 (Library
  `forum/irixnet-post-32252-ip27-6.5.30-hinv.html`, an Onyx2 deskside on IRIX
  6.5.30 and PROM 6.156) and Nekonomicon thread 16727716 (Library
  `forum/nekonomicon-thread-16727716-origin-is-sick-prom-pod.html`).
- SGI's part-number table (Library `reference/sgi-pca-numbers.html`).

## Role

An XIO board that turns one crosstalk slot into three PCI slots for
third-party cards. It carries one Bridge. The Bridge presents the board at its
XIO slot's crosstalk widget and masters the PCI bus the three slots share.

## Evidence

| Property | Status | Evidence |
|---|---|---|
| The carrier holds up to three third-party PCI boards: two full-size and one half-size | Established | 007-3456-003 printed page 34 describes capacity for three third-party PCI boards, with two full-size positions and one half-size position. |
| It provides up to 75 W | Established | 007-3456-003 printed page 34. Not modeled: power is outside the emulator's contract. |
| The "PCI module" goes in XIO slot 2, which is Crossbow 0 widget 12 | Established | 007-3456-003 printed page 98 assigns the PCI module to XIO slot 2. The widget number is the reviewed IO2 attachment in `origin2000-module.yaml`. |
| The "PCI module" of page 98 is this PCI Carrier Assembly | Inferred | The manual uses both names without equating them. The carrier is the only PCI option the same guide describes for the module, so the reading is likely, but it is not stated. |
| The carrier's converter is a Bridge | Inferred | Every SGI XIO-to-PCI board this project has evidence for uses a Bridge. The manual does not name the chip. |
| The carrier is the product SGI kernels call the "PCI shoebox" | Inferred | Function matches: three PCI slots in one XIO slot. The owner's guide never says "shoebox", and no part number ties the two together. |
| The PCI device number of each slot | Unknown | Not in any source read. The model selects devices 0, 1 and 2: the Bridge's first three device positions, one per physical slot. |
| A real PCI XIO board in slot 2 is the PCI_XIO cardcage, part 030-1062-002, record revision C, name PCI-XIO | Established | IRIXnet post 32252: `PCI-XIO Board: barcode EMR505 part 030-1062-002 rev C` and `Origin PCI XIO board, module 1 slot 2: Revision 3`. Nekonomicon thread 16727716's PROM `hinv`: `PCI_XIO IO Board, Module 1, Slot io2` on `ASIC BRIDGE Rev 3, (widget 12)`. SGI's part table: 030-1062-002 is the "Origin 2000 PCI cardcage, PCI_XIO". |
| This carrier is that PCI_XIO cardcage | Inferred | Both are the PCI option fitted in XIO slot 2, and the cardcage is the only slot-2 PCI board any real capture shows. The owner's guide names no part for the carrier, so nothing ties the two by number. |
| The Bridge revision is 3 | Established for two real boards; universality unknown | Both real PCI_XIO boards above report Bridge Rev 3. The model reports widget revision 3 but implements Bridge D behavior; no difference between the revisions that a guest exercises on this board is known. |
| The Bridge's one-wire identity content | Established for the fields, inferred for this product | See "Board-identity record" below. |
| How each slot's INTA..INTD reach Bridge pins | Inferred | A slot's INTA/INTC reach pin `device`, INTB/INTD reach pin `device ^ 4`, per the public Linux SN Bridge driver (Linux 2.5.0 `arch/ia64/sn/io/pcibr.c`). This is the Bridge's general mapping, not a carrier capture. |

## Behavioral contract

`sgi-pci-expansion` with `slots=3` builds an `sgi-bridge` at its widget and
reserves every PCI device position from 3 upward on its bus. A card plugged
with `-device ...,bus=io2.0,addr=0|1|2` (the board's bus is named by its
slot) enumerates through the
board's widget. A card at any other address is refused at realize time,
because the carrier has no fourth slot. `slots=0` (the default) keeps the bare
expansion board's old behavior: every Bridge device position is open.

A card's second interrupt follows the inferred mapping in the table above. An
IOC3 fitted in a slot, such as a Cad Duo, raises its SuperIO interrupt on the
card's INTB, so once machine creation is done the board wires the chip's
SuperIO output to Bridge pin `device ^ 4`, as the
[PCI shoehorn](sgi-pci-shoehorn.md) wires its own fitted card, unless something
already wired it. Before that, a Cad Duo's keyboard never reached IRIX from
the carrier (jamesb/qemu#874). `sn0/pci-shoehorn/cad-duo-ps2-interrupt-on-carrier`
checks that a keystroke reaches node 0's Hub.

## Board-identity record

The Bridge's one-wire part holds the fixed-offset record the
[Bridge datasheet](bridge.md#the-record-layout-and-every-constraint-on-building-it)
lays out, with the PCI_XIO cardcage's real part, revision and name and the
placeholder barcode `OPCIXIO001`:

```text
01 4f 50 43 49 58 49 4f 30 30 31 20 20 20 20 20 20 20 20 20 20 30 33 30 2d 31 30 36 32 2d 6e 29
20 20 20 30 30 32 43 20 20 20 ff ff ff ff ff ff 50 43 49 2d 58 49 4f 20 20 20 20 20 20 20 de 5f
```

The IP27 PROM's table of board part-number prefixes lists `030-1062-` as a PCI
XIO board, so the PROM types the carrier the way it types a real cardcage.
`sgi-pci-expansion` gains pass-through `board-part`, `board-barcode`,
`board-revision`, `board-name` and `bridge-revision` properties, which the
`sgi-pci-carrier` class sets as its defaults. A bare `-device sgi-pci-expansion` keeps
the placeholder part, the older text layout and the Bridge's default revision.

## Guest discovery

Before the board-identity record, an IRIX 6.5.30 boot of the
`origin2000-pci-carrier` topology with a QLogic ISP12160 at carrier device 0
(stock IP27 PROM 6.156) showed the PROM finding
the board (`Found an io Board with nic NOT-OBSERVED? .`, the model's
placeholder part) with no "IO link bad" for io2. IRIX listed `Integral SCSI
controller 2: Version QL12160, single ended` and controller 3, one per
channel, and `/hw/scsi_ctlr/2` and `/hw/scsi_ctlr/3` resolve to
`/hw/module/1/slot/io2/______________/pci/0/scsi_ctlr/0` and `/1`
(`docs/boot-logs/irix.boot.2026-09-24T10:27:28-07:00`, launched with
`--topology origin2000-pci-carrier`, a catalogue topology since replaced by
`--board pci-carrier@io2`). The blank board-name component and
the stray `?` came from the Bridge serving an older text layout of its
one-wire page.

With the board-identity record (IRIX 6.5.30, the same card,
irix.boot.2026-09-25T00:46:19-07:00 (citation: `docs/boot-logs/irix.boot.2026-09-25T00:46:19-07:00.terminal.txt`)):

- the PROM prints no "Found an io Board" line, because it now types the board;
- the PROM's `hinv -v` shows `PCI_XIO IO Board, Module 1, Slot io2` on
  `ASIC BRIDGE Rev 3, (widget 12)`, with the card as
  `adapter ID (Vendor 1077 Device 1216 class 1 subclass 1) (pci id 0)`, the
  same form as the real Nekonomicon transcript;
- IRIX names the slot `/hw/module/1/slot/io2/pci_xio`, prints
  `Origin PCI XIO board, module 1 slot 2: Revision 3` as the real capture
  does, and `hinv -mvvv` prints
  `Part:030-1062-002;Name:PCI-XIO;Serial:OPCIXIO001;Revision:C;Group:ff;Capability:ffffffff;Variety:ff;`.

The PROM's `hinv -v` lists no adapter for the empty device positions, as a
real listing does not, now that the Bridge reports an unclaimed
configuration cycle as a master time-out
(firmware.boot.2026-09-25T21:56:15-07:00 (citation: `docs/boot-logs/firmware.boot.2026-09-25T21:56:15-07:00.terminal.txt`);
the [Bridge datasheet](bridge.md) has the contract). It used to list a
`Vendor ffff Device ffff` adapter for each.

The approximated device numbers are the only thing standing between this and
a hardware-faithful slot map. A capture of a real carrier's `hinv -v`, or of
PCI configuration reads through it, would settle them.

## Not modeled here

- The **Octane (IP30) shoebox**: out of the `sgi-sn` family.
- The **Origin 200 GIGAchannel / XBOX**: an external Crossbow chassis, a
  different product (see the [shoehorn datasheet](sgi-pci-shoehorn.md)).
- Power, cooling and the physical full-size/half-size distinction. A model
  card does not have a length.

## qtest checklist

Covered in `tests/qtest/sn0-pci-carrier-test.c`:

- `sn0/pci-carrier/widget-alive`: a carrier fitted at IO2 reads a widget
  present and its link alive at Crossbow 0 port 12 once the machine is up.
- `sn0/pci-carrier/card-enumerates-in-each-slot`: a card fitted at each of
  devices 0, 1 and 2 answers its PCI identity through widget 12.
- `sn0/pci-carrier/fourth-slot-refused`: a card at device 3 is refused.

Covered in `tests/qtest/sn0-bridge-identity-test.c`:

- `sn0/bridge-identity/pci-carrier-record`: the carrier's Bridge one-wire part
  holds the record above byte for byte, the fields pass every reader's checks,
  the formatted part is `030-1062-002` and name `PCI-XIO`, and the Bridge
  reports widget revision 3. It failed against the placeholder text page.
