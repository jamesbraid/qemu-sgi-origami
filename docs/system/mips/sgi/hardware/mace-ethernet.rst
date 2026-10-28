.. SPDX-License-Identifier: BSD-3-Clause

.. _sgi-mace-ethernet:

==========================================
MACE Ethernet MAC (MAC-110, O2)
==========================================

The MAC-110 is the 10/100 Mb/s Ethernet MAC inside the MACE I/O ASIC of
the O2 (:doc:`mace`). It reaches an external PHY over a two-wire
management bus (:src:`sgi-mace-2.0-spec p. 76`) and moves frames by DMA
from a transmit ring and into receive buffers
(:src:`sgi-mace-2.0-spec p. 70`), *established*. QEMU
models it as ``sgi-mace-eth`` in ``hw/net/sgi-mace-eth.c``, a child of
``sgi-mace`` mapped at MACE offset ``0x280000``. The only machine that
fits it is the O2 test profile (:ref:`sgi-ip32`). Facts are rated as
described in :ref:`sgi-ratings`.

.. _sgi-mace-ethernet-sources:

Sources
=======

- The *MACE I/O ASIC Specification*, chip revision 2.0,
  :src:`sgi-mace-2.0-spec`, chapter 4 (pages 70 to 81). Page numbers
  below are its PDF pages.
- Linux ``arch/mips/include/asm/ip32/mace.h`` (``struct mace_ethernet``),
  ``drivers/net/ethernet/sgi/meth.c`` and ``meth.h`` at ``db2ddb871435``;
  NetBSD ``sys/arch/sgimips/mace/if_mec.c`` and ``if_mecreg.h`` at
  ``07004b2a9dac``; OpenBSD ``sys/arch/sgi/dev/if_mec.c`` at
  ``2df91d80c104``; all as software expectations.
- Two published boot reports of real O2s, OpenBSD 5.2
  (:src:`dmesgd-2432`) and NetBSD 9.99.31 (:src:`dmesgd-5290`),
  *observed*.

Identity and placement
======================

MAC control bits 31:29 are a read-only implementation revision: 0 is the
initial implementation and 1 the first revision with improved transmit
concatenation :src:`sgi-mace-2.0-spec p. 71`, *established*. Both captures
report "MAC-110 rev 1" (:src:`dmesgd-2432 mec0`, :src:`dmesgd-5290 mec0`),
*observed*, at MACE offset ``0x280000`` with interrupt 3. The model reports
revision 1.

The block occupies 512 KiB of MACE address space and the registers repeat
through it :src:`sgi-mace-2.0-spec p. 181`. The model decodes the low
9 bits of the offset.

Register map
============

All registers are 64 bits wide and defined on 64-bit boundaries
:src:`sgi-mace-2.0-spec p. 70`, *established*. Linux ``struct
mace_ethernet`` and NetBSD ``if_mecreg.h`` use the same offsets, *software
expectation*. The specification lists offset ``0x98`` twice; the model
leaves both incoming-message-queue registers (``0x80`` to ``0x98``)
unmodelled.

.. list-table::
   :header-rows: 1

   * - Offset
     - Name
     - Reset
     - Model
   * - ``0x00``
     - MAC control
     - ``0x20000001``
     - R/W, see below.
   * - ``0x08``
     - Interrupt status
     - 0
     - Write one to clear bits 7:0.
   * - ``0x10``
     - DMA control
     - 0
     - Low 16 bits.
   * - ``0x18``
     - Receive interrupt delay
     - 0
     - Low 6 bits.
   * - ``0x20``
     - Transmit interrupt alias
     - n/a
     - Write-only; writes DMA control bit 0.
   * - ``0x28``
     - Receive interrupt alias
     - n/a
     - Write-only; writes DMA control bits 9:4.
   * - ``0x30``, ``0x38``
     - Transmit ring read and write pointer
     - 0
     - Read: read pointer in 24:16, write pointer in 8:0. Write: write
       pointer.
   * - ``0x40``, ``0x48``, ``0x50``
     - Receive FIFO information, three copies
     - 0
     - Read only.
   * - ``0x58``
     - Interrupt request (write), last transmit vector (read)
     - 0
     - See below.
   * - ``0x60``
     - PHY data
     - 0
     - Bit 16 busy.
   * - ``0x68``
     - PHY address
     - 0
     - Low 10 bits.
   * - ``0x70``
     - PHY read start
     - n/a
     - Write-only.
   * - ``0x78``
     - Backoff seed
     - n/a
     - Write-only; logs ``LOG_UNIMP``.
   * - ``0xa0``
     - Station address
     - 0
     - 48 bits.
   * - ``0xa8``
     - Secondary address
     - 0
     - 48 bits; a non-zero value logs ``LOG_UNIMP``.
   * - ``0xb0``
     - Multicast hash mask
     - 0
     - Stored; a non-zero value logs ``LOG_UNIMP``.
   * - ``0xb8``
     - Transmit ring base
     - 0
     - Bits 31:13.
   * - ``0x100`` to ``0x1f8``
     - Receive cluster FIFO data port, 32 copies
     - n/a
     - See below.

The transmit diagnostic registers (``0xc0`` to ``0xf8``) are absent: access
logs ``LOG_UNIMP`` and reads return 0. The model accepts misaligned
accesses to registers only as listed under `Access widths`_.

MAC control
-----------

Bit 0 (reset 1) holds the MAC core in reset. Bit 1 is full duplex, 2 the
internal loopback, 3 the 100 Mb/s select, 4 the SIA select (or loopback
collision), 6:5 the destination filter mode, 7 link-failure enable,
and 14:8, 21:15 and 28:22 the three inter-packet gaps
:src:`sgi-mace-2.0-spec p. 71`, *established*. The model stores the low
29 bits and forces bits 31:29 to 1. It uses the filter mode and the link
failure enable. It does not use duplex, speed or the gaps. Setting
loopback or SIA logs ``LOG_UNIMP`` and, while either is set, the model
treats the link as down.

The destination filter modes are: 0 physical address only, 1 physical,
broadcast and multicast hash matches, 2 physical, broadcast and all
multicast, 3 everything :src:`sgi-mace-2.0-spec p. 71`. The model accepts
the station address, the broadcast address in modes 1 to 3, all multicast
in mode 2 and everything in mode 3. In mode 1 a multicast frame is dropped
and ``LOG_UNIMP`` logged, since the hash is not modelled.

Station address
---------------

The specification compares physical-address bits 7:0 with the first byte
on the wire :src:`sgi-mace-2.0-spec p. 77`. Linux ``meth.c`` and OpenBSD
pack the first byte in bits 47:40 instead, *software expectation*. The
model follows the drivers: octet *i* of the destination address is
compared with bits ``8 * (5 - i)`` of the register. The specification's
prose and the drivers disagree, *unknown* for the chip; a read of the
register on hardware would settle it.

Interrupt status
================

Bits 7:0 are event flags, cleared by writing one
:src:`sgi-mace-2.0-spec p. 72`, *established*:

.. list-table::
   :header-rows: 1

   * - Bit
     - Event
     - In the model
   * - 0
     - Transmit ring empty
     - Set when interrupt enable and DMA enable are on and the ring is
       empty with the link up.
   * - 1
     - Transmit packet with the interrupt request flag sent
     - Set per packet.
   * - 2
     - PHY link failure
     - Set when the link drops with MAC control bit 7 set.
   * - 3
     - Transmit memory error
     - Set on a DMA fault.
   * - 4
     - Transmit abort
     - Set on an invalid descriptor.
   * - 5
     - Receive threshold
     - See `Receive`_.
   * - 6
     - Receive cluster FIFO underflow
     - Set when a frame arrives with no buffer queued.
   * - 7
     - Receive DMA FIFO overflow
     - Never set.

Reads also return the receive FIFO read pointer in bits 12:8, the
transmit read pointer in 24:16 and the receive sequence number in 29:25.
Bit 30, the multicast hash output, is never set. Bits 2, 3, 4, 6 and 7
raise the interrupt output whenever set. Bits 0, 1 and 5 raise it only
with their DMA control enables. The output drives CRIME slot 3 through
MACE :src:`sgi-mace-2.0-spec p. 72, 179`, *established*; both captures report
interrupt 3, *observed*.

DMA control
===========

Fields :src:`sgi-mace-2.0-spec p. 73`, *established*: 15 receive DMA
enable, 14:12 receive start offset in 64-bit words, 11 packet gathering,
10 receive runts, 9 receive interrupt enable, 8:4 receive threshold,
3:2 transmit ring size (8, 16, 32 or 64 KiB), 1 transmit DMA enable,
0 transmit interrupt enable. NetBSD ``if_mecreg.h`` has the same layout,
*software expectation*.

Changing the ring size while either DMA engine is on is ignored with
``LOG_GUEST_ERROR``. The transmit interrupt alias and the receive
interrupt alias write only their fields and read nothing.

Transmit
========

The transmit ring has 64, 128, 256 or 512 slots of 128 bytes
:src:`sgi-mace-2.0-spec p. 73, 78`. Software writes the 9-bit write
pointer; the read pointer belongs to the hardware and resets only through a
DMA engine reset. The ring is empty when the pointers are equal and full
when the write pointer is one behind the read pointer
:src:`sgi-mace-2.0-spec p. 74`.

Each slot starts with a command word :src:`sgi-mace-2.0-spec p. 79`:
15:0 length minus one, 22:16 offset of the first data byte in the slot
(8 or more; 0 means the data starts at the first concatenation pointer),
23 stop on abort, 24 interrupt when sent, 27:25 valid flags for three
concatenation pointers. A pointer word holds an address in bits 31:3 and
a length minus one in 47:32. After transmission the first word is
overwritten with a status vector: length in 15:0, success in bit 23 and
done in bit 63.

The model transmits only while DMA is enabled, the MAC is out of reset and
the link is up. It reads slots from the read pointer up to the write
pointer, gathers the inline bytes and up to three pointers, sends the frame
to the network backend and writes back a status vector with the done bit,
the success bit and the length. Collision count, deferral and abort bits
are never set. A descriptor the model finds invalid, with reserved
command bits set, a length over 2048 bytes, a bad start offset or flag
combination, or pointers that do not sum to the length, logs
``LOG_GUEST_ERROR``, sets bit 4, stops the engine and clears transmit enable.
A DMA fault sets bit 3 and stops it the same way. A write to the transmit
pointer beyond the active ring is refused with ``LOG_UNIMP``.

The specification says the read pointer resets only through the DMA
engine reset :src:`sgi-mace-2.0-spec p. 74`; how software triggers that
reset is not stated. The model restarts a stopped transmit engine when it
sees the sequence "transmit disable, MAC reset pulse, new ring base,
transmit enable"; this is an *approximation* of an unknown reset.

Receive
=======

Receive buffers are 4 KiB clusters whose addresses software pushes into a
16-entry FIFO through the data port, 32 copies of one register
:src:`sgi-mace-2.0-spec p. 75`, *established*. The low 12 bits of a
pushed address are dropped. The FIFO contents do not reset; only the
read index, write index and count do. The information register holds the
count in 4:0, the read index in 11:8, a read generation bit in 12, the
write index in 19:16 and a write generation bit in 20. Software may read
the FIFO only while receive DMA is off; the model returns 0 and logs
``LOG_GUEST_ERROR`` otherwise. The specification says hardware does not
prevent overrun (:src:`sgi-mace-2.0-spec p. 75`); the model ignores a push
into a full FIFO and logs ``LOG_GUEST_ERROR``, an *approximation*.

A frame lands in a 256-byte bucket: the status vector in the first
64-bit word, then the data after the start offset set in DMA control
plus 2 bytes of padding :src:`sgi-mace-2.0-spec p. 80`. The vector holds
the true length of the packet in 15:0 (the model counts the 4-byte FCS
in it), the broadcast flag (20) and
physical-match flag (26), the sequence number in 31:27, a 16-bit partial
Internet checksum in 47:32 and a one in bit 63 :src:`sgi-mace-2.0-spec
p. 81`. The model sets only the broadcast and physical-match flags, adds
the FCS it computes, and sums the frame and FCS 16 bits at a time with
carry folded for the checksum. The other flags are never set. Frames
under 60 bytes are dropped unless runts are enabled; frames over 1514
bytes are dropped with ``LOG_GUEST_ERROR``. If no buffer is queued the
frame is dropped and bit 6 set.

With gathering on, small back-to-back frames share a cluster when the
delay counter is running and at least four buffers are queued. The delay
is the 6-bit register at ``0x18`` in 30.69 µs steps
:src:`sgi-mace-2.0-spec p. 73, 80`; the model uses 30690 ns.

The receive interrupt is on while DMA and the interrupt enable are on and
the FIFO count differs from the threshold. The specification states this
as ``!(Threshold != FIFO Count)`` and also as "disabled when equal"
:src:`sgi-mace-2.0-spec p. 73-74`, which conflict. The model uses the
inequality, an *approximation*.

PHY management
==============

Registers ``0x60``, ``0x68`` and ``0x70`` form the two-wire interface
:src:`sgi-mace-2.0-spec p. 76`, *established*. Software writes the PHY
(bits 9:5) and register (4:0) numbers, writes any value to the read-start
register, then polls busy (bit 16 of the data register) before reading
the 16-bit result. A write puts the value in the data register with the
address already set. Linux ``meth.c`` and NetBSD ``if_mec.c`` use this
sequence, *software expectation*. Each operation takes 1 µs of virtual
time in the model, a choice the specification leaves open, and a second
request while busy logs ``LOG_GUEST_ERROR``.

The captures show a National DP83840 transceiver at MII address 8, revision
1 (:src:`dmesgd-2432 nsphy0`, :src:`dmesgd-5290 nsphy0`), *observed*.
OpenBSD ``if_mec.c`` says the PHY may answer at addresses 10 or 11 and
settle at 8 shortly after, so its driver scans after a delay
(near line 697), *software expectation*. Linux probes all 32 addresses
and knows the QS6612X, ICS1889, ICS1890 and DP83840 (``meth.c``).

The model has no PHY by default. MDIO reads return ``0xffff``, writes are
dropped, the link never comes up and transmit and receive are held off, so
the default machine passes no traffic. The ``phy-ics1890=on`` property with
``phy-address=N`` attaches the ICS1890 model (:doc:`ics1890`) at address N
instead. That is a different PHY from the one in the captures, an
*approximation*. QEMU has a DP83840 model (``hw/net/dp83840.c``,
:doc:`dp8384x`) but MACE does not use it. Published value and model
default side by side:

.. list-table::
   :header-rows: 1

   * - Item
     - Published O2s
     - Model default
   * - PHY
     - DP83840 revision 1, MII address 8
     - none; ICS1890 at a chosen address by option
   * - MAC revision
     - 1
     - 1

Access widths
=============

The specification defines 64-bit access only. The model also accepts the
following, an *approximation*:

- 32-bit accesses to the low 32 bits of MAC control, DMA control, the PHY
  data register and, for writes, interrupt status, the delay, the transmit
  interrupt alias, the PHY address, the PHY read start and the backoff seed,
  which is the word at offset + 4 because the device is big-endian;
- 32-bit writes to the receive FIFO data port at offset + 4;
- 16-bit reads of the transmit read and write pointers at ``0x34`` and
  ``0x36`` and a 16-bit write of the write pointer at ``0x36``;
- an 8-bit read at ``0x46``, the receive FIFO read-pointer byte.

Any other misaligned access logs ``LOG_UNIMP``.

Interrupt request register
==========================

A write to ``0x58`` of a value under 2^32 calls MACE's interrupt-request
hook, which updates CRIME's MACE inputs directly (the specification calls
the register a diagnostic that "generates an interrupt update packet"
:src:`sgi-mace-2.0-spec p. 70`). A read returns the last transmit status
vector. Upper bits set log ``LOG_UNIMP``.

Reset
=====

Reset sets MAC control to ``0x20000001`` and clears the other state,
cancels the delay and MDIO timers and drops the interrupt line. A 0-to-1
transition of MAC control bit 0 clears DMA control, interrupt status, the
delay, the transmit pointers and the FIFO indices.

Measurements
============

No register of this part has been read on a physical machine for this
project. Published captures are listed under `Identity and placement`_.

Model limits
============

- No default PHY, so no link and no traffic without ``phy-ics1890``.
- Multicast hashing, the secondary address, MAC loopback, SIA, duplex,
  speed, the inter-packet gaps, collision backoff and the transmit
  diagnostic registers are absent. Several log ``LOG_UNIMP`` when
  programmed.
- Receive status flags other than broadcast and physical match, the
  receive FIFO overflow event and transmit abort causes are never raised.
- The transmit restart sequence and the receive threshold rule are
  approximations (`Transmit`_, `Receive`_).
- Migration works only between identical builds (``sgi-mace-eth`` version
  1).

Tests
=====

``tests/qtest/sgi-mace-eth-test.c`` covers receive admission, gathering and
threshold handling, the delay interval, transmit pointers, all four ring
sizes and reserved descriptors, DMA faults, the checksum vectors, register
reset and aliases, the MDIO registers, interrupt requests and migration.
