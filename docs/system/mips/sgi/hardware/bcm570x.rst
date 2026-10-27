.. SPDX-License-Identifier: BSD-3-Clause

.. _sgi-bcm570x:

==================================================
Broadcom BCM5701 Gigabit Ethernet controller (IO9)
==================================================

The BCM5701 is a single-port, 10/100/1000 Mb/s PCI Ethernet MAC with an
integrated copper PHY. The IO9 board of the IP53 machines (Origin 350,
Onyx 350, Tezro, and the Onyx4 host profile) carries one at PCI device 4
behind the PIC. QEMU models the MAC as ``hw/net/bcm570x.c`` (QOM type
``bcm570x``, selected by ``model=bcm5701``) and the PHY as
``hw/net/bcm570x_phy.c``, a child object of the MAC. The same type with
``model=bcm5704`` is a dual-port BCM5704; this page covers the BCM5701
only. :ref:`sgi-ip53-bcm570x` is the short entry in the IP53 platform
page; this page is the reference behind it. Facts are rated as described
in :ref:`sgi-ratings`.

.. _sgi-bcm570x-sources:

Sources
=======

- Broadcom, *BCM57XX Host Programmer's Interface Specification*, document
  57XX-PG105-R, 2008-01-29 (:src:`broadcom-57xx-pg105-r`). It covers the
  BCM5701 among other revisions. Cited by printed page or table number.
  It is the only register-level specification used. No BCM5701 data sheet
  is cited.
- The Origin 350 System User's Guide (:src:`007-4566-001 p. 69`), for the
  IO9 board and its place in the module.
- Published IRIX hardware inventories of IP53 machines, summarised on
  :ref:`sgi-sn-family-origin350`, for the PCI identity at device 4.
- Public driver source, as software expectations: OpenBSD
  ``sys/dev/pci/if_bge.c`` and ``if_bgereg.h`` at ``2df91d80c104``
  (the ``sgi-is-alive-at-7.9`` branch), and ``sys/dev/mii/brgphy.c``;
  Linux ``drivers/net/ethernet/broadcom/tg3.c`` and ``tg3.h`` at
  ``db2ddb871435``.
- The IRIX 6.5.30 kernel ``tg`` driver, as a guest whose observable
  behaviour the model must satisfy. It is a software expectation only.

Where a driver names a register differently from the specification, this
page uses the specification's name and says so.

.. _sgi-bcm570x-identity:

Identity and placement
======================

PCI identity
------------

The model's defaults, all QOM properties:

.. list-table::
   :header-rows: 1

   * - Field
     - Property
     - Default
     - Note
   * - Vendor
     - ``vendor-id``
     - ``0x14e4``
     - Broadcom.
   * - Device
     - ``device-id``
     - ``0x1645``
     - BCM5701.
   * - Subsystem vendor
     - ``subsystem-vendor-id``
     - ``0x10a9``
     - SGI.
   * - Subsystem device
     - ``subsystem-id``
     - ``0x8010``
     - Model choice.
   * - Revision
     - none
     - ``0x00``
     - Model choice.
   * - Class
     - none
     - ``0x0200``
     - Ethernet controller.
   * - Interrupt pin
     - none
     - INTA
     - One pin.

Published IRIX inventories of IP53 machines report ``14e4:1645`` at
configuration device 4, *observed* (:ref:`sgi-sn-family-origin350`), with
revision ``0x15`` according to that page. The model matches the vendor,
device and slot. Its PCI revision of ``0x00`` differs from the inventories,
and its subsystem identity is unconfirmed. Both are model choices,
*approximation*; the subsystem
identity is chosen so that the IRIX 6.5.30 ``tg`` driver binds. The
revision and subsystem identity the IO9 part really carries are
*unknown*. A read of its configuration space on an IP53 machine would
settle both.

BAR0 is one 64 KiB memory BAR. The model programs no other BAR and no
capability list.

Chip identification
-------------------

The upper half of the miscellaneous host control register (configuration
offset ``0x68``) holds the chip identification. The drivers take their
whole chip identity from it. The model reads ``0x0105`` there, the
BCM5701 B5 step in the public driver tables (OpenBSD ``if_bgereg.h``,
Linux ``tg3.h``), *inferred*. Whether the IO9 part is that step is
*unknown*. The field stays fixed across both resets. The low 16 bits
are writable storage, apart from the two interrupt bits described under
:ref:`sgi-bcm570x-interrupts`.

Placement on the IO9
--------------------

The IO9 sits in the module's lowermost slot, and its IOC4, SCSI controller
and Ethernet controller appear at PIC configuration devices 1, 3 and 4
(:src:`007-4566-001 p. 69`, *established* for the board;
:ref:`sgi-sn-family-origin350` for the device numbers, *observed*). The
machine profiles place the BCM5701 at device 4, function 0, with INTA
wired to PIC input 3. The input number follows the public Linux
convention of device number minus one, *inferred*. The physical IO9
interrupt wiring has not been measured, *unknown*.

The profile creates the device with ``model=bcm5701`` and gives it the
machine's default NIC configuration, so ``-nic`` or ``-netdev`` selects
its network backend, and ``-nic none`` leaves it without one. The
catalogue maps a netboot adapter to this device for ``onyx350``
and ``tezro`` only (:ref:`sgi-sn-launch-metadata`).

.. _sgi-bcm570x-access:

Access paths
============

Software reaches the chip three ways. *Software expectation*, from the
public drivers.

BAR0
  Offsets ``0x0000``-``0x00ff`` alias PCI configuration space. Offsets
  ``0x0100``-``0x7fff`` are the register file. Offsets ``0x8000``-``0xffff``
  are a window onto on-chip SRAM. Drivers reach ``MISC_CTL`` and
  ``PCISTATE`` through whichever path suits them, and the model keeps
  both views on one state.

Indirect register window
  Writing a register offset to configuration ``0x78`` and then reading or
  writing configuration ``0x80`` reaches that register-file offset. Every
  side effect of the BAR path fires through it too. The BCM5700 and
  BCM5701 drivers issue the global reset this way.

Indirect memory window
  Configuration ``0x7c`` selects an SRAM offset and ``0x84`` accesses
  that word. The window base also positions the BAR window in the real
  part. In the model the SRAM is 32 KiB and every SRAM offset is taken
  modulo 32 KiB, so the base has no effect on which word the BAR window
  shows.

The indirect windows respond only to 32-bit configuration accesses. Any
other width falls through to ordinary configuration storage.

BAR0 accepts 32-bit aligned reads and writes. It accepts one other
access: a byte read inside ``0x0410``-``0x0417``, the first station-address
register pair, which returns the addressed byte of the stored
little-endian register. All other widths and misaligned accesses fail
the memory transaction. *Approximation*: the widths the real part accepts
are *unknown*.

.. _sgi-bcm570x-registers:

Register map
============

The table lists registers with modelled behaviour. "Stored" means the
write is kept and read back with no other effect. Offsets are in the
register file, widths are 32 bits, and every register reads zero after
reset unless noted. Names follow :src:`broadcom-57xx-pg105-r`. The access
column shows the model's behaviour.

.. list-table::
   :header-rows: 1

   * - Offset
     - Name
     - Reset
     - Access
     - Notes
   * - ``0x0204``
     - Interrupt mailbox 0 (low)
     - ``0``
     - RW
     - Mask and acknowledge (:ref:`sgi-bcm570x-interrupts`).
   * - ``0x026c``
     - Standard receive producer mailbox
     - ``0``
     - RW
     - Refused with ``LOG_GUEST_ERROR`` if the ring control block length
       is zero or the value is 512 or more.
   * - ``0x0274``
     - Jumbo receive producer mailbox
     - ``0``
     - RW
     - Values from 256 up are refused with ``LOG_GUEST_ERROR``.
   * - ``0x0284``
     - Receive return consumer mailbox
     - ``0``
     - RW
     - Refused when the value reaches the return ring length.
   * - ``0x0304``
     - Send producer mailbox
     - ``0``
     - RW
     - Doorbell: transmits up to this index.
   * - ``0x0400``
     - MAC mode
     - ``0``
     - RW
     - Bit 0 reads back zero. The rest is stored.
   * - ``0x0404``
     - MAC status
     - ``0``
     - W1C
     - A write clears the bits written. The model never sets any.
   * - ``0x0410``-``0x0414``
     - MAC address 0, high and low
     - station address
     - RW
     - Transmit source address slot 0 and the receive filter.
   * - ``0x0418``-``0x042c``
     - MAC addresses 1 to 3
     - ``0``
     - RW
     - Source address slots 1-3 only. Not used for receive.
   * - ``0x044c``
     - MII communication
     - ``0``
     - RW
     - PHY access (:ref:`sgi-bcm570x-mii`).
   * - ``0x0450``
     - MII status
     - derived
     - RO
     - Bit 0 follows PHY link. A write logs ``LOG_UNIMP`` and does not
       change the read.
   * - ``0x0454``
     - MII mode
     - ``0``
     - RW
     - Stored. Auto-poll has no effect.
   * - ``0x0468``
     - Receive MAC mode
     - ``0``
     - RW
     - Bit 1 enables receive. Bit 0 reads back zero.
   * - ``0x2440``-``0x2448``
     - Jumbo receive ring control block
     - ``0``
     - RW
     - Host address and flags. Bits 0 and 1 matter.
   * - ``0x2450``-``0x2458``
     - Standard receive ring control block
     - ``0``
     - RW
     - Host address, then maximum length (31:16).
   * - ``0x3c00``
     - Host coalescing mode
     - ``0``
     - RW
     - Bit 1 enables. Bit 0 reads back zero.
   * - ``0x3c08``-``0x3c14``
     - Coalescing ticks and frame counts
     - ``0``
     - RW
     - Receive ticks, send ticks, receive count, send count.
   * - ``0x3c38``-``0x3c3c``
     - Status block host address
     - ``0``
     - RW
     - High, then low word.
   * - ``0x4400``
     - Buffer manager mode
     - ``0``
     - RW
     - Timed enable (:ref:`sgi-bcm570x-bufmgr`).
   * - ``0x4404``
     - Buffer manager status
     - ``0``
     - RO
     - A write logs ``LOG_GUEST_ERROR`` and is ignored.
   * - ``0x5034``
     - Firmware progress
     - ``0xffffffff``
     - RO
     - Always reads all ones. A write logs ``LOG_UNIMP`` and is
       ignored.
   * - ``0x6800``
     - General mode control
     - ``0``
     - RW
     - DMA swap bits, checksum bits.
   * - ``0x6804``
     - Miscellaneous configuration
     - ``0``
     - RW
     - Bit 0 starts the global reset.
   * - ``0x6838``-``0x6848``
     - Serial EEPROM controller
     - ``0``
     - RW
     - See :ref:`sgi-bcm570x-eeprom`.

The register file is 32 KiB. Every offset is backing storage. A write to
an offset with no special handling logs ``LOG_UNIMP`` and is stored, and a
read returns the stored word without a log. Registers the model reads for
their stored value, such as the coalescing counts, the ring control block
addresses and ``0x6800``, fall in that class, so a ``LOG_UNIMP`` line for
one does not mean the write was ignored.

Several registers need a specification detail not obvious from the
drivers.

- Receive MAC mode bit 1 resets to zero, so nothing is received until
  software sets it :src:`broadcom-57xx-pg105-r p. 391, Table 268`,
  *established*. The model drops frames, without holding them, while it is
  clear.
- The jumbo ring control block's length half is unused. Only bit 1
  (disable ring) and bit 0 (extended 64-byte descriptors) matter
  :src:`broadcom-57xx-pg105-r p. 438, Table 341`, *established*. The
  standard ring's length half is the largest frame it carries
  :src:`broadcom-57xx-pg105-r p. 91`.
- Bits 0 of the mode registers at ``0x0400``, ``0x0468``, ``0x3c00`` and
  ``0x4400`` are self-clearing resets. The model clears them without
  resetting anything else, except in the buffer manager, where the write
  zeroes the register first.
- MAC mode bits other than reset are stored. The model has no MAC state
  machine behind the port-mode, DMA-enable or statistics bits.

.. _sgi-bcm570x-reset:

Reset and the firmware handshake
================================

The chip's on-board bootcode is not modelled. Software only sees it
through one SRAM word and one register.

The word is SRAM ``0x0b50``. Drivers write the magic value ``0x4b657654``
there and poll for its one's complement ``0xb49a89ab`` when the bootcode
has finished :src:`broadcom-57xx-pg105-r p. 152`, *established* for the
handshake. The drivers' order of writing and polling is a *software
expectation*. The model complements every value written
to this word. The magic therefore reads back as ``0xb49a89ab`` whether the
driver writes it before the global reset, as OpenBSD and Linux do, or
after, as the IRIX 6.5.30 ``tg`` driver does. A second write flips the
word again. The model has no bootcode delay. *Software expectation*,
*approximation*.

The register is ``0x5034``, the firmware progress word. It reads
``0xffffffff`` in every state. The IRIX 6.5.30 ``tg`` driver stops
attaching when it reads zero, so the model never returns zero.
*Software expectation*. Which real progress values the bootcode
reports is *unknown*.

Two resets exist.

Global reset
  A write with bit 0 set to ``0x6804``, by BAR or indirect window.
  It clears the register file and all SRAM, and returns the ring indices,
  status tag and coalescing state to zero. It masks the interrupt, drops
  frames held for the receive path, cancels pending timers, rewrites the
  firmware word with the magic's complement, and reloads the station
  address. It leaves PCI configuration, the indirect window bases,
  the PHY and a transmit already in progress alone. Bit 0 of ``0x6804``
  reads zero afterwards.

Cold reset
  The QEMU reset. It clears the same state, plus the window bases and the
  transmit state, resets the PHY, and reloads the station address. It does
  *not* write the firmware word: SRAM ``0x0b50`` reads zero until the
  driver writes the magic.

The station address comes from the device's ``mac=`` setting and is loaded
on every reset into two places: SRAM ``0x0c14``/``0x0c18`` as the mailbox
the public drivers read (the high word carries the tag ``0x484b``), and
MAC address 0 at ``0x0410``/``0x0414``. The first holds address bytes 0-1
in the low half of the high word, and bytes 2-5 in the low word. Slots 1
to 3 are zero. The model has no EEPROM image by default, so this
is an *approximation* of the bootcode loading the address from the
EEPROM. The real source is *unknown*.

.. _sgi-bcm570x-mii:

MII management and the PHY
==========================

``MI_COMM`` at ``0x044c`` is the only path to the PHY. Layout
:src:`broadcom-57xx-pg105-r p. 388, Table 261`, *established*:

- bit 29: start/busy,
- bit 28: read failed,
- bits 27:26: command, ``10`` read and ``01`` write,
- bits 25:21: PHY address,
- bits 20:16: register number,
- bits 15:0: data.

The model behaves as follows.

- A write with bit 29 clear is stored. It does nothing else.
- A write with bit 29 set runs at once and returns with bit 29 clear. The
  model has no transaction delay, *approximation*; the delay is *unknown*.
- A read keeps bits 27:16 of the request, takes the PHY register as bits
  15:0, and clears everything above bit 27. Request ``0x2822ffff``
  therefore reads back as ``0x08220020``, and so does ``0xf822ffff``. This
  follows the specification's rule that command and address fields are
  ordinary read/write fields, *inferred*.
- A write keeps the whole request apart from bit 29, so bits 31:30 and
  bit 28 stay as the caller wrote them. The specification fixes bits 31:30
  at zero. The model does not enforce that on writes, *approximation*.
- The command ``00`` or ``11`` logs ``LOG_UNIMP``, clears bit 29 and
  touches nothing. The specification calls both undefined.
- The PHY address field is not decoded. Every address reaches the one PHY,
  and read-failed is never set. The real part answers only its own
  address, *unknown*.
- ``MI_STS`` (``0x0450``) bit 0 mirrors the PHY's link state.

The PHY identity registers decode to the Broadcom OUI ``0x000818`` and
model ``0x11``, the pair the OpenBSD ``brgphy`` driver matches for a
BCM5701: register 2 reads ``0x0020`` and register 3 reads ``0x6110``.
The revision nibble is zero. This is a choice that makes the public driver
bind, *approximation*. The IO9's actual PHY identity is *unknown*; a read
of PHY registers 2 and 3 on an IP53 machine would settle it.

.. list-table::
   :header-rows: 1

   * - Reg
     - Name
     - Reset
     - Behaviour
   * - ``0x00``
     - Control
     - ``0x1140``
     - Stored, apart from reset (bit 15, which restores the defaults and
       ignores the rest of the write) and restart (bit 9), which clear
       themselves.
   * - ``0x01``
     - Status
     - ``0x7909``
     - Read only. Bits 2 and 5 (link, negotiation complete) join when the
       link is up, giving ``0x792d``. The link bit does not latch low.
   * - ``0x02``, ``0x03``
     - Identity
     - ``0x0020``, ``0x6110``
     - Read only.
   * - ``0x04``
     - Advertisement
     - ``0x01e1``
     - Stored, all 16 bits.
   * - ``0x05``
     - Partner ability
     - ``0``
     - Link up: the advertisement with bit 14 (acknowledge) set. Link
       down: zero. Read only.
   * - ``0x09``
     - 1000BASE-T control
     - ``0x0200``
     - Stored.
   * - ``0x0a``
     - 1000BASE-T status
     - ``0``
     - Link up: ``0x7800``. Link down: zero. Read only.
   * - ``0x0f``
     - Extended status
     - ``0x3000``
     - Constant. Read only.
   * - ``0x19``
     - Auxiliary status
     - ``0``
     - Link up: ``0x8700`` (negotiation complete, 1000BASE-T full
       duplex). Link down: zero.

Register layout is the IEEE 802.3 clause 22 MII set, plus the Broadcom
auxiliary status summary the BSD ``brgphy`` driver reads for the settled
speed and duplex, *software expectation*. The reset and constant values
are the model's, not measured, *approximation*.

Reads and writes of any other register log ``LOG_UNIMP`` and read zero. A
write to a read-only register logs ``LOG_GUEST_ERROR`` and is ignored.
Zero rather than all ones is deliberate: all ones is what an absent PHY
reads as.

Link and negotiation
--------------------

The PHY has no cable. Its link is up when negotiation has completed and
the network backend's link is up. Negotiation starts out of reset, on a control register write that
restarts it or leaves it enabled and not isolated, and on a status or
auxiliary status read that finds it neither running nor complete with
negotiation enabled. It completes
100 ms of virtual time later. The partner always agrees to 1000BASE-T full
duplex. There is no forced-speed mode: the speed and duplex bits, loopback
(bit 14), power-down (bit 11) and the master/slave controls are stored and
do nothing. Writing isolate (bit 10) clears completion and cancels a running
negotiation, but a later status read starts a new one, because the model
does not hold negotiation off while isolated.
If negotiation is disabled the model does not start it, and a link that
had already come up stays up. A backend link drop takes the link down at
once and a recovery brings it back at once, without renegotiating.
All of this is an *approximation* of a real PHY; negotiation time and
partner behaviour on the IO9 are *unknown*.

.. _sgi-bcm570x-bufmgr:

Buffer manager startup
======================

``BUFMGR_MODE`` (``0x4400``) bit 1 enables the block. After the host sets
it, the specification says the bit stays clear until the block has started,
then reads set; the host polls it for up to 10 ms
:src:`broadcom-57xx-pg105-r p. 152, steps 38-39`, *established*. Once
started the bit persists, and clearing it requests a halt
:src:`broadcom-57xx-pg105-r p. 467, Table 383`, *established*. The
startup time is not specified, *unknown*.

The model completes startup 8 ms of virtual time after the enable
request, inside the host's 10 ms budget, *approximation*. The enable bit
reads clear meanwhile. A repeat request keeps the first deadline. Clearing
the bit halts at once, since there is no buffer pool to drain. A write
with bit 0 set first zeroes the register and cancels a pending start, and
a global reset cancels it too. The pending state and its deadline migrate.
The model has no internal buffer pool, so the status register
(``0x4404``) never reports an error or a low-buffer condition.

.. _sgi-bcm570x-rings:

Rings and DMA
=============

Ring setup
----------

Software describes each ring with a control block holding a 64-bit host
address, a length/flags word and an on-chip address. The model reads:

- the send ring block from SRAM ``0x0100``: host address, then ring length
  in bits 31:16 of the third word,
- the receive return ring block from SRAM ``0x0200``, same layout,
- the standard receive ring block from registers ``0x2450``-``0x2458``,
- the jumbo receive ring block from registers ``0x2440``-``0x2448``.

The send ring and the return ring take their lengths from the control
block. The standard receive ring is fixed at 512 descriptors and the jumbo
ring at 256, whatever the control blocks say, because the standard ring's
length half is a buffer size, not a count. The mini ring and the on-chip
send ring are not modelled. A host address written as two halves is
composed high word first, unless ``MISC_CTL`` bit 3 (word swap) is set,
which exchanges the halves. That reading of the bit is an *approximation*
drawn from how the drivers use it for a big-endian host.

The send ring and both receive rings live in host memory. The model reads
and writes them through the device's own PCI bus-master address space, so
the host bridge translates every address.

DMA byte order
--------------

The chip is big-endian internally with a 64-bit memory. The model builds
descriptors and the status block as the chip's internal image, then
permutes bytes inside each aligned doubleword according to ``MODE_CTL``:
bits 5 and 4 (word swap and byte swap) for frame data, bits 2 and 1 for
everything else. With both clear, an aligned doubleword goes out
reversed. The byte at image lane *l* (lane 0 most significant) lands at
lane ``l ^ 7 ^ (3 if byte swap) ^ (4 if word swap)`` of the same
doubleword, so both swaps set gives an unchanged order.

For frame data every combination is *established* at both bus widths
:src:`broadcom-57xx-pg105-r Tables 102-109`. For descriptors and the status
block, the no-swap and all-swap cases are established
and the single-swap cases are *inferred*. The model uses the transfer's
PCI address modulo 8 as the lane, an *approximation*: the chip's internal
buffer alignment is not modelled. How the resulting bytes reach guest
memory is the host bridge's business. Whether a 64-bit data phase or two
32-bit phases carries them on a given slot is *unknown*.

Transmit
--------

The host fills 16-byte descriptors in the send ring and writes the new
producer index to the mailbox at ``0x0304``. The model then walks from its
consumer to that index. A descriptor holds a 64-bit buffer address, a
16-bit length, 16-bit flags and, in the last word, the maximum segment
size. Descriptors accumulate until one with the END flag (``0x0004``)
completes a frame.

The model reports completion through the status block, not a register
(:ref:`sgi-bcm570x-status`). Details:

- A producer index at or past the ring length, or a zero ring length,
  logs ``LOG_GUEST_ERROR`` and sends nothing.
- A frame over 9600 bytes is dropped whole with ``LOG_GUEST_ERROR`` and
  the walk resumes at the next END. The limit is a model choice shared
  with the other SGI adapters, not the hardware's, *approximation*.
- A failed descriptor or buffer read stops the walk. The consumer rolls
  back to the start of the unfinished frame, so a later doorbell retries
  it. Frames already complete are reported.
- IP checksum (flag ``0x0002``) and TCP/UDP checksum (flag ``0x0001``)
  offload run on IPv4 frames before sending. A frame that is not IPv4 or
  whose lengths do not fit is sent unchanged.
- Source address replacement is a per-frame request :src:`broadcom-57xx-pg105-r pp. 92-95, 167`,
  *established*. Flag bit 12 (``0x1000``) asks for it and bits 14:13 pick
  one of the four station address slots. The model reads the flags from
  any descriptor of the frame and applies the replacement only to frames
  whose maximum segment size is zero. Without the flag the host's source address goes
  out unchanged.
- A non-zero maximum segment size asks for TCP segmentation. A plain IPv4
  TCP frame with more payload than one segment is cut into segments of
  that size. Each gets its own copy of the headers, a fresh IP
  identification and sequence number, and recomputed checksums, and only
  the last keeps FIN and PSH. With a vnet-header backend the model hands
  the frame to the backend for segmentation instead. Any frame that is not
  plain IPv4 TCP, or whose lengths do not fit, goes out whole. This
  offload is an *approximation* of the chip's firmware-assisted
  segmentation, whose exact output is *unknown*.

Receive
-------

The host posts 32-byte buffer descriptors on the standard or jumbo ring
and writes the producer index to its mailbox. The model accepts a frame
from the backend when all of these hold, and otherwise leaves the frame
queued in the backend:

- receive MAC mode bit 1 is set (a frame arriving while it is clear is
  discarded, not queued),
- host coalescing is enabled,
- the return ring is configured and has a free slot,
- a buffer is posted on the standard ring, or on the enabled jumbo ring.

The address filter accepts any frame with the group bit set (broadcast and
multicast) and any unicast frame for MAC address slot 0. The model
implements no hash filter, promiscuous mode, VLAN stripping or receive
rules. Frames shorter than 60 bytes are padded to 60. Frames over 9600
bytes are dropped with ``LOG_GUEST_ERROR``.

A frame no longer than the standard ring's maximum length takes a standard
buffer. A longer frame takes a jumbo buffer, which must be large enough
for it. A longer frame with no usable jumbo ring is dropped with
``LOG_GUEST_ERROR``. The jumbo ring is usable when its host address is
non-zero and neither bit 0 nor bit 1 of its flags is set. The model
does not parse the extended 64-byte jumbo descriptor, and bit 0 logs
``LOG_UNIMP`` when written.

The model copies the frame into the buffer, then writes a 32-byte
completion descriptor to the return ring. It holds the buffer address, the
ring index in bits 31:16 and frame length in bits 15:0 of the next word
(the length includes four bytes for the frame check sequence the model
does not append), a flags word, the checksum word and the opaque word
copied from the buffer descriptor. The flags set are END (``0x0004``),
jumbo ring (``0x0020``), ``0x1000`` for any IPv4 frame whose header sum
verifies, ``0x4000`` for any TCP frame (fragments included), and ``0x2000``
when the TCP or UDP sum verifies over an unfragmented datagram. The checksum word holds
the IP header sum in bits 31:16 and the transport sum in bits 15:0.
The model sums the frame itself. The error flag is never set because the
model never receives a bad frame. ``MODE_CTL`` bit 23 leaves the pseudo-header
out of the transport sum, so the transport-valid flag is not set. Bit 9
(raw receive checksum mode) logs ``LOG_UNIMP`` and reports no sums.

When a frame belongs to a ring that has nothing posted while the other
ring has a buffer, the model discards it, counts it, and logs
``LOG_GUEST_ERROR`` once per shortage. The count is the 32-bit word at SRAM ``0x0898``
(``nicNoMoreRXBDs``, :src:`broadcom-57xx-pg105-r p. 110, Table 49` and
:src:`broadcom-57xx-pg105-r p. 114, Table 54`). The specification names no
ring for the counter, so the model counts both. What the chip does with a
frame whose ring is empty is *unknown*; discarding avoids stalling the
frames behind it.

A write to a receive producer mailbox, to the return consumer, to the
receive MAC mode with the enable bit set, to the coalescing mode with
enable set, or an interrupt unmask retries a frame the backend is holding.

.. _sgi-bcm570x-arrival:

Arrival latency
---------------

The model has no wire or peer time. It therefore publishes a receive
completion a fixed floor after the backend hands over the frame: the
descriptor and data are written at once, but the status block's receive
producer and the interrupt follow 200 microseconds of virtual time later.
The floor is the ``rx-arrival-latency-us`` property. The mechanism and the
200 microsecond default are shared code with the other SGI Ethernet
adapters, and each device has its own property and line. Zero turns it off. The line holds at most 256
completions. This is an *approximation*; the hardware's real delay is
*unknown*.

.. _sgi-bcm570x-status:

Status block and host coalescing
================================

The chip reports progress through a status block that it writes to host
memory, not through registers. The model writes the block only while host
coalescing mode bit 1 is set, at the address in ``0x3c38``/``0x3c3c``.
It is 80 bytes. The fields the model fills use the public drivers'
structure, *software expectation*: a status word with bit 0 (updated) set,
a tag that increments on every update, the jumbo and standard receive
consumers at bytes 8 and 10, the receive producer at byte 16 and the send
consumer at byte 18. Offsets are those of the drivers' little-endian
structure. The chip builds the block as a big-endian image, which puts
each 16-bit field in the other half of its 32-bit word, and the
non-frame swap controls then apply. Everything else is zero. A failed block write
logs ``LOG_GUEST_ERROR``, posts no interrupt, and is usually retried by the
coalescing timers. A send doorbell that completes no descriptor is not
retried.

An update occurs only on four criteria :src:`broadcom-57xx-pg105-r p. 63`,
*established*:

- the receive descriptors consumed since the last update reach the count
  in ``0x3c10``,
- the send descriptors consumed since the last update reach ``0x3c14``,
- the receive tick budget in ``0x3c08`` expires with a frame received since
  the last update,
- the send tick budget in ``0x3c0c`` expires with a send consumer moved
  since the last update.

A zero count or tick value turns its criterion off, and with all of them
off the chip reports nothing :src:`broadcom-57xx-pg105-r pp. 453-454`,
*established*. Each budget reloads on every update, whatever caused it, and
once expired stays expired until the next update, so an idle ring reports
the next event at once. The model counts ticks of one microsecond on the
virtual clock. A failed status write is retried within 250 ms.

Deviations, each an *approximation*:

- A send doorbell that completes no descriptor still posts an update. No
  criterion covers it.
- Coalescing mode bit 3 (coalesce now) and the during-interrupt registers
  ``0x3c18``-``0x3c24`` are stored and do nothing.
- The statistics block is not DMA'd. ``0x3c28``-``0x3c34`` and
  ``0x3c40``-``0x3c44`` are stored with nothing behind them. The one
  exception is the receive shortage counter above.

.. _sgi-bcm570x-interrupts:

Interrupts
==========

The device drives INTA as a level. The line is high while an update is
pending and neither mask is set:

- the pending latch is set by a status block update the driver has not yet
  acknowledged,
- the interrupt mailbox mask is any non-zero value written to ``0x0204``,
- the global mask is ``MISC_CTL`` bit 1.

Any write to ``0x0204`` clears the pending latch. A non-zero value also
sets the mask, and zero clears it and retries a held frame
:src:`broadcom-57xx-pg105-r p. 372`, *established*. ``MISC_CTL`` bit 0
acknowledges the interrupt: a write with the bit set clears the latch,
and the bit reads back zero. The two ``MISC_CTL`` bits act through
configuration space or the BAR alias. After any reset the mailbox mask is
set.

``PCISTATE`` (configuration ``0x70``) is read-only to the guest. Bit 1
(interrupt not active) is the inverse of the line, so a driver's shared
interrupt check reads true exactly when the line is low. Bit 2 (PCI bus
mode) is set because the chip runs as a conventional PCI function. Bit 4
(32-bit bus) is set when the device is created and follows the host
bridge's REQ64# sample. Which of these the IO9 slot would show is
*unknown*; the PIC's PCI-X mode is not modelled by this device.

An update that lands while the mailbox is masked sets the pending latch,
but the next write to the mailbox, the unmask included, clears it, so no
interrupt follows. The specification says every write to the mailbox
clears interrupt state, *established*; software has to re-read the status
block after unmasking. Tagged status mode and the set-interrupt bit in
local control are not modelled, so a driver that relies on them gets no
interrupt, *approximation*.

.. _sgi-bcm570x-eeprom:

Serial EEPROM controller
========================

Registers ``0x6838`` (command), ``0x683c`` (data), ``0x6840`` (direct pin
control) and ``0x6848`` (delay) follow Tables 441-443 and the delay
register description of :src:`broadcom-57xx-pg105-r pp. 514-516`,
*established*. The command word holds read direction (bit 31), complete
(bit 30, write one to clear), controller reset (bit 29), device id
(bits 28:26), start (bit 25, self-clearing) and a word address in bits 15:2.

The model has no EEPROM by default, and the IO9 profile supplies none.
A command with start set then logs ``LOG_GUEST_ERROR``, never sets complete
and leaves the data word alone. A backing exists only when the ``eeprom``
block-device property and the ``eeprom-device-id`` property (0 to 7) are
both given. The backing must be 4 to 65,536 bytes long, a multiple of four
bytes, and the property is accepted only on the BCM5701 identity.

With a backing, a start command reads or writes the four bytes at
``4 * word address`` through the block layer, at once. The data
register carries the bytes least significant first. Other device ids, an
address past the backing's end, a write to a read-only image and block
errors log ``LOG_GUEST_ERROR`` and leave complete clear. While complete is
set another start does nothing, and software acknowledges it with a write
that sets the bit, which may also carry the next start. A write with the
reset bit set clears the data word and reads back as that bit alone. The
model does not enforce the serial timing or the 10 ms write spacing the
specification quotes. Direct pin access at ``0x6840`` logs ``LOG_UNIMP``.
Migration refuses a stream whose backing size, id or read-only setting
differ. Contents are the caller's responsibility and are not checked.

The fitted EEPROM part, its capacity and its contents on the IO9 are
*unknown*. A dump of one would settle it. The model never makes up a
signature, a checksum or an address record. The Linux ``tg3`` driver
expects the word ``0x669955aa`` at offset 0 and two CRC-32 regions at
``0x00``-``0x0f`` and ``0x74``-``0xfb`` (Linux ``tg3.c``/``tg3.h`` at
``db2ddb871435``), *software expectation*. A guest that checks them reads
zeros and fails unless an image is supplied.

.. _sgi-bcm570x-migration:

Migration
=========

The ``bcm570x`` state carries the register file, SRAM, window bases, ring
and status indices, interrupt latches, coalescing counters and timers, the
buffer manager's pending startup, the receive arrival line, the EEPROM
configuration and the PHY. The PHY state (control, advertisement, 1000BASE-T
control, negotiation flags and the settle timer) is a nested section. On
load the model re-derives the interrupt line and the backend's link state,
rebuilds a missing coalescing timer from the restored registers, and
rejects a stream whose EEPROM configuration or buffer-manager timer state
is inconsistent. Migration works only between identical builds.

.. _sgi-bcm570x-measurements:

Measurements
============

No read of this part has been taken on a physical machine for this
project. Every register value above comes from the cited specification,
the public drivers or the model. The one observed fact, the PCI identity
at device 4 of IP53 machines, comes from published inventories and is
stated under :ref:`sgi-bcm570x-identity`. It does not establish any
register value, reset value, interrupt route or PHY behaviour.

.. _sgi-bcm570x-limits:

Model limits
============

- The part is identified from the IO9's published PCI vendor, device and
  slot only. Its subsystem identity, PCI revision, PHY, EEPROM and
  interrupt routing are unmeasured.
- The bootcode, the on-chip processors, firmware patching, the on-chip
  send ring, the mini ring and ASF/APE management are absent. The
  firmware word and the progress register are static stand-ins.
- Only 32-bit register accesses (and the one byte read) succeed.
  Everything else fails the transaction.
- Unmodelled registers keep what is written, log ``LOG_UNIMP`` on writes
  and stay silent on reads. Unexpected indices, descriptors and DMA
  failures log ``LOG_GUEST_ERROR``.
- ``MISC_CTL`` bits other than the two interrupt controls are stored;
  ``PCISTATE`` is read-only.
- The MAC has no statistics, flow control, VLAN, hash filter, promiscuous
  or wake-on-LAN logic. MAC status never changes.
- Autopolling the PHY is absent. The PHY has no forced mode, no loopback
  and no power-down, and answers every MII address.
- Frame size is limited to 9600 bytes both ways.
- Interrupt coalescing is at the stated criteria; tagged status and
  coalesce-now are absent.
- Timing is invented: 8 ms buffer manager startup, 100 ms negotiation,
  200 microseconds arrival latency, immediate MII, EEPROM and DMA
  completion.
- The IO9's PCI-X mode, 64-bit bus width and the slot's byte-lane
  placement are the PIC's concern, not this device's.
- Migration works only between identical builds.

Tests
=====

``tests/qtest/bcm570x-test.c`` is the register-contract test for the MAC
and PHY behind a test PCI bridge head. It covers the PCI identity, both indirect
windows, the reset handshake and progress register, self-clearing mode
bits, buffer manager startup, MII reads and writes, PHY readback and link,
the station address registers and mailbox, the EEPROM controller and its
migration checks, the interrupt mailbox, masks and acknowledge, status
block gating, byte order and word swap, transmit with its errors, source
address, segmentation and checksum offload, split doorbells and
coalescing, receive on both rings with every checksum case, shortages,
oversize frames and ring wrap, the arrival deadline, DMA errors, reset
during coalescing, state and link migration, and the dual-port variant.

``tests/qtest/sn-builder-test.c`` covers the IP53 instantiation:
``sn/builder/ip53-static-bcm5701-network-backend`` and
``sn/builder/ip53-static-bcm5701-nic-none`` check the device, its model
and PCI identity at device 4 and the backend selection;
``sn/builder/ip53-bcm5701-direct64-tx-no-swap`` and
``sn/builder/ip53-bcm5701-direct64-tx-swap-control`` move frames through
the PIC's 64-bit direct window with the swap bits clear and set.

``tests/qtest/sn0-bcm570x-test.c`` covers DMA placement and swap
behaviour behind the Origin 200 Bridge. It is the Bridge's placement
contract, not the PIC's.

The functional tests ``tests/functional/mips64/test_irix_io9_tg_perf.py``,
``test_irix_tg.py``, ``test_irix_tg_arp.py``, ``test_irix_tg_frag.py`` and
``test_irix_tg_perf.py`` boot an installed IRIX 6.5.30 disk and move ping,
ARP, fragmented and bulk TCP and UDP traffic through the ``tg`` driver.
The first runs on the Origin 350 IO9, the others on an Origin 200
option card. They need local assets and show model behaviour.
