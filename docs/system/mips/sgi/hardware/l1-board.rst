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
vectors come from it. A machine reset, such as a watchdog timeout, resets the
module before the core fetches its vectors, so they come from the flash even
when the firmware had moved ``CS0`` to its own window.

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

On the chip the watchdog counts system clocks, the clock the core runs from,
and its period is that count divided by the system frequency
(:src:`nxp-mcf5206eum-1 p. 8-15`, *established*). The model's watchdog and
timers count QEMU's virtual clock. Without ``-icount`` that clock follows
the host's clock, including time in which the host does not run the core,
so a host stall that spans the rest of the watchdog period times the
watchdog out and resets the board although the core missed no service it
would have made, an *approximation*. With ``-icount`` a host stall of the
vCPU does not advance the virtual clock while the core is running; an idle
core can still let the clock move on toward the next timer deadline in real
time. Runs that must not depend on host load use ``-icount``; the board's
functional test runs with ``-icount shift=10,align=off``.

The model implements no register of the DRAM controller or the DMA channels:
a read returns zero and a write is dropped, and each is logged with
``LOG_UNIMP``. The firmware writes the DRAM controller registers to clear
them, a *software expectation* with no modelled effect. The two-wire bus
controller is modelled, as :ref:`sgi-l1-board-mbus` describes, and so is the
parallel port, below.

.. _sgi-l1-board-parallel-port:

Parallel port
-------------

The direction register (``PPDDR``, ``MBAR+0x1c5``) and the data register
(``PPDAT``, ``MBAR+0x1c9``) are 8 bits wide and reset to 0 (*established*, the
MCF5206E User's Manual, where the manual calls them ``PADDR`` and ``PADAT``).
A read of ``PPDAT`` returns the driven level for an output pin and the pin
level for an input; a write latches the byte, and the latch drives a pin only
once its direction bit makes it an output (*established*). The model sends the
whole output byte, the latch where the direction says output, on the
``pp-out`` line at every write and at reset, and takes the input pins from
the ``pp-in`` line; with nothing driving it the inputs read high, an
assumption and an *approximation* because the pin's real pull is *unknown*.
The manual says a direction bit affects a pin only if ``PAR`` selects
general-purpose I/O (printed p. 10-2); the model ignores ``PAR`` here and
treats the pins as always general-purpose. Register writes and the input byte
are traced (``mcf5206e_pp_write``, ``mcf5206e_pp_pins``).

With ``brick-type=cbrick`` and ``board-peers=on`` the lines reach a scan chain
(``sgi-l1-scan-tap``), the master the firmware drives at each node release:
before and after a scan it pulses a reset on the port and, between them,
checks the chain. The pin roles are a *software expectation* from the
firmware's pin print and the L1 console's ``scan`` commands: ``PPDAT`` bit 3
is ``PSI``, bit 4 ``TRST`` (active low), bit 5 ``TMS``, bit 6 ``TCK``, bit 7
``TDI`` and bit 0, the only input, ``TDO``. That ``PSI`` selects a scan
interface chip and that bit 4 is the chain's reset are *inferred*; the board
nets are *unknown*.

The chain is one IEEE 1149.1 TAP with the standard sixteen states. It takes
``TDI`` and ``TMS`` on the rising ``TCK`` edge and settles ``TDO`` on the
falling edge, so a read before the next rising edge sees the bit that edge
shifts out, which is what the firmware's order within a clock needs
(*software expectation*). ``TRST`` low holds Test-Logic-Reset; what ``TDO``
shows then is not specified, and the model leaves it at its last value until
the next falling ``TCK`` edge (*unknown*). The instruction
register is 12 bits and captures 1 followed by eleven 0s, the first bit
out being the 1. Every instruction selects a 1-bit bypass register that
captures 0, so a data scan returns its input one bit later. Only all ones is
a known instruction; the firmware does not use any other, and any other
value gets the same response rather than hanging (*software expectation* for
the checks the firmware makes, *inferred* that a 1-bit bypass and a 12-bit
register are what answers). The node-side chain, its identification codes and
the meaning of any other instruction are *unknown*, and no ``IDCODE`` is
read for this brick type. The state changes and each scan's shifted data are
traced (``sgi_l1_tap_state``, ``sgi_l1_tap_scan``).

Without ``board-peers`` no chain is fitted, a scan then reads the input pin's
level and fails at its first instruction-register check, as it should; there
is no setting to skip it. Only the C-brick has a chain here, because
the firmware's scan is read for that brick type only (package 0); other
brick types are unchanged.

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
to channel 3, 2 to channel 0, 3 to channel 1 and 4 to on-chip UART 1, so a
bay model that frames the bytes is reached with five ``-serial`` options, the
fifth a socket chardev. Nothing paces the bytes or models the UART's modem
lines, so the link is a byte pipe at the speed the host moves it.

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
for a C-brick, and ``mac`` for an I-brick's logic board. With none of the
three the behavioural defaults apply (name ``NODE``, part ``NOTOBSERVED0``,
revision ``00``; an I-brick's boards ``POWER`` and ``LOGIC``). A name alone
gives an empty part and revision, which is enough for the firmware's
classification (below); a part or revision without the other two is
refused. The record is an eight-byte common header, a
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
needs a ``node-board-name`` that begins ``IP35``. That the firmware
compares only that prefix is a *software expectation*; that the Origin 3000
C-brick's node board is an IP35 is *established*, but the full name its
EEPROM holds, any suffix after the prefix, and the part and revision are
*unknown* until a read or capture of one, so the part and revision are left
empty. The model's C-brick controller entry carries that prefix, ``IP35``,
and the catalogue's firmware board takes it from there. The behavioural L1
of a C-brick still gets an empty node-board identity, so the two
providers differ on the board name. The I-brick's ``POWER`` and ``LOGIC`` names
leave the firmware's type unknown, because the type it knows an I-brick by is
named in the logic board's record and the behavioural L1 does not use that
name.

Not fitted unless ``board-peers`` is set (:ref:`sgi-l1-board-cbrick-peers`),
and so not acknowledged: the processor and memory module records
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

.. _sgi-l1-board-cbrick-peers:

The C-brick's polled devices
----------------------------

With ``brick-type=cbrick,board-peers=on`` the board also fits the devices
the firmware's C-brick tables name on segments 0 to 3 of the bus, the
processor module 0 monitors on segment 4, a front-panel display on segment 0
and the management USB controller at ``CS3``, so that its calls are acknowledged
instead of reported as unreadable. Without ``board-peers`` (the default)
none of this exists and the bus is as described above. ``board-peers``
needs ``brick-type=cbrick``; there is no I-brick table to fit.

The wire addresses, segments and masks are the firmware's tables, a
*software expectation*. The parts are not named by any image or by a
published schematic, so each part below is an *inferred* family and the
model is the family's usual bus behaviour and nothing more. No data sheet
for the PCF8574 family, the DS1780, LM78 and ADM9240 family or the USS-820D
is among the cited sources, so none of that family behaviour is cited to a document:
it is *inferred*.

**Bus select.** The expander at ``0x4e`` is a one-byte latch whose value
decides which segments the controller reaches, as the firmware writes it
(*software expectation*). It starts at ``0xff``, every segment off, which
is the byte the firmware writes to it at start-up (*software expectation*)
and the power-on state of its family (*inferred*). A board reset leaves it
as it is, as for the other expanders below. The model reads the bits as the
firmware's select bytes suggest (*inferred*): bit 7 low enables segment 2,
bit 6 segment 3, bit 5 segment 4, bit 4 segment 5, and with bit 3 low bits
2 to 0 enable segment 6 plus that number. The firmware writes only ``0xff``,
``0x78``, ``0xb8``, ``0xd8``, ``0xe8`` and ``0xf0`` to ``0xf7``; any other
byte is decoded bit by bit the same way, and several segments can then be
enabled together, an *approximation*. When two enabled segments hold a
device at the same wire address, one device answers: the bus searches its
devices newest first, so the one fitted last wins. The firmware never enables
two segments, so this is the model's choice and not a statement about the
board. Segment 0 is the controller's own
bus and is always reachable. The parts behind the select bits are
*unknown*, and the model shows only that a segment's devices answer or do
not.

**Expanders.** Each is a one-byte quasi-bidirectional latch with no
register byte (*software expectation*; PCF8574 family, *inferred*). A write
stores the byte. A read returns the stored byte on the output pins and the
level driven on the input pins, which the firmware writes high so that the part
can read them (*inferred*). The firmware writes each expander and reads it back
(*software expectation*); beyond the supply, fan and display state its
start-up check compares, what it does with the byte is *unknown*.

The latch starts at ``0xff``, the power-on state of that family, so every
expander reads and drives all ones until the firmware's first write
(*inferred*; the bytes the firmware first writes are not power-on states).
The part has no reset pin, so a board reset leaves every latch as it was
(*inferred*). Whether the board ties the parts' supply or a bus switch to the
controller's reset is *unknown*, so keeping the outputs is an *approximation*;
a power cycle is a new process. What the node chip sees in the window before
the first write is *unknown*.

The input pins read the ``input-pins`` property of the expander, writable while
the machine runs. A pin reads low if the latch bit or the property bit is low,
as with a quasi-bidirectional pin written low (*inferred*). Nothing is attached
by default: the pins read high, an *approximation* because the pin meanings are
*unknown* beyond the masks, except for three. Remote power OK, bits 0 and 4 of
port control, reads low without a peer (*inferred*: the sheets give only what
the firmware's port step does when it reads the bit clear, which is to leave
its software LED and :term:`Bedrock` control 1 bits 2 and 3 clear). The power
control status bits read clear, because a set bit is a regulator fault
(*inferred*). Fan control bit 6, the 48 V power OK input, reads high at all
times, including with the 48 V enable off, an *approximation* the sheets do not
settle (*unknown*). The link LED bits of port control are *unknown* and read
high. The devices ``l1-port-control`` and ``l1-power-control`` are the first
two. No change is ever reported, because the interrupt wiring is *unknown*.

.. list-table::
   :header-rows: 1
   :widths: 30 10 14 14

   * - Name
     - Segment
     - Wire address
     - Input bits
   * - display control
     - 0
     - ``0x40``
     - ``0x0f``
   * - LED 0 low, high; LED 1 low, high
     - 2
     - ``0x42``, ``0x44``, ``0x46``, ``0x48``
     - ``0xff``
   * - COM control
     - 2
     - ``0x4c``
     - ``0xf1``
   * - :term:`Bedrock` control 1, 2
     - 3
     - ``0x42``, ``0x44``
     - ``0x00``
   * - port control
     - 3
     - ``0x46``
     - ``0x55``
   * - power control
     - 3
     - ``0x48``
     - ``0xde``
   * - processor module presence
     - 3
     - ``0x4a``
     - ``0xff``
   * - fan control
     - 3
     - ``0x4c``
     - ``0x40``

The processor module presence expander reads ``0xff`` because every bit is an
input and nothing drives it. The firmware reads that as both processor modules
absent (*software expectation*). No processor module record or presence is
fitted, and the model does not invent a module type; only the two module 0
monitors the firmware polls anyway are fitted (below). The codes the firmware
accepts for a present module and what each one means are *unknown*.

**Monitors.** The three node monitors, wire ``0x58``, ``0x5a`` and ``0x5c``
on segment 2, are 256-register files of the voltage and temperature monitor
family the firmware's register numbers fit (DS1780, LM78, ADM9240;
*inferred*). A write is an address byte, a register byte and a value; a read
sets the register by a write phase, then a repeated start and one byte. The
register pointer holds after a read and a write does not move it. Every
register starts at zero, so each voltage and the temperature read as
zero, as an unpowered part, an *approximation*; no environment value is
invented and no conversion is modelled. Writes are stored and read back,
including the initialisation sequence the firmware sends, so a part's
self-clearing bits are not modelled either. The alert response address is
not fitted.

**Processor module 0 monitors.** Two more monitors of the same kind sit at
wire ``0x5c`` and ``0x5e`` on segment 4, named PIMM0 A and PIMM0 B by the
firmware's table (*software expectation*). The firmware initialises,
configures and polls both whether or not a processor module is fitted: its
probe for an absent module disables monitor entries 5 and 6 by number, which
are the processor module 1 monitors, and no case disables these two
(*software expectation*). Fitting them for a board with no processor module is
therefore a model of that behaviour and not of hardware, an *approximation*.
Each polls the interrupt status registers ``0x41`` and ``0x42`` and treats a
zero byte as no event (*software expectation*); the registers read zero and
nothing sets them, so every poll is quiet. A configuration write that
is not acknowledged, or that goes to a monitor the firmware has disabled,
makes it record a configuration error that ``env`` reports as monitoring
"enabled, but not running". The disabled case needs no bus traffic: a
processor module 1 that is fitted while module 0 is absent hits it, and the
model fits no module 1 (*software expectation*). These monitors have no
EEPROM and the model fits none: the presence expander still reads ``0xff``,
both modules stay absent (*software expectation*), and no module record is
served. The part is *inferred* as for the node monitors. The final write of the
configuration, which clears an interrupt mask bit, depends on a power-state
test whose inputs are *unknown*; the monitors acknowledge every register write,
so the result of that test cannot change what the model shows.

**Display.** The firmware's display driver writes to wire ``0xf0`` and reads
``0xf1`` on segment 0, 7-bit ``0x78``, a part that has no register byte,
and an access to segment 0 writes no bus select byte (*software
expectation*). So the part must answer whatever select byte the last access
to another segment left behind (*inferred*). The model fits one
device there that acknowledges every byte, keeps the bytes of the last write
and reads ``0x00``. The firmware writes ``1`` or ``2`` and twelve bytes of
text, ``6`` and up to 72 bytes for a long second line, or ``3`` or ``4``, a
position and a character, only if the byte read after its reset is
``0x10`` or more; the model's ``0x00`` leaves the one-character form unused,
an *approximation*. The firmware resets the part by clearing and setting bit 7
of the display control expander, then reads one byte and ignores a failure.
The model does not connect that bit to the display: the line is not wired, an
*approximation*. A machine reset clears the kept bytes. What the part does with
the command bytes is *unknown*, and so is the part itself: it shows nothing,
and the kept bytes are readable only through the ``last-write`` property of
the ``sgi-l1-display`` device, a test hook. A write that fails logs "Display
write error" and a failed reset write "Display reset error", both only
logged by the firmware; neither disables the display
(*software expectation*).

**Not fitted.** Everything else on segments 4 to 13: the processor modules'
power expanders and records, the processor module 1 monitors and the eight
memory module records. Each call stays unacknowledged, and the firmware's
probe of the memory modules clears their presence silently
(*software expectation*).

**Management USB controller.** With ``board-peers`` the 64 KiB window at
``CS3`` is a model of the two registers the firmware's probe uses and
logs the rest as unimplemented. The revision register at offset ``0x18`` is
read-only and reads ``0x13``; the scratch register at ``0x1b`` stores a
byte; bit 0 of the register at ``0x1c`` reads back the last value written
and starts clear. The firmware writes ``0x1c`` as zero, then one as the last
step of its reset sequence, and its periodic check reads that bit
(*software expectation*); the part's register at that offset is the master
control and status register of the USS-820D data sheet, with bit 0 the device
enable (*inferred*), and no other bit is kept. The probe the firmware runs
treats ``0x10``, ``0x11`` and ``0x13`` as quiet passes, and the published
USS-820D data sheet puts the revision at ``0x18`` and a scratch register at
``0x1b`` and gives those three as the B, C and D revisions (*inferred*, from
the sheet's reading of it). Which revision is fitted is *unknown*; ``0x13`` is
the newest of the three and the choice is an *approximation*. The data sheet is
not among the cited sources. The two offsets, the revision at ``0x18`` and the
scratch register at ``0x1b``, agree with a public driver header (*inferred*);
the mapping of ``0x10``, ``0x11`` and ``0x13`` to the three revisions rests on
the data sheet alone, as the firmware sheet summarises it, and is *inferred*.
The scratch register of the real part has a resume interrupt enable in bit 7
that the model does not act on. The firmware's reset sequence also writes about
forty-six other bytes to endpoint, status and interrupt registers; the model
logs them as unimplemented accesses and ignores them, and what they enable is
*unknown*. Every other offset is a logged unimplemented access that reads zero.

.. _sgi-l1-board-signals:

Signals to the machine
----------------------

``signals=ID`` names a socket chardev, and the board then carries the C-brick's
node supply, reset hold, soft reset and NMI to a machine process over an
``sgi-l1-signal-link-board`` (:ref:`sgi-l1-signal-link`). It needs
``board-peers``, because the lines come from the expanders above, and the
machine refuses to start without it or with an unknown chardev. ``signal-epoch``
is the number the machine tells a restart of this process by (default 1); the
launcher must pass a new one for each restart. The device is
``/machine/peripheral/l1-signals``; its ``up`` and ``lines`` properties show the
link and the levels.

Each write to Bedrock control 1, Bedrock control 2 or fan control hands the
whole byte to a decode, which recomputes four lines and sends the ones that
changed (*software expectation* for the writes, *inferred* for what a level
means at the chip; the first three rows follow the C-brick table in the
two-wire bus sheet):

.. list-table::
   :header-rows: 1
   :widths: 16 84

   * - Line
     - Asserted when
   * - ``node-power``
     - fan control bit 7 is clear: a low bit enables the 48 V supply, the first
       of the supply enables (*software expectation*); *inferred* to be the
       node's main supply
   * - ``reset-hold``
     - any of Bedrock control 1 bit 0, bit 1 or Bedrock control 2 bit 0 is
       clear; *inferred* from the firmware clearing all three to hold and
       setting them last to release
   * - ``soft-reset``
     - Bedrock control 1 bit 7 is set (the firmware sets it, waits and clears
       it); idle until that register is first written
   * - ``nmi``
     - Bedrock control 1 bit 6 is clear (it idles set); one node board, so
       line ``nmi.0``

Which pin of the node chip any bit reaches, its polarity there, and whether a
soft reset and an NMI are separate inputs are *unknown*, so a line is a logical
event and the decode an *approximation*. Writes that change none of the four
lines send nothing, and no other expander is decoded; every write is still
traced as ``sgi_l1_expander_write`` and each line change as
``sgi_l1_signal_decode``. Before the firmware writes anything the three bytes
are the expanders' power-on ones, ``0xff``: the supply off, the reset group not
held, no NMI (*inferred*). Soft reset would read asserted from ``0xff``, whose
meaning at the chip is *unknown*, so the decode keeps it idle until Bedrock
control 1 is first written, an *approximation* that gives the machine a defined
level. What the node chip does in this window is *unknown*. The firmware's
first writes, ``0x72`` and ``0x8a``, then hold the reset. A board reset does
not touch the latches or the decode, so no line changes: a node left powered
keeps its supply and a released node stays released, as the firmware's restart
that keeps a powered brick's outputs expects (*software expectation*). The
firmware rewrites the cold values itself on its other paths.

The machine's two lines, ``power-good`` and ``node-reset-state``, arrive and are
traced and held. Nothing on the C-brick reads either, so nothing is connected
to them. The Origin 300, I-brick and R-brick decodes are not modelled.

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
- With a C-brick record and ``board-peers=on`` the same run reaches the
  prompt with every no-acknowledge report gone for the display expander, the
  processor module presence expander, the three node monitors, the two port
  controls, the connection mode write, the fan, supply and temperature set-up
  of the node and the interrupt-driven rescans of the COM, PORT, POWER and
  FAN expanders and of the node monitors. The management USB probe passes
  and the controller is registered. The processor module 0 monitors, which
  the firmware initialises and polls whether or not the module is fitted,
  acknowledge, and so does the display driver, so neither prints a report.
  One thing is left: the memory module records are probed at ``0xa0`` and not
  acknowledged, silently. The USB controller's other registers are written
  and polled as logged unimplemented accesses. ``power`` prints the supplies
  at ``0.000V`` with the firmware's default margin values where the empty bus
  printed an error code for each, and ``env`` reports that monitoring is
  enabled and running, with every supply and fan in the "Wait Pwr" state,
  both processor modules' supplies not present and their two temperature
  rows, for the absent module 0, also "Wait Pwr" at -4C/24F because the
  monitors' registers read zero (a side effect of the fitted-but-absent
  approximation), and both module 1 temperature rows not present, where with
  the empty bus it reported the configuration error. The firmware's skip of
  that restart at boot is *unknown*, so this shows the run and not the cause.
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

.. _sgi-l1-board-settings:

Settings window
---------------

The window below the clock registers holds, as a *software expectation* read from
the stock image, an eight-byte header, a settings record of 0x70 bytes at
``0x008`` and its mirror at ``0x078``, a descriptor record and a third record
with their own mirrors, and a text ring of the event log. There is no magic
number or CRC. A region is valid when its bytes, its last byte included, add to
zero modulo 256, and a header, record or mirror that fails is rebuilt from
defaults, silently, with the boot carrying on, so a blank window works. A
settings record whose first byte has the version nibble ``0xa`` and a good sum is
taken as it stands. The fields the host chooses are, as offsets in that record:
the rack in bits 23:14 and the bay in bits 13:8 of the big-endian word at 0, the
partition at ``0x1f`` (255 is replaced by 0), the brick type letter at ``0x22``
(an identity fall-back; ``C`` is taken, a stored ``4`` is not), the automatic
power-up flag in bit 7 of the byte at 4, and bit 3 of the byte at ``0x17``,
which makes the power-up code skip the brick serial-number check. The other bytes
a rebuilt record gets, and that the high nibble of ``0x17`` is the router role
and that a record of version 1 to 9 starts an upgrade nobody decoded, are in the
same reading; the model writes the defaults and version ``0xa``. A third record
left zero clears the brick serial number, which the writer does not set.

**System serial.** The firmware keeps the system serial as ASCII at
``+0x08`` of the first record, window offset ``0x010``: eight characters and a
NUL, covered by the record's sum and its mirror, with no checksum of its own
(*software expectation*). It is a letter ``L`` to ``R``, a digit 0 to 3 and six
digits. At every start the firmware rewrites a value that fails the format to
``L0000000`` and keeps a well-formed one (*software expectation*). It also
replaces a first letter that differs from the family's required letter; that
the letter is ``L`` for a C-brick is *inferred* from the firmware's profile
byte, so the model takes the letter from the value and a letter other than
``L`` may be rewritten (*unknown*). Its ``power up`` check refuses a
serial whose seven characters after the letter are all zero, or are the
disabled markers ``9999999`` and ``3999999``, with "No assigned System Serial
Number", and so a window with none never powers a node (*software
expectation*, run on the model). Which value a real machine carries, and
whether any list of issued serials exists, are *unknown*. The first
record's skip bit (record ``+0x17`` bit 3) is not relied on: the firmware's
reading of it conflicts with a model run, so what the bit does is *unknown*.

``sgi_l1_settings_image()`` writes such a window: the header, the first record
and its mirror from those fields, everything else zero. It refuses a rack above
1023, a bay above 63, partition 255 and any brick type but ``C`` or none, the
only one the reading shows being taken. The R-brick's router role and series are
not written. Given a system serial it writes it as above and refuses a value
the firmware would repair or refuse: the wrong length or format, all zeros after
the letter and the two markers; without one the bytes are those of a window
that never had the field.
``tests/unit/test-sgi-l1-settings.c`` checks the sums, the mirror, the fields, the serial and the refusals.

``settings=FILE`` starts the clock RAM with the first 2048 bytes below the clock
registers of FILE, which must be exactly 2048 bytes. The clock registers keep the
host's time. The firmware's writes are not saved back, so settings survive no
restart of the process: an *approximation* of a part that is battery-backed.
``system-serial=L1234567`` starts it with the window ``sgi_l1_settings_image()``
writes for that serial and the other fields at their defaults, and refuses a
value the image writer refuses; it excludes ``settings``, so a host that wants
a serial and other fields writes the serial into its own file. The behavioural
L1 takes the machine's system serial text, and the same text given to
``system-serial`` is meant to give both one serial; the two were not run side
by side. The launcher's ``--serial`` resolves that text for the machine; the
launcher does not yet pass it to this property. For a C-brick use a
serial starting with ``L``: the format rule accepts any letter ``L`` to ``R``,
but the firmware may rewrite one that is not the family's letter (*unknown*).

.. _sgi-l1-board-unimplemented:

Unimplemented parts
===================

These windows and registers answer with ``LOG_UNIMP`` and nothing else.
Start the machine with ``-d unimp`` to see every access, with the window or
register name and the value. ``-d guest_errors`` adds accesses to addresses
that are not registers.

- The management USB controller at ``CS3`` (except its revision, scratch and
  bit 0 of its ``0x1c`` registers with ``board-peers``) and the device at
  ``CS5``.
- The two-wire bus has the controller described above but no device on it
  unless the board is given one (:ref:`sgi-l1-board-mbus-peers`): a transfer
  to an address nothing answers is not acknowledged, and the firmware reports
  the board identification EEPROMs and the display expander as unreadable.
- The parallel port's pins, except through the scan chain
  (:ref:`sgi-l1-board-parallel-port`).
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

- The board has no node and no link to another controller. Its only link to a
  node is the signal link above, which carries four logical lines. It has no
  environmental peers: the two-wire bus carries only the identity parts
  :ref:`sgi-l1-board-mbus-peers` lists, and with ``board-peers`` the devices
  :ref:`sgi-l1-board-cbrick-peers` lists, which answer the firmware's calls but
  report nothing about a brick.
- The I-brick's L1 is not a firmware board here: the I-brick identity parts
  are served for the stock firmware, but no I-brick board runs it, so there is
  no C-brick to I-brick cable line. The C-to-C pairing on channel 0 at both
  ends is *inferred*.
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
interrupt controller, the timers, the watchdog and the four-channel UART. The
same test reads the two-wire controller from the host against a stock AT24C
EEPROM and an address nothing answers, and each identity part through the
controller, checking the record layout and the names given. With
``board-peers`` it checks, through the controller, that the select expander
reads all ones at power-up and switches segments, that a segment 0 access
leaves the select alone, that segments 4 to 13 hold nothing, and each
expander's wire address, segment, power-on value, input bits and the level its
input pins read with nothing attached, from the firmware's table written out in
the test, that the latches survive a system reset, that remote power OK reads
low and a power control status bit high only when its pin is driven, and that
each node monitor starts at zero, takes the firmware's initialisation writes
and reads them back, and keeps its register pointer between reads without
stepping it. The USB window test programs chip select 3 from test-owned code,
then checks the firmware probe's reads and writes: the revision at ``0x18``,
the scratch register at ``0x1b`` holding ``0xb5`` and every even value to
``0xfc``, the revision refusing a write, bit 0 of ``0x1c`` reading back and
clearing on reset, and any other offset logging and reading zero. The processor
module 0 monitors are checked on segment 4 for their independence from the node
monitor at the same wire address, and the display for its write forms, its read
and its answering under any select byte.

The parallel port registers are checked from the host for their reset values,
the firmware's data-then-direction order and what an output and an input read.
With ``board-peers`` the scan chain is driven pin by pin from the host as the
firmware does: its reset pulse, the 32-clock reset, the 12-bit and all-ones
instruction scans, the 32-bit data scan that must come back one bit late and
the two closing resets, with each compare the firmware makes. Further tests
check that the instruction register is 12 bits long, that ``TRST`` low holds
the chain in reset and that a write to ``TDO``'s latch bit changes nothing a
read shows.

The power-bay link is checked by test-owned code that writes three bytes to
on-chip UART 1 and reads one the peer sent, through a socket chardev as the
fifth serial backend. ``settings`` is checked by loading a file of known bytes
and reading the window back through chip select 1, and by the two refusals.
``system-serial`` is checked by reading the serial, the record sum and the
mirror back through chip select 1, and by the refused values and the
exclusion with ``settings``.

With ``signals`` a test-owned peer plays the machine from a socket. It checks
the board's hello and power-on state, each decoded bit and the bits that move
nothing, the hold needing all three bits and releasing on the last, a board
reset changing no line and keeping the latches, a reconnect restating the levels
without replaying a pulse made meanwhile, the machine's lines being accepted,
and the two refusals.

The functional test boots the C-brick with ``board-peers`` and checks which
reports are gone, that the supply table reads, what ``env`` prints, and that
the only address the firmware calls without an acknowledge is the memory module
records'.
``tests/functional/m68k/test_sgi_l1_board.py`` boots the stock firmware to the
operator console prompt, with an empty bus and with a C-brick record, when the
firmware image is available and is skipped when it is not.
