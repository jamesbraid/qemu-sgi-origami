# Datasheet: MSCSI XIO board

References rendered as citation identities name material outside this document
set. Those targets are not included or cleared for redistribution. Review and
runtime statements retain their recorded dates and scope.

**Status:** cleared by independent evidence review, 2026-09-26: the review
passed the QEMU implementation, jamesb/qemu#902 at `d972adfa9f`, and cleared
this page in jamesb/origami#1863 after the fixes it asked for. The board is a
new hardware-visible assembly (a Bridge, its board-identity record and four
QLogic ISP1040 SCSI controllers), so it is a contract change. No new register
behavior is proposed: every part on the board is an existing, reviewed model.

**QEMU file(s):** `hw/mips/sgi/baseio.c` (the `mscsi` preset of
`sgi-baseio`), `hw/mips/sgi/xio-baseio.c` (the XIO carrier that builds the
board's Bridge), `hw/mips/sgi/sn-machine.c` (the `mscsi` catalogue preset).

**Origami model:** `docs/machines/layers/modules/mscsi.yaml` (citation: `docs/machines/layers/modules/mscsi.yaml`),
fitted at launch into an empty XIO slot, such as GIGAchannel positions `io2`
and `io4` (`--board mscsi@io2 --board mscsi@io4`).

**Sources used** (none is restricted IRIX source):

- SGI HMM-408-B, *Origin2000/Onyx2 Illustrated Parts Catalog*, Library
  `sgi-hw/hmm-408-b-origin2000-onyx2-illustrated-parts-catalog.pdf`, printed
  and PDF page 33, "XIO Options": `030-0872-004` PCA MSCSI and
  `030-1243-001` PCA MSCSI B.
- Real inventories:
  - the one-module Origin 200 GIGAchannel `hinv -vm` on the Higher Intellect
    wiki's Origin 200 page (Library `wiki/preterhuman-sgi-origin-200.html`,
    SHA-256 `7e952adb…323fb4`): MSCSI boards at `io2` (part `030-1281-001`
    rev D) and `io4` (part `030-1243-001` rev F), both printed by IRIX as
    `Origin MSCSI board, module 1 slot N: Revision 3`;
  - an Origin 2000 IP27 PROM `hinv -v` in the Nekochan thread "Origin is
    sick" (Library `forum/nekonomicon-thread-16727716-origin-is-sick-prom-pod.html`,
    SHA-256 `599c1184…07dc074`): `MSCSI IO Board, Module 1, Slot io7`,
    `ASIC BRIDGE Rev 3, (widget 14)`, then `adapter PCI-SCSI Rev 5` at
    `pci id` 0, 1, 2 and 3;
  - a two-module Origin 200 `hinv -vm` posted by johnnym to the Nekochan
    forum on 9 September 2016 (thread 16731033, archived at
    `nekonomicon.irixnet.org/forum/14/16731033/1.html`, not yet in the
    Library): MSCSI boards at
    `io5` and `io6` of a GIGAchannel, part `030-1243-001` rev M, printed as
    `Origin MSCSI board, module 2 slot 5: Revision 4`;
  - a December 2001 comp.sys.sgi.hardware owner inventory with an MSCSI
    `030-1243-001` rev G in an Origin 2000, the one the
    [BaseIO datasheet](baseio.md) already cites.
- Disassembly of the stock IP27 PROM 6.156 payload's board classifier, as
  the [IOC3 boards datasheet](sgi-ioc3-boards.md) records it: a record
  whose part starts `030-0872-`, `030-1243-` or `030-1281-` is board type
  `0x22`, which public Linux `klconfig.h` names `KLTYPE_MSCSI`.
- `strings` over the IRIX 6.5.30 `unix.IP27` kernel (SHA-256 prefix
  `cbdb02ea5d33`): the same three part prefixes, and five messages
  beginning `Probing MSCSI/A` and `Probing MSCSI/B` that name an internal
  single-ended bus on channel 0 and external single-ended or differential
  buses. The code that prints them was not found or read.
- Existing reviewed contracts: [ISP1040](isp1040.md), [Bridge](bridge.md).

Grades: **established** means a capture, a manual or the shipped binaries say
it directly; **inferred** means it follows from established facts by a stated
step; **unknown** means no source here settles it.

## Role

The MSCSI is SGI's XIO SCSI board for the Origin family: one Bridge with four
SCSI controllers on its PCI bus. It fits an Origin 2000 XIO slot other than
IO2, and a GIGAchannel expansion cabinet's XIO positions. (Established:
parts catalogue, the Origin 2000 XIO configuration rules the
Origin 2000 reference (citation: `docs/machines/origin2000.md`) cites, and the three
inventories above.)

## Composition

| Fact | Value | Grade and evidence |
|---|---|---|
| Part numbers | `030-0872-004` PCA MSCSI; `030-1243-001` PCA MSCSI B; `030-1281-001` also prints as MSCSI | Established: parts catalogue for the first two, the wiki inventory for the third. How the three differ is unknown. |
| Record name | `MSCSI`; hardware-graph node `mscsi` | Established: `Location: /hw/module/1/slot/io4/mscsi`, `MSCSI Board: ... part 030-1243-001 rev F`. |
| Bridge | Revision 3 on the wiki's boards and the Origin 2000 board; revision 4 on the rev M boards | Established. IRIX prints the Bridge revision as the board revision (`Revision 3`), the same way it does for the cabinet's own Bridge. |
| SCSI controllers | Four QLogic ISP1040 parts at PCI devices 0 to 3, PCI revision 5, vendor `0x1077` device `0x1020` | Established: the PROM prints four `adapter PCI-SCSI Rev 5` at `pci id` 0-3 behind the board's Bridge, and IRIX lists `0x1077:0x1020` at PCI slots 0-3 under an MSCSI board. IRIX names each controller `QL1040B (rev. 2)`, as it does the IP29's own. |
| Other PCI functions | None | Inferred: the PROM's inventory of the board lists only the four adapters. |
| Bridge external flash | None | Inferred: nothing loads a PROM from an MSCSI, and no inventory mentions one. |
| Interrupts | Each controller's INTA on the Bridge pin matching its device number | Inferred: the Bridge's ordinary per-slot rule, which the BaseIO and IP29 ISP1040s use; no source describes a different wiring on this board. |
| Bus type per channel | Single-ended or differential per controller | Inferred that real MSCSI boards carry differential channels. The wiki inventory's IRIX prints nine of its fifteen QL1040B controllers as differential. At most seven of the fifteen are off the MSCSIs: two on the IP29, four in the cabinet's PCI openings and one PCI card on the IP29 Bridge. So at least two differential channels sit on the MSCSIs. The kernel's messages name an internal single-ended bus on channel 0 and external buses of either kind. Which of the four controllers is which, and how IRIX learns it, is unknown. |

## Board-identity record

The record the model presents is a real board's part, revision and name with
a placeholder barcode: part `030-1243-001`, revision `F`, name `MSCSI`, on
the board Bridge's own one-wire line, with Bridge revision 3. That is the
board at `io4` of the wiki's one-module GIGAchannel. The record layout is the
one the [Bridge datasheet](bridge.md) specifies for every Bridge-based board.
The barcode is a placeholder; the per-unit serial comes from the catalogue.

## Slot rules

- Origin 2000: not IO2, which the configuration rules reserve. (Established.)
- GIGAchannel: any of `io2` to `io6`. Real systems carry it at `io2`, `io4`,
  `io5` and `io6`. (Established.)
- The board's placement fixes its widget: the Crossbow link of its slot. It
  has no fixed widget of its own. (Established by the captures: widget 14 on
  an Origin 2000, the `io` positions of a GIGAchannel.)

## Model contract

- An `sgi-xio-baseio` with the `mscsi` preset: a Bridge at revision 3 with
  the record above, and four `qlogic-isp1040` functions at PCI devices 0 to 3
  with PCI revision 5, on the ordinary per-slot interrupt rule.
- No IOC3, no Timekeeper, no serial or parallel ports and no flash part: the
  preset refuses a backend for any of them.
- It fits any XIO widget. The Origin 2000's rule keeping it out of IO2 is
  not enforced, a model limitation.

## Discriminating tests

- PROM `hinv -v`: `MSCSI IO Board, Module 1, Slot ioN` with
  `ASIC BRIDGE Rev 3, (widget W)` and four `adapter PCI-SCSI Rev 5` at
  `pci id` 0 to 3, where W is the slot's Crossbow link.
- IRIX `hinv -vm`: `Location: /hw/module/1/slot/ioN/mscsi` with
  `MSCSI Board: ... part 030-1243-001 rev F`, and four more QL1040B
  controllers.
- qtest: four `0x1077:0x1020` functions at devices 0-3, revision 5; nothing
  at device 4; the board record read back byte for byte; the placed widget
  alive at reset.

## Unknowns and model selections

| Unknown | Model selection | What would settle it |
|---|---|---|
| Which controllers are differential, and how IRIX tells | None: every controller presents whatever the ISP1040 model reports for its bus | A capture of one MSCSI alone with its controllers' types, or the IRIX routine behind the `Probing MSCSI` messages read by disassembly |
| What distinguishes `030-0872`, `030-1243` and `030-1281` | One preset, `030-1243-001` rev F | A manual or catalogue entry for each |
| Interrupt wiring on the board | Per-slot rule | A board schematic, or a guest that fails under the per-slot rule |
| The board's own PCI subsystem ids on the ISP1040s | The ISP1040 model's own | A PCI configuration dump from a real board |

## Not modeled

The internal SCSI cabling, the external connectors' termination and the
distinction between the MSCSI and MSCSI B parts.
