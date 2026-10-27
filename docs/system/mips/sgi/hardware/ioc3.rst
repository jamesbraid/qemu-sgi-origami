.. SPDX-License-Identifier: BSD-3-Clause

.. _sgi-ioc3:

============================
SGI IOC3 base I/O controller
============================

The IOC3 is SGI's PCI base I/O controller. One 32-bit PCI device with one
1 MiB memory BAR holds two 16550 serial ports with ring DMA, a 10/100
Ethernet MAC with its buffer RAM, a PS/2 keyboard and mouse controller, a
parallel port, general-purpose pins, a one-wire bus and a byte bus for a
clock part. The Origin 200 IP29 board, the Origin 2000 and Onyx2 BaseIO
and MediaIO boards, the Octane IP30 board, the Origin 300 IO-8 board and
several add-in cards carry it, each wiring a different subset
(:doc:`ioc3-boards`). QEMU models PCI revision 1 as ``sgi-ioc3`` in
``hw/misc/sgi-ioc3.c``, with child devices ``sgi-ioc3-eth``
(``hw/net/sgi-ioc3-eth.c``), ``sgi-ioc3-kbc``
(``hw/input/sgi-ioc3-kbc.c``) and ``sgi-ioc3-parallel``
(``hw/char/sgi-ioc3-parallel.c``). The serial ring engine lives in
``hw/char/sgi-ioc-serial-dma.c``.

Facts are rated as described in :ref:`sgi-ratings`. SGI published no
IOC3 specification. Register offsets and field names come from the public
Linux and OpenBSD headers and are *software expectations*; behaviour no
public source states is an *approximation* and says so.

.. _sgi-ioc3-sources:

Sources
=======

- The *Origin and Onyx2 Theory of Operations Manual*,
  :src:`007-3439-002 p. 69`, Figure 2-25: the BaseIO block diagram with
  the IOC3, its buffer RAM, the timekeeper, the SuperIO serial block and
  the Ethernet PHY on a 32-bit PCI bus. Printed pages 71-72 describe the
  MediaIO card.
- The *OCTANE Technical Report*, :src:`octane-technical-report p. 2-25`:
  the IOC3 connects to the Octane board through a 32-bit PCI interface.
- An OpenBSD boot of a two-node Origin 200,
  :src:`dmesgd-8994 ioc0 and ioc1 attach lines`: PCI revision ``0x01``, a
  one-wire Ethernet-address part of family type ``0x11``, UARTs at
  ``0x20178`` and ``0x20170``, ``128KB SSRAM``, a DP83840 PHY at MII
  address 31 and a ``DS1742W`` clock.
- An OpenBSD boot of an Origin 300,
  :src:`marc-openbsd-sgi-139223952727384 lines 150-183`: PCI revision
  ``0x01`` and no PS/2 ports on the IO-8 board.
- SGI's *IRIX Device Driver Programmer's Guide*,
  :src:`007-0911-180 p. 135-139`: the external interrupt output's
  modes and pulse width.
- The *Octane Workstation Owner's Guide*, :src:`007-3435-004 p. 301`: the
  Octane's IEEE 1284-C parallel connector.
- Texas Instruments' TL16PIR552 datasheet,
  :src:`ti-tl16pir552-slls222a p. 40-45`: the parallel controller's
  16-byte FIFO, test mode and configuration registers.
- Two public photographs of Octane IP30 boards showing a
  ``TL16PIR552PH`` package beside the clock:
  :src:`bukosek-octane-motherboard-front` (a board whose owner's page,
  :src:`bukosek-sgi-octane hinv listing`, identifies it as
  ``030-1467-001`` revision D) and
  :src:`commons-ip30-030-0887-003-rev-f` (labelled ``030-0887-003``
  revision F).
- Linux at ``db2ddb871435``, as software expectations:
  ``arch/mips/include/asm/sn/ioc3.h`` (register layout and field names),
  ``drivers/mfd/ioc3.c`` (board dispatch and interrupt trigger types),
  ``drivers/net/ethernet/sgi/ioc3-eth.c``,
  ``drivers/input/serio/ioc3kbd.c``, ``drivers/w1/masters/sgi_w1.c`` and
  ``drivers/tty/serial/8250/8250_ioc3.c``.
- Linux ``drivers/tty/serial/ioc3_serial.c`` and ``ioc4_serial.c`` at
  tag ``v4.19``, removed from later releases: the serial ring DMA
  registers, entry formats and receive timer, as software expectations.
- OpenBSD at ``2df91d80c104``, as software expectations:
  ``sys/arch/sgi/pci/iocreg.h``, ``ioc.c``, ``sys/arch/sgi/dev/if_iec.c``,
  ``if_iecreg.h``, ``iockbcreg.h``, ``owmac.c``, ``owserial.c`` and
  ``sys/arch/sgi/sgi/ip30.h``.

Identity and placement
======================

Configuration space
-------------------

.. list-table::
   :header-rows: 1

   * - Offset
     - Field
     - Model value
     - Notes
   * - ``0x00``
     - Vendor, device
     - ``0x10a9``, ``0x0003``
     - OpenBSD binds on this pair.
   * - ``0x04``
     - Command, status
     - ``0x0000``, ``0x0280``
     - Status reports medium DEVSEL timing and fast back-to-back, read
       only. Command adds parity-error response to QEMU's writable bits.
   * - ``0x08``
     - Revision, class
     - ``0x01``, ``0xff0000``
     - No defined class.
   * - ``0x0d``
     - Latency timer
     - ``0``
     - Writable.
   * - ``0x10``
     - BAR 0
     - 1 MiB memory
     - The only BAR.
   * - ``0x2c``
     - Subsystem vendor, device
     - ``0x10a9``, ``subsystem-id``
     - Default ``0xc303``, the Origin 200 IP29 board.
   * - ``0x3d``
     - Interrupt pin
     - ``1`` (INTA)
     - The Ethernet line. The SuperIO line is separate board wiring.

PCI revision 1 is *observed* on Origin 200 and Origin 300 systems
(:src:`dmesgd-8994 ioc0 attach line`,
:src:`marc-openbsd-sgi-139223952727384 lines 150-165`). The Status value
``0x0280`` is a *software expectation*: the IRIX 6.5.30 miniroot's
configuration-space check reads the whole dword at ``0x04`` at reset and
fails the slot when the Status half is zero. The writable latency timer
and parity-error response are *software expectations* of the Origin 300
IO-8 board diagnostic, which writes ``0xff00`` over the latency word and
``0x146`` to Command and reads both back. Linux names an error-address
record at configuration ``0x40`` and ``0x44``; the model does not
implement it and those offsets read zero.

Board population
----------------

The subsystem identity tells a guest which functions the board wires up,
and the model builds the same population from it. The table follows the
per-board setup routines of Linux ``drivers/mfd/ioc3.c``, *software
expectation*, with parallel ports added from the board manuals and
OpenBSD's attachment, since Linux has no IOC3 parallel driver.

.. list-table::
   :header-rows: 1

   * - Identity
     - Board
     - Functions the model fits
   * - ``0xc300``
     - IP27 BaseIO-G
     - Ethernet, both serial ports, PS/2, M48T35 at byte-bus select 0
   * - ``0xc301``
     - IP27 MediaIO
     - Both serial ports, PS/2, parallel
   * - ``0xc302``
     - IP27 BaseIO
     - Ethernet, both serial ports, M48T35
   * - ``0xc303``
     - IP29 system board (Origin 200)
     - Ethernet, both serial ports, PS/2, parallel, M48T35
   * - ``0xc304``
     - IP30 system board (Octane)
     - Ethernet, both serial ports, PS/2, parallel, DS1685 at selects 1
       and 2
   * - ``0xc305``
     - MENET, first three chips
     - Ethernet, both serial ports
   * - ``0xc306``
     - MENET, fourth chip
     - Ethernet
   * - any other
     - Cad Duo fall-through
     - Ethernet, PS/2

Every board with Ethernet also gets the station-address part on its
one-wire bus. The ``population`` property overrides the table with a bit
mask: bit 0 Ethernet, 1 serial A, 2 serial B, 3 PS/2 ports, 4 parallel,
5 one-wire station address, and ``timekeeper-kind`` then picks the clock
(0 none, 1 M48T35, 2 DS1685). Realize refuses unknown bits, a one-wire
identity without Ethernet, an unknown clock kind and a clock kind without
an explicit population. A function the board does not fit is not
realized and its window is a hole. The PS/2 register bank is the
exception: it decodes on every board, and the population decides only
whether keyboard and mouse devices sit behind it.
:doc:`ioc3-boards` lists the boards and how each guest decides a
chip's population, and :doc:`baseio` describes how each machine places
and wires its chips.

Register map
============

The window
----------

All offsets are from BAR 0.

.. list-table::
   :header-rows: 1

   * - Offset
     - Contents
     - Access
     - Model
   * - ``0x00000-0x00063``
     - Main control block
     - 32-bit
     - Always present; ``0x00-0x18`` unimplemented
   * - ``0x00080-0x0009b``
     - Parallel DMA contexts
     - 32-bit
     - With the parallel port
   * - ``0x0009c-0x000af``
     - PS/2 controller
     - 32-bit
     - Always present
   * - ``0x000b0-0x000ef``
     - Serial ring base and per-port ring registers
     - 32-bit
     - With either serial port
   * - ``0x000f0-0x00153``
     - Ethernet MAC
     - 32-bit
     - With Ethernet
   * - ``0x04000``
     - Interrupt-output alias page
     - 32-bit
     - Not modelled
   * - ``0x08000``, ``0x0c000``
     - Serial ring alias pages, ports A and B, 16 KiB each
     - 32-bit
     - With that port
   * - ``0x20141``, ``0x20142``
     - UART and keyboard configuration bytes
     - 8-bit
     - Not modelled
   * - ``0x20150-0x2015a``
     - Parallel ECP register file
     - 8-bit
     - With the parallel port
   * - ``0x20168``
     - SuperIO clock register file
     - 8-bit
     - Not modelled
   * - ``0x20170``, ``0x20178``
     - 16550 register files, ports B and A
     - 8-bit
     - With that port
   * - ``0x40000-0x7ffff``
     - Ethernet buffer RAM
     - 32-bit
     - Always present
   * - ``0x80000``, ``0xa0000``, ``0xc0000``, ``0xe0000``
     - Byte-bus selects 0-3
     - 8-bit
     - Per the board's clock

The offsets are Linux's and OpenBSD's, *software expectation*. An access
anywhere else, or to a function the board does not fit, reaches no device
and returns a decode error to the PCI bus. :doc:`bridge` and
:doc:`xbridge` describe what the guest then sees.

Main control block
------------------

.. list-table::
   :header-rows: 1

   * - Offset
     - Name
     - Width
     - Reset
     - Access
     - Notes
   * - ``0x00-0x18``
     - Configuration copy and error record
     - 32
     - ``0``
     - none
     - Reads zero and writes are dropped, both with ``LOG_UNIMP``.
   * - ``0x1c``
     - Interrupt status
     - 32
     - ``0``
     - R, W1C
     - Level bits ignore the write (`Interrupts`_).
   * - ``0x20``
     - Interrupt enable, set
     - 32
     - ``0``
     - R, W1S
     - Reads the live mask.
   * - ``0x24``
     - Interrupt enable, clear
     - 32
     - ``0``
     - W1C
     - A read returns zero with ``LOG_UNIMP``.
   * - ``0x28``
     - Chip control
     - 32
     - ``0``
     - RW
     - Stores bits 0-18. Bit 22, arbiter idle, always reads one.
   * - ``0x2c``
     - Interrupt output
     - 32
     - ``0``
     - RW
     - Stores bits 0-18 and 30. Bit 31 reads the output level.
   * - ``0x30``
     - MicroLAN control
     - 32
     - ``0``
     - RW
     - A write runs one one-wire slot.
   * - ``0x34``
     - Pin control, set
     - 32
     - ``0``
     - R, W1S
     - Reads the control word.
   * - ``0x38``
     - Pin control, clear
     - 32
     - ``0``
     - W1C
     - A read returns zero with ``LOG_UNIMP``.
   * - ``0x3c``
     - Pin data
     - 32
     - ``0``
     - RW
     - Board inputs override the stored bits on read.
   * - ``0x40-0x60``
     - Pin registers 0-8
     - 32
     - ``0``
     - RW
     - Bit 0 views one bit of pin data.

Access widths and byte lanes
----------------------------

The 16550 files, the ECP file and the byte-bus selects accept only byte
accesses. Every other block accepts only aligned 32-bit accesses. QEMU's
memory core refuses other widths before they reach the device. Each
register sits at its own offset; the model adds no ``^ 3`` of its own.
Linux's 8250 glue and OpenBSD's Bridge accessors both apply that XOR for
a big-endian processor reaching a little-endian PCI device, and they
expect the same physical layout, *software expectation*.

BAR accesses pass through a chip-owned adapter that keeps a full 32-bit
word's bytes and places a narrower access at its address XOR the
complement of its size within the word. With the host bridge's ordinary
swap-clear lane placement this cancels out, so a byte access to
``0x20178`` reaches port A's receive register. The adapter is a modelling
convention, an *approximation*, not a description of the chip's pins.

Interrupts
==========

The chip has two interrupt outputs. The Ethernet block drives PCI INTA.
Everything else funnels through the SuperIO interrupt status register and
leaves on a second line that the board wires to a Bridge interrupt pin,
pin 3 on the Origin 200. OpenBSD establishes separate Ethernet and
SuperIO handlers (:src:`dmesgd-8994 ioc0 attach lines` prints
``ethernet irq 2`` and ``superio irq 3``), *observed*. The model drives
the SuperIO line from ``status & enable``, recomputed on every write to
either register and on every source change.

.. list-table::
   :header-rows: 1

   * - Bits
     - Source
     - Model behaviour
   * - 0, 9
     - Transmit ring empty, ports A and B
     - Level: set while ring DMA is on and nothing is left to send
   * - 1, 10
     - Receive ring full
     - Latched
   * - 2, 11
     - Receive high water
     - Level against the waiting-entry count
   * - 3, 12
     - Receive timer expired
     - Latched
   * - 4, 13
     - Carrier detect changed
     - Never raised
   * - 5, 14
     - Clear-to-send changed
     - Never raised
   * - 6, 15
     - 16550 pass-through
     - Level: follows the 16550's interrupt line
   * - 7, 16
     - Explicit transmit interrupt
     - Latched
   * - 8, 17
     - Ring DMA error
     - Latched
   * - 18
     - Parallel service
     - Level: cleared by a write to the port's extended control register
   * - 19, 20
     - Parallel DMA contexts A and B
     - Latched
   * - 21
     - Parallel DMA error
     - Latched
   * - 22
     - Keyboard and mouse
     - Latched
   * - 27
     - Interrupt output pulse
     - Latched on every rising edge of the output
   * - 28
     - Interrupt input pulse
     - Never raised

The source assignment is Linux's and OpenBSD's, *software expectation*.
Writing ones to the status register clears latched bits. It does not
clear the level bits: 0, 2, 6, 9, 11, 15 and 18. Linux installs a level
handler for bits 6 and 15 and an edge handler for the rest
(``drivers/mfd/ioc3.c``), which makes the two pass-throughs levels and the
keyboard bit a latch, *software expectation*. The parallel service bit
and the high-water and transmit-empty bits as levels are
*approximations*. The IRIX 6.5.30 ring console enables high water and
never acknowledges it; against a latched bit its service loop never
finished. Linux's ring driver re-reads transmit-empty after a write and
stops it by masking, which a latch would not need.

The enable mask is one register behind a set address and a clear
address. Linux's dispatcher intersects the status with a read of the set
address, so that address returns the live mask, *software expectation*.
What the clear address returns is *unknown*.

Chip control and the interrupt output
=====================================

Chip control
------------

Bits 1-7 and 8-14 of chip control name where ports A and B sit in the
SuperIO window, as the byte offset divided by eight. Linux's ``ioc3.h``
names them the serial DMA poll addresses, *software expectation*. A
port's ring engine reaches its 16550 only while its field holds ``0x2f``
for port A or ``0x2e`` for port B. Aimed anywhere else, the engine does
nothing. The Origin 300 IO-8 board diagnostic zeroes both fields, then
drives port B through its byte window with B's ring DMA still enabled and
expects the character to stay in the 16550. What the chip's engine does
at another offset is *unknown*. Bit 0 (SuperIO reset) and bits 15-18
(byte-bus strobe length) are stored and have no effect. Bit 22 reads as
arbiter idle in every state, because the IRIX console setup waits for
it, *software expectation*.

Interrupt output
----------------

Bits 16-18 select the mode, bits 0-15 the count, bit 30 diagnostic mode,
and bit 31 reads the output level. The modes are fixed low (``000``),
fixed high (``100``), one pulse (``101``), a pulse train (``110``) and a
square wave (``111``). A pulse is three ticks of 7.8 µs, 23.4 µs, and a
new command ends the running one (:src:`007-0911-180 p. 135-139`,
Linux ``ioc3.h``), *established*. The model:

- applies a fixed level within the write, so the next read sees it. The
  IO-8 board diagnostic reads the register at once 500 times and requires
  the level on every read, *software expectation*;
- starts a timed mode one nanosecond of virtual time after the write.
  A pulse train repeats every count plus one ticks. A square wave toggles
  every count plus one ticks;
- runs every transition already due before answering a read, so a tight
  read loop sees both levels of a pulse train;
- posts status bit 27 on every rising edge in every mode, whether or not
  the source is enabled, and on no falling edge. The one-pulse edge is
  documented; the fixed-high edge is a *software expectation* of the IO-8
  board diagnostic; the pulse-train and square-wave edges are *inferred*
  by extension;
- stores every write, but runs only the supported commands: fixed levels
  with any count, one pulse with count 0 or 2 to ``0xfffe``, a pulse train
  with count 3 to ``0xfffe``, a square wave with count 2 to ``0xfffe``,
  and none with bit 30 set. Any other command leaves the running output
  alone. What the chip does with diagnostic mode, the undefined modes and
  the unsupported counts is *unknown*.

The output pin itself is not wired to anything outside the chip, and
nothing raises the input-pulse source, bit 28.

General-purpose pins
====================

Pin control holds a direction bit per pin in bits 0-7, and bit 21
connects the MicroLAN engine to its pin. Pin data and the nine pin
registers at ``0x40``-``0x60`` view the same pin levels; a pin register's
bit 0 reads and writes one bit of pin data. Linux names pin 5 the PHY
reset and pins 6 and 7 the mode selects for ports B and A, *software
expectation*. With pin 5 set as an output and driven low, the chip raises
its ``gpio5-reset-request`` output, which a board can connect to the
PHY's reset input.

The ``gpio-input-mask`` and ``gpio-input-values`` properties make chosen
pins read-only board inputs: a read of pin data or of a pin register
returns the property's value for those bits, whatever software wrote. The
Octane machines use this for pin 2, which reads 1 on Octane and 0 on
Octane2 according to OpenBSD ``ip30.h``, *software expectation*. Writes
to pin data store all 32 bits, and the stored word is what reads back
outside the input mask. Pins 0-1, 3-4 and 8 have no wiring in the model,
and the edge and level selections in pin control bits 16-19 are stored
only.

Serial ports
============

16550 register files
--------------------

Each port is QEMU's 16550 core at an 8-byte window, port A at
``0x20178`` and port B at ``0x20170``, *observed*
(:src:`dmesgd-8994 com0 and com1 lines`). OpenBSD drives them with its
ordinary ``com`` driver and enables only the pass-through interrupt bits,
so the byte windows alone give it a console. The ``chardev-a`` and
``chardev-b`` properties connect the ports.

The model adds four things to the 16550 core:

- **Clock pre-divider.** While the divisor latch is selected, a write to
  the scratch register (offset 7) also programs a clock pre-divider whose
  value is twice the divider. Linux's ring driver writes it there and
  computes the clock as 44 MHz divided by the value, starting from 6, and
  Linux's 8250 glue assumes 22 MHz divided by 3, *software expectation*.
  The model derives the baud base as 22.1184 MHz divided by half the
  written value, divided by 16, so a divisor of 48 gives exactly 9600
  baud. The 22.1184 MHz crystal is an *approximation*: it differs from
  Linux's 22 MHz by half a percent and makes the standard rates exact.
  Writes of 0 and 1 leave the clock alone. The scratch byte still reads
  back as written.
- **Auto flow control bit.** Modem control bit 5 reads back as written,
  where the 16550 core drops it. The automatic flow control it would
  enable is not modelled.
- **Factory test writes.** A write to line status (bits 0-6) or modem
  status (bits 0-3) is held and ORed into the next read of that register,
  then cleared. The modem-status form is a *software expectation* of a
  diagnostic, not of the 16550 datasheet.
- **Loopback clear-to-send.** In loopback with the ring engine's flow
  control bit set, modem status reports clear-to-send asserted even when
  request-to-send is clear, a *software expectation* of the IO-8 board
  diagnostic. Only the reported level is modelled.

Transmit record
---------------

The ``record-a`` and ``record-b`` properties name an optional second
character device per port. Every character the chip hands a port's
16550 for transmission, from the byte window or the ring engine, is also
written there in full, before the port's own backend has a chance to
refuse it. A character sent while the port is in loopback is left out,
because no wire carried it. The record is the account of what the guest
transmitted even when a stalled consumer on the port's own backend loses
characters. Point it at a backend that cannot block, such as a file:
a blocking record backend stalls the transmit path it records. A
character written to the transmit holding register while the
transmitter is full is lost on the wire but still appears in the record.

Serial ring DMA
===============

Each port has an engine that moves characters between its 16550 and two
rings in memory, so a driver can run the port without touching the byte
window. The layout below follows Linux's ring drivers
(``ioc3_serial.c`` and ``ioc4_serial.c`` at ``v4.19``) and ``ioc3.h``,
*software expectation*. The firmware, Linux and OpenBSD consoles use the
byte windows. The firmware's base I/O diagnostic and the IRIX ring
console use the rings.

Registers
---------

.. list-table::
   :header-rows: 1

   * - Offset
     - Name
     - Notes
   * - ``0xb0``, ``0xb4``
     - Ring base, high and low
     - Low bit 0 selects 4 KiB rings over 1 KiB; bits 12-31 are address.
   * - ``0xb8``, ``0xd4``
     - Control, ports A and B
     - Bits 0-8 high-water mark in entries, 17 flow control, 27 receive
       drain, 28 ring DMA enable, 29 pause, 31 channel reset. Bits 16 and
       30 read status. Other bits are stored.
   * - ``+0x04``
     - Transmit produce
     - Software advances it.
   * - ``+0x08``
     - Transmit consume
     - The engine advances it.
   * - ``+0x0c``
     - Receive produce
     - The engine advances it. Bits 24-26 report characters held in the
       packer.
   * - ``+0x10``
     - Receive consume
     - Software advances it. Bit 31 arms the receive timer and reads back
       the arm.
   * - ``+0x14``
     - Receive timer
     - Reload in bits 0-11, count in bits 16-27, 16 kHz ticks.
   * - ``+0x18``
     - 16550 shadow
     - Read-only mirror of the port's line and modem status.

One base places all four rings in the order port A transmit, port A
receive, port B transmit, port B receive, each of the selected size. A
ring's address is derived, not readable. Indexes are byte offsets in
8-byte entries, as wide as the ring is long. Writes to the four indexes
store only that pointer field. The main block accepts writes to the two
indexes the engine advances, because the IO-8 board diagnostic writes and
reads them back.

Each port also has an alias page at ``0x08000`` or ``0x0c000`` showing
its seven registers at the same offsets, so a process can map one port.
Through an alias page the transmit consume index, the receive produce
index and the shadow are read-only. Offsets past the seventh register
log ``LOG_UNIMP``.

Ring entries
------------

An entry is four data bytes followed by four control or status bytes,
one per data byte.

Transmit
    The top two bits of a control byte type its data byte: ``0x40`` a
    character, ``0x80`` a modem-control write, ``0xc0`` a delay of that
    many milliseconds, ``0x00`` nothing. A ``0x00`` slot ends the entry
    and the slots after it are not sent. Bit 5 asks for the explicit
    transmit interrupt once the slot is done, or once a delay ends. The
    engine fetches an entry once and keeps it until its last slot has
    gone, so rewriting the ring under a half-sent entry changes nothing.
    A character goes to the 16550 only when its transmit holding register
    is empty. A delay holds the walk, with control bit 16 set until it
    ends.

    Outside loopback the 16550 core takes each character at once. A
    port with no backend completes it, as a line with nothing attached
    would; a backend that cannot take it holds the walk for at most two
    seconds before the core gives the character up. That bound is the
    core's approximation, not a hardware value.

    In loopback a full receive ring does not stop the transmit walk.
    The looped-back character the ring cannot take overruns the 16550,
    as it would arriving off a line, and the overrun bit reaches the
    status byte of the next character posted. Whether hardware flow
    control changes this is not known. Characters arriving from the
    backend while the port is in loopback are discarded on arrival and
    logged as a guest error, because loopback disconnects the receiver
    from the line; discarding at arrival rather than at the exact moment
    the modem-control write lands is an approximation.

Receive
    Characters pack four to an entry. Each status byte carries
    data-valid (bit 7), modem-valid (6), carrier (5), clear-to-send (4),
    break (3), framing (2), parity (1) and overrun (0). An entry posts
    when its fourth character arrives. Fewer wait in the packer until the
    receive timer expires or software drains, as Linux's ring driver
    comment describes. Unfilled slots carry no valid flag.

Receive timer
-------------

Bit 31 of a receive-consume write sets the arm. A write with bit 31
clear leaves it, matching Linux's ring driver, which arms when a read
starts and ends the read without the bit. The timer starts at the first
character received while armed. Expiry posts what the packer holds,
raises bit 3 or 12 and spends the arm. That a running timer is not
restarted by later characters is an *approximation*. Expiry raises its
source even when the ring is already empty, an *approximation*: without
it the IRIX 6.5.30 ring console stalls with the arm spent. A write to the
timer register loads the reload and the idle count; the count reads the
ticks left while the timer runs.

Commands, pause and errors
--------------------------

- Channel reset (control bit 31) returns the four indexes to zero, clears
  the packer, the arm and any delay, and keeps the other control bits and
  the timer reload.
- Receive drain (bit 27) posts what the packer holds and reads back set
  until it has. It waits while ring DMA is off or paused and while the
  ring is full, as Linux's ring driver expects.
- Pause (bit 29) stops the walk, reads back bit 30, and freezes the
  receive timer and any delay where they stand, an *approximation*.
- A full receive ring raises receive-full and keeps the characters in the
  packer and the 16550 FIFO, which stops the backend feeding more.
- A ring access the fabric refuses raises the port's DMA error source. A
  refused transmit fetch leaves the consume index on that entry. A
  refused receive write leaves the produce index unchanged.

In loopback the engine drains the receiver after each character it sends,
because the 16-byte FIFO is far smaller than the rings the diagnostic
fills. Each looped character also waits one character time on the line
before the receiver sees it, and the engine sends the next one only then.
Any access to the serial registers first applies loopback arrivals,
delays and timer expiries already due.

16550 shadow
------------

The shadow assembles bits 0-7 from line status, bit 16 delta
clear-to-send, bit 19 delta carrier, bit 20 clear-to-send, bit 23
carrier, and bits 24-28 DTR, RTS, OUT1, OUT2 and loopback from modem
control. It reads the 16550's fields directly, so a shadow read does not
clear the line-status and modem-status latches that a byte-window read
clears. In loopback it reflects the looped modem lines. Writes are
discarded without a report, because the stock firmware and OpenBSD write
it on every boot.

One-wire bus
============

MicroLAN engine
---------------

A write to MicroLAN control runs one one-wire time slot: bits 10 and up
give the time to hold the bus low and bits 2-9 the rest of the slot, both
in microseconds. Bit 1 reads done and bit 0 the sampled level. OpenBSD's
and Linux's one-wire drivers agree on this layout, *software
expectation*. The model completes the slot inside the write, so done is
already set when software polls. With pin control bit 21 clear, or on a
board with no part on the bus, the wire reads high.

Station-address part
--------------------

On a board with Ethernet the bus carries one DS2501, a 512-bit one-wire
EPROM, holding the station address. Published Origin 200 boots attach it
as a family-type ``0x11`` part (:src:`dmesgd-8994 owmac0 line`),
*observed*; OpenBSD's ``owmac`` accepts that type only with the custom
bit set and serial-number top bits ``0x5e7``. The model uses family
``0x91`` and a 48-bit serial from the ``onewire-serial`` property, whose
default keeps the ``0x5e7`` prefix above a synthetic marker. The part
answers Search ROM, Read ROM, Match ROM and Skip ROM, and the Read Memory,
Read Data with CRC and Read Status commands. Write commands are refused
with ``LOG_UNIMP``.

The first page holds the record both guests read: byte 0 ``0x0a``, the
station address reversed into bytes 5-10, and a CRC-16 over bytes 0-10
stored inverted in bytes 11 and 12, so a running CRC over the record
ends on the one-wire residue. That layout is a *software expectation* of
the OpenBSD and Linux readers. The rest of the page reads zero. The
address comes from the Ethernet child's ``mac`` or the ``board-mac``
property.

Board record part
-----------------

Setting any of ``board-part``, ``board-barcode``, ``board-revision`` or
``board-name`` adds a second, DS2505-class part on the same bus holding
that board record, with its serial from ``board-onewire-serial``. Add-in
cards that keep their identity on the IOC3's bus use it, such as the Cad
Duo card. OpenBSD reads a board record here to choose the chip's
population, and an Origin 200 bus must carry none: the published Origin
200 boot shows only the station-address part, and OpenBSD stops attaching
the chip when it reads a part number it does not know.

Ethernet MAC
============

The MAC is SGI's own design; no catalogue part matches its registers. The
PHY sits behind the MII management interface: a DP83840A at address 31
by default, a DP83843 when ``phy-dp83843`` is set, or an ICS1890 when
``phy-ics1890`` is set (:doc:`ics1890`). ``phy-addr`` sets the address.
:doc:`dp8384x` describes the DP83840A and DP83843.

Registers
---------

.. list-table::
   :header-rows: 1

   * - Offset
     - Name
     - Notes
   * - ``0xf0``
     - MAC control
     - Bit 1 promiscuous, 2 pad short frames, 3-8 receive offset in
       16-bit units, 9 buffer-RAM parity check, 13-16 transmit DMA,
       transmit, receive DMA and receive enables, 17 MAC loopback, 31
       reset.
   * - ``0xf4``
     - Interrupt status
     - Write one to clear.
   * - ``0xf8``
     - Interrupt enable
     - Writing it recomputes the line at once.
   * - ``0xfc``
     - Receive control
     - Bits 0-8 receive threshold.
   * - ``0x100``, ``0x104``
     - Receive ring base
     - Low word bits 12-31; 512 eight-byte buffer addresses.
   * - ``0x108``
     - Receive barrier
     - Stored, no effect.
   * - ``0x10c``
     - Receive consume
     - The engine advances it; byte offset, 12 bits.
   * - ``0x110``
     - Receive produce
     - Software advances it; bit 31 arms the receive timer.
   * - ``0x114``
     - Receive timer
     - Reload, bits 0-10.
   * - ``0x118``, ``0x11c``
     - Transmit control, random seed
     - Stored, no effect.
   * - ``0x120``
     - Collision and deferral counters
     - A read returns the value and clears it.
   * - ``0x124``
     - Buffer index
     - Stores ``0xffffdf1f`` of a write.
   * - ``0x128``, ``0x12c``
     - Transmit ring base
     - Low word bit 0 selects 512 descriptors over 128; bits 14-31
       address.
   * - ``0x130``
     - Transmit consume
     - The engine advances it; bit 31 idle.
   * - ``0x134``
     - Transmit produce
     - Software advances it; a write starts the walk.
   * - ``0x138``, ``0x13c``
     - Station address
     - Bytes 0-3 in the low register, 4-5 in the high.
   * - ``0x140``, ``0x144``
     - Multicast hash
     - High and low 32 bits of the 64-bit filter.
   * - ``0x148``
     - MII control
     - Bits 0-4 register, 5-9 PHY address, 10 read. Busy (bit 11) reads
       zero.
   * - ``0x14c``
     - MII read data
     - Writes log ``LOG_GUEST_ERROR``.
   * - ``0x150``
     - MII write data
     - A write request sends it to the PHY.

All reset to zero except MII read data, which reads ``0xffff`` until the
first read request. Offsets and fields are Linux's and OpenBSD's,
*software expectation*. Indexes are byte offsets, a property all three
drivers rely on.

Transmit
--------

A transmit descriptor is 128 bytes in the ring: a command word, a
buffer-count word, two 64-bit buffer pointers at 8 and 16, and 104 bytes
of inline data at 24. The command word's bits 0-10 give the frame
length, bit 12 asks for the completion interrupt, and bits 16, 17 and 18
mark the inline data and the two buffers as valid; bit 19 asks for a
checksum. The buffer-count word gives the inline length in bits 0-6 and
the buffer lengths in bits 8-18 and 20-30.

With both transmit enables set, a write to the produce index, or setting
the enables with descriptors already queued, walks the ring to the
produce index. The engine fetches each descriptor in one transfer, gathers
the inline data and buffers, trims to the frame length and sends it. The
inline length is capped at 104 even though its field can name 127. With
the checksum bit set the model computes the TCP or UDP checksum in full,
an *approximation*. With pad enabled a short frame is zero-padded to 60
bytes. Completion sets status bit 22 for descriptors that asked and bit
16 once a walk has sent anything; the consume index reads bit 31 when it
equals produce. An index off a 128-byte boundary is aligned down and
logged with ``LOG_GUEST_ERROR``. A refused descriptor or buffer read
stops the walk on that descriptor with ``LOG_GUEST_ERROR``.

Receive
-------

Receive needs both receive enables and a PHY whose data path is enabled.
A frame is accepted if promiscuous mode is on, if it is addressed to the
station address programmed in ``0x138``/``0x13c``, if it is broadcast,
or if it is multicast and its hash bit is set. The hash is the low six
bits of the frame's little-endian CRC-32, bit-reversed, indexing the low
register for 0-31; this is Linux's ``ioc3_hash``, *software expectation*.
The backend's own MAC address plays no part. A filtered frame is consumed
and dropped.

The engine reads the buffer address at the consume index and writes the
frame at the MAC control offset. The buffer's first word holds valid
(bit 31), the byte count (bits 16-26) and a 16-bit ones'-complement sum
of the frame (bits 0-15). The second word holds good (30), bad (29), long
(28), broadcast (27), multicast (26) and the byte count split into bits
12-14 and 16-25. Both guests read the count as the wire length including
the FCS, so the model adds four phantom FCS bytes, writes them as zero
and sums them as zero, an *approximation*. Frames over 2040 bytes are
truncated, logged with ``LOG_GUEST_ERROR``, and marked long and bad.

The receive sources are raised when the frame's completion is published:
the timer source (bit 0) when the timer is armed and its reload is zero,
and the threshold source (bit 1) when the buffers still available are at
or below the threshold. Both guests write a zero reload and arm the
timer. A non-zero reload logs ``LOG_UNIMP`` and raises nothing. No other
receive or transmit error source is ever raised.

The model holds each completion, its valid bit, consume index and
sources, for ``rx-arrival-latency-us`` of virtual time, 200 µs by
default, after the frame arrives; the payload is already written. This is
an *approximation*, a floor that keeps a guest from seeing a frame faster
than a wire could carry it. A rewritten consume index discards held
completions with ``LOG_GUEST_ERROR``. A refused ring-entry or buffer
access drops the frame with ``LOG_GUEST_ERROR``.

Interrupt acknowledgement
-------------------------

The line follows ``status & enable``. If a source fires again while its
bit is still set and unacknowledged, the model remembers it, and the
write that acknowledges the bit raises the line again as a fresh edge.
All three public drivers write back exactly the status they read, which
this relies on. The mechanism is an *approximation*: it keeps an
edge-triggered interrupt path from losing a completion.

Loopback, reset and the station address
---------------------------------------

MAC loopback (control bit 17) turns a frame round before FCS and status:
the received count equals the transmitted length and the second header
word is zero. PHY loopback runs the whole receive path, with the count
four higher and real status. The stock power-on diagnostic requires that
difference, *software expectation*. Either loopback disconnects the
network backend in both directions.

Writing MAC control with bit 31 set resets the engine registers but keeps
both ring bases, because the stock power-on diagnostic programs the bases
once and never again. Control then reads ``0x80200000`` until the next
write. Writes to the counters and buffer index are stored only while the
MAC is stopped and out of reset; otherwise they log ``LOG_GUEST_ERROR``.
What the chip does with those writes is *unknown*; the stored values are
an *approximation* that satisfies the stock diagnostic's read-back.

A default-generated MAC address takes SGI's ``08:00:69`` prefix, because
the firmware expects a real part's prefix in the station-address part.
An address given with ``mac`` or ``board-mac`` is kept as given, and a
``-nic mac=`` that differs from ``board-mac`` is refused.

Buffer RAM
==========

The window at ``0x40000`` is 64 Ki slots of 32 bits. Each stores 16 data
bits and a parity bit at bit 16; the rest read zero. Linux's ``ioc3.h``
names the data and parity masks, *software expectation*. The Origin 200
has the full 128 KiB: its OpenBSD driver reports ``128KB SSRAM``
(:src:`dmesgd-8994 iec0 line`), *observed*, which means a pattern at the
first slot and its complement at offset ``0x20000`` both read back. The
model keeps the two halves distinct for that reason.

While MAC control bit 9 enables parity checking, a read sets bit 17 when
the stored 17 bits have odd parity, and a write with bit 17 set stores
generated even parity instead of the written bit 16. That write rule is
an *approximation* fitted to two diagnostics' expectations. With checking
off, bit 17 reads zero and the written parity bit is stored. No frame
data passes through this RAM in the model; the MAC reads and writes guest
memory directly. Reset leaves it unchanged.

PS/2 controller
===============

The controller sits at ``0x9c``. It holds characters in the IOC3's own
format and passes bytes to QEMU's generic PS/2 keyboard and mouse
devices. The byte-level PS/2 protocol is theirs.

.. list-table::
   :header-rows: 1

   * - Offset
     - Name
     - Notes
   * - ``0x9c``
     - Control and status
     - Stores ``0x00fcffff`` of a write; status bits read zero.
   * - ``0xa0``, ``0xa4``
     - Keyboard and mouse receive
     - Read pops the port's holding.
   * - ``0xa8``, ``0xac``
     - Keyboard and mouse transmit
     - Low byte goes to the device.

A receive read packs up to three held characters, oldest in the highest
slot: slot *p* carries its byte at bits *8p* and its valid flag at bit
*29 + p*. Bit 27 is set for the mouse port and bit 28 reports a lost
character. The read empties the holding and clears the flags. The
per-character error flags never set. Offsets and fields are Linux's and
OpenBSD's, *software expectation*; the bit 27 polarity is Linux's.

Control bits 20-23 are clamps that stop a port's device after one or
three characters, keyboard and mouse in turn. A clamped port leaves the
rest of the device's bytes in the device until a read or a clamp change
releases them; an unclamped port takes everything and a fourth
character is lost with bit 28 set. Linux arms clamp-after-three on both
ports. The controller asserts its line for every character it takes and
lowers it when a read leaves both holdings empty. The chip latches that
into status bit 22, which only a write-one-to-clear acknowledges.
Per-character assertion and the release of a clamp within the read are
*approximations*. Writes setting the emulation or fast-clock bits (12,
13) log ``LOG_UNIMP``. Reads of the transmit registers and writes to the
receive registers log ``LOG_GUEST_ERROR``. Transmit completes inside the
write, so the write-pending status never reads set.

On a board without PS/2 ports, such as the Origin 300 IO-8 board, the
bank still decodes and control reads back what was written, but there is
no device: transmit writes log ``LOG_UNIMP`` and draw no reply, and
receive reads return zero. That the register bank decodes there is
*inferred*: OpenBSD's Origin 300 attachment omits only the keyboard
child, and no public source shows a board-level decode disable.

The write-only ``activate-keyboard`` property on ``sgi-ioc3-kbc`` routes
QEMU's host key events to that controller's keyboard, for machines with
two of these controllers. The guest cannot see it.

Parallel port
=============

The port has two register blocks: two DMA contexts at ``0x80`` in the
main block and a byte-wide ECP register file at ``0x20150``. Offsets are
Linux's and OpenBSD's, *software expectation*. ``parallel-chardev``
connects the cable to a character device. The model fits the port on the
Origin 200 IP29 board, the MediaIO card and the Octane IP30 board. The
Octane's IEEE 1284-C connector is *established*
(:src:`007-3435-004 p. 301`).

DMA contexts
------------

.. list-table::
   :header-rows: 1

   * - Offset
     - Name
     - Notes
   * - ``0x80``, ``0x84``
     - Context A address, high and low
     - Stored.
   * - ``0x88``
     - Context A count
     - Bits 0-16 count, 30 armed, 31 last; other bits read zero.
   * - ``0x8c``
     - DMA control
     - Bit 29 from peripheral, 30 reset, 31 enable. Bits 0-5 read
       armed, last and busy for A then B.
   * - ``0x90``, ``0x94``
     - Context B address
     - Stored.
   * - ``0x98``
     - Context B count
     - As context A.

With DMA control enabled and out of reset, and the ECP extended control
DMA bit set, the engine drains each armed context in order A then B: it
reads the count's bytes from memory and writes them to the cable at
once, clears the count and the arm, and latches that context's source
(bit 19 or 20). A context marked last also asserts the service source,
bit 18, until the next extended-control write. Busy never reads set. A
transfer from the peripheral logs ``LOG_UNIMP`` and leaves the context
armed. An unreadable buffer latches the DMA error source, bit 21, with
``LOG_GUEST_ERROR``. Transferring in the write is an *approximation*; the
chip's pacing is *unknown*.

ECP register file
-----------------

.. list-table::
   :header-rows: 1

   * - Offset
     - Name
     - Notes
   * - ``0x20150``
     - Data
     - Stored.
   * - ``0x20151``
     - Status
     - Reads ``0x08``: no fault, nothing else. Writes log
       ``LOG_GUEST_ERROR``.
   * - ``0x20152``
     - Control
     - Stored as written.
   * - ``0x20158``
     - FIFO, or configuration A in mode 7
     - A FIFO write outside test mode goes straight to the cable.
   * - ``0x20159``
     - Configuration B
     - Stored.
   * - ``0x2015a``
     - Extended control
     - Stores bits 2-7; bits 0 and 1 read FIFO empty and full.

Any other offset in the file logs ``LOG_UNIMP``. A status of ``0x08``
says no fault and never busy, so a driver waiting on the printer never
waits. Outside test mode the FIFO is bottomless: empty always reads set
and full never does. A FIFO read outside test or configuration mode
returns zero with ``LOG_GUEST_ERROR``, as does a test-mode read with
nothing queued. Every extended-control write clears the service source
first, an *approximation*; whether the service and fault-interrupt bits
are active high or low on this part is *unknown*, so the model arms
nothing from them.

Test mode (mode 6) keeps a FIFO whose depth is the ``fifo-depth``
property, zero by default. Writes queue and reads return the oldest byte;
the empty and full flags follow its occupancy. Entering mode 0 or 1
empties it. Generic parts drop a write to a full FIFO with ``LOG_UNIMP``.

TL16PIR552
----------

The TI TL16PIR552 is a dual UART and IEEE 1284 parallel controller with a
16-byte parallel FIFO (:src:`ti-tl16pir552-slls222a p. 1, 40`),
*established*. Its package sits beside the clock on Octane boards of
revision D and revision F (:src:`bukosek-octane-motherboard-front`,
:src:`commons-ip30-030-0887-003-rev-f`) and on an Origin 200 IP29 board
(:src:`theretroweb-origin200-image-1`), *observed*. How it connects to
the IOC3, and whether its UARTs are the two 16550 files, is *unknown*.

``parallel-part=1`` on ``sgi-ioc3-parallel`` selects the part's register
contract as far as the model goes:

- the test FIFO is 16 bytes; ``fifo-depth`` must be 0 or 16;
- in mode 7, configuration A reads ``0x1f`` and configuration B reads
  ``0x0b``, IRQ 7 and DMA channel 3, both read-only
  (:src:`ti-tl16pir552-slls222a p. 44`), *established* for the part.
  Writes to them log ``LOG_GUEST_ERROR``. Configuration B's bit 6
  samples the part's interrupt line; the model reads it as zero and logs
  ``LOG_UNIMP``;
- a test-mode write to a full FIFO drops the oldest byte. TI says writes
  continue and overrun but not which byte is lost
  (:src:`ti-tl16pir552-slls222a p. 41, 45`), so the order is an
  *approximation*.

Everything else, including the status and control registers, the
extended control register and DMA, behaves as on the generic port above.
The ``octane`` and ``octane2`` machines and the Fuel board select the
part. The reference Octane2's board is revision B and Fuel's fitted part
is not identified, so both selections are *inferred* from other board
revisions. The Origin 200 board keeps the generic port.

Byte bus and clocks
===================

The byte bus has four selects of 128 KiB. Which answer depends on the
board's clock, *software expectation* from Linux and OpenBSD:

- **M48T35.** A directly addressed 32 KiB timekeeper at select 0
  (``hw/rtc/m48t35.c``), on the Origin 200, the IP27 BaseIO boards and
  the Origin 300. OpenBSD names the Origin 200 part a DS1742W
  (:src:`dmesgd-8994 dsrtc0 line`), *observed*; the model's 32 KiB
  Timekeeper-compatible store is an *approximation* of it. The clock
  occupies the top eight bytes and everything below is storage, which
  SGI's driver probes by writing and reading back. ``nvram-drive`` backs
  the store with a 32 KiB image, and reset keeps its contents and time.
- **DS1685.** An indexed clock reached through an address port at select
  1 and a data port at select 2, on the Octane (:doc:`ds1685`).
  ``ds1685-drive`` backs it.

No board fits both. A select with no part, and select 3 always, is a
hole.

DMA and byte order
==================

Every DMA master in the chip, the serial rings, the Ethernet engine and
the parallel contexts, goes through one chip-owned view of the PCI
bus-master address space, so the Bridge or XBridge applies its
translation and swap controls to every access. The view reverses each
aligned 32-bit word and places a narrower access at its address XOR the
complement of its size. With the Bridge's DMA swap clear, as every
guest programs it for this slot, the two compose to host byte order:
big-endian descriptors read as values and frame bytes land in wire
order. Setting the slot's swap control reverses every 32-bit word,
frames included. The 32-bit PCI interface is *established*
(:src:`octane-technical-report p. 2-25`, :src:`007-3439-002 p. 69`);
the chip's own lane placement is *unknown*, and the view is an
*approximation* that gives the result every guest expects. OpenBSD and
Linux clear the swap bits for this slot, and the IRIX 6.5.30 kernel
never sets them, *software expectation*.

Reset
=====

A reset clears the SuperIO status and enable registers, chip control, the
interrupt output and its timer, MicroLAN control, pin control and pin
data, the ring bases and per-port registers with their timers and
packers, and the 16550 extras. The clock pre-divider is left as
programmed, an *approximation*: no reset value is known. The Ethernet
engine clears every register, including the ring bases, and drops held
completions. The PS/2 controller clears control and both holdings. The
parallel port clears its registers and FIFO. Buffer RAM and the clock
part's storage survive. The reset values are model choices; the chip's
reset state is *unknown* beyond the Status value under `Configuration
space`_.

Migration
=========

The chip and each child migrate their registers, timers, ring state,
held receive completions, PS/2 holdings, parallel FIFO and buffer RAM.
Migration works only between identical builds. A destination with a
different GPIO input mask or value, or a different parallel part or FIFO
depth, refuses the stream.

.. _sgi-ioc3-measurements:

Measurements
============

No IOC3 register has been read on a physical machine for this page. The
reference Octane2 (:ref:`sgi-measured-octane2`) reported its onboard IOC3
functions in its hardware inventory (:ref:`sgi-measured-octane2-hinv`):

.. list-table::
   :header-rows: 1

   * - Source
     - Width
     - Value
     - Chip revision
     - When
   * - ``hinv -mvv``
     - not a register read
     - serial ports ``tty1`` and ``tty2``, parallel ``plp1``, Ethernet
       ``ef0``; frontplane ``030-0891-003`` revision H
     - not reported
     - IRIX 6.5.30m running

This establishes that the board fits both serial ports, the parallel port
and Ethernet, which the ``octane2`` population matches. It does not
establish any register value, reset state, PCI revision, parallel
controller part or PHY.

.. _sgi-ioc3-limits:

Model limits
============

- The PCI error-address record, the configuration copy at ``0x00-0x18``,
  the interrupt-output alias page, the UART and keyboard configuration
  bytes and the SuperIO clock register file are not modelled. The main
  block logs ``LOG_UNIMP``; the others are holes.
- Carrier-detect and clear-to-send change sources, the interrupt input
  pulse, the external interrupt output pin and the Octane front-panel
  LEDs are not modelled.
- The 16550 auto flow control, RS-232 and RS-422 mode selection and the
  SuperIO reset bit have no effect.
- The Ethernet engine raises only the receive timer, receive threshold,
  transmit empty and transmit completion sources. Receive timer reloads
  other than zero are not modelled. Full duplex, interframe gaps, the
  random seed, the receive barrier and buffer-RAM frame staging have no
  effect. The checksum offload recomputes the whole checksum.
- The parallel port moves no data from the peripheral, implements no
  cable handshake, and models neither service thresholds nor interrupt
  polarity.
- Of the TL16PIR552, only the configuration values, test FIFO depth and
  overrun order are modelled. Its EPP address and data registers
  (offsets 3-7 of the ECP file) log ``LOG_UNIMP``; the status register's
  print and EPP timeout bits, the control register's acknowledge
  interrupt and the part's FIFO service threshold are not modelled.
- PS/2 framing errors, timeouts and line-level status are not modelled.
- Accesses of the wrong width are refused by QEMU's memory core.

Tests
=====

- ``tests/qtest/sn0-ioc3-test.c``: configuration space, board
  population and its validation, the interrupt status and enable aliases,
  the second interrupt reaching Bridge pin 3, chip control, the
  interrupt output in every mode, pin control and pin registers, the
  one-wire engine and station-address part, the Ethernet filter,
  transmit, checksum, threshold, timer, reset and PHY scan, buffer RAM
  size and parity, the PS/2 controller and its clamps, and the M48T35.
- ``tests/qtest/sn0-ioc3-serial-test.c``: the 16550 extras and the
  pre-divider, the transmit record, ring geometry and both alias pages,
  transmit entry types, the receive packer, timer and drain, pause,
  loopback timing, the shadow and migration.
- ``tests/qtest/sgi-ioc3-eth-test.c``: Ethernet ring wrap, descriptor
  bounds, receive delivery and its arrival latency, refused DMA, the
  stopped-MAC counter and buffer-index storage, the DMA swap controls,
  and the DP83840A and DP83843 PHYs, with some checks repeated with the
  ICS1890 fitted. ``tests/qtest/ics1890-test.c`` drives the ICS1890
  through this chip's MII registers.
- ``tests/qtest/sn0-ioc3-parallel-test.c``: both parallel register
  blocks, the DMA engine, the status and FIFO flags, the test FIFO, the
  TL16PIR552 variant and save and restore of the FIFO.
- ``tests/qtest/sn0-ioc3-kbc-routing-test.c``: the
  ``activate-keyboard`` property.
- ``tests/qtest/ioc3-release-fixture.c`` is a helper device for a
  release check, not a test of its own.
