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
     - SIM, timers, UARTs, parallel port, two-wire bus
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

The model implements no register of the DRAM controller, the parallel port,
the two-wire bus or the DMA channels: a read returns zero and a write is
dropped, and each is logged with ``LOG_UNIMP``. The firmware writes the
DRAM controller registers to clear them, drives the parallel port as a scan
chain master and runs its two-wire bus bring-up, all *software
expectation*; none of them has a modelled effect.

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
     - no source
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
- The two-wire bus registers, so every bus transaction the firmware starts
  times out and the firmware reports the board identification EEPROMs and
  the display expander as unreadable; a read of the status register returns
  zero, an *approximation*.
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
  environmental peers: the two-wire bus has no devices and its registers
  are not modelled.
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
``tests/functional/m68k/test_sgi_l1_board.py`` boots the stock firmware to the
operator console prompt when the firmware image is available and is skipped
when it is not.
