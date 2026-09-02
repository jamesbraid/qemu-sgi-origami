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

A MicroLAN engine drives a DS2502-class part holding the station
address. Time slots complete synchronously within the register
write; a guest's read-after-write polling cannot observe real
timing. The part answers Search ROM (SGI's PROM continues straight
from search to a function command), Match ROM (OpenBSD's ``owmac``),
and Skip ROM (Linux's w1 core, whenever a bus has one device); an
unimplemented command leaves the wire idling high and is logged,
because it is otherwise indistinguishable from an absent part.

The ROM identity is family 0x91 with the serial's required top
twelve bits 0x5e7; the serial's remainder comes from the machine.
Two CRCs are in play and differ: the CRC-8 over ROM bytes is sent as
computed, while the page's embedded CRC-16 is stored inverted so the
guest's running check lands on the fixed residue 0xb001. The two
memory-read commands (checked and unchecked framing) both return the
same stored page; redirection status reads all ones, which is the
value that tells OpenBSD's redirect walk the page is valid where it
sits.

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

Board variants
==============

The same chip sits on boards that wire up different subsets of it,
and a guest works out which board it is on from the subsystem
identity in configuration space. The ``subsystem-id`` property
carries that identity and the model selects the population from it:
the ethernet block and, with it, the one-wire station-address part;
each 16550 with its DMA registers and alias page; the PS/2
controller; and the M48T35 on byte-bus device 0. What a board does
not fit is neither realized nor mapped, and the window it would have
answered is a hole like any other. An identity the table does not
name gets the fall-through population rather than a refusal,
matching the dispatch in mainline Linux's multifunction driver the
table is taken from.

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
