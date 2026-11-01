.. SPDX-License-Identifier: BSD-3-Clause

.. _sgi-tigon:

==========================================
Alteon Tigon: Gigabit Ethernet PCI adapter
==========================================

Used in :ref:`sgi-ip27`, :ref:`sgi-ip30`, :ref:`sgi-ip35` and the Origin
3000 and Onyx 3000 bricks (:ref:`sgi-bricks`), as an add-in PCI card that
no machine fits by default.

The Alteon Tigon is a gigabit Ethernet controller with an on-chip MAC and
two embedded RISC processors. SGI sold it as the IRIS Gigabit Ethernet
board, a single-port PCI card in a fibre and a copper variant, for the
Octane, Octane2, Origin 200, Origin 300, Origin 2000 and Onyx2 (both with
an XIO adapter), Onyx 300, Origin 3000, Onyx 3000 and Fuel
:src:`007-3819-004 p. 1`, *established*.
IRIX binds it with its ``eg`` driver. QEMU models a Tigon 2 as
``hw/net/tigon.c`` (header ``include/hw/net/tigon.h``, type ``tigon``).
It is an add-in PCI card: ``-device tigon`` puts it in a free slot behind
a Bridge or XBridge. The tests use the ``origin200`` option slots and the
``origin300`` PCI expansion topology.

The model does not run the card's firmware. It accepts the host's firmware
download, then answers the host-memory ring interface that the firmware
presents. Guest drivers cannot tell the two apart as long as the register,
ring, mailbox and event contract holds. Facts are rated as described in
:ref:`sgi-ratings`.

.. _sgi-tigon-sources:

Sources
=======

- Alteon Networks, *Tigon/PCI Ethernet Controller*, revision 1.04, part
  020016, August 1997, :src:`alteon-tigon-pci-controller-rev-1.04`. It
  covers gate-array revisions 3 to 5 and adds an appendix of Tigon II
  (revision 6) errata. It describes the PCI interface, the register file
  and the DMA channels. It does not describe the host ring layout, which it
  leaves to a separate host interface definition that is not available.
- SGI, *SGI IRIS Gigabit Ethernet Board User's Guide*, :src:`007-3819-004`,
  for the board's features and supported systems.
- The host interface the public drivers implement. NetBSD
  ``sys/dev/pci/if_ti.c`` and ``if_tireg.h`` at
  ``07004b2a9dac`` are the primary reading. Linux
  ``drivers/net/ethernet/alteon/acenic.c`` and ``acenic.h`` at
  ``c321b5676d0c`` (the parent of their removal) and OpenBSD
  ``sys/dev/ic/ti.c`` and ``tireg.h`` at ``2df91d80c104`` agree with it.
  Register offsets, ring and descriptor layouts, event codes and command
  words below come from these files, as software expectations. Linux
  names the SGI board directly (``PCI_VENDOR_ID_SGI`` ``0x10a9``,
  ``PCI_DEVICE_ID_SGI_ACENIC`` ``0x0009``), and NetBSD ``pcidevs`` lists
  the same pair.
- The behaviour of the IRIX 6.5.30 ``eg`` driver, as a software expectation,
  where it differs from the public drivers.

Identity and placement
======================

.. list-table::
   :header-rows: 1

   * - Field
     - Value
     - Rating
   * - Vendor, device
     - ``0x10a9``, ``0x0009`` (``vendor-id``, ``device-id``)
     - software expectation: Linux ``acenic`` and NetBSD ``pcidevs``
   * - Class
     - ``0x020000``, Ethernet
     - inferred
   * - Subsystem vendor, subsystem, PCI revision
     - ``0``, ``0``, ``0`` (properties)
     - *unknown*
   * - Interrupt pin
     - INTA
     - software expectation: the drivers install one handler
   * - BAR0
     - 16 KB, 32-bit, non-prefetchable memory
     - software expectation: ``if_tireg.h``
   * - Chip revision
     - ``0x6``, read from MISC_HOST_CTL bits 31-28 (``chip-rev``)
     - approximation

The board's feature list names two RISC processors, which makes the part
a Tigon 2 :src:`007-3819-004 p. 2, 4`, *inferred*. The drivers read the
chip generation from MISC_HOST_CTL, not from configuration space: ``0x4``
is Tigon 1 and ``0x6`` is Tigon 2 (``if_tireg.h``). The SGI board's
value has not been read from a real board, so ``0x6`` is an
*approximation*. It selects the per-ring receive producer mailboxes the
Tigon 1 lacks. Subsystem identifiers and the PCI revision byte are
*unknown*; the model leaves them zero.

The remaining properties are ``port-count`` and ``media`` (``copper``),
which only label the card, and ``sram-size`` (1 MiB), the size of the card
SRAM the firmware download lands in. The real board's SRAM is 512 KB to
2 MB :src:`007-3819-004 p. 2`, *established*; the size on a given board is
*unknown*. ``rx-arrival-latency-us`` is described under
:ref:`sgi-tigon-receive`.

Both board variants share one driver and one register interface, so the
model has one. The board is 32- or 64-bit PCI at 33 or 66 MHz
:src:`007-3819-004 p. 27`, and the chip finds the host's width by the PCI
protocol :src:`alteon-tigon-pci-controller-rev-1.04 s. 2.2.6, p. 17`,
both *established*. The width a given slot gives the
card is covered under :ref:`sgi-tigon-byte-order`.

.. _sgi-tigon-registers:

Register map
============

One memory BAR of 16 KB. The register block is the first 2 KB and the SRAM
window is the next 2 KB, at ``0x800``. Nothing is mapped above ``0x1000``.
Every register is 32 bits, little-endian on the PCI side. The register block
accepts only 4-byte accesses, so narrower accesses are refused by the memory
core with a ``LOG_GUEST_ERROR`` message. Offsets not listed read zero and
log ``LOG_UNIMP``, and writes to them are dropped with the same log.

Reset values are the model's. The drivers always initialise these registers
before use, and no read of a real board is available, so a reset value is
*unknown* for hardware unless the notes say otherwise.

.. list-table::
   :header-rows: 1

   * - Offset
     - Name
     - Width
     - Reset
     - Access
     - Notes
   * - ``0x040``
     - MISC_HOST_CTL
     - 32
     - ``0x60000000``
     - R/W
     - Bit 0 interrupt state (read-only), bit 1 clear interrupt (strobe),
       bit 3 chip reset (strobe), bit 6 mask interrupts, bits 31-28 chip
       revision (read-only). Bits 4 and 5 (swap enables) are stored and
       have no effect.
   * - ``0x044``
     - MISC_LOCAL_CTL
     - 32
     - 0
     - R/W
     - Bit 0 reads the INTA pin, bit 1 clears and bit 2 sets the host
       interrupt. Bits 20-23 are the serial EEPROM lines. Other bits are
       stored.
   * - ``0x050``
     - MISC_CONF
     - 32
     - 0
     - R/W
     - Stored and returned. The SRAM timing it selects is not modelled.
   * - ``0x05c``
     - PCI_STATE
     - 32
     - 0
     - R/W
     - Stored. Bit 20 also reads set when the slot is narrow. Bit 18 is
       acted on only by the direct DMA channels and the wide-slot rule.
   * - ``0x068``
     - WINBASE
     - 32
     - 0
     - R/W
     - Offset of the SRAM window into card SRAM.
   * - ``0x06c``
     - WINDATA
     - 32
     - 0
     - none
     - Not implemented. ``LOG_UNIMP``.
   * - ``0x080``, ``0x084``
     - DMA write host address, high and low
     - 32
     - 0
     - R/W
     - Destination of a card-to-host transfer.
   * - ``0x090``, ``0x094``
     - DMA read host address, high and low
     - 32
     - 0
     - R/W
     - Source of a host-to-card transfer.
   * - ``0x09c``
     - DMA read length
     - 32
     - 0
     - R/W
     - Low 16 bits.
   * - ``0x0a0``
     - DMA write state
     - 32
     - 0
     - mixed
     - See :ref:`sgi-tigon-direct-dma`.
   * - ``0x0a4``
     - DMA write local address
     - 32
     - 0
     - R/W
     - Low 23 bits.
   * - ``0x0ac``
     - DMA write length
     - 32
     - 0
     - R/W
     - Low 16 bits.
   * - ``0x0b0``
     - DMA read state
     - 32
     - 0
     - mixed
     - See :ref:`sgi-tigon-direct-dma`.
   * - ``0x0b4``
     - DMA read local address
     - 32
     - 0
     - R/W
     - Low 23 bits.
   * - ``0x140``
     - CPU_STATE (processor A)
     - 32
     - ``0x00010000``
     - R/W
     - Bit 16 halts. Clearing it releases the firmware.
   * - ``0x144``
     - CPU program counter (A)
     - 32
     - 0
     - R/W
     - Stored, not used.
   * - ``0x154``, ``0x158``
     - SRAM_ADDR, SRAM_DATA
     - 32
     - 0
     - none
     - Not implemented. ``LOG_UNIMP``. The SRAM window is the download
       path.
   * - ``0x240``, ``0x244``
     - CPU control and program counter (processor B)
     - 32
     - ``0x00010000``, 0
     - R/W
     - Stored, not used.
   * - ``0x504``
     - MB_HOSTINTR
     - 32
     - 0
     - R/W
     - Interrupt hold-off and acknowledge. Bit 0 only.
   * - ``0x50c``
     - MB_CMDPROD_IDX
     - 32
     - none
     - W
     - Command ring producer doorbell.
   * - ``0x514``
     - MB_SENDPROD_IDX
     - 32
     - none
     - W
     - Transmit ring producer doorbell.
   * - ``0x51c``
     - MB_STDRXPROD_IDX
     - 32
     - none
     - W
     - Standard receive ring producer.
   * - ``0x524``
     - MB_JUMBORXPROD_IDX
     - 32
     - none
     - W
     - Jumbo receive ring producer.
   * - ``0x52c``
     - MB_MINIRXPROD_IDX
     - 32
     - none
     - none
     - Mini ring is not modelled. Writes log ``LOG_UNIMP``.
   * - ``0x600``-``0x6fc``
     - General communication registers (GCR)
     - 32
     - 0
     - R/W
     - Plain storage, with the special cases below.
   * - ``0x700``-``0x7fc``
     - Command ring
     - 32
     - 0
     - R/W
     - 64 words, card-resident.
   * - ``0x800``-``0xfff``
     - SRAM window
     - 8-32
     - 0
     - R/W
     - Byte, halfword and word accesses.

Mailbox registers other than MB_HOSTINTR are write-only. Reading one logs
``LOG_UNIMP`` and returns zero. Register names, offsets and bit meanings
are each a *software expectation* from ``if_tireg.h``. Where the Alteon manual
states them, the sections below cite it.

GCR registers the model gives meaning to:

.. list-table::
   :header-rows: 1

   * - Offset
     - Name
     - Meaning in the model
   * - ``0x600``, ``0x604``
     - PAR0, PAR1
     - Station address as the drivers read and write it. PAR0 holds
       bytes 0 and 1 in its low half. PAR1 holds bytes 2 to 5. Reset
       seeds them from the configured MAC.
   * - ``0x608``, ``0x60c``
     - GENINFO high, low
     - Address of the general information block. The low-word write
       hands the card the ring set.
   * - ``0x618``
     - OPMODE
     - Byte-swap controls, :ref:`sgi-tigon-byte-order`.
   * - ``0x628``
     - EVENTCONS_IDX
     - Stored. The model does not check the event ring for overrun.
   * - ``0x62c``
     - CMDCONS_IDX
     - Written by the model after each command doorbell.
   * - ``0x630``, ``0x634``
     - RX, TX coalescing ticks
     - Microseconds, :ref:`sgi-tigon-interrupts`.
   * - ``0x638``
     - STAT_TICKS
     - Statistics refresh period in microseconds.
   * - ``0x63c``, ``0x640``
     - TX, RX maximum coalesced descriptors
     - :ref:`sgi-tigon-interrupts`.
   * - ``0x65c``, ``0x660``
     - IFINDEX, IFMTU
     - Reported back in the statistics block.
   * - ``0x664``
     - MASK_INTRS
     - Zero permits interrupts. Any other value holds the line down and
       logs ``LOG_UNIMP``.
   * - ``0x668``, ``0x66c``
     - GLINK_STAT, LINK_STAT
     - Set to link up by the model. A guest write is stored.

The firmware-level mask polarity is a *software expectation*: the public
driver writes zero under "turn interrupts on", and nothing else attests it.

PCI State and the local control register
----------------------------------------

PCI State bit 18, "no word swap DMA", turns off word swapping for all DMA
from revision 5, and for the read channel only before that
:src:`alteon-tigon-pci-controller-rev-1.04 Table 31, p. 77`, *established*.
Bit 20 reads set on a 32-bit bus and clear on a 64-bit bus. From revision 5
a write of one forces 32-bit operation even when the host offered 64-bit
(same table), *established*. The manual gives every bit as zero at reset,
which cannot hold on a 32-bit bus, so the model follows the definition of
bit 20: it reads set whenever the card sampled a narrow slot, and also
whenever a guest wrote it set. A chip reset through MISC_HOST_CTL keeps
the stored value. A PCI reset clears it.

MISC_LOCAL_CTL bits 0 to 2 are separate from the EEPROM lines and the SRAM
bank selector: a write with bit 2 set asserts the host interrupt, a write
with bit 1 set clears it, and bit 0 reflects the pin
:src:`alteon-tigon-pci-controller-rev-1.04 Tables 27, 29, pp. 72, 74`,
*established*. None of this needs a downloaded firmware or a populated
event ring. MISC_HOST_CTL bit 6 masks
the pin, first implemented in revision 5 (pp. 73-74), *established*.
Applying it to the Tigon 2 model is *inferred* from the common register
map.

.. _sgi-tigon-eeprom:

Serial EEPROM
-------------

The drivers read the station address from a 24Cxx serial EEPROM bit-banged
through MISC_LOCAL_CTL: bit 20 is the clock, bit 21 enables the master's
data line, bit 22 is the master's data and bit 23 is the card's reply
(*software expectation*, ``if_tireg.h``). The model implements the slave
as an I2C state machine: START and STOP are data-line changes while the clock
is high, bits are sampled on the rising clock edge, the acknowledge is
driven on the rising edge that samples the eighth bit, and read data
changes on the falling edge. The control byte's bit 0
selects a read. Two address bytes follow.

The six address bytes sit at EEPROM offset ``0x8e``, behind two zero bytes
(``TI_EE_MAC_OFFSET`` ``0x8c`` plus two, ``if_ti.c`` and ``if_tireg.h``),
*software expectation*. Every other byte reads zero. The EEPROM serves the
configured MAC, not an address the guest later programmed. Writes to the
EEPROM are accepted and dropped. Each read needs its own START: the address
does not advance for a sequential read, an *approximation*.

Firmware download and bring-up
==============================

The host sequence is the NetBSD one (``ti_chipinit``, ``ti_loadfw``,
``ti_gibinit``, ``ti_init2`` in ``if_ti.c``), *software expectation*:
reset the chip, read the station address, halt the processors, write the
firmware images into card SRAM through the window, set the program counter,
release processor A, publish the general information block and bring the
stack up. The public drivers carry the firmware image in the kernel.

The model reproduces the effects the driver waits on:

1. SRAM window accesses land in card SRAM at ``WINBASE + offset`` modulo the
   SRAM size. The downloaded bytes are stored and read back, never
   executed.
2. Firmware comes up when both processor A has been released (CPU_STATE
   written with the halt bit clear) and the low GENINFO word has been
   written with a nonzero block address, in either order.
3. At that point the model reads the general information block: the event,
   transmit, standard receive, jumbo receive and receive-return control
   blocks, the event, receive-return and transmit-consumer producer
   pointers and the statistics refresh pointer. If a read fails it logs
   ``LOG_GUEST_ERROR`` and stays down until the next trigger.
4. It writes a firmware-up event (event type ``0x01``) on the event ring,
   advances the event producer index in host memory and raises INTA.

Ring sizes are fixed: event 256, transmit 512, standard receive 512, jumbo
receive 256, command 64 (``if_tireg.h``, *software expectation*). Only
two control-block fields are read: the receive-return ring length (zero
means 2048) and the transmit ring's host-ring flag. Tigon 1's
NIC-resident transmit ring is not modelled: a transmit doorbell with the
flag clear logs ``LOG_UNIMP``. The mini receive ring and the extended
scatter descriptors are not modelled.

The model does not check the host's event consumer or receive-return
consumer indices, so a guest that lets either ring fill overwrites
unread entries. A real card's behaviour there is *unknown*.

Command ring
============

The host writes a 32-bit word per command into the command ring at BAR
offset ``0x700`` and rings MB_CMDPROD_IDX. The word packs the opcode in
bits 31-24, a code in 23-12 and an index in 11-0 (NetBSD, OpenBSD and
Linux agree), *software expectation*. The model consumes words from its
own consumer index to the producer, then writes the consumer index to
CMDCONS_IDX. A producer index of 64 or more logs ``LOG_GUEST_ERROR`` and
is reduced modulo 64. Command completion itself raises no interrupt,
because a command has no host-memory ring the interrupt handler could
acknowledge. A command that posts an event (refresh, or link negotiation
when its timer fires) interrupts through that event.

.. list-table::
   :header-rows: 1

   * - Opcode
     - Command
     - Model behaviour
   * - ``0x01``
     - Host state
     - Code 1 records that the host stack is up, other codes that it is
       down. Reported as the interface administrative status.
   * - ``0x04``, ``0x11``
     - Update general communication, refresh statistics
     - Publish the statistics block and post a statistics-updated event.
   * - ``0x0a``
     - Set promiscuous mode
     - Code 1 enables, anything else disables.
   * - ``0x0b``
     - Link negotiation
     - Starts the link timer, :ref:`sgi-tigon-link`. The code is not
       examined. Before firmware-up the word logs ``LOG_UNIMP``.
   * - ``0x0c``
     - Set MAC address
     - Loads the station address from PAR0 and PAR1.
   * - ``0x0f``
     - Clear statistics
     - Zeroes the counters.
   * - ``0x10``
     - Set jumbo receive producer index
     - Same effect as the jumbo mailbox, with the index in the word.

Other nonzero words log ``LOG_UNIMP`` and are consumed. The public header
marks opcode ``0x10`` obsolete in favour of the mailbox. The model honours
both, an *approximation*: no available source says which the SGI firmware
answers. The exact opcode set the SGI firmware answers is *unknown*; the
model answers the public set.

.. _sgi-tigon-interrupts:

Interrupts
==========

One level-triggered INTA, raised when the card has advanced a host-memory
producer index (event, receive-return, transmit-consumer), by a
MISC_LOCAL_CTL bit 2 write, or by a write-channel completion with state
bit 21. There is no per-cause status register; the cause is the set of
indices in host memory.
The model keeps one pending latch and drives the pin with it when
MB_HOSTINTR is zero, MASK_INTRS is zero and MISC_HOST_CTL bit 6 is clear.
MISC_HOST_CTL bit 0 and MISC_LOCAL_CTL bit 0 read the resulting level.

A write to MB_HOSTINTR stores bit 0. Writing one acknowledges and masks
(clears the latch). Writing zero re-enables, and clears the latch unless
the previous value was one, so a completion that arrived while the line
was held off is still delivered on unmasking. This is the NetBSD sequence
(``ti_intr``), *software expectation*. A write of zero while the previous
value was zero also clears the latch, so a driver that only ever writes zero
acknowledges with it, an *approximation*.
MISC_HOST_CTL bit 1 and MISC_LOCAL_CTL bit 1 also clear the latch.

Ring completions are coalesced per direction, as the driver programs. If
the maximum count is 1 or less, or the tick value is zero, each completion
interrupts at once. Otherwise completions accumulate until the count is
reached or the tick timer, in virtual microseconds, expires. A count with
no deadline is never held, because it would stall the last completions of a
transfer. Events (firmware-up, link, statistics) are never held. The
coalescing semantics are an *approximation*: the manual defines the
registers' existence, and the model's reading follows how the public drivers
use them.

.. _sgi-tigon-byte-order:

DMA and byte order
==================

The card is a bus master: every ring, descriptor, statistics and payload
transfer goes through ``pci_dma_*`` into the PCI address space of the
slot, never to host memory directly. What the host bridge does to the bytes
is the bridge's job (:doc:`bridge`, :doc:`xbridge`).

Control bits
------------

Two independent sets of controls exist and the model keeps them apart.

- MISC_HOST_CTL bits 4 and 5 are register-file access swaps. The drivers
  set them for the host's byte order (*software expectation*). The model
  stores them and does not act on them.
- GCR OPMODE bits 1, 2 and 4 control the DMA order of descriptors and
  data: byte-swap descriptors, word-swap descriptors and byte-swap data
  (``TI_OPMODE_*`` in ``if_tireg.h``), *software expectation*.

The Alteon manual says the card swaps DMA data in groups of four or eight
bytes :src:`alteon-tigon-pci-controller-rev-1.04 s. 2.10.2, p. 22`. Each
channel normally swaps in groups of four, a per-operation "No Swap" bit
turns that off, and word swapping is normally on and turned off by PCI
State bit 18 (ss. 9.6.3, 9.6.4, pp. 82, 85), *established*. The manual does
not say how the firmware maps the OPMODE bits onto No Swap, whether the
word swap acts on 32-bit data phases, or which word of an internal
doubleword reaches byte lanes 0 to 3 of a 64-bit data phase with the word
swap off. It says host descriptor layout is a software and firmware
convention (p. 21). Those points are *unknown*. A statement of the word
placement of a 64-bit data phase with the word swap off, or a firmware
reference for the mode word, would settle them.

What the IRIX 6.5.30 driver programs is a *software expectation*, read
from the shipped kernel: MISC_HOST_CTL ``0x22000022`` (word-swap enable
and clear interrupt), OPMODE ``0x4000020c`` (descriptor word swap set,
descriptor byte swap and data byte swap clear) and PCI State
``0x760600b4``. Every set bit of the last is named by public Linux
``acenic.h`` except bit 18, which the Alteon manual defines as no word swap
DMA. IRIX therefore sets the bit that the manual says turns the card's DMA
word swap off. It writes PCI State
without reading it and does not test bit 20, so it neither acts on the
slot's width nor forces 32-bit operation. By the manual the card runs at
the width the slot gives it. The IRIX driver keeps Linux's big-endian
descriptor layout with the 64-bit buffer address as two words, high word
first.

Descriptors
-----------

Each 32-bit descriptor word is big-endian on the bus when OPMODE bit 1 is
set and little-endian when it is clear. A 64-bit host address in a
descriptor, control block or pointer is two words, high first, whatever
the swap setting (``struct ti_hostaddr``, *software expectation*). Receive
descriptors are 32 bytes (address, index and length, then reserved
words). Transmit descriptors are 16 bytes: address, then length in the high
half of the third word and flags in the low half. Receive-return
descriptors are 32 bytes. Event descriptors are 8 bytes. A descriptor is read
or written in one request and decoded from a temporary copy, so a failed
read never publishes a partly decoded descriptor.

OPMODE bit 2, descriptor word swap, acts only in the wide, swap-clear case
below. The public drivers and IRIX all set it, and the model's fixed
high-word-first address is the arrangement they write. What the bit does
to descriptor words other than address pairs is *unknown*.

Frame data
----------

OPMODE bit 4 (byte-swap data) set puts a frame on the bus as a byte
stream. Clear, each aligned 32-bit word is reversed on the bus, so a byte
at bus address ``a`` travels in lane ``a ^ 3``. This rests on two steps.
Public Linux ``acenic`` sets the bit on both byte orders to get frames in
order in host memory (*software expectation*), and the swap acts per
32-bit word (*inferred* from the two layouts of Linux's receive
descriptor, and consistent with the manual's groups of four bytes). It is
an *approximation* because no card document says how the firmware maps the
OPMODE bit onto the channel. A write touches only the lanes it covers.

IRIX clears the data byte swap. The card then puts each frame word on the
bus reversed and a swap-clear 32-bit Bridge slot reverses it again, so
frames arrive in order. In a 64-bit swap-clear slot the Bridge's doubleword
rule exchanges the halves of each doubleword instead (:doc:`bridge`).
Which of these the hardware does with a Tigon in an Origin 200 option
slot depends on facts no available source states: the slot's data-phase
width and the card's word placement with the word swap off. The composed
path is *unknown*.

Slot width
----------

The card is a 64-bit capable PCI agent. A host bridge that asserts REQ64#
for a slot tells the card at the end of reset (the interface is
``pci-64bit-device``). The model carries no data-phase width: it records
the sampled REQ64#, which makes PCI State bit 20 read clear instead of set
and enables the wide-slot rule below. In ``origin200`` the Bridge
slots 5, 6 and 7 drive REQ64#, and ``origin300`` marks two slots of each
expansion head the same way. Whether the physical option slots drive REQ64#
is *unknown*. A write of PCI State bit 20 overrides the
sampled width back to narrow, which models the manual's forced 32-bit
mode.

Wide, swap-clear placement
--------------------------

When the card sampled REQ64#, PCI State bit 18 is set and bit 20 is clear,
the model compensates for the XBridge and Bridge swap-clear 64-bit
placement. It is a composition of two models, an *approximation*, not
physical Tigon or Bridge behaviour. The physical byte placement is
*unknown*.

- Descriptors, when OPMODE descriptor word swap is set and descriptor byte
  swap is clear. On an aligned pair of words the model exchanges the two
  decoded words on read and the same exchange on write. A single word, or
  the odd last word of a descriptor, is requested at the address with bit
  2 flipped. A multi-word descriptor must start 8-byte aligned. An
  unaligned multi-word descriptor is not compensated.
- Frame payload, when OPMODE data byte swap is clear and the address
  falls in the 64-bit direct window (at or above ``0x1000000000000000``
  with bit 55 clear). Each complete aligned 8-byte unit is reversed, and
  an edge byte at address ``a`` is requested at ``a ^ 7``. Other address
  classes keep the 4-byte rule, because the card's DMA interface does not
  see the bridge's per-page choice.
- The statistics block, head copy and refreshed copy alike, takes the
  descriptor placement: the two words of each aligned 8-byte unit are
  exchanged before the write, and a block at an unaligned address is
  written unexchanged. That the block follows the descriptors is
  *inferred*, since it is host memory written by the same DMA engine under
  the same controls and no source describes a separate path. The
  placement it follows is an *approximation* and is not hardware-accurate;
  the real word placement is *unknown*. The block layout itself is
  established.

.. _sgi-tigon-direct-dma:

Direct host DMA channels
------------------------

The card has register-driven DMA between its local memory and host memory,
independent of the ring protocol. The write channel copies card bytes to
host memory and the read channel copies host bytes to the card
:src:`alteon-tigon-pci-controller-rev-1.04 ss. 9.6.1-9.6.8, pp. 81-86`,
*established*. The host low address advances without carry into the high
word. The local address is a 23-bit counter into the first 8 MiB of card
memory. Lengths are 16 bits.

The model runs a transfer when the state register is written with Active
(bit 2) set, using the addresses and length present at that moment, in
256-byte chunks, and clears Active when done. A length written afterwards
starts nothing. The manual's instructions on
whether state or length is written first conflict for a transfer started
through the state register
:src:`alteon-tigon-pci-controller-rev-1.04 ss. 9.6.3-9.6.6, pp. 82-85`.
The length and an Active-clear state write may come in either order, but
the state write that sets Active must come last. The order on revision-6
silicon is *unknown*.

.. list-table::
   :header-rows: 1

   * - State bit
     - Meaning
   * - 0
     - Write-only channel reset: clears the whole state including errors.
       Addresses and length keep their values.
   * - 1
     - No Swap: skips the four-byte-group byte swap.
   * - 2
     - Active.
   * - 18
     - Byte bucket: the transfer runs and discards its data.
   * - 20
     - No master: Active clears at once with no transfer.
   * - 21 (write channel)
     - Completion interrupt, honoured when the chip revision is 5 or
       later.
   * - 23
     - Progress. The read channel sets it during a transfer, and it stays
       set until the next state write or channel reset.
   * - 24, 25
     - Target abort and master abort, read-only.
   * - 27
     - Host address overflow, read-only.
   * - 12-16 (read state)
     - FIFO count. The read state register always returns it as all ones.

Other documented control bits are stored and ignored. The read-only error
bits stay set across later state writes and clear on channel reset. The
bit layout is from the manual's Tables 34 and 35 (pp. 82-84),
*established*. The model's choices are each an *approximation*:

- Each channel swaps data in four-byte groups unless No Swap is set, and
  exchanges the two words of each eight-byte unit unless PCI State bit 18
  is set. Only these channels and the wide-slot rule read bit 18. Ring,
  descriptor, statistics and frame DMA never word-swap, whatever it holds.
  How a host bridge then places the bytes needs its own test.
- A transfer that would cross a 4 GiB boundary does not run, sets bit 27
  and logs ``LOG_GUEST_ERROR``. A local range beyond the card SRAM logs
  ``LOG_GUEST_ERROR`` and clears Active without an error bit.
- A failed PCI transaction clears Active, leaves the addresses and length
  at the last completed chunk, sets an error bit and raises no completion
  interrupt. A transaction with no target (``MEMTX_DECODE_ERROR``) sets
  master abort. Any other failure sets target abort.

The manual defines the two aborts but not the counter, Active or interrupt
state after either (s. 2.2.3.3 and Table 3, p. 16), so the aftermath is
*unknown*.

.. _sgi-tigon-transmit:

Transmit
========

A write to MB_SENDPROD_IDX walks the host-resident transmit ring from the
model's consumer index to the producer. For each 16-byte descriptor it
reads the buffer, appends it to the frame being assembled and moves on
until a descriptor with the end flag (``0x0004``). The frame buffer holds
9600 bytes and an oversized descriptor length is clamped to it.

At the end flag the model:

1. fills checksums if the frame's descriptors asked for it. Flag ``0x0001``
   requests TCP and UDP and ``0x0002`` the IP header checksum. The model
   calculates them only when the frame is a well-formed IPv4 packet whose
   header length and total length fit inside the frame. A driver that sets
   the flags and leaves the fields zero depends on this, and without it
   every TCP segment would leave with a zero checksum. This is an
   *approximation*: no source says what the firmware computes;
2. writes the consumer index to the transmit-consumer pointer in host
   memory;
3. sends the frame to the network backend and counts it;
4. completes the descriptors through the interrupt coalescing logic.

A doorbell with no complete frame leaves the consumer index unchanged for
a frame in progress, and the next doorbell re-walks it. A failed descriptor
or payload read logs ``LOG_GUEST_ERROR`` and stops the walk with earlier
frames complete. A doorbell before firmware-up logs ``LOG_GUEST_ERROR``.
The producer index is reduced modulo 512 after a log if it is out of range.

Transmit does not look at link state. The public drivers submit
descriptors while their saved link state is down, so link down must not
make submission invalid (*software expectation*). What real firmware does
with a descriptor submitted while the physical link is down is *unknown*.

If the backend answers synchronously inside the send, the model holds that
reply until the transmit has been completed, so the guest sees consumer
index, interrupt and reply in that order.

.. _sgi-tigon-receive:

Receive
=======

A frame offered by the backend is accepted only after firmware-up and when
at least one buffer is posted on either receive ring. Otherwise the
backend queue holds it.

Address filter
--------------

The model keeps a frame addressed to the station address, to broadcast, or
to any multicast group, and in promiscuous mode every frame. Others are
consumed without a return descriptor. A frame under six bytes is consumed.
The exact multicast group filter is not modelled, so every multicast frame
is kept and the stack sorts them out. Dropping a group the driver
subscribed to would break the protocol carried on it with no message.
The return descriptor flags say unicast (``0x0020``), multicast (``0x0040``)
or broadcast (``0x0060``).

Ring selection
--------------

A frame larger than 1518 bytes goes to the jumbo ring when its control
block is published, otherwise to the standard ring. The standard ring's
maximum length is the threshold the driver sets for this choice
(``if_tireg.h``, *software expectation*). The rings carry separate control
blocks and producer pairs, so a shortage on one says nothing about the
other.

The next posted 32-byte descriptor gives the buffer address, index and
capacity. The frame is written to the buffer with the data-swap rules of
:ref:`sgi-tigon-byte-order`. It is clamped to the buffer capacity. A
descriptor claiming a buffer under 60 bytes is not a posted buffer: the
model logs ``LOG_GUEST_ERROR``, counts a discard and drops the frame. A
larger buffer is not suspect, since the length is the buffer's capacity
(*approximation*).

The model then writes a 32-byte receive-return descriptor: the buffer
address, the buffer index and landed length, the ring type
(``0x0002`` standard, ``0x0003`` jumbo) with the cast, end-of-frame
(``0x0004``) and jumbo-ring (``0x0010``) flags, and a word of checksums. The
model sets the end flag on every return descriptor, since each frame
occupies one buffer (*approximation*). The checksum word has the sum
over the IP header in its high half and over the rest of the IP packet in
its low half. Both sums are not inverted, since the driver inverts them
itself, so a well-formed header reads ``0xffff``. A frame that is not
plain IPv4 reports zero. The return
ring wraps at the length in its control block.

Arrival latency
---------------

The descriptors and payload are written as the frame arrives, but the
return-ring producer index and the interrupt wait until a fixed minimum
after the backend handed over the frame (``rx-arrival-latency-us``, default
200 microseconds, on a delay line built from code shared with the other SGI
Ethernet adapters). The
producer index is the held item, because the driver cannot see the
descriptor before it. A write to a receive producer mailbox also retries a
completion refused for a full delay line. The floor is an *approximation*:
the model has no wire, transceiver or PCI time, and the figure is not a
measurement.

Shortages
---------

When a frame needs a ring that has no posted buffer but the other ring
has one, the model discards the frame, counts it as an input discard and
logs ``LOG_GUEST_ERROR`` once at the start of the drought and once when
the ring is posted again. When neither ring has a buffer, the backend
queue holds the frame. When a frame is over the standard size and the
jumbo control block is not published, the frame is placed in the next
standard buffer and clamped to it, with no log and no discard.

Discarding at once is an *approximation*. Real firmware may hold the frame
until the host refills, and which of its counters moves is *unknown*. A
published host interface specification, or a measurement of a card whose
driver stops replenishing one ring while its counters are read, would
settle both. The model counts the discard in the interface-discard field
because that is where it counts its other receive drops.

Statistics block
================

The firmware keeps a counter block of 1 KB (``struct ti_stats``) at the
head of the general information block and refreshes a second copy at the
address in the block's last field. NetBSD and Linux give the same address
for both, so they do not show which the card writes. The IRIX driver gives
them separate addresses and its statistics command reads the counters from
the head, so a card serving it must fill the head. The model fills both.

The words the model fills (word indices from the block's start):

.. list-table::
   :header-rows: 1

   * - Word
     - Field
     - Value
   * - 12
     - ifIndex
     - IFINDEX as the host wrote it.
   * - 14
     - ifMtu
     - IFMTU as the host wrote it.
   * - 15
     - ifSpeed
     - 1 000 000 000 once the link is up, otherwise 0.
   * - 16
     - ifAdminStatus
     - 1 after a stack-up host-state command, otherwise 2.
   * - 17
     - ifOperStatus
     - 1 while the link is up, otherwise 2.
   * - 19
     - ifInDiscards
     - Frames dropped after passing the address filter.
   * - 25, 26
     - ifPhysAddress
     - The active station address in the last six bytes of the eight-byte
       field, behind two zero bytes.
   * - 36-43
     - ifHC in octets, unicast, multicast, broadcast
     - Frames and octets received, 64-bit.
   * - 44-51
     - ifHC out octets, unicast, multicast, broadcast
     - Frames and octets sent, 64-bit.

The layout is *software expectation*: the field names are the RFC 1213,
RFC 1573 and RFC 1643 MIB names in ``if_tireg.h``. A 64-bit counter is two
words, most significant first, each in the descriptor byte order. The
other words stay zero: the collision, alignment, carrier-sense, frame
check and internal MAC counters count events the model never generates, and
the firmware's own command, event and ring counters describe firmware the
model does not run. ``ifType``, ``ifDescr``, ``ifLastChange`` and
``ifHighSpeed`` stay zero because no public source says what the
firmware writes there.

All three public drivers and IRIX write a frame length into IFMTU (the
MTU plus header and check bytes: 1518 for a 1500-byte MTU, *software
expectation*), and the model reports the register unchanged. Whether real
firmware converts it is *unknown*.

A refresh happens on the refresh or update command, on the period in
STAT_TICKS (microsecond ticks; the public drivers set two seconds, IRIX
five) and, for the head only, every 100 ms whenever a published field has
moved since the last write. The 100 ms head update is an *approximation*.
IRIX programs the five-second period, sends no refresh command and still
reads the head, so a head rewritten on that period alone would show
counters from before the traffic being measured (*software expectation*).
What cadence real firmware uses is *unknown*.
Each refresh other than the 100 ms head rewrite announces a
statistics-updated event (type ``0x04``). A clear
command marks the head dirty, so the zeroes reach a reader that does not
request a refresh.

.. _sgi-tigon-link:

Link
====

The model reports a gigabit full-duplex link as a modelled fact. There is
no SerDes, PHY or negotiation. A link-negotiation command arms a timer of
50 ms of virtual time (a command received again re-arms it). When it
fires, the model sets GLINK_STAT to ``0x000c0000`` and LINK_STAT to
``0x400c0000`` and posts a link-state event (type ``0x06``, code ``0x01``,
gigabit up). Without a command the link never comes up. The model never
posts link down (code ``0x02``) and ignores the backend's carrier state.

The event type and the up and down codes (``0x01`` gigabit up, ``0x02``
down, ``0x03`` 10/100 up) are the same in all three public drivers. The
drivers take the event as carrier authority and read the status registers
only after an up event for speed and duplex (*software expectation*). The
delay and status register values are each an *approximation*: no source gives
either. What real firmware does on a carrier change before firmware-up,
during negotiation or across reset, and the exact status values for each
transition, are *unknown*.

Reset
=====

A PCI reset and a MISC_HOST_CTL chip reset (bit 3) both return the card to
the halted state. They stop the timers and clear the register block, the
processor latches, every GCR and command ring word, the parsed ring
residency, producer and consumer positions, counters, the pending
interrupt and the EEPROM state. Both set the station address back to the
configured MAC and reseed PAR0 and PAR1. The chip reset keeps PCI State.
Neither clears card SRAM, so a guest that resets mid-download does not
refetch what it wrote. The reset values are model defaults, not
measurements.

Migration
=========

State is saved in version 6 of the ``tigon`` section: the registers, SRAM,
GCR block and command ring, the ring pointers and positions the model
keeps, the statistics counters, EEPROM state, the receive delay line, and
all five timers. Rings themselves live in guest memory. After load the
model rebuilds the NIC info string and recomputes INTA from the restored
latches. Migration works only between identical builds.

Measurements
============

No read of a physical Tigon board is available to this model. Every
register value, reset value and board identifier above is therefore a
published-source value, a driver expectation or a model default, never a
measured one.

Model limits
============

- The card runs no firmware. Host-visible behaviour is reproduced, and a
  capture from the model shows what the model did, never what the firmware
  does.
- The subsystem identifiers, PCI revision, chip revision on the SGI board,
  on-card SRAM size and every reset value are unmeasured.
- Not implemented: Tigon 1's NIC-resident transmit ring, the mini receive
  ring, extended scatter descriptors, multicast group filtering, VLAN
  assist, WINDATA, SRAM_ADDR and SRAM_DATA, the semaphore and timer
  registers and the receive-side checksum choices beyond IPv4. Unused
  registers log ``LOG_UNIMP`` as listed above.
- The model ignores the backend's carrier state, posts no link-down event
  and has no 10/100 mode.
- Event and receive-return ring overrun are not detected.
- The model sets the checksum words only for IPv4.
- Guest errors that log ``LOG_GUEST_ERROR``: producer indices out of range,
  descriptor, payload and pointer DMA failures, undersized receive
  buffers, direct DMA range errors and invalid access sizes. None raises
  a bus error.
- Only the direct DMA channels and the wide-slot rule act on PCI State bit
  18. A guest that leaves it clear gets no DMA word swap on rings,
  descriptors, statistics or frames, where the hardware would apply one.
- The wide, swap-clear placement is a synthetic compensation. The
  Tigon-to-Bridge byte path is *unknown* for 64-bit data phases.

Tests
=====

``tests/qtest/tigon-test.c`` runs the card on a single Bridge with no
Origin node. It covers configuration identity, the BAR shape, chip revision
and chip reset, register round trip, PCI State and its reset behaviour,
both station-address routes including the EEPROM, firmware-up, transmit,
receive and the arrival floor, address filtering, buffer clamping,
producer-index safety, the interrupt strobes, mask and acknowledge, the
direct DMA channels and both descriptor byte orders.

``tests/qtest/sn0-tigon-test.c`` runs the card in slots of ``origin200``
through the Bridge's real byte-swap correction. It covers the ring engine
and event posting, transmit and receive payloads and errors, checksum
offload and its rejects, interrupt coalescing, MAC command and migration,
jumbo and standard ring shortages, multicast and promiscuous receive, the
IRIX acknowledge sequence, the statistics block and its byte order, and the
DMA placement matrix against every Bridge placement.

``tests/qtest/sn1-option-tigon-test.c`` runs traffic through the
``origin300`` PCI expansion topology, including PCI State width
sampling and migration, wide descriptor and payload placement, and the
direct DMA channels through the routed bus.

``tests/functional/mips64/test_irix_eg.py``, ``test_irix_eg_arp.py`` and
``test_irix_eg_frag.py`` boot an installed IRIX 6.5.30 disk and move real
TCP and UDP traffic, an ARP reply and fragmented UDP across ``eg``. They
need local guest assets and skip without them.
