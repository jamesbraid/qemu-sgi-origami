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
This line is board wiring to a Bridge pin, not the chip's PCI
interrupt pin; the PCI pin belongs to the ethernet block.

Serial DMA engines
==================

Each port has an engine moving characters between the port's 16550
and a pair of memory rings. One base register places all four rings
-- port A transmit, A receive, B transmit, B receive, in that order,
each of the size the base register's low bit selects -- so a ring's
address is derived from port and direction and is not readable back.
Ring entries are eight bytes; an index is exactly as wide as the
ring is long.

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
    expiry raises its source only while characters are waiting. The
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

A transmit ring of 128-byte inline descriptors and a receive ring of
guest-supplied buffer addresses. All DMA goes through the chip's own
PCI address space -- through the Bridge's translation, never the
plain system space.

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
block carries no status a driver reads for a refused transfer.

A default-generated MAC address takes SGI's OUI (08:00:69), since
the firmware reads the station address from the one-wire part and
expects a real part's prefix; an explicitly configured address is
never rewritten.

The PHY is a DP83840 modelled at address 31 with mii.h register
names; its autonegotiation completes on a settle window timed so the
firmware diagnostic's probe lands inside it, its link-status bit
latches low, and backend link state drives it the way lan9118's PHY
models the same thing.

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

Nothing is plugged in. The receive registers read zero -- every
per-byte valid flag clear, a port that has received nothing -- and
transmit writes are accepted and dropped, because a completing write
is what a driver needs from an unplugged port.

PCI identity
============

The chip's PCI Status register must read 0x0280 at reset (medium
DEVSEL, fast back-to-back), both bits read-only: IRIX's
configuration-space diagnostic compares the whole dword containing
it and fails the slot on a zero Status half.
