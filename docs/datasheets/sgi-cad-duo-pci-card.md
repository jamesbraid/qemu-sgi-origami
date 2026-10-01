# Datasheet: SGI Cad Duo PCI Card

References rendered as citation identities name material outside this document
set. Those targets are not included or cleared for redistribution. Review and
runtime statements retain their recorded dates and scope.

**Status:** reviewed for the original Origin 200 backplane add-in
(independent agent review, 2026-09-17), with a later material change PENDING
renewed review. The 2026-09-17 review cleared the product-identity evidence
chain, reuse of the existing CAD DUO population contract, the decision to
leave the add-in instance's second interrupt output unconnected rather than
guess a Bridge pin, and the qtest and boot-log support for the
dual-enumeration claim. **A subsequent revision (see "Interrupts", the
"PCI-XTALK ADAPTER" subsection) wires the SuperIO interrupt, to the card's
INTB, when the card is fitted into the Origin 2000 PCI shoehorn — a material
change to the previously-ratified "leave unconnected" decision, not yet
independently reviewed.** A second change, "Board-identity record on
the IOC3 one-wire bus" below, adds the card's own record part; its bus,
family and contents claims were independently reviewed on 2026-09-26. The review found the
exposure-disclosure paragraph below had reproduced the flagged snippet
instead of only describing it, and found `docs/NOW.md`'s capability
paragraph dropped this document's own shared-host-input caveat; both are
corrected in this revision. This document names a real SGI option board
and grounds it in the chip-level contract this project already implements
and has already qtested. It proposes no new register behavior.

**QEMU file(s) on `sgi-origami`:** none. The add-in card is the existing
`sgi-ioc3` PCI device (`hw/misc/sgi-ioc3.c`), attached a second time at a
free Origin 200 PCI backplane slot. The only source touched is a named
constant, `SGI_IOC3_SUBSYS_CAD_DUO_PCI_CARD_UNCAPTURED`, added next to the
class's other board identities in `include/hw/misc/sgi-ioc3.h`.

**Sources used:** the project's own reviewed IOC3 chip datasheet
([`docs/datasheets/ioc3.md`](ioc3.md)), which already documents the "CAD
DUO" population and cites mainline Linux's per-board dispatch table
(`drivers/mfd/ioc3.c`, v7.2-rc7, commit `db2ddb8`) for it; two Library
records described below; the existing component
taxonomy (citation: `docs/component-taxonomy.md`); and, for the board record, IRIX 6.5.30
kernel disassembly, an owner's capture and OpenBSD's IOC3 driver (see
provenance (citation: `docs/provenance.md#cad-duo-board-record-on-the-ioc3-one-wire-bus-2026-09-25`)). No restricted IRIX source informed
this document — see "Protected-source exposure to disclose" below for one
incidental exception this document does not rely on.

## Role

The Origin 200's system board carries one onboard IOC3
(`docs/machines/layers/modules/ip29.yaml`; subsystem identity `0xC303`, "IP29
System Board", per `docs/datasheets/ioc3.md`). That chip's PS/2 block is the
machine's only native keyboard/mouse port. This datasheet covers a second,
distinct, physically separate PCI card that plugs into one of the machine's
free option slots (Origin 200's PCI backplane carries slots 5 through 7,
`docs/machines/origin200.md`) and carries its own IOC3, adding a second,
fully independent Ethernet interface and PS/2 keyboard/mouse pair without
touching the system board's own ports.

The real product is **the SGI Cad Duo PCI Card, SGI option board
030-1155-002** — established below. It is the same real-world class of
hardware already named informally elsewhere in this project's own
documents: `docs/datasheets/psitech-rad4-km.md` already distinguishes "a
parallel research thread investigating a generic IOC3-based PCI
keyboard/mouse card (CadDuo)" from the PsiTech RAD4-KM product, without
resolving what that card is. This datasheet is that resolution.

On real Origin-family hardware this card served exactly the scenario this
project wants to model: a second keyboard and mouse for a second seat, or a
console kept on the system board's native ports while a graphics add-in
card (such as PsiTech RAD4-KM, or the RAD4-class board this project already
implements) drives a second head for a second user.

## Evidence: identity

| Property | Status | Evidence |
|---|---|---|
| A real, named SGI option board called "Cad Duo" exists, SGI part number 030-1155-002, category "Option Board" | Established | `origami-docs:library/graphics-missing-acquisition-20260813-metadata/kona-missing-4dwm-root-disposition.tsv`, record 93: `030-1155-002 / Option Board / SGI Cad Duo PCI Card`. This is a retained index record from a web-catalogued SGI parts listing, not a page image from SGI's own manual; an independent review of the same disposition lane (`kona-missing-acquisition-review.md`, 2026-08-13) already flagged that lane's source ancestry as incompletely recorded, so treat the part number and name as established but the index's own provenance as not fully retraced. |
| SGI used "CadDuo" as a real, first-party product/configuration name, not only a reseller listing | Established | SGI's own IRIX 6.4 graphics patch release notes for SG0003366/SG0003662 (`irix-6.4` globe patch series, Jurassic FTP mirror) describe "Octane CADduo dual-head operation" as a named, supported configuration alongside SI/SI+TX/SSI/MXI graphics (`docs/research/graphics/ftp-patch-3366-3662-mgras-contract.md`). |
| A real owner used a "CadDuo PCI keyboard/mouse" card as a second-seat/second-head input device, in PCI slot 5 of an Origin 2000, alongside a graphics board in a different slot | Owner-attested | Nekonomicon thread 5749, "Origin 2000 Deskside + SI-Tram Graphics" (`docs/research/graphics/configurations.md`, "Origin 2000 with the archived unusual MGRAS SI+TRAM attachment"; citation `[O2]`). The same report describes configuring "CadDuo input for Xsgi." |
| The generic IOC3 "CAD DUO" population this project already models (Ethernet with its one-wire master, PS/2, no serial, no clock) is a real fallback Linux uses for every board identity it has no dedicated entry for, including several real, named later SGI boards | Established | `docs/datasheets/ioc3.md`, "Board variants" table and its citation of `drivers/mfd/ioc3.c` (mainline Linux v7.2-rc7, commit `db2ddb8`); already implemented as `sgi_ioc3_cad_duo` in `hw/misc/sgi-ioc3.c` and already qtested (`tests/qtest/sn0-ioc3-test.c`, `ioc3_boards[]`, rows `cad-duo-io7` and `cad-duo-other`). |
| The exact PCI subsystem identity SGI wired onto a real Cad Duo PCI Card | Unknown | No configuration-space capture, board photograph, or first-party register listing for this specific board has been located. This datasheet does not guess one; see "Modeled identity" below. |
| A real card serves its own board-identity record: name `PCI_ENET`, part `030-1155-002`, revision `D` | Established | An Origin 2000 owner's `hinv -vm` (the mimgfx Origin 2000 page, formerly mimgfx.homeunix.net, Library `forum/mimgfx-origin2000-owner-inventory.html`) prints `PCI_ENET Board: barcode HKV898 part 030-1155-002 rev D` at `/hw/module/1/slot/io3/xtalk_pci/pci/1`, the card behind a PCI shoehorn, with its Ethernet as `ef1 ... slot io3, pci 1`. |
| That record is read from the card's own IOC3 one-wire bus, from a part of family `0x0b` or `0x09`, beside the Ethernet-address part | Bus and families established by IRIX disassembly; the part's exact family inferred | See "Board-identity record on the IOC3 one-wire bus". No capture shows the part's ROM identity. |
| The card's physical connectors, silkscreen, and bill of materials | Unknown | Not established by any source consulted. |

**Protected-source exposure to disclose.** While searching the Library for
"Cad Duo subsystem" without a `roots` filter broad enough to exclude it, one
result surfaced a one-line snippet from `origami-docs:reviews/
ioc3-serial-codex-fourclaim-transcript.txt` that reads as a driver
registration-table entry associating the board-part-number prefix with a
handler — the shape of a raw line from an IRIX-style driver source table.
This session did not open that file, does not know its full contents, and
does not reproduce the snippet here: republishing an apparent vendor-source
fragment into a datasheet meant for any implementer to read would defeat
the point of disclosing it. Nothing in this datasheet or in the project's
IOC3 implementation is drawn from it — the fact it happens to corroborate
(that the card's part-number prefix is a real, recognized one) is
independently established above from the parts-index record and the two
public SGI/owner sources. This is flagged because a review transcript
containing an apparent verbatim vendor-source line appears to be indexed
under a `public`-classified Library root (`origami-docs`); whoever owns
that lane should confirm whether it belongs under a restricted root
instead, and the literal snippet should reach them directly rather than
travel through further documents.

## Modeled identity

Because the real card's subsystem identity was not found, this model uses
`SGI_IOC3_SUBSYS_CAD_DUO_PCI_CARD_UNCAPTURED` (`0xFFFF`,
`include/hw/misc/sgi-ioc3.h`) — an explicit "not observed" placeholder, the
same convention `hw/mips/sgi/board-baseio.c` and `hw/mips/sgi/board-io8.c`
already use for an IOC3 population whose real subsystem identity is
unknown. Leaving the card at the `sgi-ioc3` class's own default
(`SGI_IOC3_SUBSYSTEM_ID`, the IP29 System Board's `0xC303`) would
misrepresent the add-in card as a second system board; picking one of the
seven identities `docs/datasheets/ioc3.md` already assigns to other real
boards would misrepresent it as that different board. `0xFFFF` is outside
every identity `sgi-ioc3` names, so it already resolves through
`sgi_ioc3_board()`'s existing fall-through to the CAD DUO population —
which is the behavior this datasheet is grounding, not new behavior.

A real capture of the card's own subsystem identity should replace this
placeholder and this note, the same way `docs/datasheets/ioc3.md`'s Origin
300 supplement already describes for its own `0xFFFF` placeholder.

## Register map

Identical to the chip's own contract in `docs/datasheets/ioc3.md`; this
document defines no new registers. The guest-visible population is:

| Function | Present | Notes |
|---|---|---|
| Ethernet, with its one-wire master | Yes | Independent station address and buffer RAM from the native chip; a second Ethernet interface. |
| PS/2 keyboard and mouse | Yes | The same five-register interface `docs/datasheets/ioc3.md` ("PS/2 keyboard and mouse") describes: control/status, a receive and transmit register per port. |
| Serial ports A and B | No | Matches the CAD DUO row of the board-variant table. |
| Timekeeper (M48T35-class RTC/NVRAM) | No | Matches the CAD DUO row; the machine's own Timekeeper at the native chip is unaffected. |
| Parallel port | No | Not part of the CAD DUO population. |

## Behavioral contract

Realized as an ordinary, independent `sgi-ioc3` PCI function on the Origin
200 Bridge's PCI bus, at a free option slot (this datasheet uses device 6;
device 5 is reserved for the RAD4-class graphics card this project already
implements, and device 7 stays free). Nothing about the card's own register
behavior depends on which slot it occupies — `docs/datasheets/ioc3.md`
already establishes that IOC3's sub-devices are wired the same way
regardless of population, and the population here is fixed by the
identity above, not by slot number.

Two IOC3 instances present on the same machine are already independent by
construction: each is its own PCI function with its own configuration
space, BAR and register file, and neither device model shares state with
the other. This is the same reasoning the project's own taxonomy already
gives for the Origin 2000's BaseIO-G-plus-MIO configuration ("A machine may
also hold more than one IOC3", `docs/component-taxonomy.md`).

## Interrupts

Ethernet's PCI INTA reaches Bridge through the ordinary per-slot mapping
`sgi_bridge_map_irq()` already applies to every PCI function
(`hw/pci-host/sgi-bridge.c`) — at device 6 that lands on Bridge pin 6,
exactly the way the RAD4-class card's own PCI function already does at
device 5 landing on pin 5.

**The PS/2 (SuperIO) interrupt is not wired to any Bridge pin for this
add-in instance, and this is a stated limitation, not new behavior.** The
native system-board IOC3's SuperIO interrupt reaches Bridge pin 3 through a
direct board wire the Origin 200's own board file installs
(`hw/mips/sgi/board-ip29-io.c`), a fact `docs/datasheets/ioc3.md`
("Interrupts") describes as settled for that specific board and explicitly
declines to generalize: "the pin is a property of the board and no register
described here changes with it." That same section separately records
OpenBSD's own driver source describing the SuperIO interrupt as landing on
PCI interrupt pin B for an add-in IOC3 (as against pin D for an on-board
one) — real, cited, public evidence that a genuine add-in card's second
interrupt is expected to reach the host somehow, but not a capture of which
Bridge pin an Origin 200 option slot actually wires it to. Inventing a
specific pin here would be exactly the mistake `docs/datasheets/ioc3.md`
already records once, and corrected once, for the on-board case ("An
earlier sign-off said pin 4, on two reasons that were both established as
wrong"). Until a real board capture settles it, this add-in instance's
`superio_irq` output stays unconnected, matching what already happens for
any bare `sgi-ioc3` device attached without a board file.

**Consequence for a guest driver.** The chip's own internal interrupt-status
register still correctly latches and reports pending keyboard/mouse bytes
(readable and pollable at the register level, and independently qtested —
see below), and PCI enumeration, subsystem identification and population
are unaffected. A driver that requires the SuperIO interrupt to notice a
new keystroke on this specific add-in instance will not see it fire in this
model. This project's own boot testing of this card
(`docs/boot-logs/irix.boot.2026-09-17T10:07:29-07:00`) exercises
enumeration only, not interrupt-driven live typing on the add-in card's
ports, for exactly this reason.

### The PCI-XTALK ADAPTER ("shoehorn") fitting wires it to INTB — an approximation

**This subsection is a material change to the interrupt contract above and
is PENDING renewed independent review of the interrupt-delivery claim.** Its
first version, which wired SuperIO to the card's INTA pin, was wrong and is
replaced here; the reason is below.

When this card is fitted into the SGI PCI-XTALK ADAPTER on Origin 2000,
modeled by `hw/mips/sgi/board-pci-shoehorn.c` (the "PCI shoehorn"), that board
wires the card's SuperIO interrupt output to the card's INTB, which the Bridge
routes to interrupt pin `slot ^ 4`. The ethernet stays on INTA, pin `slot`.
A PS/2 keyboard reply raises pin `slot ^ 4`, the internal Bridge forwards an
interrupt packet up through the Crossbow to the host node's Hub, and the
guest receives the keystroke.

**Basis.** A Cad Duo is a plug-in PCI card, so its interrupts can only leave
on the card edge's INTA..INTD. Three independent readers agree on where the
second one goes:

- OpenBSD's IOC3 driver (`sys/arch/sgi/pci/ioc.c`) takes an add-in IOC3's
  SuperIO interrupt from PCI INTB (and an on-board one's from INTD), as
  `docs/datasheets/ioc3.md`, "Interrupts", records.
- Three Bridge drivers route a slot's INTA/INTC to Bridge pin `slot` and its
  INTB/INTD to pin `slot ^ 4`: OpenBSD `sys/arch/sgi/xbow/xbridge.c`
  (`xbridge_intr_map()`, even interrupt pins to `device ^ 4`), Linux
  `arch/mips/pci/pci-xtalk-bridge.c` (`int_mapping[slot][1] = slot ^ 4` on
  IP27), and the Linux 2.5.0 SN Bridge driver (`arch/ia64/sn/io/pcibr.c`).
- IRIX 6.5.30 enables exactly those two pins on the shoehorn's Bridge: pins
  0 and 4 with the card at device 0 in the first boot, and pins 1 and 5
  with the card at device 1, where real captures place it (interrupt-enable
  register read live, `docs/boot-logs/irix.boot.2026-09-24T10:16:40-07:00`).

**Why the INTA version was wrong.** With SuperIO sharing the ethernet's pin
(the card then at device 0), IRIX 6.5.30 stalled after "The system is coming
up.": the shoehorn Bridge held pin 0 asserted, and the guest looped reading
and clearing the ethernet interrupt status, which was always 0, because
nothing it runs on pin 0 services a SuperIO source. With SuperIO on the INTB
pin the same boot reaches login and both pins read clear.

**Serial ports IRIX reports but the card lacks.** Fitted in the shoehorn,
IRIX lists two extra IOC3 serial ports (tty3, tty4) and builds `tty/1` and
`tty/2` under the card's hardware-graph node. This card has no serial ports,
and OpenBSD's CADDuo subdevice mask agrees (keyboard, mouse and Ethernet
only). The model does not create them: this card's IOC3 population is
Ethernet, keyboard/mouse and one-wire. IRIX classifies the card itself,
most likely from its subsystem ID, which is the uncaptured `0xffff`
placeholder here. This is an identity discrepancy, not a claim that the
ports work, and stays open until a real card's subsystem ID is captured.
**Resolved by the card's board record:** IRIX classifies the card from the
`030-1155-` record on its one-wire bus, and with the record fitted the two
ports are gone (see "Board-identity record on the IOC3 one-wire bus").

**Stated limitation.** Which card-edge line the real Cad Duo drives is
inferred from the drivers above, not captured on a card. A board trace, or a
capture of the pins a real system enables for one, would settle it.

**Discriminating test.** `tests/qtest/sn0-pci-shoehorn-test.c` drives the
fitted card's PS/2 keyboard, arms Bridge pin `slot ^ 4`, and asserts node 0's
Hub `PI_INT_PEND0` latches the programmed interrupt number and that the
card's INTA pin stays low (`sn0/pci-shoehorn/cad-duo-ps2-interrupt-reaches-hub`,
and its `placed/` twin on the catalogue-placed board). With the INTA wiring
the keystroke never reached the Hub on the armed pin and the test failed. A
companion case shows the same card behind a plain `sgi-pci-expansion`, which
does no such wiring, leaving the Hub bit clear
(`sn0/pci-shoehorn/cad-duo-ps2-interrupt-dead-bare`).

## Board-identity record on the IOC3 one-wire bus

**Reviewed** (independent agent review, 2026-09-26, landed as QEMU
`sgi-origami` PR 840). A real card carries its own SGI board record, and the
model fits one when the card is fitted into the PCI shoehorn.

Each fact below names its source class: **capture** (a real machine's
output), **IRIX disassembly** (the IRIX 6.5.30 IP27 kernel,
`assets/boot/6.5.30/unix.IP27`, SHA-256 `cbdb02ea5d334eca…`, read with
`tools/origami disasm` by its own symbol names), **PROM disassembly**,
**public driver** (OpenBSD or Linux, a cross-check only) or **model** (what
the emulator was observed to do). Where IRIX and a public driver differ,
the contract follows IRIX.

### What IRIX reads, and from where

| Fact | Grade and source |
|---|---|
| A real card reports name `PCI_ENET`, part `030-1155-002`, revision `D`, at its own PCI vertex (`.../io3/xtalk_pci/pci/1`), below the adapter's own `XTALK_PCI` record | Established, capture (the owner's `hinv -vm` under "Evidence: identity") |
| The record is read from the card's IOC3 one-wire bus. `ioc3_attach` calls `nic_ioc3_vertex_info` with the chip's MicroLAN control register (BAR offset `0x30`) and pin-control set register (`0x34`), unless the IOC3's hardware-graph path contains `baseio` or `menet`, the boards whose records sit on their Bridge's line. `nic_ioc3_vertex_info` sets pin-control bit 21 (`0x200000`, the MicroLAN enable) and hands the bus to `nic_vertex_info_set` for the card's own vertex | Established, IRIX disassembly |
| IRIX walks every part on that bus and formats a record only from a part of family `0x0b` (DS2505) or `0x09` (DS2502). `nic_mfg_next` tests the family byte of each part the search finds and skips any other family, so the station-address part (`0x91`) contributes nothing, and a card with both shows one record, as the capture does | Established, IRIX disassembly; the single record line agrees with the capture |
| For a `0x0b` part, `nic_read_mfg` selects it by Match ROM (`0x55`), issues Read Status (`0xaa`) at `0x0100`, and reads eight groups of eight redirection bytes, each followed by a two-byte check; each group must leave the CRC-16 residue `0xb001`, the first computed over the command and address too, else it prints `nic_read_mfg: invalid crc16 reading redirection map page %d`. It then reads pages 0 and 1 with `nic_read_one_page`: a redirection byte other than `0xff` names page `~byte`; Match ROM, Read Memory (`0xf0`) at `page * 32`, thirty-two bytes, and a CRC-16 over them that must leave `0xb001`. For a `0x09` part it skips the status read and one leading byte instead | Established, IRIX disassembly |
| The formatted string is attached to the vertex as its `_nic` label, which `hinv -mv` prints as the board line, and then passed to `nic_vmc_check`, the vendor-match callbacks | Established, IRIX disassembly |
| `ioc3_init` registers the callback `pci_ioc3_enet_vmc` for the part prefix `030-1155-`, and `pci_ioc3_sio_vmc` for `030-1657-` and `030-1664-`. `pci_ioc3_enet_vmc` calls `ioc3_subdev_enables(vertex, 0x09)`; `pci_ioc3_sio_vmc` passes `0x30` | Established, IRIX disassembly |
| In that mask bit 0 is Ethernet and bit 3 the keyboard and mouse, so the Cad Duo is Ethernet plus PS/2 with no serial ports | Bit 0 established, IRIX disassembly (`ioc3_attach` tests sub-device 0 before connecting the Ethernet interrupt). Bit 3 inferred: OpenBSD gives the same card keyboard/mouse and Ethernet, and the model's boot below keeps the keyboard and mouse and drops the serial ports |
| With that mask the card needs its second interrupt line: `ioc3_intb_needed` answers yes when any enabled sub-device lies outside bits 0 and 2 | Established, IRIX disassembly; agrees with the shoehorn's INTB wiring under "Interrupts" |
| The record's field layout | Established: the same `nic_mfg_next` formatter reads Bridge board records, whose layout the [Bridge datasheet](bridge.md#the-record-layout-and-every-constraint-on-building-it) establishes |
| The IP27 PROM 6.156 carries the same one-wire record reader (its strings include the same `nic_read_mfg` and `nic_read_one_page` messages and `--> Found family 0x0b Mfg. Board data:`) but no `030-1155-` string. Whether a real PROM prints a Cad Duo's record is not captured | PROM disassembly (strings only) for the first part; the second is unknown |

Public drivers, as cross-checks:

- OpenBSD's IOC3 driver (`sys/arch/sgi/pci/ioc.c`, `ioc_attach()`, OpenBSD
  tree at `2df91d80`) reads its record from the IOC3's own bus and names
  `030-1155-` the CADDuo board with keyboard/mouse and Ethernet. It agrees
  with IRIX on the bus, the prefix and the population. Its `owserial`
  (`sys/arch/sgi/dev/owserial.c`) matches families `0x0b` and `0x0f`
  (DS2506) where IRIX accepts `0x0b` and `0x09`; both accept the `0x0b` the
  model uses. It reads the redirection bytes at `0x0100` as IRIX does, and
  checks the same layout fields (`0x01` at byte 0, digits-and-dashes part,
  `0xff` at 42-47, printable name).
- Mainline Linux does not read the record. `drivers/net/ethernet/sgi/ioc3-eth.c`
  (`ioc3eth_nvmem_match()`, v7.2-rc7 `db2ddb8`) picks the address part among
  the IOC3 bus's parts by family `09`, `89` or `91`, which matters only when
  another family shares the bus.

The earlier IOC3 finding that no board-identity part sits on the bus was a
survey of Origin 200 system boards, and its warning that OpenBSD refuses an
unknown record ([IOC3 datasheet](ioc3.md#on-origin-200-system-boards-the-emulated-bus-must-answer-with-exactly-one-part))
applies to part numbers missing from OpenBSD's table. `030-1155-` is in both
OpenBSD's table and IRIX's callback list.

**Inferred, not captured.** The part's family (`0x0b` rather than `0x09`,
which IRIX would read as well), and that the card carries exactly these two
parts. A one-wire search on a real card (OpenBSD prints each part it
attaches; IRIX's `hinv -mvvv` prints the record's `Laser`, the part's ROM
serial) or a photograph naming the card's one-wire parts would settle them.

### Access contract

The same MicroLAN control register and bit-slot protocol as the Ethernet
address part ([IOC3 datasheet](ioc3.md#gpio-and-microlan)). With two parts on
the bus a guest must select one: Search ROM (`0xf0`) finds both, and Match ROM
(`0x55`) with the part's sixty-four-bit ROM identity selects it, as IRIX
does for every record read. The record part answers the DS2505 command set
the Bridge record parts answer: Read Memory (`0xf0`) streams from the
two-byte address with no leading check byte, and Read Status (`0xaa`) reads
unprogrammed status memory in eight-byte groups each closed by its CRC-16,
so IRIX's and OpenBSD's redirection reads at `0x0100` see `0xff` ("not
redirected"). The part is add-only.

### What the model stores

Fitted by the PCI shoehorn only, through the IOC3's `board-*` properties.
The Origin 200 backplane add-in, created with `-device sgi-ioc3`, carries no
record unless the command line sets those properties, and no BaseIO or
system-board IOC3 carries one.

| Bytes | Field | Value | Grade |
|---|---|---|---|
| 0 | format | `0x01` | established, the shared layout |
| 1-10 | barcode | `OCADDUO001` | **placeholder**; the real `HKV898` names one unit |
| 11-29 | part, first part | ten spaces, `030-1155-` | established, capture |
| 30-31 | check value | `6e 0f` | computed as the Bridge layout table states |
| 32-37 | part, dash number | three spaces, `002` | established, capture |
| 38-41 | revision | `D`, three spaces | letter established by capture; left alignment a choice, as for the Bridge records |
| 42-47 | group, capability, variety | `0xff` x6 | the shared layout; OpenBSD requires them |
| 48-61 | name | `PCI_ENET`, six spaces | established, capture; space padding inferred |
| 62-63 | check value | `eb 2b` | computed |

```text
01 4f 43 41 44 44 55 4f 30 30 31 20 20 20 20 20 20 20 20 20 20 30 33 30 2d 31 31 35 35 2d 6e 0f
20 20 20 30 30 32 44 20 20 20 ff ff ff ff ff ff 50 43 49 5f 45 4e 45 54 20 20 20 20 20 20 eb 2b
```

Everything past byte 63 reads `0xff`. The part's ROM identity is family
`0x0b` with the placeholder serial `0x0f0e0d0c0b0a`, continuing the
descending-byte placeholder idiom of the other modelled board parts; no
guest checks it beyond the ROM CRC-8.

### Observed on the model

IRIX 6.5.30 on the `origin2000-pci-shoehorn` topology
(irix.boot.2026-09-25T20:12:20-07:00 (citation: `docs/boot-logs/irix.boot.2026-09-25T20:12:20-07:00.terminal.txt`)):

- `hinv -mv` prints, below the adapter's `XTALK_PCI` record,
  `Location: /hw/module/1/slot/io2/xtalk_pci/pci/1` and
  `PCI_ENET Board: barcode OCADDUO001 part 030-1155-002 rev  D`, the
  capture's form;
- `hinv -mvvv` prints one record for the card,
  `Part:030-1155-002;Name:PCI_ENET;Serial:OCADDUO001;Revision:D;Group:ff;Capability:ffffffff;Variety:ff;Laser:0f0e0d0c0b0a;`,
  and none for the station-address part;
- the card keeps `ef1` (`slot io2, pci 1`), `pckb`, `pcms` and `/hw/input1`;
- IRIX no longer lists the two extra serial ports, tty3 and tty4, that the
  card's IOC3 produced without a record
  (irix.boot.2026-09-24T23:55:07-07:00 (citation: `docs/boot-logs/irix.boot.2026-09-24T23:55:07-07:00.terminal.txt`)).
  Only the BaseIO's tty1 and tty2 remain. That the real card has no serial
  ports rests on IRIX's `0x09` mask for the `030-1155-` prefix above. The
  owner's machine is consistent with it: it lists fourteen ports, which its
  BaseIO and two MENET boards would account for, but hinv gives no port
  locations, so that attribution is inferred;
- the PROM's `hinv -v` lists the card as `adapter IOC3 Rev 1 (pci id 1)`
  with no board line.

This resolves the serial-port discrepancy recorded under "Interrupts" for
the shoehorn fitting: IRIX classifies the card by this record, not by its
subsystem ID.

**Discriminating tests.** `tests/qtest/sn0-pci-shoehorn-test.c` searches the
fitted card's IOC3 bus, expects the `0x91` station-address part and a
`0x0b` part, selects the latter by Match ROM and compares its memory with the
image above, check values and unprogrammed tail included, for the `-device`
board and the catalogue-placed one; the same card behind a plain
`sgi-pci-expansion` shows only the station-address part.
`tests/qtest/sn0-ioc3-test.c` checks that the IOC3 refuses a record the
layout rules forbid.

## DMA

None. The CAD DUO population carries no serial DMA rings (no serial ports)
and no parallel DMA context (no parallel port). Ethernet DMA is the
existing, already-qtested `sgi-ioc3-eth` contract, unchanged by which slot
the chip occupies.

## Out of scope

- The card's real PCI subsystem identity (see "Modeled identity").
- The card's SuperIO/PS-2 interrupt routing to a specific Bridge pin for the
  bare Origin 200 backplane add-in stays unconnected (see "Interrupts"). The
  PCI-XTALK ADAPTER (shoehorn) fitting now wires it as a reviewed
  approximation, also under "Interrupts".
- The real card's barcode and the record part's 48-bit ROM serial, which
  name one physical unit (see "Board-identity record on the IOC3 one-wire
  bus").
- Physical connectors, silkscreen, and bill of materials.
- Any claim that this is the only real Origin-family standalone
  keyboard/mouse-only IOC3 card; the Onyx2 MIO daughterboard is a different,
  already-documented, non-standalone example (`docs/component-taxonomy.md`,
  "MIO").

## Behavioral notes

Two IOC3 instances on one Origin 200 currently receive the same host
keyboard and mouse input: `hw/input/sgi-ioc3-kbc.c`'s generic
`ps2-kbd-device`/`ps2-mouse-device` children each register with QEMU's
global input-event handlers on realization (`hw/input/ps2.c`), and QEMU
broadcasts a host keystroke or pointer event to every registered handler
rather than routing it to one device. This means a running guest can
enumerate and use two distinct PS/2 device paths, but this host's single
keyboard and mouse currently drive both of them at once rather than two
independently-addressable physical peripherals. This is a genuine
capability boundary of the current model and host-input plumbing, not a
defect in the add-in card's own register contract, and is unresolved by
this datasheet.

## qtest checklist

Covered in `tests/qtest/sn0-ioc3-test.c`:

- The add-in card, attached at a real free option slot (device 6, not the
  file's existing synthetic device-3 test slot), answers with the modeled
  subsystem identity and the CAD DUO population — Ethernet, one-wire and
  PS/2 fitted; serial A, serial B and the Timekeeper absent
  (`sn0/ioc3/addin-card-answers-at-a-free-option-slot`).
- With the native system-board IOC3 and the add-in card both present on one
  machine, a round trip on the native keyboard port and the add-in card's
  mouse port each completes correctly and neither chip's other port
  receives a byte belonging to the other chip
  (`sn0/ioc3/addin-card-ps2-ports-are-independent-of-the-native-ones`).

Board-population coverage for the CAD DUO identity in general (as opposed
to this specific add-in-card scenario) already exists and is unchanged:
`sn0/ioc3/board-cad-duo-io7` and `sn0/ioc3/board-cad-duo-other`.

Covered in `tests/qtest/sn0-pci-shoehorn-test.c` (the PCI-XTALK ADAPTER
fitting):

- The card fitted in the shoehorn on Origin 2000 enumerates through the
  adapter's crosstalk widget: its PCI vendor/device and subsystem dwords
  answer through the widget window
  (`sn0/pci-shoehorn/cad-duo-enumerates`).
- Its PS/2 keyboard interrupt reaches node 0's Hub `PI_INT_PEND0` once the
  adapter wires the SuperIO output to the card's own Bridge pin
  (`sn0/pci-shoehorn/cad-duo-ps2-interrupt-reaches-hub`), while the same card
  behind a plain `sgi-pci-expansion` leaves the Hub bit clear
  (`sn0/pci-shoehorn/cad-duo-ps2-interrupt-dead-bare`).

Not covered by qtest, and not claimed: SuperIO/PS-2 interrupt delivery for
the bare Origin 200 backplane add-in (see "Interrupts"), and cross-machine
input-routing behavior (see "Behavioral notes").
