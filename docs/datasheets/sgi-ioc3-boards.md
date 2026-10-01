# Datasheet: IOC3-bearing I/O boards, and the MENET XIO board

References rendered as citation identities name material outside this document
set. Those targets are not included or cleared for redistribution. Review and
runtime statements retain their recorded dates and scope.

**Status:** draft, pending independent review. QEMU commits `ef9570b98a` and
`e0c6ad8039` on `sgi-origami-ioc3-boards` implement it. The board catalogue below is a
reconciliation of existing contracts plus one new board. The MENET board is a
new hardware-visible assembly (a Bridge, its board-identity record and four
IOC3s with their interrupt wiring), so it is a contract change that needs
independent evidence review before it lands. No new register behavior is
proposed: every part on the board is an existing, reviewed model.

**QEMU file(s):** `hw/mips/sgi/baseio.c` (the `menet` preset of `sgi-baseio`),
`hw/mips/sgi/xio-baseio.c` (the XIO carrier that builds the board's Bridge),
`hw/mips/sgi/sn-machine.c` (the `menet` catalogue preset), on QEMU branch
`sgi-origami-ioc3-boards`.

**Origami model:** `docs/machines/layers/modules/menet.yaml` (citation: `docs/machines/layers/modules/menet.yaml`),
fitted at launch into an empty XIO slot (`-device sgi-xio-baseio,preset=menet,slot=io5`,
or `tools/origami run --board menet@io5`).

**Sources used** (none is restricted IRIX source; see "Exposure" at the end):

- SGI 108-0151-001, *IRIS 4-Port Fast Ethernet Adapter with Asynchronous
  Serial XIO Board Installation Instructions* (Library
  `io-chip/108-0151-001-iris-4port-fast-ethernet-adapter.pdf`), printed pages
  1-6 to 1-12 and 2-2 to 2-5.
- Real inventories:
  - an Origin 2000 owner's `hinv -vm` under IRIX 6.5 with PROM 6.156, two
    MENET boards in slots io7 and io8 (Library
    `forum/mimgfx-origin2000-owner-inventory.html`);
  - an Origin 2000 `hinv -v` under IRIX 6.5.4m with PROM 6.27, one MENET in
    slot io6 (comp.sys.sgi.admin, "firmware level on fiber channel cards",
    23 August 1999, `<7psh8j$pos$1@nnrp1.deja.com>`);
  - an Onyx2 deskside `hinv -m` with a MENET, part `030-0873-004`
    (comp.sys.sgi.hardware, Michael Schmidt, 5 June 2004,
    `<40C20497.1D26A44A@machtnix.net>`);
  - an Octane `hinv` with a MENET (comp.sys.sgi.hardware, 16 October 2006,
    `<1160989035.200578.256720@f16g2000cwb.googlegroups.com>`), which shows the
    same XIO board in an out-of-family chassis;
  - an SGI engineer's statement that GIGAchannel XIO slots take MENET boards
    (comp.sys.sgi.admin, Rob Warnock, 18 June 2001,
    `<9gjuq1$hek0u$1@fido.engr.sgi.com>`).
- Disassembly of the IRIX 6.5.30 IP27 kernel (`unix.IP27`, SHA-256
  `cbdb02ea…a0377bdf`) through `tools/origami disasm`: `menet_init`,
  `menet_vmc`, `io6_vmc`, `mio_vmc`, `sn00_vmc`, `ioc3_init`,
  `pci_ioc3_enet_vmc`, `pci_ioc3_sio_vmc`, `ioc3_attach` and
  `ioc3_intb_needed`, and the strings those routines load. The IRIX 6.5.30
  IP35 kernel holds the same part-prefix strings.
- Disassembly of the stock IP27 PROM payload shipped with IRIX 6.5.30
  (`assets/firmware/6.5.30/ip27prom.payload.bin`): the board classifier at
  `0x1fc41538` and its part-prefix table at `0x1fc70fe8`.
- Mainline Linux `arch/mips/pci/pci-xtalk-bridge.c` (board prefix table,
  per-board setups, default interrupt mapping), `drivers/mfd/ioc3.c` and
  `arch/mips/include/asm/sn/klconfig.h` (commit `db2ddb8`); OpenBSD
  `sys/arch/sgi/pci/ioc.c` and `sys/arch/sgi/dev/owserial.c`. Secondary
  cross-checks only.
- Existing reviewed contracts: [IOC3](ioc3.md), [BaseIO](baseio.md),
  [Bridge](bridge.md), [Cad Duo](sgi-cad-duo-pci-card.md),
  [PCI shoehorn](sgi-pci-shoehorn.md).

Grades: **established** means a capture, a manual or the shipped binaries say
it directly; **inferred** means it follows from established facts by a stated
argument; **unknown** means no held source settles it, and the model's choice
is named as a selection or approximation.

## Role

The IOC3 is one chip that SGI fitted to many boards, each wiring a different
subset of its functions ([IOC3 datasheet](ioc3.md), "The chip is capable of
these functions; a board wires up a subset"). This page lists every
IOC3-bearing board relevant to the `sgi-sn` machines, says how IRIX decides
each chip's population, and specifies the one board the model lacked: the
MENET (MENET/MSER), an XIO board carrying a Bridge and four IOC3s that gives
a module four Fast Ethernet ports and six serial ports.

## How IRIX decides an IOC3's population

IRIX 6.5.30 never reads the IOC3's PCI subsystem identity. It selects each
chip's functions with an eight-bit enable word attached to the chip's PCI
slot. The bit meanings are in [the BaseIO datasheet](baseio.md#the-enable-mechanism-read-from-disassembly)
(bit 0 Ethernet, 3 keyboard and mouse, 4 serial A, 5 serial B, 7 real-time
clock and interrupt; 1, 2 and 6 not tested on the attach path, 6 a parallel
candidate). This page adds the complete set of places the IP27 kernel writes
that word, all established by disassembly:

| Where the word comes from | Routine | Match | Slot and word |
|---|---|---|---|
| Bridge board record | `sn00_vmc` | the five Origin 200 system-board prefixes `init_all_devices` registers together, `030-1025-`, `030-1244-`, `030-1291-`, `030-1308-`, `030-1389-` | device 2: `0xff`; SuperIO second slot 3 |
| Bridge board record | `io6_vmc` | `030-0734-`, `030-1023-`, `030-1124-` | device 2: `0xbf` when the record matches `030-0734-` (BaseIO-G), `0xb7` otherwise; SuperIO second slot 4 |
| Bridge board record | `mio_vmc` | `030-0880-` (MediaIO daughtercard) | device 6: `0x7a`; no second slot |
| Bridge board record | `menet_vmc` | `030-0873-` (MENET) | devices 0, 1, 2: `0x31`; device 3: `0x01`; SuperIO second slots 4, 5, 6, 7 for devices 0-3 |
| The IOC3's own board record | `pci_ioc3_enet_vmc` | `030-1155-` (Cad Duo, PCI_ENET) | the card's slot: `0x09` |
| The IOC3's own board record | `pci_ioc3_sio_vmc` | `030-1657-`, `030-1664-` (dual-serial PCI card) | the card's slot: `0x30` |
| Hardware-graph path | `ioc3_attach` | path containing `Ibrick/xtalk/15/pci/4` (Origin 3000 I-brick) | `0x91` |

The kernel loads each prefix string and its routine through its global
table, so which strings belong to which routine is read from where the
strings sit: each routine's strings are contiguous and in the order its
registration code steps through them. The pairs this gives agree with
Linux's prefix table wherever Linux has the prefix. `init_all_devices`
registers a sixth string, `030-1252-`, with a different routine that sets no
IOC3 word; the PROM types that prefix as a PCI board.

`ioc3_attach` reads the IOC3's own one-wire board record, and so reaches the
two PCI-card rows, only when the chip's hardware-graph path contains neither
`baseio` nor `menet`. A BaseIO's and a MENET's IOC3s are therefore described
only by their Bridge's record.

`ioc3_intb_needed` returns true when the word has any bit of `0xfa` set, that
is any function other than Ethernet and the untested bit 2. Only then does
`ioc3_attach` allocate the chip's second (SuperIO) interrupt. Where the
Bridge-record routine declared a second slot (`pcibr_hints_dualslot`), IRIX
takes that interrupt from the second slot's line, whose Bridge pin is the
second slot's number. Otherwise it takes line B of the chip's own slot, except
that a chip with no Ethernet and no dual-serial record takes line A. How the
Bridge routes line B is the Bridge's own rule, pin `slot ^ 4` (Linux
`bridge_probe` default mapping; [shoehorn datasheet](sgi-pci-shoehorn.md)).
The dual-slot pins agree with Linux's per-board overrides for the IP29
(`int_mapping[2][1] = 3`) and the BaseIO (`= 4`), and with its default
`slot ^ 4` for the MENET, which it does not override.

The PROM classifies a board by the same kind of part-prefix table. The
classifier at `0x1fc41538` compares each record's part with the table at
`0x1fc70fe8` and assigns: `030-1250-`, `030-1252-` and `030-1062-` the PCI
board type `0x26`; `030-0734-`, `030-1124-`, `030-1025-`, `030-1244-`,
`030-1291-`, `030-1308-`, `030-1389-` and `030-1264-` BaseIO `0x21`;
`030-0872-`, `030-1243-` and `030-1281-` MSCSI `0x22`; `030-0873-` Ethernet
`0x23`; `030-0927-` Fibre Channel `0x29`; `030-9999-` `0x2a`. A `030-0880-`
record sets a flag on the board instead of a type. Linux's `klconfig.h` names
`0x23` `KLTYPE_ETHERNET` with the alias `KLTYPE_MENET`. (Established by
disassembly and the public header.)

## Board catalogue

| Board | Part prefixes | Bus | IOC3s: PCI device, IRIX word, population | SuperIO interrupt | Board record | Model |
|---|---|---|---|---|---|---|
| Origin 200 IP29 system board | `030-1025-`, `030-1244-`, `030-1291-`, `030-1308-`, `030-1389-` | onboard Bridge | device 2, `0xff`: Ethernet, serial A and B, keyboard and mouse, parallel, one-wire MAC part, Timekeeper | Bridge pin 3 | Bridge line | exists, `sgi-baseio` preset `ip29` |
| Origin 2000 BaseIO (IO6) | `030-1124-`, `030-1023-` | XIO, slot io1 | device 2, `0xb7`: Ethernet, serial A and B, one-wire MAC part, Timekeeper | pin 4 | Bridge line | exists, preset `io6` |
| BaseIO-G | `030-0734-` | XIO, slot io1 | device 2, `0xbf`: as IO6 plus keyboard and mouse | pin 4 | Bridge line | exists, preset `io6g` |
| MediaIO (MIO) daughtercard on BaseIO-G | `030-0880-` | the BaseIO-G's Bridge | device 6, `0x7a`: serial A and B, keyboard and mouse, parallel; no Ethernet, no clock | pin 6 (its own line A) | the same Bridge line, second record | exists, inside preset `io6g` |
| **MENET (MENET/MSER)** | `030-0873-` | XIO, any slot | devices 0-2, `0x31`: Ethernet, serial A and B, one-wire MAC part; device 3, `0x01`: Ethernet and one-wire MAC part only | pins 4, 5, 6 for devices 0-2; none for device 3 | Bridge line | **added: preset `menet`** |
| SGI Cad Duo PCI card (PCI_ENET) | `030-1155-` | PCI card | the card's slot, `0x09`: Ethernet, keyboard and mouse, one-wire MAC part | line B, pin `slot ^ 4` | the card's own IOC3 line | exists, fitted by the PCI shoehorn |
| Dual-serial PCI card (PCI_SIO_UFC) | `030-1657-`, `030-1664-` | PCI card | the card's slot, `0x30`: serial A and B only | line B, pin `slot ^ 4` | the card's own IOC3 line | **added: shoehorn `sio-slot`**; see "The dual-serial PCI card" |
| Origin 300 IO8 | `030-1673-` (Linux) | XBridge head F | device 4: Ethernet, serial A and B, parallel, Timekeeper | XBridge pin 0 | brick EEPROM through the L1 | exists, preset `io8` |
| Origin 3000 I-brick base I/O (IO7) | `030-1557-` (Linux; our I-brick layer) | XBridge | device 4, `0x91`: Ethernet, serial A, clock | not examined | not examined | documented layer only, no QEMU board |
| Octane IP30 system board | `030-0887-`, `030-1467-` (Linux), `030-0891-` (OpenBSD) | Octane Bridge | device 2 | pin 4 (Linux) | Octane | out of family, not built |

Evidence for the rows that already exist is in their own datasheets:
[IOC3](ioc3.md), [BaseIO](baseio.md), [Cad Duo](sgi-cad-duo-pci-card.md),
[XBridge](xbridge.md). The IRIX words in the table are established by
disassembly; which physical connectors they reach is not.

**Where Linux and IRIX disagree.** Linux's `drivers/mfd/ioc3.c` gives the
BaseIO-G (`0xc300`) and IP29 system board no parallel port and the Cad Duo
fall-through no serial ports, and treats every unmatched identity, including
IO7, IO8 and IO9, as a Cad Duo. IRIX's words are the authority for what an
IRIX guest brings up. The model's existing presets follow the reviewed board
datasheets, not Linux's narrower populations.

## MENET: what the board is

### Evidence

| Property | Grade | Evidence |
|---|---|---|
| One XIO board: a Bridge and four IOC3s on the board's PCI bus, 128 KB of SRAM per Ethernet path | established | 108-0151-001 printed pages 1-6 and 1-7, the component figure and block diagram |
| IOC3s 1-3 each drive a serial transceiver with two ports and a 10/100 transceiver; IOC3 4 drives only a 10/100 transceiver | established | 108-0151-001 Figure 1-5, printed page 1-7 |
| Four RJ-45 ports labelled ef0-ef3 and six mini-DIN8 serial ports labelled tty_1-tty_6 | established | 108-0151-001 printed pages 1-9 to 1-11 |
| Each Ethernet port has a socketed NIC holding its MAC address | established | 108-0151-001 Figure 1-4 callout, printed page 1-6 |
| The IOC3s are at PCI devices 0, 1, 2 and 3, each vendor `0x10a9` device `0x0003` | established | two real `hinv -v` listings: `PCI Adapter ID (vendor 4265, device 3) pci slot 0` through `3` under `Origin MENET board`, and `Fast Ethernet: ef1 ... pci 0` through `ef4 ... pci 3` (6.5.4m); the same four slots twice under the owner's two MENETs (6.5, PROM 6.156); `Fast Ethernet: ef1, version 1, pci 0` through `ef4 ... pci 3` on the Octane |
| Devices 0-2 enable Ethernet and both serial ports; device 3 enables Ethernet only | established | IRIX 6.5.30 `menet_vmc`; agrees with the manual, with Linux (`IOC3_SUBSYS_MENET` for slots 0-2, `IOC3_SUBSYS_MENET4` for slot 3) and with OpenBSD (`pa_device != 3` gets serial) |
| A MENET adds six serial ports and four Ethernet interfaces to IRIX's inventory | established | 6.5.4m listing: tty1-tty8 with one BaseIO and one MENET; owner's listing: 14 ttys with one BaseIO and two MENETs |
| No clock, keyboard, mouse or parallel port on the board | established for the IRIX words (bits 3, 6 and 7 clear); the manual lists no such connectors |
| The SuperIO interrupt of device `d` (0-2) reaches Bridge pin `d + 4`; device 3 raises none | inferred | IRIX `menet_vmc` declares slots 4-7 as the second slots of 0-3, and IRIX takes a dual-slot chip's second interrupt from the second slot's line; Linux's default `slot ^ 4` gives the same pins. No capture shows a MENET interrupt register |
| Each IOC3's Ethernet interrupt uses its own line A, Bridge pin `d` | inferred | the Bridge's ordinary per-slot rule, as on every other board |
| Board record on the Bridge's one-wire line: part `030-0873-003`, name `MENET`, board revisions C and J seen; `030-0873-004` rev A also seen | established | owner's `hinv -vm`: `Location: /hw/module/1/slot/io8/menet`, `MENET Board: barcode HLZ655 part 030-0873-003 rev J`, and io7 `... rev C`; Schmidt's Onyx2 `MENET Board: ... part 030-0873-004 rev A`. The records print at the board's slot location, not under a `pci/N` vertex as the Cad Duo's does |
| The PROM types the board MENET | established | PROM classifier: `030-0873-` gives board type `0x23`, `KLTYPE_MENET`; IRIX prints `Origin MENET board, module 1 slot 8: Revision 4` |
| The Bridge revision is 3 or 4 | established for three real boards | `Origin MENET board ... Revision 3` (6.5.4m, slot 6; owner's slot 7) and `Revision 4` (owner's slot 8, the rev J board) |
| The hardware-graph slot name is `menet` | established | owner's `Location:` lines; the manual's example path `/hw/module/1/slot/io9/menet/...` |
| Fits any XIO slot; the manual recommends odd slots before even, low before high | established | 108-0151-001 printed pages 2-4 and 2-5 |
| Interface numbering follows ioconfig; ef0 stays on the master BaseIO | established | 108-0151-001 printed page 2-2; the owner's listing numbers the two boards' ports non-sequentially |

### Conflicts and how they are resolved

- **The manual's `usrpcitest` example names `.../menet/pci/4` for a serial
  test, while every real listing puts the IOC3s at devices 0-3.** The two
  agree. IRIX hangs a dual-slot chip's SuperIO functions on its second slot's
  vertex, so device 0's serial ports appear under `pci/4`. On the model,
  IRIX 6.5.30 lists `tty` under `.../menet/pci/4`, `pci/5` and `pci/6` and
  `ef` under `pci/0` to `pci/3` (see "Guest discovery"). No device answers
  PCI configuration at 4-7.
- **OpenBSD reads the MENET part from each IOC3's own one-wire bus.** Its
  `ioc_attach` looks for a DS2505-family record (`owserial`) on the chip's bus
  and tests it for `030-0873-`. IRIX never reads that bus for a MENET chip
  (the `menet` path test) and the real inventories show no per-chip record.
  Whether the real board carries one is **unknown**. The model puts none there,
  so OpenBSD would print "unknown flavour" for these chips. A real MENET's
  OpenBSD `dmesg` would settle it.
- **Linux takes each chip's subsystem identity from the Bridge record.** It
  substitutes `0xc305` for slots 0-2 and `0xc306` for slot 3 into
  configuration reads. What the chip itself returns at offset `0x2c` is
  **unknown**. The model presents Linux's values in the chip's own
  configuration space, the same choice as the MediaIO chip's `0xc301` (see the
  [IOC3 datasheet](ioc3.md), "We match that result and not that mechanism").

### Unknowns and model selections

| Fact | Status | Model selection |
|---|---|---|
| The Ethernet transceiver part and MII address | unknown | the model's existing DP83840A at MII address 31, as on the BaseIO |
| Whether the board's Bridge has a flash part | unknown; the manual lists none | no flash |
| The IOC3 PCI revision | unknown | 1, as on every other board |
| The record's barcode | per-unit | placeholder `OMENET0001` |
| Which real part and revision to present | selection among observed boards | `030-0873-003` rev `J` with Bridge revision 4, the pairing one real board reports |
| Each IOC3's one-wire MAC part serial | per-unit | the catalogue's synthetic IOC3 serial for device 0, plus the device number for devices 1-3 |
| The MAC addresses | per-unit | QEMU's usual per-NIC default, unique per chip |
| Whether a MENET works on the Origin 200 GIGAchannel | stated by an SGI engineer, not tested | the `sgi-xio-baseio` board can be placed there by `-device`; no composition does so |

## MENET: model contract

The `menet` preset of `sgi-baseio` builds, on the Bridge bus it is given:

| PCI device | Population mask | Subsystem id | Timekeeper | One-wire part serial | SuperIO output wired to |
|---|---|---|---|---|---|
| 0 | Ethernet, serial A, serial B, one-wire identity | `0xc305` | none | base + 0 | Bridge input 4 |
| 1 | the same | `0xc305` | none | base + 1 | Bridge input 5 |
| 2 | the same | `0xc305` | none | base + 2 | Bridge input 6 |
| 3 | Ethernet, one-wire identity | `0xc306` | none | base + 3 | not connected |

Each chip's Ethernet drives its own PCI INTA, which the Bridge's per-slot rule
sends to pin `d`. The chips' serial ports take no host backend in the
catalogue-placed board; `sgi-baseio`'s `chardev-a` and siblings stay with the
BaseIO, which owns the console. A console, record, MediaIO, parallel, NVRAM or
flash id given to a MENET is refused rather than dropped, and the machine's
`baseio-preset` accepts only `io6` and `io6g`, so a MENET cannot replace the
BaseIO in IO1.

`sgi-xio-baseio` with `preset=menet` builds the board's own Bridge at any XIO
widget (the BaseIO presets stay fixed at widget 8), with Bridge revision 4 and
no flash part, and the record below on the Bridge's one-wire line. The
Origami catalogue compiles a board whose `hardware_kind` is `menet` to the
`menet` preset on an XIO-slot carrier.

### Board-identity record

The fixed-offset layout from [the Bridge datasheet](bridge.md#the-record-layout-and-every-constraint-on-building-it),
with the real part, revision and name and the placeholder barcode:

```text
01 4f 4d 45 4e 45 54 30 30 30 31 20 20 20 20 20 20 20 20 20 20 30 33 30 2d 30 38 37 33 2d 46 02
20 20 20 30 30 33 4a 20 20 20 ff ff ff ff ff ff 4d 45 4e 45 54 20 20 20 20 20 20 20 20 20 41 ad
```

Each page leaves the CRC-16 residue `0xb001`; the same routine reproduces the
plain BaseIO image in [the BaseIO datasheet](baseio.md#the-plain-baseios-record)
byte for byte.

## Guest discovery

The `origin2000-menet` topology (MENET in IO5, Crossbow 0 widget 14) booted
the stock IP27 PROM 6.156 and IRIX 6.5.30 from `root-diags.qcow2`
(irix.boot.2026-09-25T21:38:29-07:00 (citation: `docs/boot-logs/irix.boot.2026-09-25T21:38:29-07:00.terminal.txt`)). Every line below has the
form of the real listings cited above.

The PROM passed all diagnostics and its `hinv -v` typed the board and knew
which chips carry SuperIO:

```text
MENET IO Board, Module 1, Slot io5
    ASIC BRIDGE Rev 4, (widget 14)
    adapter IOC3 Rev 1
          (pci id 0)
        controller multi function SuperIO
        controller Ethernet Rev 1
    ... (pci id 1) and (pci id 2) the same ...
    adapter IOC3 Rev 1
          (pci id 3)
        controller Ethernet Rev 1
```

The PROM listing also shows `Vendor ffff Device ffff` adapters for the empty
positions 4-7, the open empty-slot finding the [Bridge datasheet](bridge.md)
records for every Bridge board.

IRIX 6.5.30:

```text
IOC3/IOC4 serial port: tty1 ... tty8
Integral Fast Ethernet: ef0, version 1, module 1, slot io1, pci 2
Fast Ethernet: ef1, version 1, module 1, slot io5, pci 0
Fast Ethernet: ef2, version 1, module 1, slot io5, pci 1
Fast Ethernet: ef3, version 1, module 1, slot io5, pci 2
Fast Ethernet: ef4, version 1, module 1, slot io5, pci 3
Origin MENET board, module 1 slot 5: Revision 4
  PCI Adapter ID (vendor 0x10a9, device 0x0003) PCI slot 0   (and 1, 2, 3)
Location: /hw/module/1/slot/io5/menet
           MENET Board: barcode OMENET0001 part 030-0873-003 rev  J
```

Six MENET serial ports join the BaseIO's two, as on the real 6.5.4m listing.
`ifconfig` brought `ef1` to `ef4` up and `RUNNING`, each with its own MAC
address read from its chip's one-wire part (`08:00:69:12:34:57` to `:59` for
`ef1` to `ef3`), and a write to a MENET tty completed. The hardware graph under
`/hw/module/1/slot/io5/menet/pci` holds `0` to `3` with `ef` and `ioc3`,
`4` to `6` with `tty/1` and `tty/2`, and `7` with neither. No Ethernet or
serial device reached the network or a host backend; the boot checks
attachment and local operation only. The ef numbering came from this disk's
ioconfig history.

## The dual-serial PCI card

SGI's dual-serial PCI card carries one IOC3 wired for serial ports A and B
and nothing else. The [IOC3 datasheet](ioc3.md) cites the assembly
030-1657-003 with an IOC3 revision C, two nine-pin serial connectors and no
Ethernet parts.

| Fact | Grade | Evidence |
|---|---|---|
| IRIX enables serial A and B and nothing else | established | IRIX 6.5.30 `ioc3_init` registers `pci_ioc3_sio_vmc`, word `0x30`, for `030-1657-` and `030-1664-`; OpenBSD's `030-1657-`/`030-1664-` entry enables the same two ports |
| IRIX takes the SuperIO interrupt from line B of the card's slot | established by disassembly | in `ioc3_attach`, a chip with no Ethernet whose own record contains either prefix allocates line B; any other chip without Ethernet takes line A |
| Line B reaches Bridge pin `slot ^ 4` | inferred | the Bridge's general line-B rule, as for the Cad Duo |
| The record sits on the card's own IOC3 one-wire bus | established | IRIX and OpenBSD read the prefix there |
| The IP27 PROM recognises the card by prefix alone | established by disassembly | the routine at `0x1fc3f410` enables the chip's MicroLAN, reads its record and searches the formatted string for either prefix (`0x1fc41e5c` is a substring search). Of its three callers, one skips the IOC3's set-up (`0x1fc27ae0`), one leaves the chip out of the IOC3 components it builds (`0x1fc3f1a8`), and the third, in the console-IOC3 search at `0x1fc374bc`, skips `0x1fc27bdc` and moves on, so the card is never chosen as the PROM's console IOC3 |
| Part 030-1657-003 | established that the assembly exists | a comp.sys.sgi.marketplace stock list names 030-1657-003 "SERIAL I/O CARD (UFC), PCI" (Chris Patterson, Minicomputer Exchange, "SGI In Stock", 26 January 2004, `<a47377f9.0401261309.38ca70ab@posting.google.com>`); the IOC3 datasheet's photographed card reads the same assembly |
| Record name `PCI_SIO_UFC` | inferred candidate | OpenBSD's comment on the entry calls the board "PCI_SIO_UFC dual serial board"; no capture prints the record |
| Record revision | unknown | declared placeholder `A` |
| Subsystem id | unknown | the Cad Duo's placeholder `0xffff` |

**Nothing IRIX-visible depends on the unknowns.** Both IRIX and the PROM
match the part prefix anywhere in the formatted record, so the dash number,
revision and name reach only the text of `hinv -mv`. IRIX names the card's
hardware-graph vertex `pci/N` whatever the record holds: the Cad Duo's
`PCI_ENET` record and this card's both print under `.../xtalk_pci/pci/1`.

**Model.** `sgi-pci-shoehorn` fits the card with `sio-slot=N` instead of the
Cad Duo's `cad-duo-slot` (setting both is refused): an `sgi-ioc3` with
population serial A and B, subsystem id `0xffff`, and a record part on its
one-wire bus holding placeholder barcode `OSIOUFC001`, part `030-1657-003`,
revision `A` and name `PCI_SIO_UFC`:

```text
01 4f 53 49 4f 55 46 43 30 30 31 20 20 20 20 20 20 20 20 20 20 30 33 30 2d 31 36 35 37 2d 02 a0
20 20 20 30 30 33 41 20 20 20 ff ff ff ff ff ff 50 43 49 5f 53 49 4f 5f 55 46 43 20 20 20 70 be
```

The chip's SuperIO output is wired to Bridge pin `slot ^ 4`. With no
Ethernet there is no station-address part, so the record part is the only
one on the bus. No catalogue composition places the card; it is fitted with
`-device`.

**Guest discovery** (irix.boot.2026-09-26T00:37:54-07:00 (citation: `docs/boot-logs/irix.boot.2026-09-26T00:37:54-07:00.terminal.txt`),
a `-device` shoehorn with `sio-slot=1` on Crossbow 0's spare widget 11, slot
io4). The PROM passed all diagnostics and, as its disassembly predicts, lists
no adapter at the card's PCI position. IRIX 6.5.30 lists `IOC3/IOC4 serial
port: tty3` and `tty4` beside the BaseIO's tty1 and tty2 and no Ethernet for
the card, places `tty/1` and `tty/2` under `/hw/module/1/slot/io4/xtalk_pci/pci/1`,
and prints the record at that location as
`PCI_SIO_UFC Board: barcode OSIOUFC001 part 030-1657-003 rev  A`. A write to
`/dev/ttyd3` completed.

**qtests** (`tests/qtest/sn0-pci-shoehorn-test.c`): `sio-enumerates` (IOC3
identity, subsystem `0xffff`, both serial ports, no Ethernet, keyboard,
parallel, clock or station-address part), `sio-board-record` (the record above
is the bus's only part), `sio-serial-interrupt-reaches-hub` (a serial
interrupt raises pin `slot ^ 4`, not `slot`, and reaches the Hub) and
`one-card-only`.

## Not modeled

- **The Origin 3000 I-brick base I/O.** The I-brick has a documented layer but
  no QEMU board.
- **The Octane system board and Octane placement of a MENET.** Out of family.
- **The Challenge-era "MENET" in 108-0163**, a different, earlier product.

## Exposure

The author read no restricted IRIX source and no Library document reasoning
from it, except that, navigating the [Bridge datasheet](bridge.md) by section,
the author read its "board identity one-wire line" section, which reports
facts from vendor firmware and driver source about the Bridge one-wire command
set and the IOC3 driver's board test. This page uses nothing from it beyond
what the IRIX disassembly above shows independently, and the implementation
changes no Bridge one-wire behavior. A reviewer of the Bridge one-wire
responder should not be this author.

## qtest checklist

In `tests/qtest/sn0-menet-test.c`, for a `-device sgi-xio-baseio,preset=menet`
board at spare widget 11 and for the catalogue-placed board at widget 14:

- `placed/widget-alive-at-startup`: widget 14 is in Crossbow 0's startup-alive
  population;
- `ioc3s-enumerate`: IOC3s answer `0x000310a9` at PCI devices 0-3, with
  subsystem dwords `0xc30510a9` for devices 0-2 and `0xc30610a9` for 3;
- `populations`: every chip has Ethernet and its one-wire MAC part; devices
  0-2 have both serial ports and device 3 none; no chip has a clock, keyboard
  controller, parallel port or IOC3-line board record;
- `onewire-serials-distinct` (device board): the four MAC parts carry four
  serials;
- `superio-reaches-hub`: a serial interrupt on each of devices 0-2 raises
  Bridge pin `d + 4`, not `d`, and reaches node 0's Hub. With the outputs
  unwired it fails on device 0.

In `tests/qtest/sn0-bridge-identity-test.c`, `menet-record`: the placed board's
Bridge record reads back byte for byte as above, formats to part
`030-0873-003` and name `MENET`, and the Bridge reports revision 4.

Before the preset existed, `-device sgi-xio-baseio,preset=menet` was refused
as fixed at widget 8.
