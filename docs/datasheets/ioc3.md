# Datasheet: IOC3 base I/O controller

References rendered as citation identities name material outside this document
set. Those targets are not included or cleared for redistribution. Review and
runtime statements retain their recorded dates and scope.

**Status:** reviewed (ds-review with codex, 2026-08-10; ethernet re-reviewed by ds-review-2
with codex, 2026-08-10) — **the serial console path and the ethernet are cleared and may be
implemented.** The verdicts are in
the datasheet review findings (citation: `docs/reviews/datasheet-review-findings.md`).

The Origin 300 IO-8 population supplement added later was independently reviewed
at author commit `c8f333cd3c82b29d4aff47ea356c55913dcfa95c` and reviewer
commit `cbb915749abe01539cfbb5b54f5eccf4dfbec4e3`. Its clearance is limited to
the machine-integration choices and open questions stated in that supplement.

The 2026-09-10 public-source interrupt-output addendum clears the normal `INT_OUT`
levels and supported timed modes for implementation. The 2026-09-11 source-reader
contract also establishes the internal loopback exercised by `olrti -internal`.
Diagnostic mode remains open.

The ethernet clearance rests on a deep pass and on five findings from it now fixed, and
**what was checked against what is worth knowing before anyone decides to re-derive any of
it**: the register map against SGI's register header, mainline Linux and OpenBSD; the
descriptor formats and the ring geometry against **mainline Linux alone**; all seventeen
interrupt sources against mainline Linux and OpenBSD's own header. SGI's own ethernet driver
entered at the re-review and is what settled the five findings — the index direction, the
byte offsets, the alignment rule, the barrier constant and the acknowledge contract — rather
than being the instrument for the descriptor formats. **The
model-breaker among them was that the receive index registers were documented backwards** —
software advances the produce index on this ring, not hardware — and the correction is in the
table and explained beside it, because the rows read as though written by analogy to the
serial rings, where the sense is genuinely the other way.

The earlier round's correctness defect — the access-width section claiming everything outside
the SuperIO window is 32 bits wide, which the byte bus contradicts — was corrected in all three
places that stated otherwise, and the register test names every byte-wide region.
**The review block on the byte-bus clock region is lifted.** Its condition was that the
access-width claim be corrected, and it has been, in all three places and without the
goalposts moving. Nothing this review raised now stands against modelling that region.

**Lifting it does not clear the clock, because a different rule does.** The Timekeeper
behind chip select 0 is a separate part, this datasheet twice refers to a datasheet of its
own for its register detail, and no such file exists yet — so the standing rule that no
device is implemented before its datasheet is reviewed still applies to the clock, and
that gate is not this one. Whoever picks the clock up writes that page first; the dangling
reference is recorded as a finding against this datasheet rather than left to be
discovered.

**Since that verdict was recorded, the clock's page has landed.** The finding above stood
when it was written and is left as written; what has changed is that
[the Timekeeper datasheet](timekeeper.md) now exists, so the reference it calls dangling no
longer is, and the condition it set — that whoever picks the clock up writes that page
first — has been met rather than waived. The clock behind chip select 0 is modelled, and so
is the other part the byte bus carries on a different board;
the DS1685 datasheet (citation: `docs/datasheets/ds1685.md`) covers that one and records its own review.
**Fork file(s):** `hw/misc/sgi-ioc3.c` (multifunction core, serial, GPIO, MicroLAN and
the one-wire Ethernet-address NIC behind it), `hw/net/sgi-ioc3-eth.c` (the ethernet
block: registers, MAC DMA engine, network backend and transceiver) under its own QOM
type `sgi-ioc3-eth`, `hw/input/sgi-ioc3-kbc.c` (PS/2), and
`hw/char/sgi-ioc3-parallel.c` (the parallel port, its ECP register file and its two-context
DMA engine). The register-only ethernet block first landed inline in `hw/misc/sgi-ioc3.c`
under the chip's existing `sgi-ioc3` type, matching how every other functional block in
this chip was built. Once the block gained a real DMA engine — descriptor rings, a
network queue, a transceiver — it was a different shape of thing than the control
registers around it, a sysbus child with its own migrated state rather than a few more
named fields on the multifunction core, so it moved into the file and QOM type planned
for it from the start. The QOM types are `sgi-ioc3`, `sgi-ioc3-eth`, `sgi-ioc3-kbc` and
`sgi-ioc3-parallel`. These names come from the QEMU naming table in
the component taxonomy (citation: `docs/component-taxonomy.md`) and are not open for re-decision
here. Two things about them are load-bearing. The `sgi-` prefix
is mandatory because QEMU already uses bare "hub" for its network hub and bare "bridge"
for PCI bridges, so unprefixed SGI names would collide with upstream concepts; the rule
then applies to every SGI device so that it has no exceptions. And the chip belongs in
`hw/misc/`, not `hw/mips/`: `hw/mips/` holds machine and platform code, while this is a
multifunction PCI device that an Origin 2000, an Octane and a plug-in card all carry.
**Sources used:** SGI's *Origin/Onyx2 Theory of Operations*, document 007-3439-002,
chapter 2, held at
`docs/library/cache/theops/origin-onyx2-theory-of-operations-007-3439-002.pdf` (sha256
`f6f30191…113309`, with a text layer, so `pdftotext -layout` reads it without character
recognition). It is the only *vendor* document here that describes the chip in system
context — its board block diagram names the support parts around the IOC3 and its board
descriptions list the ports each one provides. Then SGI's own register header,
`sys/PCI/ioc3.h` at IRIX 6.5.30, extracted
from the operator's IRIX media to `refs/irix-media/6.5.30/usr/include/sys/PCI/ioc3.h`
(931 lines, sha256 `75dd479f…c96994`) — the only source that carries offsets and bit names
for every block. Behaviour comes from the IRIX 6.5.7 tree at
`refs/irix-source/irix-6.5.7/`: the chip's core driver `irix/kern/io/ioc3.c`,
**the DMA-ring serial driver `irix/kern/io/sio_ioc3.c`, which is the source for the ring
entry format** and the only vendor code that drives the rings rather than the byte-wide
16550, the PS/2 driver `irix/kern/io/ioc3_pckm.c`, the parallel port driver
`irix/kern/io/lio/ecpplp.c`, the PROM console driver `stand/arcs/IP27prom/ioc3uart.c`, the
shared UART definitions `irix/kern/sys/ns16550.h`, and SGI's register diagnostic
`stand/arcs/ide/godzilla/ioc3/ioc3_regs.c` with its table of reset values in
`stand/arcs/ide/godzilla/include/d_ioc3.h`. Cross-checked against mainline Linux
`arch/mips/include/asm/sn/ioc3.h`, `drivers/mfd/ioc3.c`,
`drivers/net/ethernet/sgi/ioc3-eth.c`, `drivers/w1/masters/sgi_w1.c`,
`drivers/w1/slaves/w1_ds250x.c`, `drivers/w1/w1_io.c` and the PS/2 serio driver
`drivers/input/serio/ioc3kbd.c`; and against
OpenBSD 7.9 `sys/arch/sgi/pci/iocreg.h`, `sys/arch/sgi/pci/ioc.c`,
`sys/arch/sgi/dev/if_iecreg.h`, `sys/arch/sgi/dev/owmac.c`,
`sys/arch/sgi/dev/owmem_subr.c`, `sys/arch/sgi/dev/owserial.c`,
`sys/dev/onewire/onewire.c` and `sys/dev/onewire/onewirereg.h` — the last two the
generic one-wire framework `owmac` sits on, read for the exact command bytes and
ROM-selection behaviour "The Ethernet-address NIC's read protocol" section relies
on. Component register detail for the serial ports comes
from the National Semiconductor PC16550D datasheet, cached as
`docs/library/cache/misc/www.scs.stanford.edu_10wi-cs140_pintos_specs_pc16550d.pdf`,
and for the ethernet PHY from the National Semiconductor DP83840A datasheet, cached as
`docs/library/cache/misc/theretroweb.com_chip_documentation_dp83840avce-67a4e1341449e807271366.pdf`.
The OpenBSD files are not on the checked-out branch of `refs/openbsd-sgi`; read them
with `git -C refs/openbsd-sgi show origin/sgi-is-alive-at-7.9:<path>` — **the pinned
branch, not the bare `origin/sgi-is-alive`, which moves.** Our guest is OpenBSD 7.9
`GENERIC-IP27`, so a read off the moving branch is a claim about a different tree, and the
gap widens by itself. Before trusting any *negative* from that tree, run a control that it
is reachable at all — `git -C refs/openbsd-sgi ls-tree -r --name-only
origin/sgi-is-alive-at-7.9 -- sys/arch/sgi | wc -l` should print 246 — because upstream
removed this port, so a search of a checkout without it returns a clean, convincing nothing.
Two further searching hazards
apply to everything here: `git grep` skips `refs/`, which is gitignored and is where the
primary headers live, and IRIX releases genuinely differ, so a value that matters is
cited with its release.

**Everything in the register map and the reset column is IRIX-derived**, because SGI
never published an IOC3 specification and the IRIX header is the only source that
carries both offsets and bit names for every block. We read those sources to learn the
facts and wrote this document in our own words; no SGI text, code, comment or file
structure is reproduced here or in the fork. Where Linux or OpenBSD independently
confirm a fact, the confirmation is noted, because agreement across three unrelated
implementations is the closest thing to a specification we have.

For the 2026-09-10 public-source interrupt-output addendum, the source reader used
mainline Linux revision `db2ddb87143519e20a95aa36c60b36107b736a58`
(Linux 7.2-rc7), the pinned OpenBSD 7.9 SGI revision
`2df91d80c104636deeef8124252ddd5951b55fbf`, and two publicly hosted SGI manuals.
SGI's *Online Diagnostics*, document 108-0286-005, is visibly marked "SGI
Confidential" despite being publicly hosted and indexed. Only its user-facing
description of `olrti` was read. The source reader did not read or search
`refs/irix-source/`, `assets/`, source-reader notebooks, vendor-derived review
transcripts, or a broad Library result set. QEMU's IOC3 source was inspected only
to identify the existing modeled gap. No device code was written or reviewed.

For the 2026-09-12 PROM netboot driver subsection, a designated reader disassembled
the IP35 PROM payload's embedded base-I/O PROM netboot driver and the Origin 200
io6prom payload and distilled the facts below in the reader's own words, cited inline
as *the base-I/O PROM netboot driver (authorized tainted read, 2026-09-12)*. No IRIX
source tree and no QEMU device source were opened. The material read was
firmware-derived — the two payloads named above, plus the reader's own prior IP35 and
IP27 PROM disassembly artifacts. The full reading, with its locators, is
the PROM netboot driver notebook (citation: `docs/notebooks/ioc3-prom-netboot-driver.md`). The
reader is excluded from implementing or independently reviewing IOC3, ethernet or
netboot device code.

## Role

IOC3 is the chip that makes an Origin bootable and reachable. It is a 32-bit PCI
device that hangs off a Bridge, and the silicon is *capable of* two serial ports, a
10/100 ethernet controller, a PS/2 keyboard and mouse controller, a parallel port, a
general-purpose I/O block, a one-wire bus, and a byte-wide expansion bus that the
real-time clock sits on. The board determines which one-wire parts and byte-bus devices
are attached.

For origami the serial console is the only part on the critical path. The firmware
prints its first characters through an IOC3 UART, and every guest we care about takes
its console from the same place, so the serial blocks are modelled first and the rest
follows.

### What the chip is, physically

A photographed IOC3 is marked `L2A0366 / IOC3 Rev. C / SGI 1996 / 099-0126-003`: **SGI
part `099-0126-003`, silicon revision C, fabricated by LSI Logic as part `L2A0366`.** The
chip reports a PCI revision of 1 on the Origin 200 in OpenBSD's attach line, which is a
different number from the silicon revision letter and does not contradict it.

SGI's board block diagram puts these parts around the chip on a BaseIO board: a 64Kx18
static RAM for the ethernet buffer; a time-of-day clock with 32 KB of non-volatile RAM; a
block drawn as *Super IO* feeding the two serial outputs; **two** one-wire NICs;
1 MB of flash carrying a serial number; the media-independent interface out to an external
physical-layer chip and on to the ethernet connector; an oscillator; and a media expansion
connector. Two SCSI controllers with their own static RAM sit beside it on the same PCI
bus, and the real-time interrupt input and output reach the outside world through an
opto-isolator. That list is what an implementer should expect the chip to be talking to.

### The chip is capable of these functions; a board wires up a subset

**Do not infer a machine's ports from the presence of an IOC3.** SGI configured the same
chip differently on different boards, and a board may leave any function unconnected. The
worked example is SGI's own dual-serial PCI card, assembly `030-1657-003`, built around an
IOC3 revision C (LSI Logic part L2A0366, SGI part `099-0126-003`): it carries two nine-pin
serial connectors and no ethernet connector, magnetics or physical-layer chip at all. Both
guests encode that as data rather than assuming. OpenBSD 7.9 reads the board part number
off the one-wire bus and looks it up in a table of boards, and its entry for the
`030-1657-` card enables serial ports A and B and nothing else. Mainline Linux reads the
PCI subsystem identity and dispatches to a per-board setup function. So the register map
below describes the whole chip, and **which ports exist on a given machine comes from that
machine's own inventory**, never from this document.

A machine may also hold more than one IOC3. The dual-serial card is an add-in card and
both guests attach their driver per instance. Standard Origin 2000 BaseIO contains one
IOC3. A documented BaseIO-G plus MIO option adds a second IOC3 on the MIO daughterboard,
where it provides option-specific serial, parallel, keyboard and mouse functions alongside
the RAD audio device. SGI's four-port MENET ethernet card carries four, of which the fourth
has no SuperIO block. None of those populations transfers to Origin 200 merely because the
ASIC is shared.

The BaseIO population is visible in *SGI Origin 2000 and Onyx2 Theory of
Operations Manual* 007-3439-002, revision 002, SHA-256
`f6f30191b662aeef6c9888f109c4997d58ac9c4a77acd24263937c6392113309`,
Figure 2-25 on printed page 69/PDF page 83. The MIO description is on printed
pages 71-72/PDF pages 85-86. The printed-to-PDF offset of 14 is trusted, and the
original Figure 2-25 page was visually checked.

### Board variants

PCI subsystem identity selects a board-specific population. These entries
describe how software interprets the identity; the machine inventory decides
which board and external devices are installed. For example, the
Origin 200 machine model (citation: `docs/machines/origin200.md`) records its two serial
ports, Ethernet, PS/2 controller, parallel port, and Timekeeper separately.

The board identities Linux knows, which are the clearest statement anywhere of how much
board-to-board variation this chip sees:

| Subsystem identity | Board | What the board wires up |
|---|---|---|
| `0xc300` | IP27 BaseIO6G | ethernet with its one-wire master, both serial ports, Timekeeper clock, PS/2 |
| `0xc301` | IP27 MIO | both serial ports, PS/2 — no ethernet, so no one-wire either, and no clock |
| `0xc302` | IP27 BaseIO | ethernet with its one-wire master, both serial ports, Timekeeper clock — no PS/2 |
| `0xc303` | IP29 System Board — **the Origin 200** | ethernet with its one-wire master, both serial ports, Timekeeper clock, PS/2 |
| `0xc304` | IP30 System Board — the Octane | ethernet with its one-wire master, both serial ports, PS/2, DS1685/DS1687 clock at byte-bus selects 1 and 2, front-panel LEDs |
| `0xc305` | MENET, a four-port ethernet card | ethernet with its one-wire master and both serial ports — except the fourth of its four IOC3s, which has no SuperIO block and so is ethernet only |
| `0xc306` | MENET4 | ethernet with its one-wire master, nothing else |
| every other identity, including `0xc307`–`0xc30A` (IO7, IO8, IO9, IP34 System Board) | CAD DUO | ethernet with its one-wire master, PS/2 — no serial ports, no clock |

IRIX does not use these identities. [The IOC3 boards datasheet](sgi-ioc3-boards.md)
gives IRIX 6.5.30's own per-board function table and the full list of IOC3
boards in the SN family, including the MENET's.

**The last row is a fall-through, not a board with an identity of its own.** The driver
walks its table and runs the CAD DUO setup for anything it does not match, so an identity it
has never heard of still gets a population. The later SGI boards land there: the Bridge host
controller driver hands out `0xc307` through `0xc30A` for IO7, IO8, IO9 and the IP34 System
Board (`arch/mips/pci/pci-xtalk-bridge.c`, mainline Linux v7.2-rc7, commit `db2ddb8`), and
the multifunction driver has no entry for any of them, so a current Linux drives ethernet,
one-wire and PS/2 on those boards and nothing else. Reading them as unsupported would be
wrong in the opposite direction from reading them as their own row.

**The one-wire master is not something a board fits on its own.** It is registered inside
the ethernet setup routine, in the same call, so it is present exactly where ethernet is
and absent everywhere ethernet is — which is why the table pairs them in every row rather
than giving one-wire a column that would only ever repeat the ethernet one. A board with no
ethernet has no station-address part on its bus, and the emulated bus on such a board must
read as an idle wire.

**SGI's own manual confirms three rows of that table independently**, which matters because
the rest of it would otherwise rest on one Linux driver. The manual's BaseIO board provides
one 10/100 ethernet link, *two* serial ports, two SCSI ports, a real-time interrupt output
and input, flash, NVRAM and a time-of-day clock — and no parallel, keyboard or mouse port,
exactly as the `0xc302` row says. The MediaIO daughtercard, which mounts on a
specially-configured BaseIO called the BaseIO-G, adds a parallel port, *two more* serial
ports, audio, a keyboard port and a mouse port — which is the `0xc301` row, a second IOC3
carrying serial and PS/2 and no ethernet or clock of its own. The four-serial-port Origin
2000 is therefore two boards and two chips, not one chip with four ports.

**The "what the board wires up" column is the set of sub-devices mainline Linux's
multifunction driver instantiates for that identity, and that is narrower than the hardware
present.** Its per-board setup routines register an ethernet, a one-wire master, the two
16550s, a Timekeeper or DS1685 clock, a PS/2 controller and — on the Octane — front-panel
LEDs, dispatched by subsystem identity (`drivers/mfd/ioc3.c`, mainline Linux v7.2-rc7,
commit `db2ddb8`). **None of those routines ever registers a parallel port**: the driver
carries no parallel-port sub-driver at all, so the parallel port is absent from every row
regardless of which boards physically carry one. Read the column as "what a current Linux
drives", not "what the board has". The gap already shows inside this table — SGI's manual
has the MIO daughtercard adding a parallel port, yet the `0xc301` MIO row omits it,
because Linux drives the MIO's serial and PS/2 and leaves its parallel port alone.

**The Origin 200 has a parallel port** (James, Origin 200 machine contract), even though the
column above cannot show it. The parallel-port register block is part of the IOC3 register
map every board carries — two DMA-context buffer-pointer
and control registers at `0x80`–`0x98`, and a byte-wide register file in the SuperIO window —
present independent of subsystem identity, as mainline Linux's register header names them
(`arch/mips/include/asm/sn/ioc3.h`, v7.2-rc7). Whether a guest drives that hardware is a
separate question, and the two families answer it differently: Linux instantiates no parallel
sub-device and OpenBSD does not touch the port, while IRIX ships a driver for this machine
family and attaches to it. The Origin 200 machine model (citation: `docs/machines/origin200.md`)
records the parallel port in the board's own inventory, and the model implements the block.
Its register-level contract is specified in the parallel-port section below.

### Origin 300 IO-8 population supplement

**Status: independently reviewed for machine integration.** IP45 uses IOC3 function
`00:04.0`, identity `0x10a9:0x0003`, PCI revision 1, on the 33 MHz head-F bus.
The exact subsystem vendor and device fields were not resolved by this bounded
review. The initial emulator retains the IOC3 class's public SGI subsystem
vendor `0x10a9` and exposes `0xffff` as an explicit subsystem-device
compatibility placeholder. This prevents the IP45 instance from inheriting
QEMU's Origin 200 system-board value `0xc303`. The placeholder is not an IP45
hardware fact and must be replaced by a configuration-space capture. The
Origin 300 identity qtest must read `0x10a9:0xffff` and reject
`0x10a9:0xc303`. The public machine captures and SGI's diagnostic device table
independently place the function at device 4.

The guest-visible IOC3 population is Ethernet, serial A, serial B, the parallel
function and a byte-bus Timekeeper at device select 0. It has no keyboard or
mouse functions. OpenBSD selects its parallel-function driver and the capture
attempts that attachment, but the user's guide lists every IO-8 connector and
shows no rear-panel parallel connector. The population contract therefore does
not establish a physical parallel port.

The IOC3 contains its MicroLAN engine, but IP45 does not fit the IOC3-attached
Ethernet-address identity part. OpenBSD `2df91d80c1`,
`sys/arch/sgi/pci/ioc.c` lines 530 through 546, obtains the address from the
brick EEPROM through the L1 instead. The machine contract selects explicit
population mask `0x17`: Ethernet, serial A, serial B and the parallel function,
with keyboard/mouse bit `0x08` and one-wire-identity bit `0x20` clear. It selects
M48T35-compatible `timekeeper-kind` 1 separately, subject to the focused qtest
required later in this supplement. The presence of the MicroLAN engine does not
justify synthesizing a device on its wire.

**Status of this paragraph: independently reviewed 2026-09-14 for the PHY's
address, identity and loopback selector; the DP83843 reset-value approximation
remains open.** The
Ethernet block's transceiver answers at MII address 1 on this board, not the
Origin 200's 31: the IP35 BASEIO PROM 6.210's MII read and write helper functions
hard-code that address, and never vary it. The public capture cited below for this
supplement's other IOC3 facts also names the part at that address —
`nsphyter0 at iec0 phy 1: DP83843 10/100 PHY, rev. 0`, line 173 — a DP83843, not the
Origin 200's DP83840A. [The DP83840 datasheet's own Origin 300
section](dp83840.md#origin-300-io-8-board-address-1-a-different-part) covers the
address, the identity fix (a `dp83843` property that also clears the firmware's
`enet_phy_reg` identity warnings), the Twister/PHY loopback sub-test fix (the
DP83843's register `0x17` selects loopback differently from the DP83840A's, which
the firmware's `enet_tw_loop` sub-test depends on), and the machine wiring
(`hw/mips/sn1.c`'s `sn1_configure_ioc3`, `phy-addr` and `phy-dp83843` alias chains,
and `sn1-model.h`'s per-board descriptor) in full; this paragraph exists so the
fact is not missing from the supplement that already covers this board's other
IOC3 population choices.

The board exposes two distinct interrupt sources. Ethernet PCI INTA at
`00:04.0` reaches head-F physical interrupt pin 4. The SuperIO interrupt output,
which covers the serial and parallel side, reaches head-F pin 0. XBridge owns
the pin status and sends the set and optional clear crosstalk writes to the
guest-programmed target and pending-bit address. The board fixes neither a
Bedrock pending-bit number nor a destination CPU.

Serial A at BAR-relative offset `0x20178` is the guest console, and serial B at
`0x20170` is the second rear-panel port. In the one-chassis emulator interface,
character backend index 0 remains assigned to the separate L1 management
channel. IOC3 serial A and B use indexes 1 and 2. This assignment is an emulator
integration contract. It does not describe a hardware numbering register.

OpenBSD labels the byte-bus-select-0 interface on an Origin 300 as DS1742W. That
observation establishes a directly addressed Timekeeper-compatible clock and
non-volatile store at select 0, but it does not identify the exact IP45 package,
capacity or complete decode. The current QEMU IOC3 can attach its existing
M48T35-compatible 32 KB Timekeeper and an `nvram-drive`. Doing so is an explicit
compatibility choice until board-specific evidence settles those physical
fields. At QEMU commit `ebeef315652bae8abfee0038ee18680950f90ef0`,
`sn0/ioc3/clock-storage-holds-a-write` and
`sn0/ioc3/clock-storage-persists-with-a-drive` cover the emulator class's exact
32 KB storage and backing-file persistence.
`sn0/ioc3/clock-storage-survives-a-system-reset` covers reset, and
`sn0/ioc3/population-is-instance-data` covers explicit M48T35-compatible kind
selection. Those Origin 200 tests do not discriminate the IP45 population or
select-0 mapping.

Clearance of the Origin 300 compatibility choice therefore requires a focused
`origin300/ioc3/timekeeper-compatibility` qtest. It must realize IOC3 with
`population=0x17,timekeeper-kind=1`, prove that select 0 is mapped at BAR offset
`0x80000`, exercise the public consumer's M48T35 clock-register placement at the
top of the selected storage, prove persistence through `nvram-drive`, and prove
that NVRAM and clock state survive identical-build migration. The remaining
32 KB physical decode and exact package stay outside the hardware claim. Do not
present the Origin 200's 32 KB identification as an IP45 measurement.

Sources for this supplement are *SGI Origin 300 User's Guide* `007-4365-002`,
revision 002, Library root `techpubs`, printed pages 9, 12 and 14. *sMDK-based
Field Diagnostics for SGI Origin 300 System and SGI 3000 Family*
`108-0264-004`, Library root `origami-docs`, printed pages 2-9 through 2-14. The
public captures at
`library/cache/boot-logs/openbsd-sgi-list/marc-125897882029839-origin-300-installation-issues.html`
lines 55 through 90 and
`library/cache/boot-logs/openbsd-sgi-list/marc-139223952727384-origin-300-numalink-1.html`
lines 150 through 183. OpenBSD `2df91d80c1`
`sys/arch/sgi/pci/ioc.c` lines 307 through 359, 391 through 427 and 464 through
546, and `sys/arch/sgi/dev/dsrtc.c` lines 118 through 178. QEMU
`ebeef315652bae8abfee0038ee18680950f90ef0`,
`tests/qtest/sn0-ioc3-test.c` lines 3745 through 3915 and 4678 through 4853,
defines the existing emulator-class coverage and its machine boundary. No
restricted vendor source was used.

Note what changes between the Origin 200 and the Octane: not just which ports exist, but
which real-time clock part answers and where. That is covered under the byte bus below and
it is the single easiest thing to get wrong on this chip.

### External access widths

Machine placement and PCI device numbering belong to the containing board. The
chip's ordinary register blocks are 32 bits wide. Two regions are byte-wide: the
SuperIO window at offset `0x20000` and the four byte-bus device selects from
`0x80000`. Both need their access width declared independently of the devices a
board connects to them.

## Address windows

One PCI memory base address register covers the whole chip. Every offset in this
datasheet is measured from that base.

| Range | Contents |
|---|---|
| `0x00000`–`0x001FF` | Main register block: interrupts, control, GPIO, MicroLAN, parallel port, PS/2, serial DMA, ethernet |
| `0x04000` | Alias page exposing the interrupt-output register alone |
| `0x08000` | Alias page exposing the serial port A DMA registers alone |
| `0x0C000` | Alias page exposing the serial port B DMA registers alone |
| `0x20000`–`0x201FF` | SuperIO window: the two 16550 UARTs, parallel port, real-time clock, configuration bytes |
| `0x40000`–`0x7FFFF` | Ethernet buffer RAM, exposed for diagnostics — 256 KB of address space holding 128 KB of 16-bit RAM, all of it populated on the Origin 200 |
| `0x80000` | Byte bus device select 0 — **the Origin 200's real-time clock** |
| `0xA0000` | Byte bus device select 1 — the Octane's clock address port |
| `0xC0000` | Byte bus device select 2 — the Octane's clock data port |
| `0xE0000` | Byte bus device select 3 |

The alias pages exist so a privileged process can map one port's DMA registers into
user space without exposing the rest of the chip. Each alias page is 16 KB. Within an
alias page the seven serial registers appear in the same order and at the same four-byte
stride they have in the main block, so port A's alias runs from `0x8000` to `0x8018` and
port B's from `0xC000` to `0xC018`. **Three registers are read-only through the alias, and
they are not three indexes**: they are the transmit consume index, the receive produce
index — the two that hardware advances — and the 16550 shadow register, which is not an
index at all. The emulator can implement the alias pages as additional views of the same
state.

## Register map — serial console first

### PCI configuration space

| Offset | Name | Width | Reset | R/W | Behavior |
|---|---|---|---|---|---|
| `0x00` | Identity | 32 | `0x000310A9` | RO | Device `0x0003`, vendor `0x10A9`. OpenBSD matches on exactly this pair to bind its driver. |
| `0x04` | Status and command | 32 | `0x02800000` | mixed | Command in the low half, Status in the high half; reset Command `0`, reset Status `0x0280`. Guests set bus-master and memory-space enables in the Command half before touching anything else; parity-response, SERR enable and a drop-on-parity-error mode live there too, and the usual PCI error latches occupy the Status half's top byte. |
| `0x08` | Revision and class | 32 | variant-dependent | RO | Class `0xFF`, subclass `0x00`, and the chip's PCI revision byte. SGI's diagnostic records `0xFF000000` for a revision-0 part; OpenBSD records revision 1 on another IOC3. |
| `0x0C` | Latency timer | 32 | `0` | RW | The PROM writes `0xFF00` here during console setup. |
| `0x10` | Base address | 32 | `0` | RW | Single memory window, 1 MB, covering every window this datasheet lists. |
| `0x2C` | Subsystem identity | 32 | board-dependent | RO | Subsystem vendor `0x10A9` in the low half, board identity in the high half — `0xC303` for the Origin 200's IP29 System Board. The identity selects which sub-devices a driver instantiates, per the board-variants table. Mainline Linux gets that identity from its Bridge host controller driver rather than from this register, and the wrinkle described with this configuration space covers why the two must agree. |
| `0x40` | Error address, low | 32 | `0` | RO | Bit 0 marks the record valid, bits 1–4 name which internal master faulted, bit 5 flags that more than one error occurred, bits 6–31 carry the address. |
| `0x44` | Error address, high | 32 | `0` | RO | Upper address bits of the same record. |

**The Command and Status register at `0x04` resets with a non-zero Status half.** A 32-bit
read at reset returns `0x02800000`: Command `0` and Status `0x0280`. The machine requires
this. The single-CPU IRIX miniroot's `io_config_space` diagnostic (`diag_io6confSpace_sanity`)
reads this register at
reset for each PCI device and compares the full 32 bits, and for the IOC3 it expects
`0x02800000`. A model that leaves the Status half zero miscompares, the diagnostic fails the
I/O slot, and the miniroot takes a general exception and drops to its monitor before the
installer runs. That is observed on the console of a real boot, captured in
`docs/boot-logs/irix.boot.2026-08-23T09:15-07:00` and analysed in
the config-space reset-value finding (citation: `docs/plans/archive/pci-confspace-reset-values-handoff.md`).

**What the Status bits mean, and how well each part is known.** The PCI Local Bus
Specification — revision 2.2 of December 1998, held as
`docs/library/cache/io-chip/pci-local-bus-spec-2.2-1998.pdf` — defines the Status half. The
value `0x0280` sets two bits: bit 9, one of the DEVSEL decode-timing pair (bits 10:9), whose
`01` value reports medium timing; and bit 7, Fast Back-to-Back Capable. So the IOC3 reports
medium DEVSEL and fast-back-to-back capability at reset, and no other capability. That reading
is a derivation, not two separate observations: the reset value is what the machine's
diagnostic requires (observed on the console), and the bit meanings are the specification's.
SGI never published an IOC3 register specification, so there is no independent vendor
reset-value table for this register to check the value against — the diagnostic contract is
the authority for it.

For the write mask an implementer needs, the Status half splits into read-only capability bits
and write-one-to-clear error latches, and only the second group is writable:

- **Read-only reset state, writes ignored:** DEVSEL timing (bits 10:9, `01` here), Fast
  Back-to-Back Capable (bit 7, set here), 66 MHz Capable (bit 5, clear), Capabilities List
  (bit 4, clear).
- **Write-one-to-clear error latches, reset `0`:** Master Data Parity Error (bit 8), Signaled
  Target Abort (bit 11), Received Target Abort (bit 12), Received Master Abort (bit 13),
  Signaled System Error (bit 14) and Detected Parity Error (bit 15). A model with no PCI error
  injection can implement these as reset-zero latches that never set.
- **The Command half** is the ordinary PCI Command register, writable per its own mask. The
  diagnostic reads offset `0x04` before the guest sets any enable, which is why it expects
  Command `0` at that point.

There is a wrinkle in the subsystem identity worth recording rather than omitting: for a
Bridge-attached IOC3, mainline Linux's Bridge host controller driver does not pass
configuration reads through to the chip at all. It answers them itself — a *synthetic*
subsystem identity at offset `0x2C`, chosen per slot from a table selected by the board part
number it reads off the Bridge's own one-wire line, a manufactured interrupt pin at `0x3C`,
the chip's real registers for `0x00`–`0x10` and `0x40`–`0x44`, and zero for everything else
(`arch/mips/pci/pci-xtalk-bridge.c`, mainline Linux v7.2-rc7, commit `db2ddb8`). So on this
machine the value Linux acts on comes from that table and never from this register. Both
paths have to produce `0xC303` for Linux to run the base I/O setup; OpenBSD never reads the
register at all.

**We match that result and not that mechanism, deliberately.** origami's Bridge model does
not intercept configuration space: its window is a plain pass-through to the device
(`sgi_bridge_config_read` calls `pci_data_read`), so every guest that reads offset `0x2C`
gets whatever the chip itself presents there. On the Origin 200 the two produce the same
`0xC303`, which is why nothing differs today. The mechanisms are not the same though, and
they diverge as soon as more than one board is in play: on real hardware a MENET card's four
identities come from the Bridge, per slot, while here each chip carries its own. The
device-property route is the natural QEMU shape — identity belongs to the device that has
it — and nothing on our path needs the substitution, so do not read the pass-through as
fidelity to the hardware's arrangement.

The model takes the identity from a `subsystem-id` device property and builds the
sub-device population the board-variants table gives for it, defaulting to the Origin 200's
`0xC303`; an unmatched value gets the CAD DUO fall-through exactly as the driver's dispatch
does. The board's clock comes with the population: the Origin 200's Timekeeper at byte-bus
select 0, the Octane's DS1685/DS1687 across selects 1 and 2, each realized on the chip's own
bus so that a reset of the chip reaches it, and whichever part the board does not fit left
unparented rather than realized and unmapped. One of the Octane's sub-devices is still an
explicit gap rather than substituted for: its front-panel LEDs on two of the general-purpose
pins, which no other board carries. A guest on that board finds them absent rather than
answered by the wrong part. Everything else the Octane's entry names is populated.

The master identifiers reported in the error record are: 0 and 1 for serial port A
transmit and receive, 2 and 3 for serial port B transmit and receive, 4 for the
parallel port, 8 through 10 for the ethernet transmit descriptor and its two buffer
reads, 11 for the ethernet receive descriptor read, and 12 for the ethernet receive
buffer write. Modelling the field is cheap and makes DMA faults debuggable.

### SuperIO window and the two 16550 register files

**Every register in this window is one byte, and every access the firmware and the guest's
serial driver make to it is one byte.** Each consecutive address is a different register, so
an access spanning four addresses is addressing four registers, not one wide one. Getting
that wrong is the most likely way to produce a console that emits nothing. The other
byte-wide region on this chip is the byte bus, described further down; everything else is
32 bits.

**Register N sits at byte offset N, and the byte-lane arithmetic both guests perform is not
this chip's business.** Mainline Linux's IOC3 serial driver reads every register as
`readb(base + (offset ^ 3))`, and OpenBSD's `com` driver reads plain offsets — which looks
like a disagreement about the register layout and is not one. OpenBSD does the identical
exclusive-or one layer down, in the Bridge's own byte accessors, which compute `(handle +
offset) ^ 3`. Both guests expect the same physical layout and differ only in which layer
compensates. So a model validated against one is not at risk from the other.

What the exclusive-or compensates for is a big-endian processor reaching a little-endian PCI
device with no swapping in between — and OpenBSD confirms there is none, disabling the
Bridge's programmed-I/O byte swap outright with the comment that the IOC3 serial console
depends on it being off. **In our model that mapping belongs to the memory region's declared
endianness, not to hand-written offset arithmetic**; a device that computes `^ 3` itself and
a region declared with the wrong endianness will cancel out on one guest and not the other.
Declare the endianness and let the memory system place the lanes, exactly as the DMA section
below leaves byte order to the Bridge.

**What a wider-than-byte access to one of these registers does is not settled by any source
we hold, so the model picks and the datasheet records the pick.** Nothing in SGI's header,
its diagnostic table or either guest states whether the chip rejects such an access, narrows
it to a byte, or does something else — and no correct driver issues one, so no observation
settles it either. **Declare it invalid above one byte and log the attempt**, matching this
project's preference for loud over silent: the log can only ever fire on a defect, since no
guest on any path we care about issues a wide access to a byte-wide register file. This is a
choice, not a finding. If a guest is ever seen doing it, that log line is the evidence, and
the choice should be revisited rather than defended.

**That choice is a `valid` declaration, and it makes the `impl` maximum redundant here — the
two are alternatives, not a pair.** Declaring the region valid only at one byte means nothing
wider is ever dispatched to the callback, so there is nothing left for an implementation
maximum to decompose. The other design, accepting wide accesses and setting the
implementation maximum to one so the memory system splits them into per-byte calls, is
coherent too and never errors — but it is a *different* choice, and taking it would mean
dropping the rejection above. Declare the implementation maximum as one anyway, for
self-documentation and so the region stays correct if the valid width is ever widened, but do
not mistake it for the thing doing the work.

**Neither behaviour is reachable from a register test, and that is a property of the harness
rather than of this chip.** A test that issues a wide access does so through the address
space, which clamps the length to the region's *valid* maximum and to the address's own
alignment and then loops, so a four-byte write to a one-byte-valid region arrives as four
one-byte writes at consecutive addresses and succeeds silently. A guest's wide load takes the
processor path instead, arriving as a single dispatch that the validity check refuses. So the
rejection can only be observed from guest-side code — a boot test or a test ROM — and a
register test asserting anything about wide accesses to this window is asserting something
about the harness.

That is the hardware fact. Turning it into a model correctly is an access-width question,
and the working rules carry the mechanism under the convention that `impl` is not `valid` —
follow it rather than re-deriving it here. What is specific to this chip is that the failure
is **silent**: nothing on the boot path ever issues an access wider than a byte, so a model
that mishandles wide accesses still produces a working console, and only a register test
issuing a width no guest issues can see it. That applies to the byte bus as much as to this
window.

| Offset from `0x20000` | Contents |
|---|---|
| `0x141` | UART configuration byte |
| `0x142` | Keyboard configuration byte |
| `0x150` | Parallel port register file |
| `0x168` | A real-time clock register file — **not the Origin 200's clock**; see below |
| `0x170` | Serial port B, eight 16550 registers |
| `0x178` | Serial port A, eight 16550 registers |

**There is a third real-time clock location and it is a decoy.** SGI's header places a
clock register file inside the SuperIO window at `0x20168`, distinct from both byte-bus
clocks. Neither guest uses it on any machine we model: OpenBSD and Linux both take the
Origin 200's clock from byte-bus chip select 0 and the Octane's from chip selects 1 and 2.
An implementer who finds the SuperIO offset first and builds a clock there will produce a
device no driver ever reads.

**The SuperIO block is an external part on the byte bus, and the chip fronts it.** SGI's
board block diagram settles this when it is *rendered* rather than text-extracted, because
the information is in the lines between the boxes and a text layer keeps only the labels.
At high zoom the timekeeper's two nets, drawn and labelled `A` and `D`, each leave the
timekeeper, run across into the *Super IO* box and drop down into the IOC3 — one address
bus and one data bus, shared three ways. Super I/O has one further pin into the IOC3 beyond
that shared pair, and its two remaining connections leave the board as the SERA and SERB
outputs. The bus the timekeeper sits on is the byte bus, so the SuperIO block sits on the
byte bus too.

This does not contradict the register header putting a SuperIO window inside the chip's
map, and the apparent conflict was never real: the IOC3 decodes that window and turns each
access into a byte-bus cycle to the external part. The header describes the programming
interface a driver sees; the diagram describes the silicon behind it.

**Nothing about the model changes.** A guest sees byte-wide 16550s at `0x20178` and
`0x20170` either way, and that is how OpenBSD drives even the dual-serial card's ports. What
it buys is that three oddities in the chip control register stop being arbitrary: the
SuperIO reset bit exists because an external part needs one where an on-die block would fold
into chip reset; the byte-bus strobe length shares a register with the serial DMA port-offset
fields because external bus timing and port location are the same problem; and the DMA
engines have to be *told* where the UARTs sit because a fronted third-party part's register
layout is not something the fronting chip knows. The practical payload of this paragraph is
still the stride: assume symmetry with the chip's 32-bit blocks and the console emits
nothing.

Port A sits at the higher offset of the two, which is the reverse of the order the
names suggest. The IRIX header also carries a second, much lower set of offsets for the
Indy-class machines that used this chip, placing the ports at `0x18` and `0x20`.
Origami uses the `0x170` and `0x178` layout, which is what every Origin and Octane
build selects.

Each port presents the standard eight-register 16550 file, whose full behaviour is in
the PC16550D datasheet:

| Offset | Read | Write |
|---|---|---|
| `+0` | Receive buffer (line control bit 7 clear) or divisor low (bit 7 set) | Transmit holding register or divisor low |
| `+1` | Interrupt enable or divisor high | Interrupt enable or divisor high |
| `+2` | Interrupt identification | FIFO control |
| `+3` | Line control | Line control |
| `+4` | Modem control | Modem control — six bits, not the PC16550D's five; see **Modem control has a sixth bit** |
| `+5` | Line status | Line status — writable for factory testing; see **Line status takes a write** |
| `+6` | Modem status | Modem status — the four change indicators take a write; see **Modem status takes a write** |
| `+7` | Scratch | Scratch, or the clock pre-divider when the divisor latch is selected |

**Line status takes a write, and that is a documented facility rather than a quirk.** The
PC16550D datasheet's line-status section says so outright: "The Line Status Register is
intended for read operations only. Writing to this register is not recommended as this
operation is only used for factory testing." The Origin 300 IO-8 board test is factory
testing, and it uses it — it writes `0x7f` to port A's line status register and requires all
seven low bits back, reporting a mismatch as "Serial port A line status register (SIO_LSR)
of the IOC3 appears to be bad" with its own `sec1_2.c line:268`. Observed: the model
answered `0x00000060`, the idle transmitter-empty pair, against the expected `0x0000007f` at
`0x920000000f82017d` (run `prom-20260923T163110-bf2160`). The `0x7f` write is observed by
tracing the port's register writes (run `prom-20260923T163330-27a4e4`), and bit 7 is not in
it.

**One detail does not match the National part, and it is the same divergence modem control
showed.** National's note continues that in FIFO mode a write cannot reach bit 0 or bit 7,
and only reaches bits 2 through 4 through a loopback-loaded byte. The same trace shows the
FIFOs enabled when the board test writes — the last FIFO-control write before it sets the
enable — and the test still requires bit 0 back. Bit 7 it does not require, which is the
half of National's note that does hold. So this port accepts a bit-0 write in FIFO mode
where a PC16550D would not, which is one more reading in favour of the TI-style part the
modem-control bit and the clock constant both point at.

**What the model does, and the approximation in it.** A write stores bits 0 through 6, and
the next read of the register returns them and takes them away again. The returning half is
the specification's: reading line status is what resets the overrun, parity, framing and
break indicators. The taking-away of bit 0 is not — the data-ready indicator clears by
reading the receive buffer, not by reading status, so a strict model would leave a written
bit 0 set until something read a character that was never there. The model clears the whole
test value on the one read instead, so a port cannot be left permanently claiming a
character is waiting. Nothing observed distinguishes the two: the board test writes the
register, reads it once and moves on. A guest that wrote line status and then read it twice,
expecting data-ready to survive the first read, would tell them apart, and nothing this
project boots writes the register at all. *(the write established by the PC16550D
datasheet; bit 0 taking it in FIFO mode inferred from the diagnostic's expectation; the
clearing rule for bit 0 approximated, with the discriminating test named)*

**Modem status takes a write into its four change indicators, and only the board test says
so.** The Origin 300 IO-8 board test writes `0x0f` to port A's modem status register, reads
it back masked to the low four bits and requires `0x0f`, reporting a mismatch as "Serial port
A modem status register (SIO_MSR) of the IOC3 appears to be bad, before masking with 0xf, it
was ..." with its own `sec1_2.c line:281`. Observed: the model answered `0x00000000` against
the expected `0x0000000f` on the two-processor `origin300` machine (run
`prom-20260923T164307-277fcf`). The write, the read and the mask are read from the test's own
code (the store of `0x0f` to offset `+6`, the byte load and the `andi 0xf` at `0x100174b4` to
`0x100174c8` in the Origin 300 package's `io8bt`), not inferred from the expected value.

Unlike line status, the PC16550D datasheet describes no write to this register, so the
board test is the whole of the evidence. It expects the four change indicators —
delta clear-to-send, delta data-set-ready, trailing-edge ring indicator and delta carrier
detect — to hold a written value. It says nothing about the four live levels above them, which
it masks away.

**What the model does, and the approximation in it.** A write stores the low four bits and
the next read returns them and clears them, which is the specification's own rule for the
change indicators: reading modem status resets them. The live levels stay the 16550 core's.
A written indicator raises no modem-status interrupt in the model; whether the part raises
one is unknown, and a guest that enabled the modem-status interrupt and then wrote the
register would tell. *(write of bits 0–3 inferred from the diagnostic's expectation; clearing on read
from the specification; the interrupt side effect unknown)*

**Modem control has a sixth bit, and the PC16550D does not.** The Origin 300 IO-8 board
test writes `0x3f` to port A's modem control register and requires all six low bits back,
reporting a mismatch as "Serial port A modem control register (SIO_MCR) of the IOC3 appears
to be bad" with its own `sec1_2.c line:251`. Observed: the model answered `0x0000001f`
against the expected `0x0000003f` at `0x920000000f82017c` on the two-processor `origin300`
machine (run `prom-20260923T160032-140e46`), and the `0x3f` write is observed too, by
tracing the port's register writes through a whole run (run `prom-20260923T162505-32485f`)
rather than inferred from the expected value. Bits 6 and 7 stay unobserved, because the
test never writes them.

Bit 5 is the automatic-flow-control enable. National Semiconductor's PC16550D hardwires
bits 5 through 7 of this register to zero, so a strict PC16550D cannot answer `0x3f`; Texas
Instruments' 16550 variants put auto-RTS/CTS there, which mainline Linux records outright
in `include/uapi/linux/serial_reg.h` as "Enable auto-RTS/CTS (TI16C550C/TI16C750)". That
fits what **IOC3 overloads the scratch register** already notices independently — SGI's header names
the UART input-clock constant for Texas Instruments — so the SuperIO block being a TI-style
part is the reading two separate pieces of evidence share.

**Limitation: the model holds the bit and does not act on it.** Nothing observed exercises
automatic flow control. The board test writes the register and reads it back, and no guest
this project boots sets the bit at all, so the emulator stores it and continues to leave
flow control to whatever the driver does with request-to-send itself. A model that performed
auto-RTS/CTS and one that only remembers the request are indistinguishable to everything
run so far. What would tell them apart is a guest that enables the bit and then lets its
receive path fall behind, expecting the port to drop request-to-send without being asked:
until something does that, the behaviour is unmodelled rather than approximated, and this
must not be called hardware-accurate flow control. *(bit 5's storage inferred from the diagnostic's
expectation and the TI register layout it matches; behaviour unmodelled)*

**IOC3 overloads the scratch register.** While the divisor-latch bit in line control
is set, a write to the scratch register programs the UART's clock pre-divider rather
than storing a scratch byte. The pre-divider value the PROM writes is twice the divider it
wants, and the resulting reference clock is the crystal divided by that divider. The PROM
writes 6, giving a divide of 3, from which the ordinary 16550 divisor latches then select
the baud rate. An emulator that treats the scratch register as plain storage
will still boot, because the guest only ever reads back what it wrote, but the baud
rate it computes will not match the host serial port. Model the overload.

**The `sgi-origami` branch now does that modeling.** Commit `91aac9b11d` ("hw/misc/sgi-ioc3: program the UART
clock from the scratch pre-divider") derives each port's baud base, on the same
scratch write the divisor latch already exposes, from the crystal argued for below
divided by the pre-divider and by 16. A written pre-divider of 0 or 1 divides by
zero and is read as leaving the existing baud base alone, since nothing describes a
meaning for either value. Line control is re-issued immediately after the
pre-divider is programmed, so a divisor already latched before the pre-divider
arrives still takes effect without a further register touch. Reset states no
pre-divider value, so the model leaves whatever baud base already applies
untouched. *(approximated, reviewed)*

**And note what that sentence costs: the overload is unobservable from a boot.** A model
that stores the byte and a model that programs the clock produce identical boots, so no boot
test can distinguish them and a green transcript is not evidence the overload exists. The
register test is the only thing that can hold this, which puts it in the same class as the
window offsets — correct-looking either way, visible only to a structure test. Assert it
there or it is not asserted at all.

**The sgi-origami branch's implementation gives this a register test.** `91aac9b11d`
adds `scratch-predivider-reaches-backend-baud-rate`,
`unprogrammed-predivider-leaves-backend-silent`,
`scratch-predivider-survives-migration`, and
`scratch-predivider-survives-a-system-reset` to `sn0-ioc3-serial-test`, driving a port's
own UART registers against a `wctablet` backend that answers only at exactly 9600
baud, so a wrong baud base reads back as silence rather than a mismatched byte.
Migration gets its own case because the derived baud base lives in a realize-time
property of the embedded `SerialState`, not a migrated field of its own — a
`post_load` that only reads the surviving pre-divider would still lose to the
nested serial device's own migration recomputing the stock default first. Reset
gets its own case for the opposite reason: neither `sgi_ioc3_reset_hold` nor the
embedded 16550 core's own reset touches the derived baud base, so nothing recomputes
the wrong default in the first place, and only a case that reprograms solely the
divisor and line control after a reset — never rewriting the pre-divider itself —
can tell that apart from a model that quietly cleared it too.

**What the sources state, and what follows from them.** SGI's IRIX header defines the UART
input clock as **22,000,000 Hz** with a pre-divisor of **3**, and the PROM writes twice that
pre-divisor — 6 — to the scratch register. Those three numbers are stated outright and are
what a model should be built from. Note the header names the constant for Texas Instruments,
which fits the SuperIO block being an external part rather than something on the IOC3 die.

**The real crystal is very likely 22.1184 MHz, and this is an inference, not a stated fact.**
SGI's manual specifies these as 460-kilobaud ports. A 16550 reaches exactly 460,800 baud only
from a 7.3728 MHz reference, and 7.3728 × 3 is 22.1184 MHz — a standard crystal, and exactly
three times the reference the pre-divisor produces. The header's 22,000,000 divided by 3 and
16 gives 458,333 baud, which is not a standard rate and is half a percent off the figure the
manual advertises. The most economical reading is that the header rounds a 22.1184 MHz part.

Nothing observable turns on which figure a model uses, because the guest programs its own
divisor and reads back only what it wrote. It matters only when the emulated baud rate has to
line up with a real host serial port, and there the exact figure is the better bet. Treated as
an inference because the two things it rests on are a rounded constant and a marketing
specification, neither of which states a crystal frequency.

### Serial DMA rings and the 16550 shadow

Beyond the character-at-a-time 16550 interface, each port has a DMA engine that moves
characters between memory rings and the UART. IRIX and Linux both use it, and OpenBSD
deliberately does not, preferring to drive the 16550 directly. **A guest can reach a
working console without any of these registers**, so they are not first-boot critical.

**The firmware is not in the group that leaves them alone, and an earlier draft of this
section said it was.** Its console path does drive the 16550 directly, which is what that
claim was about, but its base I/O diagnostic drives both DMA engines in loopback on every
stock boot and fails the base I/O slot when they do not answer. So these registers are
not optional for a machine that boots stock firmware to its own monitor, only for one
that needs a console. The accesses it issues, and the three things in their order that no
register table can give, are in the behavioral contract's account of the base I/O
diagnostic's serial DMA sequence.

**Model limitation, stated: the transmit hold on an unwritable host backend is
bounded.** A real 16550 clocks every byte out of its shift register at the programmed
baud rate whether or not anything is on the wire. The model instead holds a byte while the
host chardev it feeds is connected but not writable, so that a reader that is merely slow
receives every byte; after QEMU PR #529 the serial DMA engine polls that same
transmitter-empty level before advancing its ring. Left unbounded, a reader that never
drains, or one that has not attached yet, parks the guest on transmitter-empty forever:
four stock boots on 2026-09-11 stopped after their first 134 console bytes that way. QEMU
PR #543 therefore bounds the hold at two seconds of virtual time, after which the byte
completes, dropped, and further bytes complete at once until the backend is writable again,
when held delivery resumes. The two-second figure is emulator policy chosen to sit well
above host scheduling gaps, not a hardware fact, and the latch is not migrated. The
discriminating tests are `never-drained-backend-completes-the-byte-wide-write`,
`dead-backend-completes-further-writes-at-once` and
`recovered-backend-resumes-delivery` in `sn0-ioc3-serial-test`, beside the unchanged
`blocked-backend-keeps-every-character`. *(approximated, reviewed)*

**What the bound costs is the consumer's copy, not the record.** A dropped byte is gone
from the port's backend, so a console client that stalls past the bound misses it and a
durable log taken from that backend misses it too. Each port therefore has an optional
second character device carrying every character the chip handed its UART, which no
consumer can refuse; IRIX runs take `console/ioc3-a.log` from there. That is an emulator
facility with no guest-visible effect and no bearing on the hardware contract above —
the QEMU device notes (QEMU document identity) describe it in
`docs/specs/sgi-ioc3.rst` under *Transmit record*.

Ring base, shared by both ports:

| Offset | Name | Width | Reset | R/W | Behavior |
|---|---|---|---|---|---|
| `0xB0` | Ring base, high | 32 | `0` | RW | Upper address bits of the ring area. |
| `0xB4` | Ring base, low | 32 | `0` | RW | Bit 0 selects ring size: clear means 1 KB rings, set means 4 KB rings. Bits 12–31 carry the base address, so the ring area is 4 KB aligned. |

Per port, seven registers four bytes apart — port A begins at `0xB8`, port B at `0xD4`:

| Port A | Port B | Name | Reset | R/W | Behavior |
|---|---|---|---|---|---|
| `0xB8` | `0xD4` | Control | `0` | RW | Bits 0–8 set the receive high-water mark. Bit 16 reads back as the transmit timer running. Bit 17 enables hardware flow control. Bits 18 and 19 ask the engine to post a receive record when carrier-detect or clear-to-send changes. Bit 20 selects quadruple speed. Bit 21 is a simulation aid that bypasses the clock divider. Bit 27 drains the receive buffer to memory. Bit 28 enables ring DMA. Bit 29 pauses DMA and bit 30 reads back once the pause has taken effect. Bit 31 resets the DMA channels. |
| `0xBC` | `0xD8` | Transmit produce index | `0` | RW | Software advances this to hand buffered characters to the engine. |
| `0xC0` | `0xDC` | Transmit consume index | `0` | RW | Hardware advances this and software can place it. Bits 24–27 report how many bytes are still held in the unpacker, and a write does not set them. Read-only through the alias pages. |
| `0xC4` | `0xE0` | Receive produce index | `0` | RW | Hardware advances this and software can place it. Bits 24–26 report how many bytes are held in the packer, and a write does not set them. Read-only through the alias pages. |
| `0xC8` | `0xE4` | Receive consume index | `0` | RW | Software advances this. Bit 31 arms the receive timer. |
| `0xCC` | `0xE8` | Receive timer | `0` | RW | Bits 0–11 hold the reload value, bits 16–27 read back the current count. Writing the reload loads the count as well, and an unarmed timer holds it; see **A reload write loads the count**. The timer ticks at 16 kHz, so a reload of N expires after roughly N/16 milliseconds of receive idleness. |
| `0xD0` | `0xEC` | 16550 shadow | live | RO | A read-only mirror of the UART's status and modem lines, so software using DMA never has to touch the byte-wide window; the bit layout is below. It is not a stored register and does not read a fixed zero — it reflects whatever the lines carry, which at reset is the transmitter empty and the console port's carrier and clear-to-send asserted. IRIX's DMA-ring console driver reads carrier and clear-to-send from here rather than from the byte-wide modem-status register, so a model that returned zero reported the line dead and hung the console past `failsoftd`. |

All four index registers address the ring in 8-byte units: the pointer occupies bits 3
upward, so 1 KB rings use bits 3–9 and 4 KB rings use bits 3–11.

**The two hardware-advanced index registers take a write, which is why the serial DMA
register table calls them read-write.** The Origin 300 IO-8 board test's IOC3 section writes the
transmit consume index at `0xC0` and reads it back, failing on a mismatch with "Serial
port A register (RX/TX produce/consume) of the IOC3 appears to be bad" and its own
`sec1_2.c line:114`. A model that refuses the write answers the stale engine value
instead — received `0x000003f8` against an expected `0x00000ff8` at
`0x920000000f8000c0` on the two-processor `origin300` machine (run
`prom-20260923T160032-140e46`).

This is the same shape of evidence as the shadow-register write that **The shadow's
driven-output bits look writable** analyses, so it
gets the same discriminating check: is the write a duplicate of something else that
already put that value there? Here it is not, and the register trace of a whole run shows
why (run `prom-20260923T160313-bf8684`, tracing this block's reads, writes and refusals).
Immediately before the failing access the test selects 4 KB rings by writing `0x00000001`
to the ring base at `0xB4`, writes `0x00000ff8` to the transmit produce index at `0xBC`
and reads it straight back, then writes `0x00000000` to that same produce index — and only
then writes `0x00000ff8` to the consume index and reads it. With produce at zero, a base
of zero and nothing transmitting, no engine advance can put `0xff8` in the consume
register. Software's own write is the only thing that can, so the register stores one.

The receive produce index at `0xC4` follows by the same reading rather than by its own
observation: the test writes both, and its own error text names all four indexes
together, but the transmit index fails first so the test never reaches the receive one.
The model treats the pair alike. *(both writes inferred from the diagnostic's expectation
and the traced write sequence; not measured on hardware)*

**What the read-only reading rested on, and why it lost.** No driver writes either
register, and that is where it came from. Driver behaviour shows what software expects,
not what the silicon permits, so it could never have settled the question on its own;
the board test writes both registers deliberately, which is the kind of evidence that
can. The alias pages keep their own read-only contract, which is separate and still
holds: a page exists so a process can be handed one port's registers without being handed
the engine, and `tests/qtest/sn0-ioc3-serial-test.c` pins both halves — the alias
refusing these two and the main block taking them.

The change from read-only is cleared by the
IO-8 board test review (citation: `docs/reviews/io8bt-ioc3-isp12160-xbridge-review.md`) on the trace
argument of run `prom-20260923T160313-bf8684`.

**A reload write loads the count, and an unarmed timer holds it.** The Origin 300 IO-8
board test's serial DMA check resets port A's channel, writes `0xfff` to the receive timer
with the timer unarmed, and reads it back under a mask of `0x0fff0fff` expecting
`0x0fff0fff` — the reload and the same value in the count field. It reports a mismatch as
"Serial port A rx timer register (SRTR) of the IOC3 appears to be bad" with its own
`sec1_3.c line:1005`. Observed: a model whose idle count read zero answered `0x00000fff`
(run `prom-20260924T160137-915f94`, with this block's accesses traced: channel reset, the
four indexes zeroed with the arming bit clear, the reload written, ring DMA enabled, the
reload written again and read). What the count reads after the timer expires, or after a
running timer is disarmed, is not established; the model keeps zero there. A guest that
read the count after an expiry, or after disarming, would tell. *(load on write
inferred from the diagnostic's expectation; the count after expiry or disarm unknown)*

**The same check measures the count against a 10 ms delay, and on a loaded host the
emulator does not always meet it.** After the reload test the board test
arms the timer, sends one loopback entry, waits 10,000 µs by the processor's count
register (`0x2710` µs at `0x100236dc`) and requires the count above `0xf40` — fewer than
191 ticks, 12 ms, elapsed (`sec1_3.c line:1077`, "counted down too fast"). Both clocks run
on QEMU's virtual clock, which follows host time without an instruction counter, so a
processor thread descheduled between the end of the delay and the read lets the timer
run on. Measured on this host at a load average of 70 to 125 on 32 threads, one host
thread per emulated processor: three runs read `0x0f39`, `0x0e73` and `0x0f0d` (12.3,
24.75 and 15.0 ms) and failed, and three read `0x0f5d` (10.1 ms) and passed (runs
`prom-20260924T161654-65e963`, `prom-20260924T165546-c49eaf`,
`prom-20260924T170243-577997`, `prom-20260924T162956-e30120`,
`prom-20260924T164032-c60b04` and `prom-20260924T171055-45763f`). A failure there on a
loaded host is a property of the run, not of this register. Those counts predate the
line time below, which leaves this check more margin on port A.

**A loopback character reaches the receiver a character time after it is sent, and the
receive timer starts then.** A 16550 in internal loopback still clocks each character
through its shift registers at the programmed rate, and the timer counts idleness from
the last delivery, so it cannot start before the last character of the entry arrives.
*(inferred: the PC16550D datasheet's Modem Control Register section (bit 4, "loop")
wires the Transmitter Shift Register's output into the Receiver Shift Register input,
both clocked by the baud generator, and says "in the loopback mode, data that is
transmitted is immediately received." That sentence is read as "immediately" once the
transmitter shift register has moved it, without a separate line or MODEM propagation
delay layered on top of the shift registers' own bit-by-bit clocking — not as a
zero-time transfer of the whole character. That the IOC3 starts the timer at arrival
rather than at the doorbell follows from the timer counting receive idleness)* The
model once delivered at the doorbell because its 16550 core loops a character back the
moment it is written. QEMU commit `hw/misc/sgi-ioc3: deliver a loopback character after
its line time` holds each looped-back character for the 16550 core's own character time
at the port's programmed rate and 8N1 frame, sends the entry's next character only when
it lands, and starts the timer from the last arrival, applying an arrival already due at
the next register access the way the receive timer's expiry is. *(approximated: the
model delivers the character exactly one 10-bit frame — start, eight data and stop bits,
float-rounded — after the THR write, with no transmit-FIFO run-ahead across the ring
entry, and the byte-wide 16550 window shows the whole character at once, through
LSR data-ready, once that frame elapses. A real 16550 raises data-ready around the
middle of the stop bit rather than at its end, and the IOC3 polls the UART at its own
rate rather than being told the instant it changes; the QEMU device
notes (QEMU document identity) record the same gap the other way, in
`docs/specs/sgi-ioc3.rst` under *Transmit*, as a consume index that can trail
silicon's. `sn0-ioc3-serial-test` pins the delivered behavior:
`loopback-arrives-a-character-later`,
`late-loopback-arrival-keeps-its-time`, `loopback-in-flight-survives-migration`.)*

What the check actually sends is not what its setup suggests, and the disassembly shows
why. Its setup helper (`0x1002ab50`) sets divisor-latch access, writes divisor 12, the
pre-divider `0x18` and 8N1, then modem control `0x11` (loopback and
data-terminal-ready): 9600 baud, 1.04 ms, about 17 ticks, a character. The check also
sets control bit 20, quadruple speed, which the model takes as not changing the line
rate: Linux's full ring driver (`drivers/tty/serial/ioc3_serial.c`) sets it on every
port and calls it high-speed polling of the UART by the engine. Against that reading,
Linux's own SN header names the same field `SSCR_HIGH_SPD` and comments it "4X speed"
(`include/asm-mips/sn/ioc3.h`), which could be read as a line-rate multiplier instead;
the driver comment is the better evidence, but the conflict is worth seeing.
*(inferred from the driver comment, against the header's own naming)* The sub-section's
per-port loop sets the UART base it hands that helper correctly, per port, at
`0x1001dc90` (port B, `base | 0x20170`) and `0x1001dcc0` (port A, `base | 0x20178`);
inside every pass it is then overwritten with port A's window at `0x10022dc4`, and
nothing restores it before the setup call. The DMA register base follows the port
throughout. So on port B's pass the sub-section reprograms port A's 16550 and drives
port B's engine, and port B's 16550 keeps whatever rate it last had. The ring entry the
doorbell hands over is left from the sub-section's earlier conditions and carries four
characters, `abcd`, not one. *(established from the disassembly at the addresses given;
the four-character count is corroborated by the measured 94-tick port A timing below,
not by the register trace of run `prom-20260928T162947-6092fe`, which records only
serial-DMA register accesses and cannot show the UART writes or the ring's byte
contents. Port B's leftover rate is inferred from its timing in that same trace, about
90 µs a character, which fits 115200 baud.)* With the line time the model reads port A
at `0xfa1` or `0xfa0` (94 or 95 ticks:
10 ms less four 9600-baud characters; one window of ten read 126) and port B at `0xf64`
(155 ticks), against `0xf5e` (161) on both before. The test's printed nominal `0x0f71`
(142 ticks) matches neither and appears to be a fixed reference value, not a measurement
of this sequence. *(counts measured on runs `prom-20260928T162446-1e77c8`, two
processors, and `prom-20260928T162947-6092fe`, four, each five passes and passed; what
the shipped part reads is not measured)*

On the two-chassis pair (`--topology origin300-two-chassis --cpus 8`) the line time does
not stop the check failing on this host. Two runs of five passes, with each emulated
processor's thread pinned to its own host core by `taskset` and the main loop on a
ninth, each failed once at `sec1_3.c line:1077`, both times on the second chassis's IOC3
with processor 7 running the test: port B read `0xf0c` (243 ticks, run
`prom-20260928T164149-b76cb1`) and port A `0xf2a` (213 ticks, run
`prom-20260928T165023-eac88e`); every other window on the pair read 94 to 96, 125 or 137
on port A and 155 or 156 on port B. No host core was idle: other guests and test suites
ran unpinned across the same cores. In the 0.6 s sample enclosing each failing window
processor 7's thread spent 391 and 352 ms runnable but not running. That is the host
descheduling the test's processor, as above, not a register or line-time difference.
*(measured; schedstat sampled every 0.5 s, so the wait inside the 10 ms window itself is
inferred from the enclosing sample)*

On the two-chassis Origin 300 the same check fails far more often and by far more, with
the same message: of seven runs of `-d 5 -e 1` on 26 September, five of them with
`-n 0`, six recorded a failure and the one that passed ran under a debugger. Two traced runs made three reads: `0x0a33` (92.8 ms, port A, run
`prom-20260926T230147-98fe70`), `0x0f5d` (10.1 ms, port A) then `0x0b63` (73.8 ms, port
B, run `prom-20260926T233053-cb5533`), against `0x0f5c`, `0x0f4b` and `0x0f22` (10.2,
11.3 and 13.8 ms) on one chassis with four processors at the same load (runs
`prom-20260927T000205-9cad9a` and `prom-20260927T000735-13f116`, the second with the
lock profiler running). The first pair run used QEMU before the receive-timer change
below, the others included it; the count check does not depend on it. Life count `0x35` is
port A's check and `0x5d` port B's. The extra delay is lock contention, not an IOC3
difference. The idle sMDK processors poll the Bedrock PI's pending, mask and error
registers (their register state holds those addresses), and every device-register read
takes QEMU's global lock. QEMU's lock profiler over about 45 s counted 22 million
device-register acquisitions averaging 5.3 µs of wait on the pair, and the main loop
waiting 13 ms on average for the lock; on one chassis with four processors, 47 million
averaging 0.6 µs, and 2.3 ms. With seven pollers instead of three, the one processor
running the test waits far longer between the end of its delay and its read of this
register. *(measured on this host at load average 70 to 157; the profiler adds its own
overhead, the single-chassis figures were taken during the test and the pair's just after
it, and the acquisitions were not traced per address, so their attribution to the PI
polls is inferred)*

**The receive-timer source is set by the time the count has run out.** After the 10 ms
count check the board test writes a reload of 2 to the same port, arms the timer, sends
one loopback entry, waits 10,000 µs and requires the port's receive-timer source set in the
status register: bit 3 for port A, bit 12 for port B ("rx timer register interrupt bit
(2^3) in SIO_IR reg of the IOC3 should have set", `sec1_3.c line:1120`; the sequence is at
`0x10023b50` to `0x10023cbc`). Two ticks is 125 µs, so by the read the timer has long run
out; the test requires only that the source follows within those 10 ms. Observed: a model
that raised the source only when its timer callback ran left bit 12 clear at that read and
failed (run `prom-20260926T175211-04c4e5`, four processors on one node, loaded host), because
the host had not yet serviced the timer. The register value the error screen prints,
`0x00000002`, is read after the test has disarmed the timer, so it shows the reload and
says nothing about the count at the failing read. The model now applies an expiry already
due before any access to the status register or to either port's serial DMA registers,
reads and writes both, so the source and the count read off the clock agree, and an
acknowledgement or a disarm cannot be undone by a late callback.
`tests/qtest/sn0-ioc3-serial-test.c` pins it for both ports with the VM stopped, which
leaves the clock moving but services no timer. *(the source following the count's expiry
inferred from the diagnostic's expectation; that they coincide exactly is the model's
choice; the access-time catch-up is an emulator mechanism, not a hardware claim)*

**The receive high-water mark reads as a count of characters rather than entries, and
that is a reading rather than a measurement.** SGI's base I/O diagnostic programs 511
against 1 KB rings, which hold 128 entries of four characters each — 512. Counted in
entries a mark of 511 could never be reached; counted in characters it is one short of
full, which is what a mark deliberately set out of the way looks like, and the field's
nine bits reach exactly that. Nothing we run can tell the two readings apart, precisely
because the only driver that programs the mark sets it where it will not fire. What would
settle it is a guest that programs a mark it intends to reach. **The ethernet block's
receive watermark is a different register counting descriptors**, and the two are easy to
carry across.

The shadow register packs the 16550 state as: data-ready, overrun, parity error,
framing error, break, transmit-holding-empty and transmit-shift-empty in bits 0–6;
a flag in bit 7 that the character at the head of the receive FIFO carries an error;
delta-clear-to-send in bit 16 and delta-carrier-detect in bit 19; the current
clear-to-send and carrier-detect levels in bits 20 and 23; the driven
data-terminal-ready and request-to-send levels in bits 24 and 25; the two general
16550 outputs in bits 26 and 27; and loopback in bit 28.

**The shadow's driven-output bits look writable in a register trace and are not.** SGI's
base I/O diagnostic writes `0x13000000` to both ports' shadow registers while setting up
its DMA loopback test — data-terminal-ready, request-to-send and loopback, exactly the
driven-output field this register reports. Read alone, that is a write to three bits this
table calls read-only. Two accesses earlier the same diagnostic has already set the same
three bits through the byte-wide modem control register, and **that** is what puts the
port in loopback: the shadow write is a duplicate, and the port loops back whether or not
anything stores it. The read-only model therefore survives the one piece of evidence that
appears to overturn it. Recorded because the first reading is the wrong one, and a reader
meeting the trace without the two accesses above it will reach it.

A ring entry is 8 bytes and carries four characters, not one: four data bytes followed
by four status-or-control bytes, one per data byte. Software can therefore test all
four validity flags with a single 32-bit read of the status half, which is exactly what
IRIX does — its ring entry overlays that four-and-four byte view with a pair of 32-bit words
for precisely this purpose, and the four rings live in one block four ring-sizes long.

**This format is not in SGI's register header.** The header defines the ring registers and
their bit fields and stops; the entry layout comes from SGI's own DMA-ring serial driver,
which is the only vendor code that drives the rings rather than the byte-wide 16550. Neither
of the other two implementations we normally cross-check against can confirm it — OpenBSD
puts the ports in pass-through mode and drives the 16550 directly, and mainline Linux
replaced its full IOC3 serial driver with a small shim that does the same. An out-of-tree
Linux tree we hold does carry a full ring driver, and it is the cross-check to use if this
section is ever implemented.

The single ring base register pair addresses one contiguous block holding four rings in
a fixed order — port A transmit, port A receive, port B transmit, port B receive — each
of the size the base register's low bit selects. The block is therefore four times the
ring size, 16 KB when 4 KB rings are selected, and the base itself is 4 KB aligned.
Both ports share one base, so a model must derive each ring's address from the port and
direction rather than expecting four base registers.

In a receive entry the status byte means, from bit 0 upward: characters were lost to
overrun, a parity error, a framing error, a break was received, the current
clear-to-send level, the current carrier-detect level, then a flag that the modem and
overrun fields are meaningful and a flag that the data byte and the error fields are
meaningful.

### Clear-to-send in loopback under hardware flow control

**Looped back with hardware flow control enabled, a port reports clear-to-send asserted
even though software left request-to-send clear.** The Origin 300 IO-8 board test's serial
DMA check expects the level. Its setup routine (`0x1002ab50` in the Origin 300
package's `io8bt`) writes modem control `0x11` — data-terminal-ready and loopback, no
request-to-send — and the shadow `0x11000000`; the check then writes control `0x10120020`
(ring DMA, quadruple speed, hardware flow control, high-water mark 32), sends `"abcd"`,
masks each received status byte with `0xdf` to drop carrier-detect and requires `0xd0`:
data valid, modem valid and clear-to-send. On a PC16550D in loopback, clear-to-send follows
request-to-send and would read clear. Observed: the model, built that way, answered
`0x61626364c0c0c0c0` against the expected `0x61626364d0d0d0d0` at `sec1_3.c line:450`
(runs `prom-20260924T153905-beb2b7`, and `prom-20260924T154059-ccd097` with the port's
register writes traced). The expected value is a constant in the test's code at
`0x1001ff24`, not computed from anything the port reports.

**The mechanism is not established.** Two readings produce the level, and the board test
cannot tell them apart. Hardware flow control may drive request-to-send itself while the
receive side has room, as flow-control engines commonly do, so the loopback carries it back
as clear-to-send; or this part's loopback may route its outputs differently from National's.
No public document describes either, and every serial DMA check in the test enables flow
control, so none of them isolates it. What would settle it is a capture of this port's
modem status or shadow in loopback with flow control disabled and request-to-send clear.

**What the model does.** In loopback with the port's flow-control bit set, the model reports
clear-to-send asserted in all three places the chip reports it — the receive status byte,
the shadow and the byte-wide modem status register — and leaves modem control as software
wrote it. It generates no delta-clear-to-send for the change. Outside loopback, or without
flow control, the port behaves as before. It performs no flow control: transmission is not
held for clear-to-send and request-to-send is not dropped at the high-water mark.
*(level inferred from the diagnostic's expectation under that condition; mechanism unknown;
approximated)*

In a transmit entry the control byte's top two bits are a type field. Both clear means
the entry ends there: that slot and every slot behind it in the same entry are not
sent, and the engine moves on to the next entry. Bit 6 alone means the data byte is a
character to send. Bit 7 alone
routes the data byte to the modem control register instead. Both set makes the data
byte a delay in milliseconds. Independently of the type field, bit 5 asks for an
interrupt once that byte has gone out.

**An empty slot ends its entry wherever it sits, not only after the last character.** The
Origin 300 IO-8 board test's serial DMA check is the evidence. It builds a transmit ring
whose entries carry empty slots in the middle — one entry's control bytes are `0x40`,
`0x00`, `0x40`, `0x40` — and computes the receive stream it expects in its own buffer: the
first slot's character, then the next entry's first character, with the two characters behind
the empty slot absent. A model that skipped the empty slot and sent the rest delivered those
two characters as well, and the test failed with "Serial port A dma data miscompare at loc
..." and its own `sec1_3.c line:901`: received `0x9bc85ed7d0d0d0d0`, expected
`0x5ed7cab9d0d0d0d0` (run `prom-20260924T155550-f38614`, with the transmit ring at
`0x4b0000`, the receive ring at `0x4b1000` and the expected buffer at `0x4b4000` read back
from the live guest after the failure). The other slots in the same ring — trailing empties,
as in the entry `0x40`, `0x40`, `0x40`, `0x00` — agree with either reading, which is why
the earlier model passed everything else. *(inferred from the expectation of SGI's factory test
for this board, not measured on hardware)*

### Interrupt status and enable registers

| Offset | Name | Width | Reset | R/W | Behavior |
|---|---|---|---|---|---|
| `0x1C` | Interrupt status | 32 | `0` | RW — reads status, **written to acknowledge** | Every source the chip can raise. Writing a one to a latched bit clears it. Writing a one to a level-following bit does nothing — the source clears when the underlying block deasserts. SGI's manufacturing table gives it a writable mask of all ones. |
| `0x20` | Interrupt enable, set | 32 | `0` | W1S | Writing a one enables that source. Reading returns the live mask. |
| `0x24` | Interrupt enable, clear | 32 | `0` | W1C | Writing a one disables that source. Read behaviour unverified. |

**The interrupt status register is not read-only, and writing it is how software
acknowledges an interrupt.** Modelling it read-only produces a machine that prints its
banner and then wedges the moment a character arrives on the real serial port, because the
guest's handler re-enables a source that is still pending and takes the same interrupt
forever. Three independent implementations write it:

- IRIX's IOC3 core driver writes all ones to it during initialisation, and SGI's own SN0
  serial diagnostic writes `0xFFFFFFFF` to it.
- IRIX's PS/2 driver writes the keyboard bit to it in three places, each time immediately
  before re-enabling that source, with its own comments calling the write clearing and
  acknowledging the interrupt. IRIX's parallel port driver writes its pass-through and
  DMA-context bits the same way.
- OpenBSD 7.9's IOC3 driver writes the register's own read-back value into it at attach —
  an idiom that only makes sense for write-one-to-clear — and its SuperIO handler writes
  the serviced bits back before re-enabling them.

SGI's manufacturing register table, which this datasheet uses as its authority for reset
values, gives this register a writable mask of all ones rather than marking it read-only.

**Which bits the write actually clears, and how well each part of that is known.** Mainline
Linux states the answer directly for the bits it handles: it carries a mask naming which
sources are level-triggered, that mask holds exactly **serial port A at bit 6 and serial
port B at bit 15**, and it installs the kernel's level handler for those two and the edge
handler for everything else. The keyboard bit 22, which it also handles, therefore gets the
edge handler — and IRIX independently treats that bit as latched, writing it to this
register to clear and to acknowledge.

So the contract is:

| Bits | Behaviour | How well established |
|---|---|---|
| 6 and 15, the two serial pass-throughs | Follow the 16550's own interrupt level; a write cannot clear them | **Stated outright** by Linux's level mask |
| 22, keyboard and mouse | Latched; write-one-to-clear. What asserts the line it latches is **approximated** — see the PS/2 section below | Linux's edge handler, and IRIX writes it to acknowledge |
| 18, the parallel pass-through | Follows its sub-block's level, like the other two pass-throughs | **Supported by IRIX's parallel driver**, which quiets the parallel port's own service interrupt *before* writing this bit — the step a latched bit would not need. See below |
| 2 and 11, the two receive high-water marks | Level: follows whether the port's receive ring holds the mark's worth of characters; a write cannot clear it, and it falls when software consumes back below the mark | **Measured**: IRIX 6.5.30's ring console enables it, never acknowledges it, and stormed a latch that never falls; the level model clears the storm. See below |
| 0/9 transmit ring empty and 1/10 receive ring full | Modelled latched, but open the same shape as high-water | Bit 0 was observed set and never acknowledged, but it is never enabled, so nothing yet forces the reading |
| everything else | Latched; write-one-to-clear | Nothing contradicts it, nothing states it |

For a level-following bit the real acknowledgement is whatever quiets the sub-block, which
for a serial port is the 16550 register read the guest's own `com` driver performs, and the
write to this register can only be a no-op. **Model all three pass-through bits as
level-following and ignore writes to them**, because a write-one-to-clear that cleared a
still-asserted level would silently drop an interrupt the model would never re-raise.

**The receive high-water bit is a level of a different kind, settled by measurement.** What
quiets it is not a sub-block read but software advancing the receive consume index past the
mark, so the model recomputes it against the waiting count on every engine pass and ignores
writes to it, for the same reason the pass-throughs ignore them. IRIX 6.5.30's ring console
enables the high-water source and never writes a one to it; against a model that latched the
bit, its service loop spun on the unclearable source at tens of thousands of status reads a
second until it tore the port down and retried, and the level model clears that storm.
Transmit-ring-empty and receive-ring-full are open questions of the same shape — bit 0 was
seen set and never acknowledged — but no guest we run enables either, so nothing forces the
reading and both stay modelled as latched for now.

**What settles bit 18 is the order of operations in IRIX's parallel driver.** On taking the
parallel pass-through interrupt it first clears the service-interrupt bit in the parallel
port's *own* extended control register — the driver's comment calls that clearing the
service interrupt — and only then writes the pass-through bit to the interrupt status
register and masks the source. Quieting the sub-block first is the step a latched bit would
not need, and it is the same order OpenBSD's serial path follows, where the `com` driver's
16550 read quiets the port before the handler writes the bit. The same driver treats the
parallel port's two DMA-context bits differently, writing them to the status register
directly to clear them, which is what a latched bit looks like in the same file.

That is behavioural evidence from vendor code rather than a statement in a header, and no
header states it. It is strong enough to stop calling bit 18 an assumption, and the reading
is load-bearing rather than free: IRIX drives the parallel port on our path, and a transfer
completes through exactly this bit.

The chip asserts its PCI interrupt when the bitwise AND of status and enable is
non-zero. IRIX computes exactly that, reading the status register and the enable-set
register and taking their intersection, which confirms that the set address reads back
the live mask rather than returning zero or the last value written. No source reads the
enable-clear address, so what it returns is unknown; the safe model is to have it
return the same mask, and the qtest should assert only the behaviour of the set
address until a guest proves otherwise. IRIX funnels every write to the two enable
addresses through a helper of its own, but that is a driver-side spinlock and a workaround
for its real-time interrupt source, not a property of the registers — the writes underneath
are plain stores.

Source bits, from bit 0 upward: serial port A transmit ring empty, receive ring full,
receive high-water reached, receive timer expired, carrier-detect changed,
clear-to-send changed, a pass-through interrupt from the port's own 16550, an explicit
transmit completion, and a PCI error on that port's DMA. Bits 9 through 17 repeat the
same nine sources for port B. Bits 18 through 21 cover the parallel port: its
pass-through interrupt, its two DMA contexts, and a PCI error. Bit 22 is the shared
keyboard and mouse interrupt. Bit 27 fires when the interrupt-output block emits a
pulse and bit 28 when its input pin is pulsed.

The pass-through bits matter more than the rest. OpenBSD enables only the
pass-through bit for each serial port and lets its ordinary 16550 driver work the
byte-wide registers, so **a correct pass-through path is sufficient for an OpenBSD
console** and the whole DMA ring apparatus can come later.

### Parallel port

**IRIX drives the parallel port on origami's path**, and the block is implemented: the DMA
engine drains an armed context, raises the completion the driver waits on, and carries the
bytes through to a host backend. What follows is the bit-level detail the register-structure
survey left open, and it is the specification the model was built from rather than a
description of the model. It rests on IRIX-derived sources alone — neither guest we
cross-check against carries a parallel driver — and is behavioral evidence from vendor code
rather than anything SGI published.

The port has two faces. A DMA engine in the main register block moves buffers between
memory and the port through **two transfer contexts** that the engine alternates between,
and an **ECP/IEEE-1284 register file** in the byte-wide SuperIO window carries the
peripheral-facing control, status and mode logic — the same standard-parallel-port and
Extended Capabilities Port register model a PC SuperIO chip presents. The chip is an
Extended Capabilities Port: the compatibility, byte and ECP modes of IEEE 1284, with a
FIFO and a DMA path behind them.

The DMA registers in the main block, four bytes wide each:

| Offset | Name | Width | Reset | R/W | Behavior |
|---|---|---|---|---|---|
| `0x80` | Context A buffer address, high | 32 | `0` | RW | Upper 32 bits of the context's buffer physical address. |
| `0x84` | Context A buffer address, low | 32 | `0` | RW | Lower 32 bits. The two halves form one 64-bit physical address; the driver imposes no sub-byte alignment. |
| `0x88` | Context A count and control | 32 | `0` | RW | Bits 0–16 hold the byte count for this context, so one context moves up to 131071 bytes; the same field reads back the count still outstanding during and after the transfer. Bit 30 arms the context. Bit 31 marks it the last context of a chain, so the engine asserts terminal count once it drains. |
| `0x8C` | DMA control, shared | 32 | `0` | RW | Bits 0, 1 and 2 read back context A's armed flag, its last flag, and whether the engine is currently draining it; bits 3, 4 and 5 report the same three for context B. Bit 29 sets direction — clear drives the peripheral, set reads from it. Bit 30 resets the DMA state machine while held. Bit 31 enables the DMA engine. |
| `0x90` | Context B buffer address, high | 32 | `0` | RW | As context A. |
| `0x94` | Context B buffer address, low | 32 | `0` | RW | As context A. |
| `0x98` | Context B count and control | 32 | `0` | RW | As context A's count and control register. |

The byte-wide ECP register file, offsets measured from the SuperIO window base `0x20000`:

| Offset | Name | Reset | R/W | Behavior |
|---|---|---|---|---|
| `0x150` | Data | `0` | RW | The eight parallel data lines in the compatibility modes; also where software places the IEEE-1284 extensibility-request value during negotiation; the FIFO data path in ECP mode. |
| `0x151` | Status | line-driven | RO | The peripheral's status lines: bit 2 a device attention signal, bit 3 nFault with a clear level meaning a fault, bit 4 Select, bit 5 paper error, bit 6 nAck, bit 7 Busy. Like the 16550 shadow, this reflects the cable rather than storing a value. |
| `0x152` | Control | unverified | RW | The port's output control lines: bit 0 nStrobe, bit 1 nAutoFeed, bit 2 nInit, bit 3 nSelectIn, bit 5 transfer direction with a set level selecting read-from-peripheral. |
| `0x158` | FIFO / configuration A | unverified | RW | The data FIFO in the compatibility-FIFO and ECP modes; the configuration-A register in configuration mode. |
| `0x159` | Configuration B | unverified | RW | The configuration-B register. |
| `0x15A` | Extended control | `0x01` | RW | Bit 0 reads FIFO empty and bit 1 reads FIFO full; bit 2 controls the service interrupt, bit 3 enables the DMA path, bit 4 controls the nFault interrupt; bits 5–7 are the mode field. The empty bit is set and the full bit clear out of reset, which is the presence check the driver's probe relies on. |

**Mode selection is the extended-control register's top three bits**, the standard ECP
encoding. The values the driver uses are compatibility non-FIFO (field `000`),
compatibility FIFO (field `010`, whole-byte value `0x40`, the driver's default) and ECP
(field `011`, `0x60`); the header also names the byte/PS2 field `001` (`0x20`) and a
configuration mode (field `111`). The driver reaches ECP mode only after a successful
IEEE-1284 negotiation: it places the extensibility-request value for ECP on the data
register, drives the negotiation handshake on the control register's nAutoFeed, nSelectIn
and nStrobe lines, and watches the status register for the peripheral's Select, paper-error
and nAck responses; if the peripheral does not confirm ECP support it falls back to
compatibility FIFO. Absent a driver-driven mode change the port sits in compatibility FIFO
mode. Byte-wide input (peripheral-to-host) is defined in the register set but the shipping
driver is output-only, so the read direction is documented as present rather than exercised.

**A transfer is double-buffered across the two contexts.** Software chains memory
descriptors, each a buffer address and a byte count. It loads context A's address and count
registers, arms the context, and marks it last only if the chain ends there; if a second
descriptor follows it loads context B the same way, and thereafter refills whichever context
just completed. It sets direction and the engine-enable bit in the shared DMA control
register, enables the DMA path in the extended-control register, and enables the four
parallel interrupt sources. As each context drains, the chip raises that context's
completion interrupt; the driver acknowledges it by writing the bit back to the interrupt
status register, subtracts the context's byte count from the residual, and loads the next
descriptor. To avoid one interrupt too many, it clears the context's enable in the interrupt
mask for the final descriptor.

**Completion is the pass-through service interrupt, not a context interrupt.** When the
context carrying the last flag drains and the engine asserts terminal count, the chip raises
the parallel pass-through interrupt; the driver quiets the service condition in the
extended-control register first, then acknowledges the pass-through bit and resets the DMA
state machine by pulsing the shared control register's reset bit high then low, polling the
chip-control register's arbiter-diagnostic fields until the parallel master goes idle.
A PCI error mid-transfer raises the parallel error interrupt instead: the driver disables
DMA, reads the busy flags to find the active context, recovers the residual from that
context's count register, and resets.

Those four sources are the parallel port's share of the main-block interrupt register
already tabulated above — the pass-through at bit 18, context A at bit 19, context B at
bit 20, and the PCI error at bit 21. The completion path here is why bit 18 is modelled
level-following while bits 19 and 20 are latched: the driver quiets the port's own service
condition before writing bit 18, but clears the two context bits by writing them directly.

**One polarity is left open.** The extended-control register's service-interrupt and
nFault-interrupt control bits (bits 2 and 4) are enable-or-mask controls whose exact active
sense the sources do not pin down cleanly: the driver holds both set while the port idles in
compatibility mode and manipulates the service bit around a DMA transfer, and a vendor
comment describes the interrupt-arming conditions with these bits at zero, which do not
obviously reconcile. The bit positions and their roles are certain; the precise
active-high-versus-low arming is not. IRIX drives the port on our path without settling it:
the driver quiets the service condition by writing that register, so a model can set the
condition at terminal count and drop it on the next write there without ever reading the two
control bits to decide, which is what the implementation does. The question stays open and
nothing on our path forces it.

### Chip control and interrupt output

| Offset | Name | Width | Reset | R/W | Behavior |
|---|---|---|---|---|---|
| `0x28` | Control | 32 | `0` | RW | Bit 0 resets the SuperIO block. Bits 1–7 and 8–14 tell the DMA engines where serial ports A and B live inside the SuperIO window, each as the port's byte offset divided by 8. Bits 15–18 set the byte bus strobe length. Bits 19–21 report which internal master currently wants the PCI bus and bit 22 reports the arbiter idle; both are diagnostic and read-only. |
| `0x2C` | Interrupt output | 32 | `0` | mixed | Bits 0–15 hold the interval count. Bits 16–18 select the mode. Bit 30 controls diagnostic mode. Bit 31 reports the current output state. |

The PROM programs the control register with port A at byte offset `0x178` and port B
at `0x170`, encoded as `0x2F` and `0x2E` after the divide by 8, together with a byte
bus strobe length of 4. The resulting value is `0x00022E5E`. A qtest can assert
exactly that.

**An engine aimed anywhere but its port's 16550 does not reach it.** Mainline Linux's
header names the two fields the DMA poll address of ports A and B
(`arch/mips/include/asm/sn/ioc3.h`, `SIO_CR_SER_A_BASE` and `SIO_CR_SER_B_BASE`), which is
the established part. The Origin 300 IO-8 board test shows it mattering: its serial DMA
checks leave port B's ring DMA enabled, and its byte-wide loopback test then writes
`0x00038000` here — both port fields zero — sends a character through port B's 16550 and
waits for it there, failing with "CHECK CONNECTOR - serial port B line status register
(SIO_LSR) 2^0 never set" and its own `sec1_2.c line:431` when it does not arrive. The setup
routine is at `0x1001d650` in its code. Observed: a model that wired each engine to its
own port regardless took the character into the ring first (run
`prom-20260924T174128-c381c4`). The model now lets an engine touch its port only while
the field holds that port's offset, `0x2F` for A or `0x2E` for B, and does nothing with an
engine aimed elsewhere. What the part's engine reads or writes at another SuperIO offset
is unknown. *(field meaning established by the public header; the detachment inferred
from the diagnostic's expectation; behaviour at other offsets unknown)*

**What the firmware leaves there, read on this model.** The register reads `0x0047AE5E`
— stored `0x0007AE5E`, ports at `0x2F` and `0x2E`, strobe field `0xF`, plus the model's
arbiter-idle bit — after the IP27 PROM 6.156 reaches its maintenance menu on `origin200`
(run `prom-20260924T185925-962aa8`), after the IP35 PROM 6.210 and the sMDK reach
`smdk>` on `origin300` (run `prom-20260924T185634-4e6fce`), and with IRIX 6.5.30 at a
root shell on `origin300` and on `origin200` (runs `irix-20260924T190836-482b2c` and
`irix-20260924T191235-5ecf75`; on `origin200` port A's ring DMA was enabled, control
`0x10100004`). So both PROMs aim the console IOC3's engines before any operating system
runs, and gating the engines on these fields leaves that console working. That strobe value differs from the `0x00022E5E` this
section gives for the PROM without naming its source or revision; the
model stores either, and nothing it runs depends on the strobe. Mainline Linux's ring
serial driver for SN2 IOC3 cards writes the port fields itself before using DMA
(`drivers/serial/ioc3_serial.c` in Linux's history tree), which shows software expecting
to aim the engines, not what an Origin guest does. Still unchecked: an IOC3 other than
the console one, such as a second node's, whose firmware may leave the fields at their
reset zero. *(firmware values observed on this model; non-console IOC3s unknown)*

#### Normal interrupt-output contract

The normal output contract is public and dual-sourced. Mainline Linux revision
`db2ddb87143519e20a95aa36c60b36107b736a58` defines the register at offset
`0x2c`, the five valid mode encodings, the count conversion, the diagnostic and
output-state bits, and the GPIO route in
`arch/mips/include/asm/sn/ioc3.h`, lines 109–131 and 398–486. The same
definitions were already present in Linux's initial public git import
`1da177e4c3f41524e886b7f1b8a0c1fc7321cac2`. A public kernel-history copy also
records the definitions in
[Linux 2.3.48](https://ftp.riken.jp/Linux/kernel/v2.3/patch-html/patch-2.3.48/linux_include_asm-mips64_ioc3.h.html).
SGI's *IRIX Device Driver Programmer's Guide*, document 007-0911-180, May 2001,
for IRIX 6.5.12, chapter 6, pages 135–139, is available as
[document 007-0911-180](https://www.infania.net/misc1/sgi_techpubs/techpubs/007-0911-180.pdf).
It independently defines the external behavior: a fixed level, one pulse, a
pulse train or a square wave, asynchronous response to a PIO command, a 23.4
microsecond pulse, and termination of a running mode when software issues a new
command. For the guide's user-facing interface, pulse-train periods are truncated
to a multiple of 7.8 microseconds, while square-wave toggle intervals are truncated
to a multiple of 23.4 microseconds. The programmed square-wave interval separates
state changes, so a full cycle takes twice the interval.

| `INT_OUT[18:16]` | Normal-mode behavior |
|---|---|
| `000` | Drive the output low. |
| `100` | Drive the output high. |
| `101` | Emit one high pulse, then return low. |
| `110` | Emit repeated high pulses. |
| `111` | Toggle the output state at each interval. |
| `001`, `010`, `011` | Undefined. No behavior is part of this contract. |

Linux defines the hardware tick as 7.8 microseconds and a pulse as three ticks,
or 23.4 microseconds. Its minimum and maximum supported timed intervals are 3 and
65535 ticks. With the `(COUNT + 1)` conversion, the supported raw `COUNT` range is
`0x0002` through `0xfffe`. Public evidence does not define timed-mode behavior for
raw counts `0`, `1` or `0xffff`. A new valid command terminates any mode already
running.

Pin-control bit 20 enables the `INT_OUT` route to GPIO pin 0. SuperIO interrupt
status bit 27 names the real-time output-pulse source, and bit 28 names the
real-time input-pulse source. The published sources do not establish the precise
phase at which bit 27 posts or how either source acknowledges. They also do not
establish an instruction-cycle deadline for the asynchronous output transition,
the relationship among `INT_OUT` state, `GPDR`, `GPPR[0]`, the external connector
and any QEMU output line, or how an incoming asserted edge posts status bit 28.

#### Internal real-time interrupt loopback

The internal loopback is the output-pulse interrupt source at SuperIO status bit
27. It is independent of diagnostic-mode bit 30 and does not turn the output back
through the external-input source at bit 28. Enabling the local source arms bit
27 in the interrupt-enable mask. A strobe programs one-pulse mode, preserving the
interval count, and produces the normal 23.4-microsecond high pulse. Its rising
edge posts bit 27 locally while the signal also follows the fitted board's
real-time output path. No external cable is required.

An internal-only test disables bit 28 and enables bit 27 before issuing a strobe.
This matters because the compatibility library enables the external-input source
while setting up its receive counter. If both sources remain enabled and a cable
loops the output back to the same machine, one strobe can produce two separately
serviceable interrupts: bit 27 on the outgoing rising edge, then bit 28 on the
returning falling edge after one pulse width.

Both real-time sources use the IOC3 SuperIO interrupt output. On standard BaseIO
that is PCI interrupt pin D. The reviewed evidence does not identify the
numbered Bridge input to which this board wires pin D. The Bridge then delivers
the interrupt to the Hub and CPU selected by its programmed interrupt
destination. The IOC3 handler acknowledges both status bits through
the status register's write-one-to-clear operation, records the event for waiters,
and restores the selected enable mask. If either enabled status bit remains set,
the PCI interrupt remains asserted.

The hardware graph exposes an `ei` character child under the IOC3 PCI vertex. On
the standard Origin 2000 BaseIO population, `olrti -vertex` takes the parent
`/hw/module/<module>/slot/io1/baseio/pci/2` and opens its `ei` child. The complete
device vertex is therefore
`/hw/module/<module>/slot/io1/baseio/pci/2/ei`. The console-adjacent interface also
receives the stable alias `/hw/external_int/1`; additional interfaces are numbered
by system enumeration. Real-time support must be enabled for the IOC3 instance or
the child and alias are not created.

**A fixed level is visible to the very next read.** The Origin 300 IO-8 board test
writes `0x80040001` — output high — and at once reads the register 500 times, with no
delay between, requiring bit 31 set on each read; it then does the same for low. It
reports a miss as "IOC3 register error, INT_OUT reg (addr 0x2c) did not hold bit 2^31
(INT_OUT bit) to a constant high" with its own `sec1_7.c line:311`; the write and the
loop are at `0x10031da0` to `0x10031e18` in its code. Observed: a model that completed
every command on a timer one nanosecond after the write returned `0x00040001` once (run
`prom-20260924T171055-45763f`) and passed on another run (`prom-20260924T164032-c60b04`):
the next read and that timer race. So the guide's
"asynchronous response to a PIO command" is fast enough, for a fixed level, that the
next PIO read already sees it; the model applies fixed levels inside the write and keeps
a timed mode's deferred start. *(fixed-level timing inferred from the diagnostic's
expectation; timed-mode first-edge latency unknown)*

**A timed mode's level is the clock's at every read.** The same test writes `0x60005` —
periodic pulses, count 5, so a 46.8 µs period high for 23.4 µs — and at once reads the
register 500 times, requiring both levels among the reads ("should be pulsing but appears
to be stuck low" or "stuck high", `sec1_7.c line:420`; the loop is at `0x10032884` to
`0x1003299c`). On the shipped part the level is combinational on the chip's own clock.
Observed: a model that moved the level only when its timer callback ran saw only low (run
`prom-20260924T172547-2227c4`), because the guest's loop outran the host's servicing of
that timer. The model now runs every transition already due before answering a read, and
schedules each next transition from its due time, so a read reflects the virtual clock
whatever the host was doing. *(periodic sampling behaviour inferred from the
diagnostic's expectation; the read-time catch-up is an emulator mechanism, not a
hardware claim)*

**Any rising edge of the output posts bit 27, not only a one-shot's.** The Origin 300
IO-8 board test masks every SuperIO source through the enable-clear address, clears the
status, commands the output high (`0x40005`), waits, commands it low (`0x5`) and requires
bit 27 set, reporting a mismatch as "SuperIO interrupt register (SIO_IR) of the IOC3 should
have RT_INT (bit 2^27) set after RTO was pulsing" with its own `sec1_7.c line:356`; the
sequence is read from its code at `0x10032338` to `0x100323c0`. Observed: a model that
posted the bit only for a one-shot answered zero (run `prom-20260924T164032-c60b04`). So a
fixed-high command's edge posts it, and the bit posts with the source masked, as the
one-shot's already does. The test reads the status only after both the rise and the
fall, so it cannot tell which edge posts; the model posts on the rise, as the one-shot
contract establishes for its pulse, and on every rise whatever the mode — fixed high,
one-shot, each periodic pulse and each square-wave rise — and on no fall. That extension
changes how often an enabled source interrupts a guest running a periodic or square wave;
a guest that enables bit 27 with a periodic output and counts its interrupts against the
pulses would tell. *(fixed-high posting inferred from the diagnostic's expectation, the
edge by the one-shot contract; periodic and square-wave posting inferred by extension;
the edge's exact phase unknown, as the normal interrupt-output contract says)*

For QEMU, reset clears the interrupt-output register, both real-time status bits,
their enable bits, and the GPIO output route, leaves the modeled output low, and
cancels a pending timed transition. There is no separate loopback latch to reset:
the selection is bit 27's interrupt-enable state. Identical-build migration must
preserve the output mode, level and remaining timer phase, status bits 27 and 28,
their enables, the GPIO route, and the receive-event count. Restore reschedules a
pending transition and recomputes the PCI interrupt level so a pulse is neither
lost nor duplicated. These are emulator lifecycle requirements; the reviewed
evidence does not establish IOC3's hardware reset state.

Public Linux names bit 30 as diagnostic mode but does not define it. Nothing in
the internal-loopback sequence sets bit 30, so its behavior remains outside this
contract.

**A write is stored as written, including a command this contract gives no
behavior.** The Origin 300 IO-8 board test writes `0x4007ffff` — diagnostic mode,
square wave, count `0xffff` — reads the register back under the same mask and requires
`0x4007ffff`, reporting a mismatch as "IOC3 register error, INT_OUT reg (addr 0x2c)
could not hold all bits high" with its own `sec1_7.c line:221`; the written value and
the mask are both constants in its code at `0x10031678` to `0x100316b8`. Observed: a
model that refused such a write outright read back zero (run
`prom-20260924T162956-e30120`). So bits 0–18 and 30 hold whatever software writes, and
bit 31 stays the output state, which the test masks away. What the output does after a
diagnostic-mode write, an undefined mode or an unsupported count is still outside this
contract: the model stores the value and leaves the running output alone, a choice
nothing observed contradicts. *(storage of that one value inferred from the diagnostic's
expectation; storage of every other undefined command inferred by extension; behavior
unknown)*

### GPIO and MicroLAN

| Offset | Name | Width | Reset | R/W | Behavior |
|---|---|---|---|---|---|
| `0x30` | MicroLAN control | 32 | `0` | RW | One-wire bus engine: a write starts a timed bus slot, polling reports its result. |
| `0x34` | Pin control, set | 32 | `0` | W1S | Writing a one sets that control bit. |
| `0x38` | Pin control, clear | 32 | `0` | W1C | Writing a one clears that control bit. |
| `0x3C` | Pin data | 32 | `0` | RW | Nine meaningful bits, one per pin. SGI's diagnostic confirms the width by testing only bits 0–8. |
| `0x40` + 4n | Pin register n | 32 | `0` | RW | One register per pin, used to drive or read a single pin without a read-modify-write of the data register. The pin value rides in the register's low bit and mirrors bit n of the pin-data register at `0x3C`. Mainline Linux reserves the whole span as sixteen pin registers, `0x40`–`0x7C` (`gppr[16]` in `arch/mips/include/asm/sn/ioc3.h`, v7.2-rc7), which is why the parallel-port block begins at `0x80`; on Origin base I/O only pins 0–8 are meaningful. |

In the pin control register bits 0–7 set each pin's direction, with a one meaning
output. Bits 16–19 choose edge or level sensitivity for the pins that can raise
external interrupts. Bit 20 routes the interrupt-output block to pin 0. Bit 21 routes
the MicroLAN engine to pin 8, and nothing on the one-wire bus works until it is set.

Three pins have fixed meanings on Origin base I/O: pin 5 resets the ethernet PHY, and
pins 6 and 7 select the electrical mode of the two serial ports, choosing between
RS-232 and RS-422 signalling. Mainline Linux's register header names all three —
`GPPR_PHY_RESET_PIN` 5, `GPPR_UARTB_MODESEL_PIN` 6, `GPPR_UARTA_MODESEL_PIN` 7
(`arch/mips/include/asm/sn/ioc3.h`, v7.2-rc7) — so **pin 7 selects port A and pin 6 selects
port B**, agreeing with the IRIX header; only OpenBSD's header dissents. Linux's serial setup
drives pins 6 and 7 as outputs and writes RS-232 to both — `ioc3_serial_setup` in
`drivers/mfd/ioc3.c` sets the two mode-select direction bits and writes zero to `gppr[6]`
and `gppr[7]`; pin 5's direction is set by the ethernet driver, not here. That assignment is settled among the questions this
datasheet records as decided rather than open, and nothing observable depends on it, because
every driver programs both pins identically and to the same value.

**The MicroLAN control register's bit layout is named by no header, but it is
dual-sourced.** The IRIX header gives the register an offset and a name and stops. Two
unrelated implementations drive it directly and agree on every field: OpenBSD's
bit-banging routine, and mainline Linux's dedicated one-wire bus master driver, which
serves both this register and the Bridge's board-identity line. Agreement between them is
what closes this, so the layout below is documented rather than inferred.

A one-wire operation is a single write followed by polling. The write places the time
to hold the bus low in bits 10 and up, and the remainder of the time slot in bits 2
and up, both in microseconds. Hardware then performs the slot and sets bit 1 when it
is finished; bit 0 carries the level sampled from the bus. The timings map cleanly onto
the one-wire standard, and the two guests differ only within the standard's tolerances:
OpenBSD uses 500 microseconds low with a 65 microsecond remainder for a reset and
presence detect where Linux uses 520 with 65; both use 6 with 13 for a read slot; both use
80 with 30 to write a zero; and OpenBSD uses 6 with 110 to write a one where Linux reuses
its 6-and-13 read-slot timing. A model that performs the slot rather than pattern-matching
the exact microsecond values satisfies both.

Nothing on the bus works until the MicroLAN enable bit in the pin control register is set,
which both guests do as their first act on the chip.

### What is actually on the one-wire bus, from thirty-one real machines

The chip's MicroLAN can carry either of two kinds of NIC, and OpenBSD has a driver
for each — but **on real Origin 200 hardware there is exactly one part per bus and it is the
Ethernet-address NIC.** Across thirty-one captured boots covering forty controllers:
**forty one-wire buses, forty Ethernet-address NICs, one per bus, and no board-identity NIC
anywhere.** That is observation rather than inference, and it settles a question this section
previously answered from driver tables alone.

| Part | Family byte | What it holds | OpenBSD driver | Seen on real hardware |
|---|---|---|---|---|
| Ethernet-address memory, the UniqueWare custom DS2501 | **`0x91`** — the custom-part bit `0x80` over type `0x11` | The board's ethernet address | `owmac` | **40 of 40** |
| DS1982, or the compatible DS1981 and DS2502 | `0x09` | Same role, catalogue part | `owmac` | never |
| DS2505, or DS2506 | `0x0B`, or `0x0F` | Board part number and serial number | `owserial` | **never** |

**The real part is not the catalogue DS1982, and the family byte is worth getting right.**
OpenBSD splits that byte into a custom flag in the top bit and a seven-bit type below it, so
the `0x11` a boot log prints is the *type* and the whole byte is `0x91`. Its driver matches
such a part only when the custom bit is set, the byte is `0x91`, and the serial number's top
twelve bits are `0x5e7` — a manufacturer code. A plain `0x09` part would also attach, by the
other arm of the same test, so either would satisfy the guest; only `0x91` matches what the
hardware does.

**The absent part is the more useful finding.** OpenBSD reads a board part number from an
`owserial` part to decide which subdevices exist, and falls back to identifying the chip by
its PCI device number when nothing answers. No Origin 200 in any capture carries that part,
so **real hardware always takes the fallback path**. A silent bus would reach the same
identification branch, but it would fail the separate Ethernet-address requirement.

SGI's board block diagram draws two NICs on a BaseIO board, but **they are not both
on this chip's bus** — one connects to the IOC3 and the other to the Bridge, which has its
own board-identity line. The IP27 PROM manual's mention of a base-I/O card carrying two of
them is Origin 2000 scope, not Origin 200: that manual describes the two machines in
alternating sections without marking which, and "card" is the Origin 2000's word for a
separate board where the Origin 200 uses its IP29 System Board. IRIX's own driver walking past
several parts in search of a readable one is the driver being permissive about what it might
meet, not a description of what is fitted on every board.

**Which physical bus IOC3's own board-population dispatch reads is now settled for Origin
2000's BaseIO6G, by live GDB against a running guest rather than inference.** A breakpoint on
the IOC3 attach routine's call into that board-population NIC walk read the live arguments the
kernel passes in: a real MMIO address, `0x8600030` in Bridge's PCI device window —
this chip's own MicroLAN control register (`0x30` above), not Bridge's separate one-wire bus. A
passive `sgi_ioc3_onewire_slot`/`sgi_bridge_onewire_slot` trace confirmed it independently: the
whole walk's guest-visible one-wire traffic lands entirely on this chip's bus, none on
Bridge's, both before and after adding the record it reads. BaseIO6G therefore does carry two
one-wire parts on two different buses exactly as the board diagram draws, and a branch
modelled both: the existing Ethernet-address NIC here, and a second, independent manufacturing-
record part alongside it (an `mfg-part` property on the QEMU branch
`fix-baseio6g-keyboard-mouse-20260917`, which never landed on `sgi-origami`; trunk's
`sgi-ioc3` has no such property). Trunk has a general mechanism instead: since QEMU
`dee9dc8992`, the `board-part`, `board-barcode`, `board-revision`, `board-name` and
`board-onewire-serial` properties put a second family-`0x0b` part holding a fixed-offset
board record on the chip's own one-wire bus, beside the station-address part. Unset, the bus
carries the station-address part alone. No BaseIO-G preset sets them. This does
not reopen the Origin 200 finding above — no Origin 200 capture carries a second part, and
none is added to that board's own onboard IOC3 (`sgi-ip29-io`) here; the new part is scoped to
BaseIO6G specifically.

**What this settles and what it does not.** IOC3's board-population NIC walk reads a
correctly-formatted `"030-1155-"` record and this project's own qtest coverage confirms the
record round-trips correctly over the wire protocol. That branch carried the
device-2 manufacturing-record part but left it disabled by default (`mfg-part` unset for
every board): populating it is what first lets the IOC3 keyboard-mouse child driver's attach
code run on this machine, and doing so exposes a real, separate, deterministic guest hang
(confirmed by a controlled comparison — reverting only the manufacturing-record commit restores
a clean boot to login on the same host, same disk, same command line). Live GDB traced the
stall into an IOC3 interrupt-vector register write deep in that attach path, alongside heavy
guest-error activity across `hw/misc/sgi-hub.c`'s largely unimplemented I/O-interface register
block (`docs/datasheets/hub-io-interface.md`'s `0x40_0100` onward -- local access protection,
widget access, interrupt destination) and repeated SCSI-controller reinitialization, consistent
with the whole BaseIO board being released and rebuilt in a loop. The exact triggering condition
inside that chain is not yet isolated.

Keyboard and mouse do not depend on closing that hang. On Origin 2000's BaseIO6G they are
delivered instead by the MediaIO daughtercard's own second IOC3 at PCI device 6, which
enumerates as a separate board-identity part and brings up the keyboard, mouse, two extra serial
ports and the parallel port on a live IRIX boot (see
[the BaseIO datasheet's per-IOC3 enable result](baseio.md#how-irix-enables-each-ioc3s-functions)).
The dormant device-2 record path is a deferred alternative, not a blocker for that
functionality; closing its hang needs further live-GDB work against the stall, most likely
landing part of the Hub I/O-interface register block first -- a separate, larger unit of work
from the one-wire record itself. QEMU `a9d12cf8d2` has since made the Hub store its local
access protection, widget access and interrupt destination registers (without enforcing
them), so whether the hang survives is untested. jamesb/origami#2016 tracks re-adding the
record through the `board-*` properties on the device-2 IOC3 and booting IRIX against it.

### On Origin 200 system boards the emulated bus must answer, with exactly one part

**A silent bus is wrong, and the earlier decision to leave it silent rested on a claim about
the firmware that does not hold.** That decision said the ethernet address would come from
our synthesized firmware instead. **OpenBSD has no firmware path for it on this machine.**
Its driver takes the station address from an `owmac` part when one is present; when none is,
the only other source is a system-controller query that is guarded to the later machine
family, so on this one the guard short-circuits and the address is set to all ones. The
ethernet driver then attaches and prints `ff:ff:ff:ff:ff:ff` on the console. **Nothing
errors** — a plausible-looking boot with a broken address and no diagnostic naming the cause.

So the contract has two halves and both are requirements:

- **The bus must carry a readable Ethernet-address NIC.** Without it the guest boots with an
  all-ones station address.
- **The bus must not carry a board-identity NIC on these boards.** With one, OpenBSD reads a
  part number, finds no entry for it in its table of known boards, prints that the chip is of
  unknown flavour and returns **without attaching anything at all** — no console, no ethernet.
  Its table has no Origin 200 entry, so any serial part we invented would land there. The
  table does name add-in boards, the MENET, the Cad Duo (`030-1155-`) and the dual-serial
  board, and a real Cad Duo carries its record on this bus beside the Ethernet-address part
  ([Cad Duo datasheet](sgi-cad-duo-pci-card.md#board-identity-record-on-the-ioc3-one-wire-bus)).
  The model fits that second part only there, through the `board-*` properties.

That pairing is exactly what the thirty-one captures show: one part on every bus, the
Ethernet-address NIC, and no board-identity NIC on any machine. **The machine that boots is
the one that matches the hardware.** The current model therefore supersedes rather than
reinterprets the silent-bus decision.

Present the part as the UniqueWare custom family byte `0x91` with the `0x5e7` serial prefix,
matching what real hardware reports, rather than the catalogue `0x09` part. Both attach, so
this costs nothing and removes a difference from real machines for no benefit.

**The Bridge's own one-wire is the opposite obligation and is not this bus.** That one must
answer with a board part number a current Linux recognises, or Linux does not attach the
Bridge at all. Same chip family, opposite contract, adjacent in the machine — which is
precisely the pair that gets built symmetrically by reflex. It belongs to the Bridge
datasheet, not this one.

### The Ethernet-address NIC's read protocol

**Reviewed** (codex, two rounds — 2026-08-10 and 2026-08-12, the second after the section was
briefly lost from the shared working tree and recovered verbatim; see
the gate reconciliation (citation: `docs/reviews/ioc3-onewire-ethernet-address-gate.md`) for both rounds'
findings and their fixes) — **the device model this section specifies is implemented and
committed** (`hw/misc/sgi-ioc3.c`'s one-wire state machine, `hw/net/sgi-ioc3-eth.c`'s child
device) and confirmed working against a real OpenBSD boot.

The contract above says the bus must carry a readable Ethernet-address NIC; this section is
what "readable" requires at the wire level. It comes from the two guest drivers that actually
read the part rather than from a vendor register header for it: none of the vendor material in
our holdings describes this part's own read protocol, only the chip in front of it (SGI's
`sys/PCI/ioc3.h`, cited above, documents the MicroLAN control register a guest drives, not what
answers behind it).

**Selecting the part.** Both guests' one-wire frameworks begin with a search: OpenBSD's
generic `onewire(4)` subsystem (`sys/dev/onewire/onewire.c`) and the Linux `w1` core each
issue the Search ROM command, `0xf0`, to discover which sixty-four-bit ROM identifiers answer
on the bus — the same command byte and bit-triplet protocol the Bridge's board-identity line
already implements, reused unchanged here. With exactly one part answering, every triplet
resolves the same way regardless of which choice the offered bit encodes, exactly as the
Bridge's own model already treats its single part.

Once selected, the two guests address the part two different ways for the rest of a
transaction, and the model must answer both. Linux's `w1` core selects a device with a Skip
ROM (`0xcc`) whenever only one is registered on the bus (`w1_reset_select_slave()`,
`drivers/w1/w1_io.c`) — which is always true here — so Linux never sends a Match ROM to this
part. OpenBSD's `owmac` driver (`sys/arch/sgi/dev/owmac.c`) always addresses the part it found
by a Match ROM (`0x55`) followed by the eight bytes of the ROM identifier it discovered during
the search, family byte first, never a Skip ROM. **The model must decode a Match ROM
correctly, not just tolerate it**: it is a real, load-bearing part of the only guest that boots
this machine today, not a fallback path. A Match ROM that carries a different sixty-four-bit
value must leave the part unselected — real one-wire semantics for the general case, though on
our single-part bus a mismatch only happens if a guest's own arithmetic is wrong.

**The part's identity.** Family byte `0x91` — the custom-part bit `0x80` over type `0x11` —
with the serial number's top twelve bits `0x5e7`: both are required, and both are what OpenBSD's
`owmac_match()` actually tests (`sys/dev/onewire/onewirereg.h`'s `ONEWIRE_ROM_FAMILY_CUSTOM`,
`ONEWIRE_ROM_FAMILY` and `ONEWIRE_ROM_SN` macros against `owmac.c`'s comparison), not merely
described above; that these specific values match real hardware is the existing "What is
actually on the one-wire bus" finding above, from the thirty-one-capture survey, not something
this section re-establishes. Type `0x11` is the DS2501, a 512-bit part, so `0x91` is DS2501-class
by memory size while answering the DS2502 command set: Dallas's UniqueWare variants are the
DS2501-UNW at `0x91` and the DS2502-UNW at `0x89`, and mainline Linux sizes `0x91` at 64 bytes
against the DS2502 families' 128 while reading `0x09`, `0x89` and `0x91` through one
`w1_ds2502_read_page()` (`drivers/w1/slaves/w1_ds250x.c`). Only the serial number's **remaining**
thirty-six bits, below that required twelve-bit prefix, are unconstrained by either guest
driver. **The model's choice for those thirty-six bits, and the ROM CRC-8 that follows from
the complete value, are ours to make** — the same way the Bridge's own placeholder serial bytes
are a choice — not a transcription of a real unit's per-board serial number, which neither guest
checks beyond that twelve-bit prefix.

**Reading the page that holds the address.** The two guests issue two different command bytes
for what is functionally the same operation. OpenBSD's `owmac_read_page()` issues the plain
Read Memory command, `0xf0` — the same byte value as Search ROM, safe to reuse because it only
means "read memory" once a part is already selected, a different phase of the protocol from
where Search ROM is recognised. Linux's `w1_ds2502_read_page()`
(`drivers/w1/slaves/w1_ds250x.c`), which family `0x91` shares with the catalogue DS2502 and its
`0x89` UniqueWare variant, issues the Read Memory-with-CRC command, `0xc3`, instead. Both are
followed by a two-byte target address, low byte then high byte.

**What the two commands answer with is fully specified for the leading CRC-8, and only a
choice for the trailing one.** Linux's `w1_ds2502_read_page()` is explicit about `0xc3`: a byte
carrying the CRC-8 of the three bytes just sent (the command and the two address bytes), then
the thirty-two bytes of page data, then a byte carrying the CRC-8 of those thirty-two bytes —
and it computes both itself and abandons the page if either fails to match what it read.
OpenBSD's `owmac_read_page()` establishes far less about `0xf0`'s answer: after sending the
command and the two address bytes it reads one more byte and discards it — its own comment
says `/* XXX should verify this crc value */`, so it names the byte a CRC without confirming
what it covers — then reads the thirty-two data bytes and resets the bus without reading
anything further.

**IRIX settles what OpenBSD leaves open, for the leading byte.** Its own one-wire
ethernet-address routine, in the standalone firmware's one-wire layer with a matching copy in
the kernel, sends the plain
`0xf0` with a zero-value two-byte address and requires the very next byte to equal `0x8d` — and
the standard Dallas/Maxim CRC-8 this section specifies below, run over the three bytes
`{0xf0, 0x00, 0x00}` IRIX actually sends, computes to exactly `0x8d`. That is specific to the
command IRIX sends, not a value that would turn up regardless, and SGI's own diagnostic for
this same check calls the byte a command CRC rather than anything else. **The leading CRC-8
after `0xf0` is therefore observed, not chosen** — the same status Linux's fully-specified
`0xc3` framing already gives its own leading CRC-8. **The trailing CRC-8 after `0xf0`'s thirty-
two data bytes stays a choice**: IRIX reads exactly fourteen bytes for this call — the
leading CRC-8, then thirteen bytes of data — and never reaches where a trailing byte would
sit after thirty-two, so no guest here confirms or rules one out for that command.

The published part specification settles it, and the two commands are **not** framed alike.
**Plain Read Memory, `0xf0`, carries no per-page CRC**: the leading CRC-8 over the command and
its two address bytes, then data from the requested address onward, closed by a single CRC-8
only if the read runs to the end of memory. **Read Memory-with-CRC, `0xc3`, is the variant that
appends a CRC-8 at the end of every thirty-two-byte page**, which is what Linux's
`w1_ds2502_read_page()` describes. The two share only the leading command-and-address CRC-8.
The model follows that split. It previously answered both commands alike, appending a trailing
page CRC-8 on the `0xf0` path as well; that was a fidelity fault against the published
specification, harmless only because neither guest here clocks the surplus byte, and it has
been corrected.

A claim in the earlier text does not survive either: it said the model must produce a correct
trailing CRC-8 after `0xf0` because Linux is the only guest that checks it. Linux does not
issue `0xf0` for this part at all — it issues `0xc3`, as this same section establishes — so
there was no guest checking a trailing byte on that command.

**Reading the redirection table.** Only OpenBSD reads this. Before it trusts a page, `owmac`
calls `owmac_read_redirect()`, which issues the Read Status command, `0xaa`, followed by a
two-byte address (`owmac` always asks for offset `0x0001`), and reads back the same shape of
answer: after sending the command and the two address bytes it reads one more byte, names it a
CRC by the same unverified comment, and discards it without establishing what it covers either
— then reads four bytes it stores as a per-page redirection table. `owmac_read_page()` runs
those four bytes through
`owmem_redirect()` (`sys/arch/sgi/dev/owmem_subr.c`) purely as a validity gate on whether the
read proceeds at all — **not** as a source for the address actually read, which
`owmac_read_page` recomputes from the page number it was originally asked for regardless of
what redirection said. `owmem_redirect()`'s loop stops and succeeds the moment the entry it is
looking at is `0xff`; it fails with `-1` (which `owmac_read_page` turns into `EINVAL`) only
after `EEPROM_NPAGES` redirections without landing on one, or when a redirection points outside
the table — it is bounded both ways, not an unbounded loop. **The model serves the real
eight-byte status field**: a write-protect byte at `0x0000`, a page-redirection byte for each
of the four pages at `0x0001` through `0x0004`, two reserved bytes, and a factory-programmed
`0x00` at `0x0007`, closed by a CRC-8 over the bytes read.

The byte OpenBSD reads at `0x0001` is still `0xff`, and that is what lands on the loop's
immediate-success case — but it now arrives for the right reason. This is an add-only part
whose unprogrammed state is all ones, and a redirection byte of `0xff` is the part's own way of
saying the page is not redirected. The model previously short-circuited that offset to four
`0xff` bytes deliberately, to stop the walk; serving blank status memory produces the same byte
without a coded special case standing in for part behaviour. A test pins the guest-visible
value at that offset, because a value that now falls out of initialisation is easier to break
silently than a constant was.

**What page zero holds.** Both guests fetch the same thirty-two-byte page at offset zero over
the wire, and neither ever asks for any other page — but they consume different slices of it.
OpenBSD's `owmac_read_page()` reads the full thirty-two bytes and its `owmac_read_mac()`
examines only bytes 0 and 5 through 10 of them. Linux's one-wire layer also caches the full
thirty-two-byte page (`w1_ds2502_read_page()`), but `ioc3eth_get_mac_addr()` asks its nvmem
layer for only the first sixteen of those bytes and examines bytes 0 through 12. So the two
guests agree on what the meaningful bytes contain and where they sit, not on reading or
decoding the page as a whole:

| Bytes | Content |
|---|---|
| 0 | A fixed marker, `0x0a`. Both `owmac_read_mac()` and Linux's `ioc3eth_get_mac_addr()` refuse the page if this byte is anything else. Neither source we hold explains what the value means; the model reproduces it without interpreting it. |
| 1–4 | Unconstrained by either guest. The model's choice, held at zero. |
| 5–10 | The six-byte station address, stored **reversed**: byte 10 is the first octet a guest reports, byte 5 the last. `owmac_read_mac()` builds `sc_enaddr[0..5]` from `buf[10..5]` in that order; Linux's `ioc3eth_get_mac_addr()` builds `mac_addr[i]` from `prom[10-i]` for `i` in `0..5`. The two guests agree on the order independently. |
| 11–12 | An embedded CRC-16 over bytes 0 through 10, stored inverted and low byte first — the same convention the Bridge's identity page already uses for its own embedded check — so that a CRC-16 run across all thirteen bytes lands on the fixed residue `0xb001`. **Only Linux checks this.** `ioc3eth_get_mac_addr()` computes `crc16(CRC16_INIT, prom, 13)` and requires exactly that residue before it will trust the address; OpenBSD's `owmac_read_mac()` never computes a CRC-16 over the page at all. This is the same "two CRC-16s, checked by different code" shape the Bridge's own identity page already carries, and the same consequence follows: a model that gets the framing CRC-8 right but leaves this embedded CRC-16 wrong, or absent, satisfies OpenBSD and fails Linux silently. |
| 13–31 | Unconstrained by either guest. The model's choice, held at zero. |

**What address to answer with.** The address behind this part must be the same address the
machine's ethernet DMA engine and its network backend were configured with —
`s->eth.conf.macaddr` in the fork's `SGIIOC3EthState`, the ethernet block's own child device
(`hw/net/sgi-ioc3-eth.c`) — not an independent constant. A guest reads this part once, at
attach, and then writes what it read into the MAC address registers the engine itself answers
from; if the two addresses disagree, the engine ends up programmed with whatever the guest read
here rather than with the address the host-side backend expects frames addressed to, which
defeats the point of answering at all. The current model derives the responder's contents from
that configured address. Before the responder landed, the silent bus made OpenBSD assign and
print `ff:ff:ff:ff:ff:ff`, a broadcast address invalid as a unicast source.

**Origin 300 separates the diagnostic from the netboot address source.** Its
IOC3 has a MicroLAN master, but the reviewed IO8 machine population does not
fit an IOC3 station-address responder, and the real IO8 population remains
unmeasured. The loaded BASEIO PROM's Ethernet-address diagnostic at
`0xc0000000130bfbac` passes the mapped IOC3 base plus `0x30` (MicroLAN)
and `0x34` (GPIO) to its one-wire reader. That reader accepts several ROM
families. Its custom DS2501 `0x91` branch uses the `0xf0 00 00` packet and CRC
checks described above. Diagnostic failure returns status `0x7f`. This software probe does
not establish that a part is fitted to Origin 300 or that production `ef`
uses it.

The production `ef` path instead takes the station address as twelve ASCII
hexadecimal characters from the IO8 board-information area through L1 request
`0x0006`, component 1, area 2. The [L1 contract](l1-controller.md#origin-300-io8-ethernet-identity)
owns that record format and the no-`setenv` sash runs. Public OpenBSD IP35
`ioc.c` likewise calls `l1_get_brick_ethernet_address` when no IOC3 one-wire
address child attaches; an IOC3 one-wire bus probe is not proof of a fitted
part. The board record's address and the IOC3 Ethernet device's configured
`mac` property must agree, including an explicit operator-supplied address.
Netboot success with a corrected L1 record is not evidence of a widget-15
Ethernet part.

The related boards have different populations. Origin 200 has a custom
DS2501 station-address part behind IOC3 MicroLAN and a separate Bridge
identity connection. SGI's published Origin 2000 BaseIO diagram also draws
separate IOC3 and Bridge NICs without identifying their family bytes or page
contents. Neither wiring transfers to Origin 300. Keep its unsupported IOC3
responder explicit until an identified IO8 capture or schematic settles that
bus. A QEMU register test establishes only what the model answers.

**The CRC-8.** Standard Dallas/Maxim one-wire eight-bit CRC: polynomial `x^8+x^5+x^4+1`, whose
reflected form used bit-serially is `0x8c`, seeded at zero, one byte in at a time, transmitted
as computed rather than inverted — confirmed against both guests' own lookup tables
(`refs/openbsd-sgi`'s `crc8_table` in `sys/dev/onewire/onewire_subr.c` and `refs/linux`'s
`w1_crc8_table` in `drivers/w1/w1_io.c`), which are identical table for table. This is a public
one-wire-standard algorithm, not vendor-specific code, and the model computes it bit-serially
rather than reproducing either table. It is a different width and a different transmission
convention from the CRC-16 the Bridge's identity page and this page's embedded check both use —
the CRC-16 is sent inverted and this CRC-8 is not — and the two must not be confused for each
other when either part of this chip is extended.

**Register mechanics.** This device answers through the same MicroLAN control register and the
same bit-slot protocol described above under "GPIO and MicroLAN" — timing fields packed the
same way, a slot completed synchronously inside the write that starts it. Nothing about the
register changes for this part; only what answers behind it does.

**IRIX reads this part too, through a layer neither the IOC3 driver nor the ethernet driver
owns.** It is not in the I/O controller's own driver, nor in that driver's ethernet sibling —
a search of both for the usual station-address and one-wire naming patterns returns nothing,
which is what sent an earlier pass of this section looking no further and recording the
question as open. The routine that does it is the ethernet-address reader in the standalone
firmware's one-wire layer, reached through a wrapper that enables the bus pin first, with a
matching copy in the kernel — present and
identical at IRIX 6.5.7, at 6.5.5 in both the f and m trees, and in the 655f rebuild. IRIX is
not a guest this task's own milestone gates on, but the milestone that boots IRIX does depend
on this part answering correctly, so the framing this section specifies for it is not a detail
that can wait.

### Ethernet

The ethernet block begins at `0xF0` and runs to `0x150`, and its buffer RAM is
exposed in the 256 KB window at `0x40000`.

**The MAC is SGI's own design.** It is not a SEEQ part, and notes elsewhere in this
project that described it as SEEQ-derived were wrong. No IOC3 driver in any reference
tree — IRIX, Linux or OpenBSD — mentions SEEQ, the register set and descriptor
formats match no catalogue part, and OpenBSD's driver states that it derives from the
driver for the MACE ethernet in the O2. The SEEQ 8003 belongs to the Indy and Indigo2,
where it sits behind the HPC bus and is driven by an entirely different driver. Anyone
looking for a part datasheet to fill the ethernet gap will not find one, because there
is no part. The register set in this datasheet is the specification.

| Offset | Name | Reset | R/W | Behavior |
|---|---|---|---|---|
| `0xF0` | MAC control | `0` | RW | Bit 0 selects full duplex, bit 1 promiscuous receive, bit 2 automatic pad generation. Bits 3–8 set the offset at which received frames start inside their buffer, **in sixteen-bit halfwords rather than bytes** — SGI's own driver comments the field this way when it programs it (`sys/PCI/ioc3.h`'s `EMCR_RXOFF_SHIFT` and `bsd/mips/if_ef.c`'s `ssram_discovery`-adjacent attach code, IRIX 6.5.7), and the value it writes, 33 halfwords, is exactly the sixty-six-byte offset its own receive-buffer layout needs: eight bytes for the two header words plus its own alignment padding. A byte-offset reading of this field would be off by a factor of two. **The field is genuinely software-chosen, not fixed at 33**: the base-I/O PROM's own netboot driver programs the same field to 17 halfwords instead, matching its own smaller receive-buffer layout — see [PROM netboot driver expectations](#prom-netboot-driver-expectations-ip27-and-ip35-prom). **Bit 9 enables buffer-RAM parity checking and is set in ordinary operation**; bit 11 reports that bad parity was seen. Bit 12 selects the larger receive buffer size. Bits 13–16 enable, in order, transmit DMA, transmit, receive DMA and receive. Bit 17 selects internal loopback. Bits 18–21 are arbiter diagnostics. Bit 31 resets the block. |
| `0xF4` | Interrupt status | `0` | RW — **written to acknowledge** | One bit per event, receive sources in the low bits and transmit sources in the high bits. Every driver reads it and writes the asserted bits straight back; OpenBSD masks with the enable register first, while IRIX and Linux write back the whole status word they read. **This is the same contract as the SuperIO interrupt register at `0x1C`, and modelling it read-only produces the same failure on the ethernet line.** **No engine owns this register**, and what that forbids is set out in the behavioral contract's "The interrupt status register belongs to no one engine". |
| `0xF8` | Interrupt enable | `0` | RW | Same bit assignment as the status register. |
| `0xFC` | Receive control and status | `0` | RW | Bits 0–8 set the receive interrupt threshold, which is **a low watermark on buffers still available to the engine, counted downward** — not a count of frames delivered. Linux calls it that in as many words and sets it to 15 against the 64 buffers it supplies, asking to hear when its supply runs low. OpenBSD sets it one below the buffers it supplies, which puts the watermark exactly where the first delivered frame lands. Both are the same register meaning something different to each driver, so **a model that raises this source once per frame regardless of the value programmed here satisfies OpenBSD by accident and misreports for any other watermark.** **The comparison is at-or-below rather than strictly-below**, which the behavioral contract's "Receiving, in order" derives from OpenBSD's own wording and chosen value. Bits 30 and 31 are simulation aids. |
| `0x100` | Receive ring base, high | `0` | RW | Upper address bits. |
| `0x104` | Receive ring base, low | `0` | RW | Bits 12–31 carry the base; the ring is 4 KB aligned. |
| `0x108` | Receive barrier | `0` | RW | Bits 16–31 and bit 8 are documented as a barrier marking the last entry hardware may write. **No driver advances it** — of the three, two write a constant once at initialisation, SGI's and mainline Linux's arriving at the same value by different routes, and the third writes zero. A model that stores the value and acts on nothing is correct. |
| `0x10C` | Receive consume index | `0` | RW | 12 bits, **advanced by hardware**. Software writes it once, to zero, at reset, in all three drivers; OpenBSD reads it and divides by the entry size to count received packets. **Holds a byte offset, not an entry number** — the entry is shifted left by three. |
| `0x110` | Receive produce index | `0` | RW | 12 bits, **advanced by software** as it supplies buffers. Byte offset, as above. Bit 31 arms the receive timer and is **independent of the write** — Linux and OpenBSD happen to set it on every write, SGI's driver writes the index without it in the interrupt path and writes a plain zero at reset. A model must accept the index with bit 31 clear and must not couple arming to the act of writing. **Arming is what puts the receive timer in service**, so a model that stores this bit without connecting it to the timer at `0x114` leaves the source every driver relies on unable to fire. |
| `0x114` | Receive timer | `0` | RW | 11-bit idle timeout, counting receive idleness before the receive-timer interrupt source is raised. **Zero is the value that matters, because it is the only one either driver writes**, and both name its meaning themselves: OpenBSD's comment is "Set up RX timer to interrupt immediately upon reception", Linux's is "Interrupt immediately". Armed through bit 31 of the produce index at `0x110` and loaded with zero, this register **is how a delivered frame prompts a driver** — the timeout never has to elapse because there is none. A model may implement the whole register as "raise the receive-timer source on delivery whenever the timer is armed and its reload is zero" and satisfy both drivers. What it may not do is treat this as storage, because then nothing raises that source at all. |
| `0x118` | Transmit control and status | `0` | RW | Three seven-bit interframe gap fields at bits 0, 8 and 16. Bit 31 reports that no transmit clock is present, which is how a driver notices an absent or unpowered PHY. |
| `0x11C` | Random seed | `0` | RW | Seeds the collision backoff. |
| `0x120` | Collision and deferral counters | `0` | RO, **clear on read** | Collisions in bits 0–15, deferrals in bits 16–31. Both guests read it once during reset purely to zero it. |
| `0x124` | Buffer index | `0` | RO | Internal produce and consume positions for the transmit and receive buffer RAM, in four fields at bits 0, 8, 14 and 23. |
| `0x128` | Transmit ring base, high | `0` | RW | Upper address bits. |
| `0x12C` | Transmit ring base, low | `0` | RW | Bit 0 selects ring length: clear means 128 descriptors, set means 512. Bits 14–31 carry the base, so **the register accepts a 16 KB-aligned base**. The rule is that **a ring is aligned to its own size** — 16 KB at 128 descriptors, 64 KB at 512 — and SGI's own ethernet driver states it outright in those terms rather than leaving it to be inferred. Mainline Linux aligns to 16 KB for the 128-descriptor ring it uses; IRIX aligns to 64 KB because it uses the 512-entry ring, which is where the over-general "drivers align to 64 KB" reading came from. The 64 KB alignment constant in SGI's header is real and is the 512-entry case, not a contradiction. A model that rejects a 16 KB-aligned base is rejecting something both the hardware and a real driver accept. |
| `0x130` | Transmit consume index | `0` | RW | 16 bits, advanced by hardware; bit 31 reports the engine idle. **Byte offset**, shifted left by seven for the 128-byte descriptor — all three drivers divide by the descriptor size on read. |
| `0x134` | Transmit produce index | `0` | RW | 16 bits, advanced by software. Byte offset, shifted left by seven. Sixteen bits for 512 descriptors of 128 bytes is a byte offset on its face; an entry number would need nine. |
| `0x138` | MAC address, high | `0` | RW | Upper bits of the station address. |
| `0x13C` | MAC address, low | `0` | RW | Lower bits of the station address. |
| `0x140` | Hash filter, high | `0` | RW | Multicast filter, upper half. |
| `0x144` | Hash filter, low | `0` | RW | Multicast filter, lower half. |
| `0x148` | MII control | `0` | RW | Bits 0–4 select the PHY register, bits 5–9 the PHY address, bit 10 triggers a read, and bit 11 reads back busy. |
| `0x14C` | MII data, read | `0` | RO | Sixteen data bits from the last read. |
| `0x150` | MII data, write | `0` | WO | Sixteen data bits for a write. |

Interrupt sources, by bit: receive timer expiry, receive threshold reached, receive
overflow, receive buffer overflow, receive memory error and receive parity error at
bits 0–5; then transmit empty at bit 16, retry-limit, excessive deferral, late
collision, oversized frame, transmit buffer underflow, explicit completion, collision
counter wrap, deferral counter wrap, transmit memory error and transmit parity error
in bits 17–26.

The PHY is reached only through the two MII registers, and the driver finds it by
scanning the 32 possible addresses and reading the identifier registers, so any
plausible MII PHY model will satisfy it. The part the Linux driver was written against
is the National Semiconductor DP83840A, whose datasheet is cached in the document
library and gives the register meanings that scan depends on.

### PROM netboot driver expectations (IP27 and IP35 PROM)

The base-I/O PROM that both the Origin 200 and the Origin 300 load before the boot
menu carries its own IOC3 ethernet driver, used for BOOTP and TFTP netboot. It is one
driver, byte-identical in structure across the two machines at IRIX 6.5.30 media, and
it asks less of the chip than either kernel driver above — which makes it the
tightest consumer of the receive contract this datasheet describes. Everything below
is read from the PROM's own code: *the base-I/O PROM netboot driver (authorized
tainted read, 2026-09-12)*, with the full reading in
the PROM netboot driver notebook (citation: `docs/notebooks/ioc3-prom-netboot-driver.md`).

**Established.**

- The driver is **polled, never interrupt-driven, for receive**. It leaves the
  receive timer at `0x114` and the receive threshold in `0xFC` at zero, and it writes
  the receive produce index at `0x110` **with bit 31 clear**, so the timer is never
  armed. A model whose only delivery notice is an interrupt source will never be
  noticed.
- It **never reads the receive consume index at `0x10C`.** The register is written to
  zero once during reset and not touched again. Its position in the ring is its own
  count of entries it has consumed, advanced by one per completion and wrapped at
  512.
- **The buffer's valid bit is the entire completion test.** The poll reads exactly one
  32-bit word — the first header word of the buffer whose address sits in the ring
  entry at the driver's own next position — and enters its receive loop only when bit
  31 is set. It then parses that buffer. Nothing else is checked first.
- It **drains many completions per poll**: it walks forward entry by entry, stopping
  at the first clear valid bit or when it reaches its own produce index.
- It supplies **at most 65 buffers at a time** into the 512-entry ring, as a sliding
  window. Entries outside that window still hold the addresses of buffers it has
  already released upward.
- **Refill is a batch, published by one register write.** For each free slot it
  allocates a buffer, **zeroes that buffer's two header words**, writes the address
  into the ring entry, and only then writes the produce index once. So every entry
  below the produce index is fully prepared before that write.
- It programs the MAC control register at `0xF0` with a **receive-offset field value
  of 17** — 34 bytes by this datasheet's halfword rule, confirmed by the driver's own
  arithmetic — together with promiscuous receive, pad generation and buffer-RAM
  parity. This is **not** the 33 the IRIX kernel driver programs: a model must honour
  the programmed field rather than a constant.
- It acknowledges the interrupt status at `0xF4` by writing back the whole word it
  read, and decodes only the error sources; the receive-timer and threshold bits are
  not in its mask.
- **Transmit: the frame is fully written before the doorbell.** Frames whose payload
  is under 89 bytes — every TFTP acknowledgement — are copied **inline into the
  transmit descriptor**, destination and source addresses, ethertype and payload
  together, and both descriptor command words are stored, before the transmit
  produce index at `0x134` is written. Longer frames use the descriptor's pointer
  form instead. The driver does not wait for transmit completion before returning; it
  reclaims descriptors later.
- The driver contains **no `sync` and no cache instruction**, so it relies on no
  explicit barrier between its stores and its register writes.

**Inferred.**

- A completion the driver cannot see stalls it permanently rather than degrading it:
  it has no index to resynchronise from, so an entry skipped or filled out of order
  leaves it parked there. This follows from the code but has not been produced on the
  model.

**Unknown.**

- Whether real silicon stalls on a set valid bit rather than overwriting the buffer.
  The driver clears that word before republishing, so it behaves as if hardware
  stalls, but nothing here proves it must. A discriminating test would present a
  buffer whose valid bit is still set and see whether a frame lands in it.

### Add-in adapter coverage of the base-I/O PROM's netboot path

**Established, on this project's model.**

This is a harness observation, not a tainted read: an Origin 200 guest was
booted three times, once each with a Tigon, a BCM570x and an Xframe adapter
attached alongside the integral IOC3, each reached through the same
maintenance-menu command monitor formerly driven by `tools/origami test
prom-netboot`. In every case, the command monitor's own `hinv` listed only
"Integral Fast Ethernet" — the same two-entry hardware-inventory record count
the plain IOC3-only boot also writes — never the added card, even though
QEMU's `info pci` shows each card on the bus throughout. Sending
`boot -f bootp()/sash64` in each of the three runs
produced the same guest-visible outcome as the integral controller alone with
no boot file configured on it — `Setting $netaddr to 192.0.2.15`, then
`Unable to load bootp()/sash64: no such file or directory` — and a packet
capture bound to the added card's own network backend recorded zero frames in
every run, while the integral controller's own capture recorded a real DHCP
exchange each time. The base-I/O PROM's netboot path only ever dials out over
the integral IOC3 in this harness: run irix-20260913T075501-9eba4a
(Tigon/`eg`), irix-20260913T080025-6795ba (BCM570x/`tg`), and
irix-20260913T080117-7d6677 (Xframe/`xg`).

This establishes the model's own behavior, consistently across three
different add-in network devices. It does not by itself establish that real
Origin 200 firmware would treat a real Tigon, BCM570x or Xframe card the same
way, which remains untested. The base-I/O PROM's own SCSI probe narrates
itself ("Walking SCSI Adapter 0", "Walking SCSI Adapter 1") in every one of
these runs. Nothing comparable ever narrates a PCI or GIO Ethernet slot walk,
on this PROM build, with or without a card attached.

### The ethernet buffer RAM window: 256 KB of address space, 128 KB of RAM

**Address space and RAM are different quantities here, and confusing them puts the
populated size out by a factor of two.** The RAM is 16 bits wide and the diagnostic window
gives each word its own 32-bit slot: bits 0–15 are the data, bit 16 is the parity bit
stored alongside them, and bits 17–31 belong to no storage — though bit 17 is where the
parity checker answers on a read while it is enabled. So the window's `0x40000`
bytes of address space are 65,536 slots holding 65,536 sixteen-bit words — **128 KB of
RAM in 256 KB of address space.** SGI's own header carries both numbers without
reconciling them: `IOC3_SSRAM_LEN` is `0x40000`, commented "256kb (address space size,
may not be fully populated)", beside a data mask of `0x0000FFFF` and a parity mask of
`0x00010000` (IRIX 6.5.7 `sys/PCI/ioc3.h`, and identically in Linux's
`asm/sn/ioc3.h`).

**A chip with the smaller RAM has the first half of the window and nothing in the second.**
Every driver sizes it the same way, and the arithmetic is what says where the halves
divide: assume the larger RAM by setting the buffer-size bit in the MAC control register,
write a pattern to the word at the start of the window, write its complement to a word
further in, read both back, and if either fails to match, clear the buffer-size bit and use
the smaller RAM. SGI's own driver puts that second word at `IOC3_SSRAM_LEN / (2 *
sizeof(uint32_t))` in 32-bit units — the halfway point of the address window, offset
`0x20000` (IRIX 6.5.7 `bsd/mips/if_ef.c`, revision 1.73, `ssram_discovery`). OpenBSD's
`iec` driver probes that same offset, and it prints a size by halving an address length:
`0x20000 / 2` is the 64 KB it reports on the smaller branch and `0x40000 / 2` the 128 KB
it reports on the larger. So the designer's driver and the guest we boot agree that 64 KB
of RAM ends at address offset `0x20000` and that 128 KB of RAM fills the window.

**Mainline Linux probes at `0x10000` instead, a quarter of the way in, which is inside the
smaller RAM.** Its second word is `&ssram[0x4000]` in 32-bit units where SGI's is
`&ssram[0x8000]`. That probe cannot fail on a 64 KB part, so mainline Linux reports 128 KB
on every chip. It is recorded here because our model makes both drivers print 128 KB and a
reader checking our offsets against Linux's will find a disagreement that is Linux's, not
ours — and because a model built to Linux's offset alone would be a model nothing had
tested at the boundary.

**The Origin 200 has the full 128 KB, and that is observed rather than reasoned.** Every
one of the thirty-one captures in our boot-log library that reaches the ethernet driver
prints `iec0 at ioc0: 128KB SSRAM`, and every one of them identifies its machine as
`mainbus0 at root: Origin 200`. They are not one machine submitted repeatedly: they span
R10000 and R12000 processors, three memory sizes, one-node and two-node configurations,
and nine of them carry a second controller whose `iec1` prints 128 KB as well.

**What is observed is which branch the driver took, not the number it printed.** The
driver holds two constants and prints the one its probe selected, so "128KB" is a label
for a branch rather than a measurement of a size. The measurement underneath it is this:
on a real Origin 200, a pattern written to the word at the start of the window and its
complement written to the word at offset `0x20000` both read back afterwards. That is
what our model has to reproduce, and reproducing it is what makes the printed number
match.

One further reading falls out of the same branch. OpenBSD compares the first read against
the data mask but compares the second as a whole 32-bit word, against `0xAA55`, so on real
hardware that read returns exactly `0xAA55` — the bits above the parity bit read zero, and
the parity bit is not set for that pattern. SGI's and Linux's drivers mask both reads and
would not have shown this.

**Each slot stores seventeen bits and reads back eighteen.** Every slot in `0x40000`–
`0x7FFFF` stores what was written to its low seventeen bits — sixteen of data and the
parity bit beside them. **Bit 17 is not storage. It is the parity checker's answer,
generated on every read**, and it is set when the seventeen stored bits do not carry even
parity. Bits above it read zero. There is no unpopulated remainder to decide the behaviour
of on this board, so the question of whether an absent upper half would alias, wrap or read
as nothing does not arise for the Origin 200 and is left unanswered here. What the model
must not do is alias the two halves onto each other: the halves being distinct storage is
the entire content of the size probe, and a window that aliased at `0x20000` would report
64 KB while looking, from every other angle, like working memory.

**The checker only answers while the ethernet block's MAC control register has buffer-RAM
parity checking enabled.** With that bit clear, bit 17 reads zero whatever the stored
parity is. This matters beyond fidelity: OpenBSD's size probe compares its second read as a
whole thirty-two-bit word rather than through the data mask, so a checker that reported
unconditionally would put a bit into that comparison and send a driver that found 128 KB of
RAM down the branch for 64 KB.

**Writes to bit 17 are ignored.** Software cannot place the checker's answer; it can only
place the parity bit the checker reads.

### What is observed about the parity checker, and what is read into it

**Observed, from SGI's own power-on diagnostic running against this model.** The firmware's
`enet_ssram` test writes four words to the first slot and reads each back. The written
values and the values it required are these, and the written values come from logging the
window's own accesses rather than from the console:

| Written by the firmware | Data ones | Parity bit | Total | Firmware required |
|---|---|---|---|---|
| `0x0000AA55` | 8 | 0 | 8, even | accepted as written |
| `0x000157AA` | 9 | 1 | 10, even | accepted as written |
| `0x0000BAD1` | 9 | 0 | 9, odd | `0x0002BAD1` |
| `0x0001BAD0` | 8 | 1 | 9, odd | `0x0003BAD0` |

Also observed: the firmware never writes bit 17 in any of those four words, so the bit it
requires back is one the chip produced. It writes MAC control `0x200` — buffer-RAM parity
checking, and nothing else — before the first of these accesses, and leaves it set through
the size probe that follows. Its earlier address-walk phase does write bit 17 set in every
word and reads it back clear without complaining, which is what says a write to the bit is
ignored. The capture is
the firmware boot that first passed this test (citation: `docs/boot-logs/firmware.enet-diag-pass.2026-08-18T02:05+00:00`).

**Read into it, and it is a reading rather than a finding.** Even parity across the
seventeen stored bits is the rule that separates the two accepted words from the two
rejected ones, and it is the simplest rule that does. Four words is not many. A fifth
pattern that broke the rule would overturn it, and no capture we hold contains one.

**Unresolved on purpose.** Nothing here separates "bit 17 is a checker's output" from "bit
17 is a generator's output that software reads back". Both produce all four answers above.
The checker reading is modelled because that is what SGI's diagnostics manual says the test
exercises — it names the logic under test as the chip's parity generation *and checking*.
A capture in which software wrote bad parity and then read the *parity bit itself* back
changed would settle it; no guest on our path does that.

**A disagreement with the designer's own header, recorded rather than resolved.** That
header carries a parity mask of `0x00010000` — one bit — and names nothing above it
(IRIX 6.5.7 `sys/PCI/ioc3.h`, and identically in Linux's `asm/sn/ioc3.h`). That is not
wrong about what it describes: `0x00010000` is the *stored* parity bit, and both drivers
mask their reads with it. The header simply does not describe bit 17, because no driver
reads it and the header exists to serve drivers. **The firmware is the only software that
reaches the checker, and it is what this section is written from.**

### PS/2 keyboard and mouse

| Offset | Name | Reset | R/W | Behavior |
|---|---|---|---|---|
| `0x9C` | Control and status | `0` | RW | Bit-level control and live state of both ports' clock and data lines. |
| `0xA0` | Keyboard receive | `0` | RO | Up to three received bytes with their flags. |
| `0xA4` | Mouse receive | `0` | RO | Same layout as the keyboard receive register. |
| `0xA8` | Keyboard transmit | `0` | WO | Low eight bits are the byte to send. |
| `0xAC` | Mouse transmit | `0` | WO | Low eight bits are the byte to send. |

The controller is a bit-level engine, not a command-level 8042 clone. The control and
status register reports, per port, that a write is still in progress, the line-control
bit of the last write, and the live state of the clock and data lines; it lets software
pull either line low by hand; and it reports whether each port's state machine is idle
and whether a transfer has been pending long enough to time out. Two enable bits turn
those timeouts into interrupts. Two further pairs of bits make the controller hold the
clock line low after one or after three received characters, which is how software
throttles a device without losing bytes. An emulation mode bit and a mode that runs the
clock eight times faster round out the register.

Mainline Linux's `arch/mips/include/asm/sn/ioc3.h` at
`db2ddb87143519e20a95aa36c60b36107b736a58`, lines 289–298, assigns
bits 16 and 17 to keyboard and mouse timeout status, and bits 18 and 19 to
their separate timeout interrupt enables (`KM_CSR_K_TO_EN` and
`KM_CSR_M_TO_EN`). Its comments say a timeout plus its enable can assert
`SIO_IR`. The public header and driver do not establish enable readback,
status-bit write handling, or the hardware reset value.

The model retains writes to the two enables and reads them back until changed
or guest reset. It leaves the timeout status bits read-only and clear because
it does not simulate a stalled PS/2 line. These are bounded approximations,
not measured IOC3 behavior. A register test must toggle each enable, check
readback and reset, and show that writing status bits does not set them. A
revision-matched register trace before and after each write and reset would
settle the register assumptions. A timed-line trace and test are needed before
claiming timeout interrupt delivery.

Each receive register packs up to three bytes received since the last read, at bits 0,
8 and 16, each with its own framing-or-parity error flag in bits 24–26 and its own
valid flag in bits 29–31. **The oldest byte occupies the highest position**: the first
character received since the last read lands in bits 16–23 with its flags in bits 26
and 31, and the third lands in bits 0–7 with its flags in bits 24 and 29. Getting that
order backwards produces a keyboard that types in reverse within every group of three,
which is a memorable bug to avoid. Bit 27 says whether the bytes came from the keyboard
or the mouse and bit 28 reports that a fourth byte arrived and was lost.

**Mainline Linux covers this block, and its driver dual-sources the register contract above.**
Both the field layout and a working serio driver are public: `arch/mips/include/asm/sn/ioc3.h`
(v7.2-rc7) defines the control-and-status, receive and transmit bit fields, and
`drivers/input/serio/ioc3kbd.c` drives them. The MFD core instantiates the PS/2 sub-device on
the boards whose setup registers it — the Origin 200's IP29 System Board among them
(`ioc3_kbd_setup`, `drivers/mfd/ioc3.c`). Four behaviors the register table alone does not
give follow from that driver:

- **The write-pending bits are polled, not just reported.** Before writing a byte to the
  keyboard transmit register the driver waits for the keyboard write-pending bit to clear, and
  the same for the mouse — so a transmit register write while the port's write-pending bit is
  set is out of contract, and the model should hold that bit set for the duration of a
  transmit rather than clearing it instantly.
- **One shared interrupt, demuxed by reading both receive registers.** On the single keyboard
  and mouse interrupt (status bit 22) the handler reads both the keyboard and the mouse receive
  register and dispatches whichever bytes carry their valid flags, oldest first. The model
  raises the one shared source and lets the guest sort keyboard from mouse by the valid bits
  and bit 27, exactly as the interrupt table already says.
- **Port enable is a write of the clamp-after-three bits.** The driver's only initialisation
  write to the control-and-status register sets the keyboard and mouse "hold clock low after
  three characters" bits and nothing else, which is what arms the ports to receive.
- **A read of a receive register drains it.** The register packs bytes "since the last
  read", so a read clears the accumulated bytes, their valid and error flags, and the
  fourth-byte-lost overflow flag, and releases any clock clamp — the next characters
  accumulate from empty. A model that returned the stored bits without clearing on read
  would re-deliver them on the following interrupt.

Linux tags both ports as generic 8042 serio ports, but the byte-level PS/2 protocol is handled
by the keyboard and mouse layers above it; this block itself is the bit-level line engine the
paragraphs above describe, not an 8042.

**Two things about the data-available line are approximated, and the model says so in its
own comments.** Everything above — three characters held, oldest in the highest position,
the clamp pairs, the fourth-character overflow flag, and a read that clears the characters,
their flags and the clamp together — is either stated by the register table or settled by
mainline Linux's driver and its header, which also pins the clamp bits' positions. What
neither settles is how the line that drives interrupt-status bit 22 behaves, and there are
two gaps.

The first is whether the line is asserted once per received character or held as a level
over whatever the controller is holding. Nothing distinguishes them: both public drivers
read both receive registers on the one interrupt and neither looks at the line itself. The
model asserts per character, because an assertion then always names a character the guest
has not read, and no arrival can be swallowed because the line happened to be up already. A
level over the holding would strand a character that arrives while an earlier one is still
unread and the guest has already acknowledged. The discriminating test is
`sn0/ioc3/ps2-backlog-survives-an-early-acknowledge`, which acknowledges before reading the
way Linux's edge handler does and requires the backlog behind the clamp to raise the source
a second time.

The second is when a clamped device resumes. Real hardware releases the clock line as the
guest's read empties the holding, and the device needs a character time to clock its next
byte out; the model resumes it inside the read instead, with no time modelled. The
consequence is guest-visible: a driver that reads once and only then acknowledges can clear
the assertion those resumed characters just made, and is not told about them again until
another character arrives. Neither public driver is exposed — Linux acknowledges before it
reads, and OpenBSD's handler reads until the register comes back with no valid character,
so it has drained the resumed characters before it acknowledges — and IRIX's order relative
to its read is not established by any public source. The discriminating tests are the pair
`sn0/ioc3/ps2-backlog-survives-an-early-acknowledge` and
`sn0/ioc3/ps2-drain-then-acknowledge-takes-one`, which walk the two known driver orders over
the same five-character backlog. Driving the resume off a character-time timer would remove
the approximation, and a capture of IRIX's own acknowledge order on this bit would say
whether it needs removing.


### Byte bus and board-dependent clock attachment

Four device selects at `0x80000`, `0xA0000`, `0xC0000` and `0xE0000` give access to
byte-wide devices outside the chip, and the chip control register sets the strobe
length. The SuperIO block described above sits on this bus as well, which is why its window
is byte-wide and why its strobe timing is configured here.

**These four windows are byte-wide, and a clock window is easy to model at the
wrong width.** The Origin 200's Timekeeper behind select 0 is a flat 32 KB
region of byte-wide cells that mainline Linux drives with a byte-wide clock driver — larger
than the serial window, and shaped like a plain block of memory, which is exactly what
invites mapping it without a thought about access width. It needs its access width declared
for the same reason the SuperIO window does, and the same silent failure applies: no guest
on our path reads it wide, so nothing on a boot will notice.

**The byte lanes are straight, and this is measured rather than assumed.** A guest byte
access to register N on one of these windows reaches register N. There is no displacement
within the word anywhere on this path, which a test settles directly: freezing the clock at
a time whose fields are all distinct and reading the eight bytes of the clock's register
block one at a time returns control, seconds, minutes, hours, weekday, date, month and year
in that order. The competing arrangement, where the low two address bits are inverted within
each word, was run as its own case and failed on its first comparison, so the two are
distinguished by measurement and not by argument. The chip's other byte-wide window agrees:
the serial part answers its line-status and interrupt-identification registers at their
plain offsets, which is also why the firmware's console output is readable.

The reason is that the chip presents these registers on big-endian byte lanes, so a
big-endian host reaches register N at address N with no arithmetic, and the I/O bridge must
not apply its narrow-access correction to this card. Both public guests encode exactly that
by different routes, which is what makes it a convention rather than a coincidence. OpenBSD
hands this chip and its two siblings a non-swizzling accessor and gives every other card the
swizzling one. Linux applies an unconditional inversion inside its byte accessors and then
pre-permutes its own structure for this machine so the two cancel — which is why that
structure lists its fields out of order, and why the same permutation appears in its serial
structure for the same reason. A single-byte port has nothing to cancel against, which is
the real reason the other clock's two ports are declared four bytes apart while the
Timekeeper needs no such thing.

**Do not assume the boot flash is on this bus.** The chip's byte bus can certainly carry
flash, and on some boards it does — but SGI's BaseIO block diagram, read at high zoom,
routes that board's 1 MB flash to the *Bridge* rather than to the IOC3, and its byte bus
carries only the timekeeper and the SuperIO part. Whether the Origin 200 wires its flash to
the IOC3's byte bus, to the Bridge, or elsewhere is a machine question this datasheet cannot
answer and does not need to: the boot flash is supplied by the machine model, and the reset
fetch is settled separately through uncached memory space off the Hub rather than through
any of these windows.

The machine inventory selects one of these board attachments. The two clocks
are not aliases: one is directly addressed and the other uses separate address
and data ports.

| Machine | Chip select | Part | How it is addressed |
|---|---|---|---|
| **Origin 200** | 0, at `0x80000` | **MK48T35-class Timekeeper** — battery-backed non-volatile memory with a clock in its top bytes | Directly: a flat 32 KB window |
| Origin 300 | 0, at `0x80000` | DS1742W-compatible interface observed, exact IP45 package and capacity unknown | Directly, with the full fitted decode still open |
| Octane | 1 and 2, at `0xA0000` and `0xC0000` | Dallas DS1687 | Indirectly: chip select 1 is the address port, chip select 2 the data port |

**SGI's own board block diagram settles the part class before any driver is consulted.** It
labels the clock beside the IOC3 *TOD / 32KB NVRAM* — a time-of-day clock with 32 kilobytes
of non-volatile RAM. That size is the identification: an MK48T35-class Timekeeper is a
32 KB non-volatile static RAM with the clock in its top bytes, and mainline Linux maps
exactly 32768 bytes for it. A DS1687 carries on the order of a hundred bytes and cannot be
what the diagram describes. So the vendor document, the Linux driver and the OpenBSD driver
all agree, and the DS1687 reading is contradicted by all three.

Two independent kernels agree with it. Mainline Linux maps the Origin 200's board identity to its
base I/O setup path, which installs an M48T35 real-time clock driver over a 32 KB window at
byte-bus device 0; it installs the DS1685/DS1687 driver only on the Octane's board, over
one byte at device 1 and one byte at device 2, and explicitly configures that driver for
indirect register access. OpenBSD 7.9 sets its clock base to byte-bus device 0 for every
Origin-class machine and to byte-bus device 1 only for the Octane, and prints the part it
finds at that base as a DS1742W — a Timekeeper, the same family as the M48T35.

Earlier notes in this project named the byte-bus clock a DS1687 and cited the Dallas
DS1685/DS1687 datasheet for it. **That is the Octane's clock and the wrong part for this
machine**; a model built to it would not answer an Origin 200 driver at all. The cached
DS1685/DS1687 datasheet stays useful for the Octane if we ever model one, and is not the
reference for the Origin 200.

The clock is a separate device with its own model and its own datasheet, so its register
detail is not repeated here. [The Timekeeper datasheet](timekeeper.md) carries
that device's register behavior. This datasheet owns the four IOC3 chip-select
windows and their byte-wide access; the machine model owns which device is
connected to each select.

## Behavioral contract

### Console bring-up, as the PROM actually does it

The PROM's own console routine is the authoritative init order, and any device model
should be tested against it before anything else. It proceeds as follows.

First it enables the chip: it sets bus-master and memory-space enable in the PCI
command register and writes `0xFF00` to the latency timer.

Second it tells the DMA engines where the UARTs live, writing the chip control
register with port A's SuperIO offset divided by 8 in bits 1–7, port B's in bits 8–14,
and a byte bus strobe length of 4 in bits 15–18.

Third it sets up the pins: it clears the pin data register, then uses the pin control
set register to enable the interrupt output on pin 0, enable the MicroLAN on pin 8, and
select the transceiver mode for both serial ports.

Fourth it configures the UART itself, using only the byte-wide window. It waits for the
transmitter to drain, sets the divisor-latch bit in line control, writes the divisor
high and low bytes, writes the clock pre-divider through the scratch register while the
divisor latch is still selected, then writes line control again to select eight data
bits with one stop bit and clear the divisor latch. It disables all UART interrupts,
enables the FIFOs and resets both of them with a second write, and finally asserts
data-terminal-ready and request-to-send so that a terminal expecting flow control sees
sensible levels.

From then on the PROM polls line status for data-ready to receive and for
transmit-holding-empty to send, one character at a time, and reports overrun, framing
and parity errors as they appear. There is no interrupt and no DMA on this path.

### The base I/O diagnostic's serial DMA sequence

This is the only driver of the serial DMA engines anything on this machine has been
observed to run. It is recorded from a register trace of the diagnostic on the emulated
machine, in the order it issues the accesses, and it does the same to both ports:

1. Control ← `0x80000000`: reset the DMA channels.
2. Control ← `0x00000001`.
3. All four index registers ← 0, including the two hardware advances.
4. Control ← `0x001201ff`: high-water mark 511, hardware flow control, quadruple speed.
5. The byte-wide modem control register read back `0x03` and written `0x13` —
   data-terminal-ready, request-to-send and **loopback** — and the 16550 shadow written
   `0x13000000`, the same three bits in its driven-output field.
6. Transmit produce index ← `0x3f8`, which with 1 KB rings is the ring's last entry: 127
   entries handed over at once.
7. Ring base ← `0xa0000000_01ade000`, size bit clear, so 1 KB rings and a 4 KB block. The
   top half is crosstalk attributes rather than address.
8. Receive timer ← `0xfff`.
9. Control ← `0x101201ff`: ring DMA enabled.
10. On failure, control ← 0 and the modem control register restored.

**Three things in that order are load-bearing and none is derivable from the register
table.**

**The engine has to start on the enable, not only on the doorbell.** The transmit produce
index is handed its 127 entries before the ring base is written and before ring DMA is
enabled — so at the moment software advances that index there is no ring to walk. An
engine that starts only on the doorbell never walks at all here, and the diagnostic
reports a timeout.

**The diagnostic never reads a register in the block.** Between enabling and giving up it
issues no read of any of the seven, so whatever it waits on is the receive ring in memory
or the SuperIO interrupt status register. A model that moves the indices correctly but
writes nothing into the ring satisfies nothing.

**Loopback puts the two directions in each other's way.** Every character the transmit
engine sends arrives at the same port's receiver, and the receive FIFO between them is
sixteen bytes deep against a ring the diagnostic fills a few hundred characters at a
time.

### What a guest minimally needs

A guest reaches a working console with only the PCI command register, the chip control
register, the pin registers, and one byte-wide 16550 file. OpenBSD adds the interrupt
status and enable registers so that it can take the serial pass-through interrupt.
Neither needs the serial DMA rings, the ethernet, the PS/2 block or the byte bus. That
ordering is the implementation plan: get the byte-wide UART and the interrupt
pass-through right, prove a banner, then widen.

**A console is a lower bar than a stock firmware boot, and this list is the console's
bar.** The firmware's base I/O diagnostic drives both serial DMA engines and fails its
slot without them, so a machine meant to reach the firmware's own monitor needs the rings
even though no guest needs them to print.

### Ethernet operation

The driver resets the block through the MAC control register, programs the station
address and the two ring bases, sets the receive offset and buffer size, then enables
receive and transmit and their DMA engines.

**The ethernet receive ring inverts the serial rings' producer/consumer sense, and the rows
above are easy to write backwards by analogy.** On the serial side hardware produces received
characters and software consumes them, so the receive produce index is hardware's. On the
ethernet side the receive ring holds *buffer addresses that software supplies*, so **software
is the producer** — it advances the produce index as it refills, and hardware advances the
consume index as it uses buffers up. A model built the other way watches a register no guest
writes and advances one the guest is already writing: no packets arrive, and neither symptom
names the cause.

**The receive ring holds buffer addresses, not frames.** It is a fixed array of 512
entries, each one 64-bit address of a separately allocated buffer — 4 KB of ring in total,
which is why the base register keeps bits 31 down to 12. Frame data never lands in the
ring. Getting this backwards produces a model that writes received frames over the pointer
array, and the symptom is memory corruption rather than a missing packet.

Each *buffer* opens with a word carrying a valid bit in bit 31, a byte
count in bits 16–26 and a partial IP checksum in bits 0–15, followed by a second word
holding the receive status vector — CRC error, framing error, code error, invalid
preamble, the frame length in two pieces, and flags for multicast, broadcast, long
event, bad packet, good packet and carrier. Padding then precedes the frame itself, at
the offset the MAC control register selected.

**The partial checksum must be exact, never approximate.** The engine leaves a sixteen-bit
one's-complement sum of the whole received frame — MAC header through the trailing FCS — in
bits 0–15, and a driver offloading the receive checksum finishes the transport checksum from
it, marking the frame verified only when its reconstruction folds to all ones. This model
computes that sum. The value has to be exact: a *wrong* non-zero partial that happened to fold
to all ones would make the driver trust a corrupt packet without checking it — worse than the
fail-closed alternative of a constant zero, which never folds to all ones, so every guest just
re-verifies in software and none drops the frame. Those are the only two safe values, the
exact sum or zero. The Ethernet FCS the backend strips is not there to be summed, so the model
appends four defined zero bytes where it would sit, sizes the byte count to include them, and
sums them as zero; the driver reads and subtracts those same four bytes, so the reconstruction
is self-consistent whatever they hold.

**The status vector's exact bit positions**, cross-checked between SGI's own header
(`sys/PCI/ioc3.h`'s `ioc3_erxbuf` fields, IRIX 6.5.7) and mainline Linux's header, which
name the same bits: CRC error at bit 0, framing error at bit 1, code error at bit 2,
invalid preamble at bit 3; the frame length's low three bits at bits 12–14 and its high
ten bits at bits 16–25 — the "two pieces" above, reassembled as the high field shifted
left three and or'd with the low field; then multicast at bit 26, broadcast at bit 27,
long event at bit 28, bad packet at bit 29, good packet at bit 30 and carrier at bit 31.
A model that only ever produces well-formed deliveries has nothing to set in the error
bits or in long-event/bad-packet, and good-packet is the one status bit such a model
always asserts.

The transmit ring, by contrast, holds the descriptors themselves inline: 128 entries of
128 bytes each in the default configuration.

### Who advances what, and when — the sequence the register table does not give

**The register table says which side owns each index and stops there, and ownership is not a
sequence.** An implementer needs to know at what moment hardware moves an index and which
interrupt bit it raises, and that is not derivable from a column saying "advanced by
hardware". This is what the drivers require, read from their interrupt handlers, which are
the clearest statement of what they believe the engine has already done.

**Receiving, in order.** Software supplies buffers and advances the **receive produce index**
as it refills. The engine takes the next buffer, writes the frame into it at the offset the
MAC control register selected, fills in that buffer's own header — the valid bit, the byte
count and the status vector — and then advances the **receive consume index**. It then raises
the **receive-timer source**, which is what a delivered frame prompts a driver with: both
drivers arm the timer through bit 31 of the produce index and load it with zero, and both
say in their own words that zero means interrupt immediately on reception. The receive
threshold source is a separate thing that fires when available buffers reach the watermark
at `0xFC`, which is a supply warning rather than a delivery notice.

**The comparison is at-or-below rather than strictly-below, and the reason is worth three
lines rather than one.** OpenBSD's comment says the source fires when available descriptors
"**reach**" the limit, and it picks a value placing the watermark exactly where the first
delivered frame lands — under a strict comparison it would have had to program one more and
did not. That is a strong fact about the driver and an unverified inference about the silicon,
and **no guest we run can tell the two readings apart**, because OpenBSD enables the receive
timer as well and Linux never enables this source. So the direction is a reasoned choice
rather than a measurement: implement at-or-below because it is what the only driver that
programs this source asks for. The full statement, including the narrower unknown that sits
inside it, is in what the IOC3 receive path must do (citation: `docs/library/iec-receive-contract.md`).

**The base-I/O PROM's own netboot driver takes a third path through this same contract,
polling rather than interrupting.** It never arms the receive timer and leaves the receive
threshold at its zero reset value, so neither source above ever fires for it. It detects a
delivered frame purely by polling the valid bit in the buffer at its own next ring position,
and it never reads the receive consume index at all — it tracks its own consumed count
instead of reading hardware's. The roles above still hold: software still produces, hardware
still consumes. This driver simply never asks hardware to prompt it.
[PROM netboot driver expectations](#prom-netboot-driver-expectations-ip27-and-ip35-prom) has
this driver's full contract.

Two details in that order are load-bearing, and each is a thing a model can get wrong while
looking right:

- **The buffer's valid bit is the driver's real test, not the index.** OpenBSD computes how
  many buffers arrived by differencing the consume index, and then checks the valid bit in
  each buffer's header and drops the frame if it is clear. So the index and the header must
  agree: an engine that advanced the index without writing the header hands the driver a
  packet it counts and then throws away, and the interface reports input errors on a link
  that is working.
- **Software clears the valid bit to recycle a buffer.** The driver writes zero over the
  status word when it is done with one. A model that treated the bit as its own to set and
  never observed software clearing it would refill correctly by luck and desynchronise the
  first time a frame was dropped.

**Transmitting, in order.** Software fills a descriptor and advances the **transmit produce
index**. The engine sends the frame and advances the **transmit consume index**, and raises a
transmit interrupt — the descriptor's command word can ask for one on completion, and the
status register also carries a transmit-empty bit for the queue draining entirely.

**The transmit consume index is a byte offset with a status bit in it**, which is the trap on
this side: OpenBSD masks the engine-idle bit off before dividing by the descriptor size to get
an entry number. A model that advanced the index by entries rather than by bytes, or that let
the idle bit reach the arithmetic, produces a completion count that is wrong by a factor of
the descriptor size and a driver that frees the wrong buffers.

**Inserting a transmit checksum.** When a descriptor's command word sets the insert-checksum
bit, the engine finishes the frame's transport checksum for the driver: mainline Linux
describes the hardware as summing the whole frame in one's complement and writing the folded
result back at the byte offset the command word carries. A driver offloading a TCP or UDP
checksum pre-loads that field with the transport pseudo-header partial and lets the engine
complete it. The IP header checksum is not offloaded — the driver still fills that in
software — so only the one transport checksum is inserted, which is why the field's own name
for the IP checksum understates what it does. The observable contract is narrow: a frame sent
with the bit set must leave with a correct TCP or UDP checksum, so a model may compute that
checksum directly rather than replaying the whole-frame sum. **A model that ignores the bit
leaves the driver's partial value on the wire.** Every offloaded frame then carries a wrong
transport checksum and the far end drops it; under QEMU user-mode networking the slirp stack
is that far end, so a guest's outbound TCP and DNS fail silently while its software-summed
DHCP and ICMP still work. That signature — offloaded traffic dropped, software-summed traffic
fine — is how the gap was traced on a Linux guest. Not every guest offloads at every chip
revision: one gates the offload on the chip's PCI revision, treating a revision-0 part's
checksum hardware as defective and declining it there, and using it from revision 1 up. This
model reports revision 1, so that guest sets the bit like a guest which offloads regardless of
revision, and the engine finishes both their checksums the same way.

**Padding a short frame.** When the pad-enable bit in the MAC control register is set, the
engine extends any frame it assembles shorter than the sixty-byte minimum — the 802.3 floor,
before the frame-check sequence — up to sixty bytes of zeros before the frame leaves, and
reports the padded length. With the bit clear the frame goes at its assembled length. A driver
may pad in its own transmit path and leave the bit inert, and two of the three drivers do
exactly that — which is what makes a model that stores the bit and never pads look correct.
The third sets the bit and never pads in software, in both its kernel and its netboot paths,
so against that guest an unimplemented pad ships sub-sixty-byte frames — an ARP request, a
bare acknowledgement — at their true short length. A backend that does not enforce the
Ethernet minimum, QEMU's user-mode networking among them, carries them regardless, so the gap
stays latent until a backend or a peer that drops runts is in the path. A model honours the
bit by zero-padding the assembled frame to sixty bytes when it is set.

**What the drivers do not require.** Neither one polls an index without an interrupt to
prompt it, and neither reads the buffer index register that reports the engine's own positions
in the buffer RAM. A model that raises the right interrupts and moves the two consume indexes
correctly satisfies both without implementing either of those.

**Which interrupt source a delivery raises is not free, and picking the wrong one fails
silently.** A driver enables the sources it intends to serve and dispatches on those alone,
so a source it did not enable prompts it no more than no interrupt at all — the line never
asserts, and nothing anywhere reports a source being raised that nobody asked for. **A
receive delivery must raise the receive-timer source.** Both drivers enable it and Linux
enables no other receive source at all: it dispatches its receive handler on the timer
source and nothing else, so a model raising receive threshold reached instead leaves a
Linux guest unable to receive a single frame. OpenBSD happens to enable both sources and
so survives that substitution, which is exactly why checking one driver here licenses a
model the other cannot use.

On the transmit side the choice is genuinely narrower and the two conditions a guest's own
completion accounting watches for are enough: explicit completion when a descriptor's own
interrupt-on-completion bit asked for it, and transmit empty once a doorbell's whole batch
has drained. Note that transmit empty is a source a driver may deliberately leave disabled
— OpenBSD enables every transmit source except that one — so it cannot stand in for
explicit completion.

**The interrupt status register belongs to no one engine.** Receiving and transmitting are
independent and either can raise a source while the other is mid-operation, so the register
is shared mutable state between them rather than something a path may treat as its own. What
that forbids: **no path may write back a value it read before doing work that can itself
raise a source.** Any bit set in that interval is destroyed by the write, and the loss is
invisible from inside the path that caused it — the value written is exactly the value read,
every register involved holds a legal value afterwards, and the only trace is a driver that
is never prompted for something that did happen. Sending a frame is such an interval,
because a frame handed to the network can be delivered back to this same engine before the
send returns. Set a source by or-ing it into the register's live contents at the moment the
condition arises, and acknowledge only by clearing the bits a guest wrote.

Transmit descriptors are 128 bytes: a command word, a buffer-count word, two 64-bit
buffer pointers, and 104 bytes of inline data. The command word carries the total byte
count in bits 0–10, a request for an interrupt on completion in bit 12, validity flags
for the inline data and each of the two buffers in bits 16–18, a request to insert an
IP checksum in bit 19, and the checksum's byte offset in bits 20–26. The buffer-count
word holds the inline byte count in bits 0–6 and the two buffer lengths at bits 8 and
20. The inline area lets short frames go out without a second DMA read, which is why
the descriptor is so large.

## Interrupts

**IOC3 presents two interrupt outputs, not one.** The chip does not advertise itself as a
PCI multifunction device, so nothing in configuration space says so, but OpenBSD 7.9's
driver says it outright in a comment and then acts on it: it maps the ethernet interrupt
to PCI interrupt pin A and the SuperIO interrupt to pin D for an on-board chip (pin B for
an add-in one), and establishes two separate handlers at two different priority levels —
network priority for the ethernet, terminal priority for the SuperIO. Mainline Linux
likewise maps a second interrupt for the base I/O boards rather than sharing one. A model
that offers a single line will have one of the two sources silently unserviced.

**Where the second one lands is settled: the ethernet interrupt on PCI interrupt pin A, and
the SuperIO interrupt on Bridge interrupt pin 3, ratified, and confirmed on both guests.**
OpenBSD breaks on the first empty PCI slot and uses that index directly — with devices 0, 1
and 2 occupied on real hardware, that is pin 3 — and mainline Linux reaches the same pin
through the per-board override table it keys on the board part number read over the Bridge's
one-wire. **The two guests agree**, so no divergence has to be modelled and nothing is placed
at a slot the real board does not have.

An earlier sign-off said pin 4, on two reasons that were both established as wrong: that
OpenBSD takes the pin *after* the first empty slot, and that Linux hard-codes 4. Recorded
because a reader meeting pin 4 in an older document, or in a test that encodes it, should
know it is superseded rather than merely unmentioned.

**Nothing in this datasheet depends on which pin it is.** The chip presents two interrupt
outputs and the machine wires them; the pin is a property of the board and no register
described here changes with it. The interrupt chain is owned by
the architecture notes (citation: `docs/architecture.md`).

Each output is a level. The SuperIO line asserts whenever any bit is set in both the
SuperIO interrupt status register and its enable mask, and deasserts when that
intersection empties, so it must be recomputed on every write to either register and on
every hardware event that sets or clears a status bit. The ethernet block has its own
status and enable pair driving its own line, and the SuperIO interrupt status register has
no ethernet bit at all — which is the register-level reason the two outputs are separate.

Status bits reach zero two ways, and the difference is per bit. Latched bits are cleared by
writing a one to them in the status register. Level-following bits — the three pass-through
bits — clear when the sub-block that raised them stops asserting, and ignore writes. The
acknowledge contract above states which is which.

The two enable addresses are one mask register behind two write aliases: write-one-to-set at
`0x20`, write-one-to-clear at `0x24`. Model them that way, since the set address demonstrably
reads back the live mask.

A guest's service loop, in the order OpenBSD 7.9 performs it, is the sequence to test
against: read the status register, intersect it with the enable mask, return immediately if
nothing is pending, disable the pending sources through the clear address, run each
subdevice handler, then for each serviced source write its bit to the status register and
re-enable it through the set address.

## DMA

Every DMA master in this chip sits behind a Bridge, so all of it must go through
`dma_memory_read` and `dma_memory_write` and honour the Bridge's address translation.
Using the physical-memory accessors would bypass the translation the guest programmed
and is wrong for every path here.

There are three independently based ring structures. The serial side has one base
register pair covering all four of its rings — a transmit and a receive ring per port,
each 1 KB or 4 KB, addressed in 8-byte units. The ethernet receive ring has its own
base, 4 KB aligned, with 12-bit indices, and holds 512 eight-byte buffer *addresses*
rather than frame data. The ethernet transmit ring holds either 128 or 512 descriptors of
128 bytes inline; its base register accepts 16 KB alignment and drivers align a ring to its
own size, 16 KB at 128 descriptors and 64 KB at 512; and it uses 16-bit indices, which are
byte offsets rather than entry numbers. The parallel port has two DMA contexts with
their own address and count registers, which origami does not intend to implement.

**The receive barrier register looks like a flow-control mechanism and is not one in
practice.** Its documented role is to mark the last entry hardware may write, and an earlier
version of this datasheet warned that a model ignoring it would corrupt the ring under a slow
guest. No driver advances it: of the three drivers, two write a constant once at
initialisation — and that constant is a crosstalk barrier attribute shifted into an address
field rather than an index, which SGI's own driver makes plain by writing that attribute
shifted down by thirty-two, arriving at the same value mainline Linux reaches by a different
route — and the third writes zero. The value the drivers write also leaves the
register's own barrier-enable bit clear, so the mechanism is switched off by the encoding of
what they write, which is a second and independent reason the model owes it nothing. **A model that stores the register and acts on nothing is
correct**, and the earlier warning inverted the risk, pointing effort at a mechanism nothing
drives.

Byte order needs care, and for this device the answer is that **the Bridge's DMA byte swap is
switched off**. The Bridge can swap bytes on the DMA path and does so for ordinary PCI
devices, but OpenBSD explicitly clears both swap bits for the IOC3 — and for the IOC4 and
RAD1 alongside it — when it configures the device's I/O window. So descriptors and ring
entries are read by the chip exactly as software wrote them, with no swap anywhere in the
path. Descriptor and ring fields are defined here as the values software sees, which for this
device is also what the hardware sees. The swap machinery still belongs in the Bridge model
rather than smeared into this device; what belongs here is the knowledge that for this device
it is disabled, so a model that quietly swaps would corrupt every descriptor.

## Out of scope

What remains here is deferred on purpose rather than overlooked, under
the decision to model a chip's functional ports rather than its diagnostic and simulation
fields (citation: `docs/decisions/D-0021-model-functional-ports-only.md`). The model implements the ports
that carry traffic and leaves the diagnostic and simulation-only machinery around them
alone, because none of it changes what a guest can do with the chip. The test for leaving
this list is whether a guest on a path we run reaches the field: the parallel port sat here
on exactly this reasoning until IRIX was observed probing for it.

- **Arbiter diagnostic fields:** the chip control register's report of which internal master
  currently wants the PCI bus, bits 19–21, and the MAC control register's arbiter
  diagnostics, bits 18–21. Nothing outside SGI's diagnostic looks at those. This does *not*
  include the arbiter-idle bit 22 of the chip control register, which IRIX's parallel driver
  polls while it resets the DMA state machine, and which the model answers.
- **The bad-parity report and the simulation-only bits** across the ethernet block. Note
  that this does *not* include the buffer-RAM parity enable at MAC control bit 9, which
  every driver sets on every boot, nor the buffer RAM window at `0x40000`, which every
  driver writes and reads to size the RAM.
- **Emulation mode and the eight-times clock** in the PS/2 controller.

Everything out of scope gets a catch-all that logs unimplemented accesses rather than
failing silently.

## Behavioral notes

The ordinary device registers reset to zero, with one behavioral exception.
The 16550 shadow register is a live mirror of the port's UART
lines rather than a stored value: it reads whatever the lines carry, which at reset is the
transmitter empty and the console port's carrier and clear-to-send asserted, not zero. The
diagnostic table lists it as zero, but a mirror reads the lines, and the DMA-ring console
driver reads carrier from it — a fixed zero reports the line dead — so the model reports
the live lines here, which the firmware boot tolerates. That aside, the reset function is
trivial and a reset-value qtest is exhaustive rather than a sample.

PCI configuration-space identity, revision/class, subsystem identity, command,
and status are the other exceptions described in the reset requirements. SGI's
register diagnostic records PCI identity `0x000310A9` and revision/class
`0xFF000000` for a revision-0 part. The implemented IOC3 variant reports PCI
revision 1, making its revision/class word `0xFF000001`. The subsystem identity
is supplied by the containing board rather than by the chip.

A reset that never runs looks exactly like a chip whose registers all read zero, which is
what this chip reads anyway — so the usual symptom of a device that was realized without a
bus and therefore never reset is invisible here. That makes the identity words the only
early evidence that reset ran at all, and it is worth reading them back before trusting
anything else.

The pin control register and the interrupt enable register are each a set/clear address
pair rather than a single read-write register, so there is no address at which software
can do a read-modify-write of the whole word. For the interrupt enable, IRIX proves the
set address reads back the live mask. For the pin control pair, no source reads either
address for its value — OpenBSD reads the set address only to flush a posted write —
so the read behaviour of the pin control pair is unverified in both directions. Making
every one of these four addresses return its live word is the conservative model,
because a model that returns zero would break any driver that samples the mask, while
returning the live word can only break a driver that depends on reading zero, and no
such driver exists in the three trees we hold.

The IRIX header notes that its ethernet section tracks revision 4.4 of SGI's internal
IOC3 specification, while the Linux driver cites revision 4.51. Both describe the same
register set, and no difference between them is visible in the two headers.

The chip appears in machines other than the Origin — Octane and the Indy-class parts
use it too — and the IRIX header carries conditional definitions for those. Origami
only ever selects the Origin layout, and the differences are confined to the SuperIO
offsets, the byte bus slot the clock answers on, and a handful of Octane-only pin
assignments for memory control and front-panel LEDs.

## Receive arrival latency (an approximation)

**Approximated, not established.** A received frame's completion — the receive header word's
valid bit, the consume index and the receive source — is published a fixed minimum after the
backend hands the frame over, on the delay line shared by the four SGI ethernet adapters. The
receive arrival-latency floor (citation: `docs/designs/receive-arrival-latency.md`) carries the mechanism, the
200 microsecond default and the evidence for it, the limitations and the cost measured against
it. What follows is this device's own share.

**What is wrong without it.** The model carries no wire, peer, transceiver or
DMA time at all. In a traced IRIX 6.5.30 boot running 50 pings, 16 were lost, and the time from
the guest's transmit doorbell to the reply's completion separates the two
outcomes: 13 of the 27 classified cycles whose replies completed within 74
microseconds were lost — 10 of the 17 within 24 — and none of the 14 that took
75 microseconds or more was. The median was 20.4 microseconds when the ping was
lost against 85.5 when it succeeded, and the fastest completion was 14.3
microseconds. Two 60-byte
frames at 100 Mbit/s are 9.6 microseconds of payload serialisation on their own,
before preamble, interframe gap, any peer stack, transceiver or DMA time, so the
model was presenting the reply sooner than the wire by itself allows. The Bridge
and the Hub were cleared first: all 61 receive raises in that boot landed on a
quiet line with a proper edge, so the loss was not an interrupt-edge defect.
Delaying the receive interrupt by 1 millisecond of virtual time removed the loss
entirely, 0 of 150 pings against 28 of 200 without it.

**What this device was measured on.** The floor sweep behind the 200 microsecond default and the
saturated-UDP cost table were both measured on this adapter and the Xframe. The numbers are on
the shared page; this adapter is the one that decides the default.

**The readiness retry.** A write to `RX_PRODUCE` also retries a completion the
bus had refused, rather than leaving it held until some unrelated later event
releases it. That a produce-index write is what restarts placement — the
engine takes the next buffer once one is posted — is established from both
drivers' interrupt handlers, cited above under what the IOC3 receive path
must do (citation: `docs/library/iec-receive-contract.md`). That the same write should also
retry a completion already stalled on the arrival line is this model's own
inference from that established fact, not a rule either driver or the manual
states about a retry.

**The discriminating test.** `tests/qtest/sgi-ioc3-eth-test.c`'s
`sgi-ioc3-eth/receive-waits-for-arrival-deadline` hands the device a frame and
waits for the payload to land in the posted buffer without advancing the virtual
clock. It then asserts that the header word's valid bit, the consume index and
the receive source are all still absent one nanosecond short of the floor, and
all three present one nanosecond past it.

## Settled since the first draft, recorded so nobody re-litigates them

Questions this datasheet once carried as open, or once answered differently, are listed
here with their answers rather than deleted, because a reader who remembers one open — or
remembers the old answer — needs to see that it was decided and on what.

- **Which serial port pins 6 and 7 select the mode for.** Pin 7 selects port A and pin 6
  selects port B, following the IRIX and Linux headers, which agree; OpenBSD's register
  header says the opposite and is the odd one out. Nothing observable depends on it,
  because every driver programs both pins identically and to the same value. Settled in
  the Origin 200 machine-contract sign-offs.
- **Whether the emulated one-wire bus answers.** Settled once as *stays silent, firmware
  supplies the address*, and **superseded**: OpenBSD has no firmware path for the station
  address on this machine, so a silent bus produces an all-ones address printed on the
  console. The bus must carry an Ethernet-address NIC and must not carry a board-serial
  part, as the one-wire section above sets out.
- **Where the chip sits.** PCI device 2 behind the Bridge at crosstalk widget 8. Settled in
  the same sign-offs.
- **Whether the MicroLAN register layout is documented.** It is dual-sourced: mainline
  Linux's one-wire bus master driver reads the register exactly the way OpenBSD's
  bit-banging routine does. This is no longer an inference.
- **Whether the two 16550 shadow-register writes OpenBSD issues on every boot should
  log a guest error.** No: they should stay read-only and stay silent. `ioc.c` writes
  zero to both ports' shadow registers, alongside both serial control registers, to put
  the ports into pass-through mode before attaching its ordinary 16550 driver — a real
  write, on every OpenBSD boot with a serial child present, confirmed at
  `sys/arch/sgi/pci/ioc.c` in the OpenBSD 7.9 GENERIC-IP27 tree. Mainline Linux's `mfd`
  driver reaches the same pass-through state without ever touching the shadow register —
  it clears only the DMA-enable bit in the control register — which corroborates that the
  shadow holds nothing software needs to set. The register stays modeled read-only, since
  every header agrees it mirrors live UART lines the port drives, but the write is not
  guest misbehavior: it is a real driver's harmless, deliberate no-op, so the model
  discards it silently rather than logging a guest error that would fire on every OpenBSD
  boot.
- **Whether a write to the two hardware-owned serial DMA index registers is guest
  misbehavior.** Settled once as *yes, keep the guest-error log — no driver in any of the
  three trees we hold writes them, so a write there is a real anomaly rather than an
  expected pattern*, and **superseded**: the firmware is a fourth driver and nobody had
  checked it. SGI's base I/O diagnostic writes zero to both ports' transmit consume and
  receive produce indices on every stock boot, immediately after resetting the DMA
  channels — observed in a register trace of the diagnostic running on this machine,
  rather than read out of a header. The old claim was true of the three operating-system
  trees and false of the firmware, which is how it survived. **The log has to go**: a
  stock boot that logs guest errors teaches everyone to ignore the class.
  **This does not settle what the model should do with the write itself, and cannot.**
  The diagnostic writes zero onto registers the channel reset it just issued has already
  zeroed, so accepting the write and refusing it leave identical state and produce
  identical behavior; no trace distinguishes them. The model goes on refusing, which is
  what it already did, and that stays a choice rather than a finding. A driver that wrote
  a non-zero index is what would settle it.

## Open questions for review

1. **One suspect value in SGI's diagnostic table.** The writable-bit mask the
   diagnostic uses for serial port B's control register is the mask belonging to the
   index registers rather than the one it uses for port A's control register. It reads
   like a transcription slip in SGI's own table. Reset values are unaffected — every
   entry is zero — so this only matters if we lift the mask column into a qtest, which
   we should not do for that one register.

## qtest checklist

SGI's manufacturing diagnostic is a table of register, access type, writable mask and
reset value, walked by a generic test. **That table is the basis for the register
qtest**: it is a machine-checkable register contract written by the people who built
the chip, and reproducing its shape — not its contents — gives us the same coverage.

**For much of this chip the register test is not a second opinion, it is the only
instrument.** A field whose only consumer writes it and reads back its own value is
invisible to any boot, whatever the field is: a model that stores the bits and a model that
acts on them produce identical transcripts, so a green boot certifies nothing about it. That
is not a quirk of one register — it covers the scratch register's pre-divider overload, the
chip control register's serial port base offsets (the firmware writes them on every boot
and nothing reads them back; the model now acts on them, but a boot that never re-aims an
engine still cannot tell a model that does from one that does not), the pin control set
and clear pair that no source reads for its
value, the
ethernet station address and hash filters while no frame filtering is modelled, the random
seed, and the interframe gap fields. For every one of them a boot test and a wrong model
agree. Assert them here or they are not asserted at all.

The same reasoning is why the access-width item below issues a width nothing on the boot
path issues: the firmware and the guest's serial driver use byte accesses exclusively, so
the boot cannot see the defect that item exists to catch.

The qtest must cover:

- **Identity.** The PCI identity word reads `0x000310A9` and the implemented
  variant's revision/class word reads `0xFF000001`.
- **Reset values.** Every register in the map reads zero after reset, except four: the
  identity word (`0x000310A9`), the revision and class word (`0xFF000001`), the subsystem
  identity supplied by the containing board, and the Command and Status register at
  `0x04` (`0x02800000` —
  Command `0`, Status `0x0280`). The last is the value the IRIX `io_config_space` diagnostic
  requires; a model that reads `0` there fails the I/O slot. The two serial-DMA 16550 shadow
  registers are a further exception of a different kind: they are live mirrors of their
  ports' lines rather than stored zeros, so they read the reset line state rather than zero —
  assert them with the shadow-mirror check below, not here.
- **The serial-DMA 16550 shadow mirrors its port.** After reset, each port's shadow reports
  carrier-detect and clear-to-send asserted and the transmitter empty, and nothing received;
  a data-terminal-ready and request-to-send driven through the byte-wide modem-control
  register then appear in it. This is the register IRIX's DMA-ring console reads carrier from,
  and a shadow stuck at zero reports the line dead — so the check that catches it is that the
  shadow is non-zero and tracks the port, which no boot in loopback exercises.

The Origin 200 machine test, not this reusable chip checklist, verifies that board's
subsystem identity value.
- **The interrupt status register acknowledges.** With a latched status bit set — use the
  keyboard bit 22, which both Linux and IRIX treat as latched — writing a one to it clears
  it and writing a zero leaves it alone. With serial bit 6 or 15 set by a still-asserted
  16550, writing a one leaves it set. Do not assert that this register is read-only; that is
  the assertion that would certify a machine which banners and then wedges. The parallel
  pass-through at bit 18 behaves like the other two — assert it the same way, with the bit
  held by a still-asserted parallel port.
- **A source raised during a transmit survives the transmit.** Queue a descriptor whose
  frame the backend loops back, so a receive source is raised inside the send. **Arm the
  receive timer with a zero reload and program a watermark the delivery will cross**, so both
  receive sources are raised, then assert that **the receive-timer source, the receive-threshold
  source and the transmit sources are all set** when the transmit walk returns.
  Asserting only that the transmit source is set is the weaker check and passes on a model
  that writes a pre-send snapshot of the register back over the receive bit — which is the
  defect this exists to catch, and it is invisible in every other observable.
  **Naming "the receive source" without saying which is the same weakness one level down**:
  the two are separately losable, demonstrated by mutating a model to preserve the threshold
  source through the walk while dropping a timer source raised inside it, which a test
  asserting whichever source the implementer happened to raise will pass. The timer source is
  the one to name if only one is asserted, because it is the one Linux dispatches on.
- **A delivered frame's header is asserted, not just its arrival.** Check the valid bit, the
  byte count including the four-byte FCS, and **the good-packet bit** in the status vector.
  **Linux drops every frame without the good-packet bit and OpenBSD never reads it**, so it is
  a field only one of the two guests can miss — and a suite written against the other guest
  passes complete while that guest cannot receive at all. That is not hypothetical: this
  device shipped a whole suite in which nothing asserted that bit.
- **Read-only registers stay read-only.** Writes to the
  transmit consume and receive produce index registers of both serial ports, the
  ethernet collision and buffer index registers, and the 16550 shadow registers leave
  them unchanged.
- **The collision and deferral counter clears on read.** A second read returns zero.
- **Write-one-to-set and write-one-to-clear pairs.** Writing a bit to the interrupt
  enable set address sets it in the mask, writing the same bit to the clear address
  clears it, and the set address reads back the live mask after each. Assert the same
  set-and-clear behaviour for the pin control pair through its effect on the pins.
  Do not assert what the clear address returns on a read, or what either pin control
  address returns, until a guest pins those down.
- **The normal interrupt output follows its timed contract.** A fixed-low or fixed-high
  command makes current-state bit 31 report the commanded state by the next read of the
  register, as the interrupt-output section records. Do not require a timed mode's first edge
  within the MMIO write. A one-shot pulse keeps
  bit 31 high for three 7.8 microsecond ticks and then returns it low. Periodic-pulse mode
  starts successive three-tick pulses at `(COUNT + 1)`-tick intervals. Square-wave mode
  toggles at each `(COUNT + 1)`-tick interval. A new valid mode cancels the mode already
  running. Any assertion that depends on the programmed interval must use raw `COUNT`
  values from `0x0002` through `0xfffe`. Leave `0`, `1` and `0xffff` unasserted.
  Reset cancels any model timer. Identical-build migration preserves the output level
  and enough timer phase to avoid a duplicate or lost transition. Reset and migration
  are QEMU lifecycle requirements, not public claims about IOC3 hardware state.
- **Preserve the existing register-write compatibility check.** QEMU fork revision
  `9aca6618bc6d8f84e192dea4ff925970d6a653e6`,
  `tests/qtest/sn0-ioc3-test.c`, lines 311–331, already expects writes to bit 31 and
  reserved bits to be ignored. Keep that model-compatibility expectation, but do not
  treat it as public hardware evidence. The public sources name bit 31 as the current
  output state and do not define write behavior for bit 31 or the reserved field.
- **The internal real-time loopback uses the output-pulse source.** With bit 28
  disabled and bit 27 enabled, one-pulse mode posts bit 27 at the rising edge,
  asserts the SuperIO PCI interrupt, and leaves bit 28 clear. A write-one-to-clear
  acknowledgement deasserts the PCI interrupt. Reset clears the status and enable
  state and cancels the pulse. Migration during a pulse preserves its remaining
  time and produces neither a duplicate rising edge nor a lost falling edge.
  Keep diagnostic-mode bit 30 outside this test.
- **Narrow registers ignore the bits they do not own.** The pin data register keeps
  only nine bits. The serial index registers keep only the pointer field for the ring
  size currently selected. The serial receive timer keeps twelve bits — the ethernet one at
  `0x114` is a different register and keeps eleven.
- **The PROM's control-register value round-trips.** Writing port A at `0x178`, port B
  at `0x170` and strobe length 4 produces `0x00022E5E` on read-back.
- **The SuperIO window is byte-wide.** Byte writes to consecutive offsets in a 16550
  file land in distinct registers, and the two files at `0x170` and `0x178` do not
  alias each other. Assert the firmware's own first access explicitly: a byte store of
  `0x03` at port A's offset `+3` selects eight bits, no parity, one stop bit, and must
  reach the line-control register alone.
- **Do not assert anything about wide accesses to a byte-wide region from a register test.**
  An earlier version of this checklist did, twice, and both versions were tests that could
  not fail on the defect they named. The reason is structural: a register test reaches the
  device through the address space, which clamps each transfer to the region's *valid*
  maximum and to the address's alignment before dispatching, and then loops. So a four-byte
  write to a one-byte-valid region arrives as four one-byte writes at consecutive addresses
  and succeeds — **whatever the device does**, because the harness performed the split. The
  assertion passes for a correct model and an incorrect one alike, and it is measuring the
  memory system rather than the chip.

  The behaviour that is actually specified — a wide access being refused and logged — takes
  the processor path, arrives as a single dispatch, and is refused by the validity check.
  That path exists only for guest-side code, so **the check belongs in a bare-metal test ROM
  or a boot assertion, not here.** What a register test can still hold is the item above:
  byte accesses at consecutive offsets land in distinct registers, in every byte-wide region
  including the four byte-bus selects and the 32 KB clock window behind select 0, which is
  the one most likely to be mapped as a plain block.
- **The scratch register overload.** With the divisor-latch bit set, a write to the
  scratch offset changes the port's baud clock; with it clear, the same write is stored
  and read back unchanged.
- **The serial ring block is one allocation of four rings.** With a base written and
  4 KB rings selected, the four rings resolve to base, base plus 4 KB, base plus 8 KB
  and base plus 12 KB for port A transmit, port A receive, port B transmit and port B
  receive respectively, and the low bit of the base register does not appear in any
  resolved address.
- **PS/2 receive packing.** Three bytes delivered in order read back with the first in
  bits 16–23 and the third in bits 0–7, with the matching valid bits set.
- **Alias pages mirror the main block.** A write through the serial port A alias page
  is visible at the corresponding main-block offset and the reverse, and the three
  registers that are read-only through the alias — the transmit consume index, the receive
  produce index and the 16550 shadow — reject writes made through it.
- **Both interrupt outputs exist and each is a level.** Setting a SuperIO status bit with
  its enable bit clear does not raise the SuperIO line; setting the enable then raises it;
  clearing either lowers it. Assert the same for the ethernet status and enable pair on the
  ethernet line, and assert that an ethernet source never raises the SuperIO line or
  appears in the SuperIO status register.
- **The byte bus decodes four selects.** Reads and writes at `0x80000`, `0xA0000`,
  `0xC0000` and `0xE0000` reach four distinct targets and do not alias one another. The
  Origin 200 machine wires only select 0.
- **The ethernet buffer RAM window is real storage, and its two halves are distinct.** A
  pattern written at `0x40000` and its complement written at `0x60000` both read back
  afterwards, which is the sizing probe SGI's own driver and OpenBSD's both perform and
  the only assertion that fails on a window aliasing at its halfway point. Assert Linux's
  pair — `0x40000` and `0x50000` — as well, since that is what a second guest issues, but
  do not assert only that pair: `0x50000` is inside the smaller RAM, so a model that
  stopped there would pass it. Assert the window's last slot at `0x7FFFC` too, so the
  region is shown to run to its end rather than to somewhere past the probe.
- **The buffer RAM slots store seventeen bits.** With parity checking disabled, writing
  all ones to a slot reads back `0x0001FFFF`: sixteen data bits and the stored parity bit,
  with nothing above them. The parity bit is storage — writing it and reading it back
  returns what was written.
- **The parity checker reports through bit 17, and only when it is enabled.** With
  buffer-RAM parity checking enabled in the MAC control register, a slot holding an odd
  number of ones across its seventeen stored bits reads back with bit 17 set, and one
  holding an even number does not. Assert both, and assert the even case as well as the odd
  one: a model that set the bit unconditionally passes a test that only checks the odd case
  and then fails the firmware on its first two writes. Assert that clearing the enable takes
  the report away again without disturbing the storage under it, and that a write with bit 17
  set reads back with it clear.
- **The size probe survives parity checking.** Run the 64 KB-against-128 KB probe with
  checking enabled, which is how the firmware runs it, and assert both words read back
  exactly. This is the assertion that catches a checker reporting unconditionally, whose
  visible symptom is a guest printing the wrong RAM size rather than anything about parity.
- **The buffer RAM window does not wrap.** A value written to the last slot at `0x7FFFC`
  is not visible at the first slot, and the two hold different values at the same time.

Boot-test assertions come after all of these pass: first that the firmware's console
init sequence produces the register state this datasheet describes, then that a
banner appears on the
serial port.
