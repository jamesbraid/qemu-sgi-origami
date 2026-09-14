===========================
SGI IOC3 base I/O controller
===========================

The IOC3 is one PCI device with a single BAR covering serial ports,
an ethernet MAC, PS/2 ports, a one-wire bus holding the station
address, byte-bus device selects (the M48T35 timekeeper among them),
and buffer RAM. QEMU models it as a composite: the PCI shell and
SuperIO blocks in ``hw/misc/sgi-ioc3.c``, the ethernet engine as a
child device in ``hw/net/sgi-ioc3-eth.c`` with its DP83840 PHY child
in ``hw/net/dp83840.c``. Register-level sources are collected in the
Origami project tree's IOC3 datasheet.

Unimplemented regions of the window are holes, not zeroes: an access
there reaches no device and is reported. On a machine this small,
zero is the right answer to a great many questions, which is what
makes answering all of them dangerous.

Serial ports
============

The SuperIO window holds two 16550 register files at a *one-byte*
stride, where the chip's other blocks are 32 bits wide, and only
byte accesses are served -- the most likely way to produce a silent
console is to get either wrong. The machine is big-endian and the
ports declare little-endian; byte-lane placement is the bus's and
the guest's business, and the device must not add its own.

The SuperIO interrupt line follows the intersection of the status
and enable registers and drops when it empties; it is recomputed on
every write and source change rather than raised per event. A serial
port's bit is a pass-through of the 16550's own line -- no status
write clears it; the guest's own register reads are what quiet it.
The interrupt-enable and pin-control register pairs are set/clear
aliases with a read asymmetry: the set alias reads back the live
mask, the clear alias does not.
This line is board wiring to a Bridge pin, not the chip's PCI
interrupt pin; the PCI pin belongs to the ethernet block.

Transmit record
===============

The ``record-a`` and ``record-b`` properties take an optional second
character device per port. Every character the chip hands that port's
UART to transmit is written there as well as to the port's own
backend, and nothing the port's consumer does can refuse it. A
character sent in loopback is left out, because no wire carried it.

This exists because a port's own backend can refuse bytes and the
transmitter can then abandon them. A 16550 does not wait for a
listener -- it clocks each character out at the programmed baud rate
whatever is on the far end of the wire -- so the model's transmit
hold is bounded and gives up, and a consumer that stalls past the
bound costs that consumer those characters. A durable log taken from
the port's backend loses them with it. The record is taken before
that decision, so it is the account of what the guest transmitted.

Point it at a backend that cannot apply backpressure of its own, a
file being the intended case: the write is completed rather than
watched, so a record backend that blocks would stall the transmit
path it exists to observe. Two limits are worth stating. A guest that
writes the transmit holding register while the transmitter is full
has that character overwritten on the wire, here and on the part, but
it still appears in the record; no guest on this machine does that,
and the DMA engine cannot, because it waits for the transmit-ready
level first. And the record follows the character the chip handed
over, so a byte the backend later refuses and the hold bound later
abandons is in the record and not on the wire -- which is the point.

Serial DMA engines
==================

Each port has an engine moving characters between the port's 16550
and a pair of memory rings. One base register places all four rings
-- port A transmit, A receive, B transmit, B receive, in that order,
each of the size the base register's low bit selects -- so a ring's
address is derived from port and direction and is not readable back.
The chip-control register's port-offset fields are stored, not acted
on: real silicon multiplexes the ports through them, and every guest
programs the two offsets the model implements.
Ring entries are eight bytes; an index is exactly as wide as the
ring is long. The receive timer's reload is 12 bits, with the running
count readable in bits 16-27; a pause request (control bit 29) is
acknowledged in bit 30 while the walk is stopped.

The console path never uses these engines: firmware and both guests
drive the ports through the byte windows with ring DMA off. What
exercises them is the firmware's base I/O diagnostic (both ports in
loopback, full rings) and IRIX's DMA-ring console.

Transmit
    Each entry carries four control bytes typed per data byte:
    a character, a modem-control write, a delay, or nothing -- the
    empty type is how a driver ends a short entry, and an engine
    sending all four regardless puts memory garbage on the wire. A
    descriptor fetch the fabric refuses stops the walk with the
    consume index on the entry. In loopback the receiver is drained
    per transmitted character, because the receive FIFO is sixteen
    bytes against rings the diagnostic fills five hundred characters
    at a time.

Receive
    Characters pack four to an entry; the produce index reports the
    packer's held count so a driver can see the difference between a
    packing engine and one writing an entry per character. A full
    ring, or a fabric-refused entry write, refuses the delivery and
    keeps the characters -- in the packer, and behind it the FIFO,
    whose fullness is what stops the chardev feeding more. The
    receive timer counts idle time on the 16 kHz clock from the last
    delivery, is armed through bit 31 of the consume index, and on
    expiry raises its source only while characters are waiting.
    Re-arming it over a non-empty ring rewinds it, prompting the
    driver again for the remainder -- a model choice no source
    states. The model posts a short group when the engine goes idle
    rather than on expiry; the interrupt still waits for the timer,
    so an interrupt-driven driver sees datasheet timing while an
    index-polling guest sees a short entry earlier than silicon
    shows one. The
    high-water source is a *level* in characters (not entries),
    recomputed wherever the waiting count moves: a latched raise
    storms a guest whose service loop cannot clear it, which is
    exactly IRIX's ring-console behavior against a latching model.

Two control-register bits are commands, not settings: the channel
reset returns the four indices to the ring start while preserving
the watermark and timer reload, and the receive drain (bit 27) moves
whatever the port and packer hold into the ring and reads back clear
immediately -- IRIX polls it to zero and tears the console down
forever against a model that stores it.

The 16550 shadow register is a read-only mirror of the port's status
and modem lines, assembled fresh per read -- deliberately not read
through the byte-wide ops, whose modem/line-status reads clear delta
and error latches. The DMA-side console driver reads carrier from
here, so a shadow left at zero hangs console open on a cached
carrier-down. Writes to it, and to the two hardware-owned indices,
are discarded without a guest-error report: stock firmware and
OpenBSD write them on every boot.

Each port also has an alias page: a second view of its seven
registers for mapping one engine into user space without the rest of
the chip. Through the page the shadow is read-only where the main
block tolerates the write.

Station-address one-wire part
=============================

A MicroLAN engine drives a one-wire bus carrying one DS2501, the
512-bit sibling of the DS2502, which holds the station address in its
first page. Time slots complete synchronously within the register
write; a guest's read-after-write polling cannot observe real
timing. The part answers Search ROM (SGI's PROM continues straight
from search to a function command), Match ROM (OpenBSD's ``owmac``),
and Skip ROM (Linux's w1 core, whenever a bus has one device); an
unimplemented command leaves the wire idling high and is logged,
because it is otherwise indistinguishable from an absent part.

The ROM identity is family 0x91 -- the custom bit over base type
0x11 -- with the serial's required top twelve bits 0x5e7; the
serial's remainder comes from the machine. The two memory-read
commands are not interchangeable: Read Data with CRC closes every
32-byte page with a CRC-8 over that page's data, while plain Read
Memory carries no per-page CRC at all and emits a single CRC-8 only
where a read reaches the end of memory. Read Status serves the
eight-byte status field -- the write-protect byte, four page
redirection bytes, two reserved bytes and a factory-programmed 0x00
-- closed by a CRC-8 over the bytes read. Only that last byte is
programmed, so the redirection byte OpenBSD's redirect walk reads is
unprogrammed EPROM, and its all-ones value is what tells the walk the
page is valid where it sits.

The station-address record in page zero is image content rather than
device behaviour: a marker byte, the address reversed into bytes 5
through 10, and a checksum over the first eleven bytes at bytes 11
and 12. That checksum is the record format's own, not a CRC the part
computes -- an embedded page checksum is a software file-structure
convention, and a part's own CRCs always cover a whole page. It is
stored inverted so a guest's running check lands on the fixed residue
0xb001.

Ethernet engine
===============

A transmit ring of 128-byte inline descriptors (128 or 512 of them by
the base register's low bit) and a receive ring of 512 guest-supplied
buffer addresses indexed by byte offset. All DMA goes through the
chip's own PCI address space -- through the Bridge's translation,
never the plain system space.

Descriptor geometry the drivers depend on: the inline data area is
104 bytes at offset 24 with a 7-bit length field; the command word's
total byte count is 11 bits and an over-count wraps as written; a
buffer transfer may not cross a 16 KB boundary, which is why a
descriptor carries two buffer pointers; pad-enable extends a short
frame to 60 zero-padded bytes ahead of the FCS; insert-checksum
completes a correct UDP checksum from the driver's pseudo-header
partial. On receive, the frame begins 66 bytes (33 halfwords,
OpenBSD's value) into the buffer; the received-count field is 11 bits
and also arrives split across the error word (three low bits at 12,
ten high at 16), and delivery truncates at 2040 bytes with the
long-event and bad-packet bits set. The receive timer is 11 bits
wide. The threshold source compares at-or-below the watermark:
OpenBSD writes its whole available count as the mark and asks to be
told when the count reaches it.

Receive filtering happens before any memory is written: station
address (from the guest-programmed MAC registers -- never the
backend's address, which has no relation to what the driver believes
its address is), broadcast, promiscuous, and the multicast hash --
the low six bits of the reflected ethernet CRC, themselves
bit-reversed, replicating Linux's ``ioc3_hash()``. A filtered frame
reports itself *consumed*: returning zero would wedge the queue on a
frame no retry can ever pass.

Two length and checksum facts follow from QEMU's net layer
delivering no FCS:

* Both guests read the byte-count field as the wire length including
  the four-byte FCS, so the model reports four phantom bytes (backed
  by zeroes in the buffer). Without them, every minimum-length frame
  -- DHCP's short broadcasts, typically -- fails the guest's own
  length check and vanishes.
* The receive checksum-offload field is the real 16-bit one's
  complement sum over the whole packet, which Linux finishes into
  the transport checksum and trusts only when its reconstruction
  folds to 0xffff; the phantom FCS is summed as zero, which the
  driver's own subtraction makes self-consistent.

The two loopbacks differ observably and the firmware requires it:
the MAC's own loopback short-circuits ahead of FCS and status
generation (the diagnostic demands the byte count unchanged and a
zero status word), while the PHY's loopback runs the whole
controller (count four higher, status real). Either loopback
disconnects the backend in both directions, or outside traffic
shifts the diagnostic's frame comparisons.

The MAC-control reset bit resets the engine but not the ring base
registers: SGI's power-on diagnostic programs the bases, asserts
reset, rewrites only the indices, and never writes the bases again.
A receive descriptor fetch the fabric refuses drops the frame -- the
block carries no status a driver reads for a refused transfer. The
receive-timer arm bit is a command and the MII busy bit derived,
neither storage: a bare index write must not disarm the timer, and
busy never reads set because requests complete inside the write.

A default-generated MAC address takes SGI's OUI (08:00:69), since
the firmware reads the station address from the one-wire part and
expects a real part's prefix; an explicitly configured address is
never rewritten.

The PHY is a DP83840 modelled at address 31 with mii.h register
names; its autonegotiation completes on a settle window timed so the
firmware diagnostic's probe lands inside it, its link-status bit
latches low, and backend link state drives it the way lan9118's PHY
models the same thing. Its LBREMR pin-latched bits reflect board
wiring nothing observed, modelled as normal operation, and the
settle window has a second constraint besides the diagnostic probe:
the firmware's reset-value sweep spends about a tenth of a second of
virtual time, which the settle must outlast.

Buffer RAM
==========

The ethernet buffer RAM is storage, seventeen bits per slot: sixteen
data bits and the stored parity bit beside them. No frame data ever
lands here -- the engine's rings live in guest memory -- so parity
is not computed but stored, and the checker reports odd parity
across all seventeen bits when the ethernet block enables checking.
The two halves of the window are separate storage because a driver's
size probe writes a pattern at the first slot and its complement at
the midpoint; an aliasing window reports half the RAM. Reset does
not clear it: it is memory, not a register file.

PS/2 ports
==========

The controller (``hw/input/sgi-ioc3-kbc.c``) wraps two generic ps2
devices, a keyboard and a mouse, and presents SGI's five registers: a
control/status register, a receive and a transmit register per port.
A transmit write forwards its low byte to the port's ps2 device,
whose replies queue up. A receive read packs up to three queued bytes,
oldest in the highest slot, each with its valid flag, and sets the
overflow flag when a fourth is still waiting; the read pops the queue,
so the next read starts from empty. The two ports share one interrupt,
chip interrupt-status bit 22, which follows the controller's
data-available line -- asserted while either port holds bytes, cleared
once the receive registers drain -- and which the guest also clears by
writing it. The block is a bit-level line engine, not an 8042 clone:
the byte-level PS/2 command protocol lives in the ps2 devices above
it, and the control register's write-pending status reads clear, since
a synchronous transmit completes within the register write.

Parallel port
=============

The port (``hw/char/sgi-ioc3-parallel.c``) has two faces that sit
far apart in the chip's window, so the device presents two regions:
a four-byte-wide DMA register block belonging to the main register
block, and the byte-wide IEEE-1284/ECP register file belonging to
the SuperIO window. What is modelled is the register contract,
because that is all a source establishes. IRIX's driver probes for
the port by reading the extended control register and taking
"transmit FIFO empty, not full" as the port answering; without that
the driver never attaches and opening the device node fails.
Everything past the probe -- the mode field, the negotiation lines,
the two DMA contexts -- is a register file a driver writes and reads
back. The ECP file is byte-wide and declares its implementation
width: an undeclared maximum would take one four-byte access apart
into four calls and run the FIFO write four times over.

Nothing is on the cable. Four behaviours follow from that rather than
from the specified hardware:

* The status register reads a constant idle cable: no fault, no
  peripheral selected, no paper error, never busy. Only nFault's
  level is pinned by a source -- clear means a fault -- and
  never-busy is the level that stops a driver waiting for the
  printer from waiting forever.
* The transmit FIFO is bottomless. A byte written to it leaves
  within the write and is discarded, empty always reads set and full
  never does, because no source gives the FIFO a depth or a drain
  rate. A driver polling for room before each byte never waits, and
  one polling for the queue to drain finishes on its first look.
* The DMA engine stores its two contexts and moves no bytes: both
  busy flags stay clear and a count reads back what software loaded,
  nothing having been subtracted from it, because there is no
  peripheral to move bytes to.
* No interrupt is ever raised. The chip reserves four sources for
  the port and nothing here drives one. The active sense of the
  extended control register's service and nFault interrupt controls
  is unresolved in the sources, so the model stores those bits and
  arms nothing rather than picking a polarity a guest would then be
  measured against.

Two boards fit the port: the Origin 200's own IP29 system board and
the IP27 MediaIO daughtercard. No other identity in the table gets
one, and on a board without it both register regions are holes like
any other. Mainline Linux's multifunction driver registers no
parallel sub-device for any identity, so a Linux that drives nothing
here is not evidence about the block either way.

Board variants
==============

The same chip sits on boards that wire up different subsets of it,
and a guest works out which board it is on from the subsystem
identity in configuration space. The ``subsystem-id`` property
carries that identity and the model selects the population from it:
the ethernet block and, with it, the one-wire station-address part;
each 16550 with its DMA registers and alias page; the PS/2
controller; the parallel port; and the M48T35 on byte-bus device 0.
What a board does not fit is neither realized nor mapped, and the
window it would have answered is a hole like any other. An identity
the table does not name gets the fall-through population rather than
a refusal, matching the dispatch in mainline Linux's multifunction
driver the table is taken from.

One-wire is not selected separately. The station-address part is
wired up with the ethernet, so a board without ethernet has no part
on its bus and the wire reads idle -- the same answer the pin-control
gate gives, reached the same way.

Two of the Octane board's sub-devices are a deliberate gap: its clock
is a DS1685 at two other byte-bus selects, which no device here
models, and its front-panel LEDs are not modelled either. That board
gets everything else and an empty window where those belong, rather
than an M48T35 answering to a layout it does not have.

The default is the Origin 200's own board, so the machine and every
test that does not ask for another are unaffected.

PCI identity
============

The chip's PCI Status register must read 0x0280 at reset (medium
DEVSEL, fast back-to-back), both bits read-only: IRIX's
configuration-space diagnostic compares the whole dword containing
it and fails the slot on a zero Status half. Chip-control bit 22 (PCI
arbiter idle) is read-only and always reads set: the model has no
serial-DMA engine driving PCI traffic, and a kernel's console setup
does not complete until it reads idle.

M48T35 timekeeper
=================

The timekeeper sits on the IOC3's byte bus (``hw/rtc/m48t35.c``):
32 KB of battery-backed byte-wide cells with the clock in the top
eight, a control byte first and the six fields after it. Everything
below the clock must answer as storage, and it is probed: SGI's
driver identifies the clock part by writing 0xff at offset 0x6 with
offset 0xb zero and reading 0x6 back. The bank of eight just below
the clock is unidentified by any source and reads as storage too.

Control bit 6 freezes the clock so that six reads describe one
instant; bit 7 arms a write, whose fields are collected and applied
together when the bit clears. Both read paths share one offset from
the host clock, so releasing a freeze cannot make the clock jump. A
clock field written with the write bit clear is dropped: the
counters are running and the hardware has nowhere to put it. The
weekday register is stored, not counted; the part leaves it alone
until software writes it.

The two-digit year counts from 1970 and every driver adds it back;
calendar digits instead would boot cleanly and report the mid-1990s.
A guest-set time is kept as an offset differenced through the same
clock the read path uses (whichever ``-rtc clock=`` selected), not
through the host clock unconditionally. The optional battery is a
drive of exactly 32 KB, the same contract as ``mac_nvram``; reset
preserves cells and time but cancels an unfinished freeze or write
transaction, or the next guest inherits a stopped clock.
