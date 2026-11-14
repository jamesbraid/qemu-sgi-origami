.. SPDX-License-Identifier: BSD-3-Clause

.. _sgi-l1-board:

===================================
L1 board: ColdFire controller board
===================================

Used in no machine. The board is the controller of the Origin 3000
C-, I- and R-bricks (:ref:`sgi-bricks`). It has its own processor, so
it runs as a stand-alone machine of ``qemu-system-m68k`` and not inside a
MIPS machine, and nothing connects it to a node.

.. seealso::

   - :doc:`l1`: the node-facing request protocol and the operator console
     the controller's firmware provides.
   - :doc:`l1-brick-profiles`: what each brick's controller reports.

The L1 of an Origin 3000 C-, I- or R-brick is a small computer on standby
power: a Motorola MCF5206e ColdFire processor, a boot flash, a clock RAM,
static RAM, a four-channel UART and a management USB controller. Its
firmware identifies the brick, runs the console and drives the node's power
and reset. The ``sgi-l1-board`` machine of ``qemu-system-m68k`` models the
parts of that board the firmware needs to start, with the stock firmware
as the boot flash contents. Every other part of the board answers as a
logged, unimplemented window. Facts are rated as described in
:ref:`sgi-ratings`.

.. _sgi-l1-board-sources:

Sources
=======

- Motorola's *MCF5206e User's Manual*, revision 1, :src:`nxp-mcf5206eum-1`:
  the module base register, the chip selects, the interrupt controller,
  the software watchdog, the timers and the register map. Page numbers
  below are its printed pages.
- National Semiconductor's *PC16550D UART* datasheet, :src:`ns-pc16550d-1995`,
  for the register set that the QEMU 16550 model implements.
- The Dallas DS1742 data sheet, :src:`analog-ds1742-rev-6-13`, for the clock
  register layout.
- The shipped L1 firmware, 1.44.0, Base 1MB image, which runs on the C-, I-
  and R-brick controllers, :src:`l1-firmware-1.44.0-base`. What it programs and
  expects is a *software expectation*. It is the controller's software, so a
  fitted board agrees with it only if that image runs there, which is
  *inferred*.
- The board itself has no published schematic or part list, so every fitted
  part and every wiring fact below that the manual and the firmware do not
  fix is *unknown* or *inferred* as stated.

.. _sgi-l1-board-identity:

Identity
========

The firmware executes the 32-bit divide instructions, which the MCF5206e
provides (:src:`nxp-mcf5206eum-1 p. 1-15`, *established*). The manual
describes the hardware divider for the MCF5206e only and calls that part an
enhanced version of the MCF5206; that the plain MCF5206 lacks the divide
instructions is *inferred*. The firmware also needs about 7.8 KB of on-chip
SRAM, within the MCF5206e's 8 KB (:src:`nxp-mcf5206eum-1 p. 5-1`,
*established*). The controller is therefore an MCF5206e, a *software
expectation*. The fitted part number and revision are *unknown*.

The model uses QEMU's ``m5206`` core. That core implements the ColdFire
base instruction set with the 32-bit multiply and divide, and no
multiply-accumulate unit. The firmware's reached code uses no
multiply-accumulate instruction, but part of the code is unreached, so that the
firmware uses none is *inferred*. The model's core is an
*approximation* of the MCF5206e's, and the divide is the only part of the
MCF5206e that the firmware needs beyond the base set.

The firmware assumes a 48 MHz processor clock: the on-chip UART divisor
constant, the 100 Hz tick, the two-wire bus divider and the 12 MHz timer
output all agree on it, a *software expectation*. The fitted oscillator is
*unknown*. The model clock is the ``clock-hz`` property of the module,
48 MHz by default, an *approximation*; it sets the timer rates, the watchdog
period and nothing else, because the model has no baud-rate generator that
reads it. A value of zero is refused when the board starts.

.. _sgi-l1-board-map:

Memory map
==========

The firmware programs this map in its board set-up and relies on it for
every personality. The sizes of the flash, RAM and clock RAM are what the
model implements.

.. list-table::
   :header-rows: 1
   :widths: 14 22 40 24

   * - Window
     - Base and size
     - Device
     - Rating
   * - On-chip SRAM
     - ``0x30100000``, 8 KiB, placed by ``RAMBAR``
     - SRAM; the vector copy, the reset stack and a retained boot record
       live here
     - *software expectation*
   * - Module registers
     - ``0x10000000``, placed by ``MBAR``
     - SIM, timers, UARTs, parallel port, two-wire (M-Bus) controller
     - *software expectation*
   * - ``CS0``
     - ``0xffe00000``, 2 MiB window, cut to 1 MiB by the firmware's mirror
       probe; port 16 bits
     - boot flash, 1 MiB
     - *software expectation*
   * - ``CS1``
     - ``0x20000000``, 64 KiB window, port 8 bits
     - 2 KiB clock RAM
     - *software expectation*; the part is *inferred*
   * - ``CS2``
     - ``0x30000000``, 512 KiB window, port 16 bits
     - external static RAM
     - *software expectation*
   * - ``CS3``
     - ``0x40000000``, 64 KiB window, port 8 bits
     - management USB controller, slot 0
     - *software expectation*
   * - ``CS4``
     - ``0x50000000``, 64 KiB window, port 8 bits
     - four-channel UART
     - *software expectation*; the part is *inferred*
   * - ``CS5``
     - ``0x60000000``, 64 KiB window, port 8 bits
     - second USB slot, or a second UART on other brick types
     - *software expectation*
   * - ``CS6``, ``CS7``
     - none
     - not used, so the pins are write enables
     - *software expectation*

The chip-select register layout, the base and mask rule and the control
register fields are the manual's (:src:`nxp-mcf5206eum-1 p. 9-28`,
*established*). The firmware's flash window is 2 MiB until it finds the
first 1024 words at ``0xffe00000`` repeated at ``0xfff00000``, which cuts
the mask to 1 MiB; the model's 1 MiB part repeats through a 2 MiB window, so
the probe finds the repeat, a *software expectation*. The firmware sizes
the static RAM by saving a word at ``0x3001fffc``, clearing it, writing
all ones at ``0x3007fffc`` and cutting the window to 128 KiB if the cleared
word changed, a *software expectation*. The model has 512 KiB by default,
which the probe keeps. ``-m 128K`` makes the 128 KiB part repeat through the
512 KiB window, so the probe cuts the window.

The model maps a chip select's window when its mask register has been
written and its control register enables reads or writes. It ignores the
supervisor and user access-space mask bits and the wait-state, burst and
address-setup fields, which have no effect on a model without bus timing, each
an *approximation*. A window whose mask is not a contiguous block mask is
left unmapped and logged.

Every address that no window, SRAM, module block or chip select covers is
the default memory. The chip's default memory control register is cleared by
the firmware, which gives no responder
(:src:`nxp-mcf5206eum-1 pp. 9-38 to 9-39`, *established*). The model
answers such an access with zero and logs it with ``LOG_UNIMP``; the
hardware's response, a bus time-out or a hang, is *unknown*.

.. _sgi-l1-board-reset:

Reset and the boot flash
========================

At reset the ``MBAR`` valid bit is clear and the on-chip modules and SRAM are
not reachable (:src:`nxp-mcf5206eum-1 p. 8-2`, *established*). ``CS0`` is the
global select: it answers every external access except CPU space and
internal-module accesses until ``CSMR0`` is first written, so the boot flash
can sit at any address (:src:`nxp-mcf5206eum-1 p. 9-8`, *established*). The
core fetches its initial stack and program counter from address 0 and
``0x4``, which the flash answers. The model reproduces this: until the first
write to ``CSMR0`` the flash answers the whole address space, and the
vectors come from it.

The firmware's loader reads the byte at ``0x10000040`` before it writes
``MBAR``, so the read is an external cycle answered by the global ``CS0``,
and it tests bit 5 of the byte, the position of the watchdog-reset bit of
the reset status register. The flash holds ``0xff`` at that offset, so the
bit reads set, a *software expectation*. Whether the fitted board decodes
the flash address lines so that this read lands on flash offset ``0x40`` is
*unknown*. The model repeats the flash through the global window every
1 MiB and so returns the flash byte at offset ``0x40``, an *approximation*.
On a cold start the firmware then runs its second program copy, whichever
way the read goes, a *software expectation*.

The port width and acknowledge mode of the global ``CS0`` come from the
levels of three interrupt pins at reset (:src:`nxp-mcf5206eum-1 p. 9-8`,
*established*). The levels the fitted board applies are *unknown*. The
model's ``CSCR0`` reads ``0x3d9f`` after reset: a 16-bit port with automatic
acknowledge, the setting the firmware's later 16-bit write implies, an
*approximation*.

The model takes the boot flash contents as ``-bios``: a raw image that
starts at flash offset 0 and at most 1 MiB long. The shorter image is
padded with ``0xff``. Without ``-bios`` the whole flash is ``0xff``, so the
core's initial stack and program counter read as all ones and the machine
runs nothing useful. The flash is read-only. A write to it, which is how
the firmware would send a command, is logged with ``LOG_UNIMP`` and
ignored; the flash command set is not modelled, an *approximation* of the
AMD-style set the firmware uses (a *software expectation*).

A reset keeps the contents of the SRAM and the external RAM. The manual
calls the SRAM contents after a hardware reset undefined (:src:`nxp-mcf5206eum-1
p. 5-3`, *established*) and the firmware depends on the SRAM keeping a record
across a reset, a *software expectation*. Whether the fitted SRAM keeps
its contents across a reset is *unknown*; the model keeps them, an
*approximation*. At power-on both RAMs are zero in the model, an
*approximation*.

.. _sgi-l1-board-sim:

System integration module
=========================

The module's registers sit at ``MBAR`` once the core has written ``MBAR``
with its valid bit set. The model places the block, and the on-chip SRAM
beside it, when the core's ``MOVEC`` write to ``MBAR`` or ``RAMBAR`` arrives.
Both bases are 1 KiB and 8 KiB aligned. The access-space mask bits and the
``RAMBAR`` write-protect bit (bit 8, :src:`nxp-mcf5206eum-1 p. 5-2`) are not
modelled: the SRAM is always writable, each an *approximation*.

The model implements these registers. A narrow access to a wider register
reads or writes the matching byte lanes.

.. list-table::
   :header-rows: 1
   :widths: 14 14 12 24 36

   * - Register
     - Offset
     - Reset
     - Behaviour
     - Rating
   * - ``SIMR``
     - ``0x03``
     - ``0xc0``
     - read and write, no effect
     - reset *established* (:src:`nxp-mcf5206eum-1 p. 8-9`)
   * - ``MARB``
     - ``0x07``
     - ``0x00``
     - read and write, no effect
     - reset *established* (:src:`nxp-mcf5206eum-1 p. 8-18`)
   * - ``ICR1`` to ``ICR15``
     - ``0x14`` to ``0x22``
     - ``0x04``, ``0x08``, ``0x0c``, ``0x10``, ``0x14``, ``0x18``, ``0x1c``,
       ``0x1c``, ``0x80``, ``0x80``, ``0x80``, then zero
     - the level of ``ICR1`` to ``ICR8`` is fixed, the autovector bit of
       ``ICR8`` is zero
     - reset *established* (:src:`nxp-mcf5206eum-1 p. 8-10`); the offsets of
       ``ICR14`` and ``ICR15`` are *inferred* from the firmware's writes
   * - ``IMR``
     - ``0x36``
     - ``0x3ffe``
     - 16 bits, one bit per source, set masks it; bit 0 is reserved and reads
       as zero
     - *established* (:src:`nxp-mcf5206eum-1 p. 8-12`)
   * - ``IPR``
     - ``0x3a``
     - ``0x0000``
     - 16 bits, read only, follows the sources; a write is ignored and logged
       as a guest error
     - *established* (:src:`nxp-mcf5206eum-1 p. 8-13`)
   * - ``RSR``
     - ``0x40``
     - ``0x80``, or ``0x20`` after a watchdog reset
     - a write of one clears a bit
     - *established* (:src:`nxp-mcf5206eum-1 p. 8-13`)
   * - ``SYPCR``
     - ``0x41``
     - ``0x00``
     - write once after reset
     - *established* (:src:`nxp-mcf5206eum-1 p. 8-14`)
   * - ``SWIVR``
     - ``0x42``
     - ``0x0f``
     - read and write
     - reset *established*; the manual calls it write only, the model reads
       it back, an *approximation*
   * - ``SWSR``
     - ``0x43``
     - none
     - write only; ``0x55`` then ``0xaa`` services the watchdog; a read
       returns zero and is logged as a guest error
     - *established* (:src:`nxp-mcf5206eum-1 p. 8-16`)
   * - ``CSAR0`` to ``CSAR7``
     - ``0x64`` and each ``0x0c``
     - zero
     - 16 bits, base address
     - ``CSAR0`` reset *established*; the others are uninitialised on the
       chip and zero in the model, an *approximation*
   * - ``CSMR0`` to ``CSMR7``
     - ``0x68`` and each ``0x0c``
     - zero
     - 32 bits, block mask; the first write of ``CSMR0`` ends the global select
     - ``CSMR0`` reset *established* (:src:`nxp-mcf5206eum-1 p. 9-29`); the others
       as ``CSAR``
   * - ``CSCR0`` to ``CSCR7``
     - ``0x6e`` and each ``0x0c``
     - ``0x3d9f`` for ``CSCR0``, zero otherwise
     - 16 bits, read and write enable decide whether the window is mapped
     - *approximation*, see :ref:`sgi-l1-board-reset`
   * - ``DMCR``
     - ``0xc6``
     - ``0x0000``
     - read and write; a non-zero value is logged
     - reset *established* (:src:`nxp-mcf5206eum-1 p. 9-38`)
   * - ``PAR``
     - ``0xca``
     - ``0x0000``
     - 16 bits; bit 6 selects encoded interrupt levels
     - *established* (:src:`nxp-mcf5206eum-1 pp. 8-16 to 8-17`); the manual's
       diagram is 16 bits wide and its text calls the register 8 bits

``SYPCR`` set to ``0xfe`` is a *software expectation*: the watchdog is
enabled, resets on timeout and has the longest period. The timeout is
2\ :sup:`24` system clocks, 0.35 s at the model clock, from the manual's
period table (:src:`nxp-mcf5206eum-1 p. 8-15`, *established*). The same write
also enables the bus time-out monitor (``BME``, bit 2), which the model
ignores, an *approximation*. The model
arms the watchdog when ``SYPCR`` enables it, restarts it on the service pair
and, on a timeout with the reset option set, requests a machine reset and
reads ``RSR`` as ``0x20`` afterwards. A timeout without that option
raises the level 7 interrupt. The reset pin pulse the chip drives is not
connected, an *approximation*.

The model implements no register of the DRAM controller, the parallel port
or the DMA channels: a read returns zero and a write is dropped, and each is
logged with ``LOG_UNIMP``. The firmware writes the DRAM controller registers
to clear them and drives the parallel port as a scan chain master, both
*software expectation*; neither has a modelled effect. The two-wire bus
controller is modelled, as :ref:`sgi-l1-board-mbus` describes.

.. _sgi-l1-board-interrupts:

Interrupt controller
====================

The firmware sets ``PAR`` to ``0x0071``, which selects encoded interrupt
priority levels on the three external pins, and programs the sources as the
table shows, a *software expectation*.

.. list-table::
   :header-rows: 1
   :widths: 16 12 34 38

   * - Source
     - Level
     - Programming
     - Model
   * - Timer 1
     - 5
     - ``ICR9`` ``0x94``, autovector
     - interrupt on the timer reference
   * - Two-wire bus
     - 6
     - ``ICR11`` ``0x99``
     - interrupt while the controller's interrupt flag and enable are both
       set
   * - On-chip UART 1
     - 6
     - ``ICR12`` ``0x1a``, vector from the UART's vector register
     - interrupt from the UART
   * - Four-channel UART
     - 4
     - ``ICR4`` ``0x90``, autovector
     - the four channel lines through one OR gate to external level 4, an
       *approximation*
   * - Management USB slot 0
     - 2
     - ``ICR2`` ``0x88``
     - no source
   * - Watchdog
     - 7
     - ``ICR8`` ``0x1d``
     - interrupt when the reset option is clear

With encoded levels the interrupt pins give a level and the interrupt
pending bit of that level sets (:src:`nxp-mcf5206eum-1 p. 8-4`,
*established*). The model's board presents the highest asserted of its
external level lines as that level, which assumes a priority encoder on the
board, *inferred*. The interrupt-level field of the registers for the
external sources and the watchdog cannot be changed, a rule the manual states
for the individually vectored mode (:src:`nxp-mcf5206eum-1 p. 8-4`,
*established*); the model applies it to the encoded mode as well, an
*approximation*. Within a level the higher priority field wins.

Whether the four-channel UART part has a combined interrupt output, or the
board wires each channel's output to the interrupt pins some other way, is
*unknown*; the OR of the four channels onto one level 4 line is the model's
choice.

An external source whose autovector bit is clear needs an external
acknowledge cycle. The model logs that as unimplemented and uses the
autovector. The sources that drive external levels 1, 2, 3 and 7 on a brick
are *unknown*, and the model has none.

The model gives the core the highest pending, unmasked source at the moment the
core accepts an interrupt, through the board-operations hook of the ColdFire
core, and leaves a level-sensitive source asserted until its device clears
it.

.. _sgi-l1-board-timers:

Timers
======

The two timers have the manual's register layout (:src:`nxp-mcf5206eum-1
p. 14-4`, *established*). The firmware uses timer 1 as the 100 Hz
kernel tick: mode ``0x001d``, clock divided by 16, restart at the reference
and interrupt on the reference, with reference ``0x752f``, a *software
expectation*. It uses timer 2 in toggle mode at reference 1 on the system
clock, which makes a 12 MHz output, a *software expectation*; whether the
board feeds that output to the four-channel UART is *unknown*.

The model counts in virtual time at the module clock. A timer that interrupts
on the reference is scheduled for each event; the others set their reference
flag when a register is read, so timer 2 costs no event per clock. The
capture input, the output pin and the external clock input are not modelled:
the timers have no connected pins, an *approximation*.

.. _sgi-l1-board-uarts:

UARTs
=====

The on-chip UARTs are QEMU's ColdFire UART model, ``mcf-uart``, reached at the
manual's offsets. The firmware opens only UART 1, at 19,200 baud, as the
link to the power bay on these bricks, a *software expectation*. The model
does not pace bytes at a baud rate and passes them to the serial backend as
they come.

The four-channel UART at ``CS4`` has four channels with eight byte registers
each at ``0x00``, ``0x08``, ``0x10`` and ``0x18``, a *software expectation*.
The firmware probes it by clearing each channel's line control register,
writing ``0xa0`` plus the channel number to its scratch register and reading
all four back, and drops the whole device on a mismatch. It uses the
enhanced register set, selected by writing ``0xbf`` to the line control
register, which is a feature of the 16C654 family and not of the PC16550D, a
*software expectation*; the part marking is *unknown*. It runs the channels
from a 12 MHz clock at the divisors 7, 13 and 20 for 107,142, 57,692 and
37,500 baud, a *software expectation*.

The model uses four QEMU 16550A UARTs on a 12 MHz clock. They implement the
PC16550D registers, the scratch register and the FIFOs (:src:`ns-pc16550d-1995`,
*established* for the part's register set). They do not implement the enhanced
register set: the firmware's writes to it reach the FIFO control and modem
registers, an *approximation*, and the automatic flow control the firmware
asks for on the node link is absent. The model drives no modem input line,
so the modem status register reads as the 16550A model gives it, with the
ring indicator clear unless a serial backend reports it, an *approximation*.

Channel 2 is the L1 operator console at 38,400 baud, channel 3 the node link at
57,600, and channels 0 and 1 the two peer links at 107,142, on these bricks, a
*software expectation*. The model connects serial backend 0 to channel 2, 1
to channel 3, 2 to channel 0, 3 to channel 1 and 4 to on-chip UART 1.

.. _sgi-l1-board-mbus:

Two-wire bus controller
=======================

The two-wire bus is the MCF5206e's M-Bus module, five byte registers in the
module block (:src:`nxp-mcf5206eum-1 pp. 13-6 to 13-11`, *established*):

.. list-table::
   :header-rows: 1
   :widths: 14 14 14 58

   * - Register
     - Offset
     - Reset
     - Defined fields
   * - ``MADR``
     - ``+0x1e0``
     - ``0x00``
     - the module's own slave address, bits 7 to 1
   * - ``MFDR``
     - ``+0x1e4``
     - ``0x00``
     - the divider selector, bits 5 to 0
   * - ``MBCR``
     - ``+0x1e8``
     - ``0x00``
     - ``MEN`` 7, ``MIEN`` 6, ``MSTA`` 5, ``MTX`` 4, ``TXAK`` 3, ``RSTA`` 2
       (write one to repeat the START, reads zero)
   * - ``MBSR``
     - ``+0x1ec``
     - ``0x81``
     - ``MCF`` 7, ``MAAS`` 6, ``MBB`` 5, ``MAL`` 4, ``SRW`` 2, ``MIF`` 1,
       ``RXAK`` 0; software clears ``MAL`` and ``MIF`` by writing zero to
       them and no other bit
   * - ``MBDR``
     - ``+0x1f0``
     - ``0x00``
     - the data byte

The manual's behaviour, all *established* (:src:`nxp-mcf5206eum-1 pp. 13-2
to 13-14`):

- ``MSTA`` rising in an enabled module sends a START; ``MSTA`` falling sends
  a STOP and returns the module to slave mode. ``MBB`` follows the START and
  STOP seen on the bus, whoever sends them.
- With ``MTX`` set, a write to ``MBDR`` sends the byte most significant bit
  first. The first byte of a transfer is the calling byte: the target's
  seven-bit address and the read bit. ``MCF`` is clear while a byte is on
  the bus and sets on the falling edge of its ninth clock together with
  ``MIF``. ``RXAK`` is the level of the data line on the ninth clock: zero
  is an acknowledge. A transmitter's ``TXAK`` does not affect it.
- A master receiver clears ``MTX``, reads ``MBDR`` once to start the first
  byte, and each later read of ``MBDR`` returns the byte and starts the
  next. ``TXAK`` clear drives an acknowledge on the ninth clock of a
  received byte and ``TXAK`` set drives none. The software sets ``TXAK``
  before reading the second-last byte and clears ``MSTA`` before reading the
  last.
- ``MIF`` requests an interrupt only while ``MIEN`` is set; clearing
  ``MIEN`` hides the request without clearing the flag.
- A START asked for while the bus is busy, or a repeated START asked for by a
  module that is not the master, loses arbitration: ``MAL`` and ``MIF`` set,
  ``MSTA`` clears and no STOP is sent.
- The divider selector indexes a table of 64 divisors, not a linear
  scale: ``0x00`` is 28, ``0x07`` 68, ``0x13`` 480, ``0x20`` 20 and ``0x3f``
  2048 (:src:`nxp-mcf5206eum-1 p. 13-7`). The manual gives no clock-high and
  clock-low split.

The firmware's use is a *software expectation*: it clears ``MBCR``, writes
``MFDR`` ``0x13``, ``MADR`` zero and ``MBCR`` ``0x80``, and starts a transfer
with ``MBCR`` ``0xf0`` and a calling byte; ``0xf4`` repeats the START,
``0xe0`` starts a receive, ``0xe8`` sets ``TXAK`` and ``0x80`` stops. It
accepts a byte transfer when ``MBSR`` masked with ``0xb2`` is ``0xa2``. If
the bus reads busy at bring-up it writes ``MBCR`` ``0xa0``, reads ``MBDR``
and clears ``MBSR`` and ``MBCR``, up to 1000 times. The same bring-up runs
as bus recovery after any failed transaction: the driver clears ``MBCR``,
sets the divider, enables the module again and clocks the bus only if
``MBB`` is still set, then returns the same error. It does not retry the
transfer. A bus on which nothing answers therefore sees one controller
reinitialisation per refused call and no repeated attempt. With the 48 MHz
clock the selector ``0x13`` gives a 100 kHz clock, which agrees with the
assumed processor clock (see :ref:`sgi-l1-board-identity`).

The model gives the controller a ColdFire clock input, the module clock, and
an interrupt output to ``ICR11``. The controller has a standard QEMU two-wire
bus, with a timed wire so that the calling byte, the acknowledge clock and
the byte completion are separate events at the divided clock. Its
choices where the manual is silent are each an *approximation*:

- The clock-high and clock-low phases are equal, each half the period the
  selector gives. The manual fixes only the total.
- A change to ``MFDR`` takes effect at the next byte.
- A STOP asked for while a byte is on the bus waits for the end of that byte.
- Writing zero to ``MEN`` stops any transfer, releases both lines and
  returns every status bit to its reset value. The manual says the module is
  held in software reset and the registers stay readable, but not what
  they read: the model keeps ``MADR``, ``MFDR`` and ``MBDR`` and nothing
  checks that choice.
- ``MEN`` must be set before any other ``MBCR`` bit has an effect
  (:src:`nxp-mcf5206eum-1 p. 13-8`, *established*). Bits written while it is
  clear are stored and read back but start nothing, and they do not count as
  an earlier state: a write that sets ``MEN`` and ``MSTA`` together starts a
  transfer. The manual does not say whether one write may do both, an
  *approximation*.
- The interrupt pending bit in the SIM sets at once, not one processor clock
  after the request, as the manual describes for the generic SIM
  (:src:`nxp-mcf5206eum-1 pp. 8-10 to 8-12`). The timers and UARTs of this
  model do the same.

What the model does not do, and logs: the slave role. ``MADR`` is stored, no
address is matched, ``MAAS`` and ``SRW`` never set, and a nonzero ``MADR``
is logged with ``LOG_UNIMP``; the firmware writes zero. Another master on
the bus is not modelled either. The manual lists five causes of arbitration
loss (:src:`nxp-mcf5206eum-1 p. 13-10`, *established*). The model produces
two of them: a START asked for while the bus is busy, and a repeated START
asked for in slave mode. Cause 5, a STOP detected that the master did not
request, is not modelled, and neither are causes 1 and 2, where SDA reads
low while the master drives it high, because they need a second master.
Word and longword accesses and misaligned
accesses that overlap the five registers are logged with ``LOG_GUEST_ERROR``
and ignored: the manual says nothing about them, so their hardware result is
*unknown*. The model has no migration for the controller; the board refuses
migration as a whole. The controller reports the clock phases it last
used as the read-only properties ``scl-low-ns`` and ``scl-high-ns``, so that
a test can check the divider table without timing the bus.

Which parts are fitted on the bus, at what addresses and with what clock
stretching is *unknown* for the board except where the firmware's own tables
name an address (:ref:`sgi-l1-board-mbus-peers`).

.. _sgi-l1-board-mbus-peers:

Devices on the two-wire bus
---------------------------

The firmware's own tables name the parts it expects on segment 0 of the bus,
the part with no bus switch in front of it, a *software expectation*:

.. list-table::
   :header-rows: 1
   :widths: 14 14 26 46

   * - Wire address
     - Seven-bit
     - Firmware table
     - Model
   * - ``0xa4``
     - ``0x52``
     - the C-brick's node board; the I- and R-bricks' power board
     - a 256-byte EEPROM window with the board identity record
   * - ``0xa8``
     - ``0x54``
     - the I- and R-bricks' logic board
     - the same, for an I-brick
   * - ``0x40``
     - ``0x20``
     - the display I/O expander, one byte wide; a PCF8574-family part
       (*inferred*)
     - nothing: a call is not acknowledged
   * - ``0x4e``
     - ``0x27``
     - the bus select expander, one byte wide
     - nothing: a call is not acknowledged
   * - ``0xa0``, ``0xac``
     - ``0x50``, ``0x56``
     - identification routes, read only when the brick type is already
       ``%``; they never change it
     - nothing: never called, because the model presents no such brick

The identity parts exist only when the machine option ``brick-type`` is
``cbrick`` or ``ibrick``; with it unset the bus is empty, as the sections
above describe. The model builds each part's record with the behavioural L1's
own builder (``hw/misc/sgi-l1-identity.c``), from the properties that L1
takes: ``node-board-name``, ``node-board-part`` and ``node-board-revision``
for a C-brick, and ``mac`` for an I-brick's logic board. The behavioural
defaults apply (name ``NODE``, part ``NOTOBSERVED0``, revision ``00``; an
I-brick's boards ``POWER`` and ``LOGIC``), so both models serve the same
record for the same inputs. The record is an eight-byte common header, a
chassis area and a board area, each summing to zero, as the L1 page's whole
image records are; the contents are placeholders and a marked synthetic
serial, an *approximation*.

What the firmware reads from them is a *software expectation*: it calls the
part with one word-address byte, then a repeated START and reads bytes.
The part is *inferred* to be a 24C04-compatible EEPROM: the firmware adds
the upper offset bit to the slave byte, so offsets 256 to 511 of a 512-byte
record go to wire address ``0xa6`` for the node board and ``0xaa`` for the
logic board. Its write protection is *unknown*, and the stock firmware image
contains no routine that writes these records. The model answers one 256-byte
window at the first address and does not acknowledge a call to the second
page's address, an *approximation*; the records fit the first 256 bytes. The
part accepts writes, which last until the machine exits, and nothing says a
write-protect pin is wired. The firmware decides the brick
type from the board name in the record: ``IP35`` at ``0xa4`` makes it a
C-brick (:ref:`sgi-l1-board-identity-run`). The behavioural defaults do
not make that name for a C-brick, so a C-brick run with the stock firmware
needs ``node-board-name=IP35``; the I-brick's ``POWER`` and ``LOGIC`` names
leave the firmware's type unknown, because the type it knows an I-brick by is
named in the logic board's record and the behavioural L1 does not use that
name.

Not fitted, and so not acknowledged: the processor and memory module records
that sit behind a bus segment select (the select expander itself, the
monitors, the other expanders, the memory module EEPROMs and the display
expander). The firmware's tables give these parts' addresses and register
sequences but not the parts, their pin meanings or the display text path, so
the model does not invent answers for them. The firmware selects a segment by writing one
byte to the select expander before each access to a device on segments 2 to
13, and skips the write for segment 0, so a segment chosen earlier stays
enabled and the identity parts stay reachable. With the expander not fitted,
each such access ends with the error and no retry. With the PIMM presence
expander not fitted (segment 3), the firmware's read of it fails and it
treats both processor modules as absent, so it disables their supplies,
expanders and EEPROM records, and monitor entries 5 and 6, and does not call
them again; all of this is a *software expectation*.

.. _sgi-l1-board-identity-run:

What the firmware does with the identity part
---------------------------------------------

Run once on this model with the Base 1MB image as boot flash, a *software
expectation* and not a statement about a board:

- With nothing on the bus the firmware calls ``0xa4``, ``0xa8``, the
  display expander (``0x41``, a read) and then ``0xa4`` and ``0xa8`` again,
  in that order; none acknowledges. It probes the display before it knows
  the brick type. It prints the display expander as unreadable ("no
  acknowledge") and the missing serial number. Before the
  controller existed the same run printed the display expander and both
  board EEPROM probes as time-outs.
- With a C-brick record named ``IP35`` the record is accepted: no missing
  serial number message, the prompt is ``---c---L1>``, and ``brick`` prints
  type ``C (IP35)``, ``[1MB flash]`` and the record's serial. The firmware
  then registers the C-brick's monitors and expanders, and each call to one
  that is not fitted reports no acknowledge; polling repeats those reports.
- With an I-brick's ``POWER`` and ``LOGIC`` records the type stays unknown
  (prompt ``---?---L1>``, ``brick`` prints ``Unknown``), with the serial and
  the source read as NVRAM.

.. _sgi-l1-board-nvram:

Clock RAM
=========

The firmware keeps its settings, a log ring and the time in 2 KiB of
battery-backed RAM at ``CS1``, with the clock registers in the top eight
bytes: a control register at ``0x7f8`` with a read-latch bit and a write bit,
then BCD seconds, minutes, hours, day, date, month and year. That layout is
the one the DS1742 data sheet gives (:src:`analog-ds1742-rev-6-13`,
*established*), and the firmware's use of it is a *software expectation*.
Whether the fitted part is a DS1742 is *unknown*.

The model uses QEMU's M48T02 timekeeper, which has the same layout. It takes
the time from the host clock and keeps the low bits of the control register
as plain storage, so the century digits the firmware writes are not part of the
time, an *approximation*. The RAM starts zero and is not kept between runs, an
*approximation* of a battery-backed part. The firmware rebuilds an invalid
header and settings records, a *software expectation*.

.. _sgi-l1-board-unimplemented:

Unimplemented parts
===================

These windows and registers answer with ``LOG_UNIMP`` and nothing else.
Start the machine with ``-d unimp`` to see every access, with the window or
register name and the value. ``-d guest_errors`` adds accesses to addresses
that are not registers.

- The management USB controller at ``CS3`` and the device at ``CS5``.
- The two-wire bus has the controller described above but no device on it
  unless the board is given one (:ref:`sgi-l1-board-mbus-peers`): a transfer
  to an address nothing answers is not acknowledged, and the firmware reports
  the board identification EEPROMs and the display expander as unreadable.
- The parallel port, which the firmware drives as a JTAG scan master.
- The DRAM controller registers, the DMA registers and ``DMCR``'s default
  memory behaviour.
- The output port of the on-chip UARTs, which drives the pin that the firmware
  pulses to reset external parts; no pin is connected.
- The flash command set.

.. _sgi-l1-board-migration:

Migration
=========

The model refuses migration and snapshots with a message that names the
board.

.. _sgi-l1-board-limits:

Model limits
============

- The board has no node, no link to another controller and no
  environmental peers: the two-wire bus carries only the identity parts
  :ref:`sgi-l1-board-mbus-peers` lists.
- The processor is QEMU's ``m5206`` core, with the instruction cache not
  modelled and the control-register ``MOVEC`` writes limited to the
  registers the core accepts.
- There is no bus timing: wait states, port widths and the bus time-out
  monitor have no effect.
- The clock, the boot strap levels, the flash part, the four-channel UART
  part, the USB controller and the clock RAM part are *unknown*; the
  model's choices are marked above.
- A run shows only what the model does. It does not show what the board
  does.

.. _sgi-l1-board-tests:

Tests
=====

``tests/qtest/sgi-l1-board-test.c`` runs test-owned ColdFire code from a test
flash image and checks the global chip select, the chip-select windows, the
interrupt controller, the timers, the watchdog and the four-channel UART.
The same test reads the two-wire controller from the host against a stock
AT24C EEPROM and an address nothing answers, and each identity part through the
controller, checking the record layout and the names given.
``tests/functional/m68k/test_sgi_l1_board.py`` boots the stock firmware to the
operator console prompt, with an empty bus and with a C-brick record, when the
firmware image is available and is skipped when it is not.
