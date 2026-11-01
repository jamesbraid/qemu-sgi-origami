.. SPDX-License-Identifier: BSD-3-Clause

.. _sgi-ioc4:

====================
IOC4: I/O controller
====================

Used in :ref:`sgi-ip53`: the IO9 base I/O board of the Origin 350, Onyx
350, Tezro and Onyx 4 host machines (:ref:`sgi-ip53-ioc4`).

.. seealso::

   Platform pages: :ref:`sgi-ip53`.

.. seealso::

   - IOC3 (:doc:`ioc3`): the base I/O controller of the earlier platforms.

The IOC4 is SGI's PCI I/O controller. One PCI function holds four 16550-compatible
serial ports with ring DMA, a PS/2 keyboard and mouse controller, one
ATA/ATAPI channel, general-purpose pins and a byte bus for a timekeeper.
QEMU models PCI revision ``0x53`` as ``sgi-ioc4`` in
``hw/misc/sgi-ioc4.c``, with ``sgi-ioc3-kbc``
(``hw/input/sgi-ioc3-kbc.c``) as its PS/2 child. The model covers the
serial ports, their DMA, the PS/2 controller, the interrupt registers and
the timekeeper window. ATA and the general-purpose pins are not modelled.

Facts are rated as described in :ref:`sgi-ratings`. SGI published no
IOC4 specification. Register offsets and field names come from the
public Linux and OpenBSD drivers and are each a *software expectation*. Reset
values, access widths and timing that no public source states are
each an *approximation* and say so.

.. _sgi-ioc4-sources:

Sources
=======

- OpenBSD ``sys/arch/sgi/pci/iof.c`` and ``iofreg.h``,
  ``sys/arch/sgi/dev/com_iof.c``, ``iockbc.c`` and ``iockbcreg.h``,
  ``sys/arch/sgi/sgi/ip27_machdep.c`` and ``sys/dev/pci/pcidevs`` at
  ``2df91d80c104``, as software expectations: PCI identity, the
  interrupt registers and dispatch, UART windows and clock selection,
  the PS/2 registers and the early console path.
- Linux/MIPS ``drivers/tty/serial/ioc4_serial.c`` and
  ``include/linux/ioc4.h`` at ``c9bace5bce3e``, as software
  expectations: the serial DMA registers, ring entries, interrupt bits
  and error capture.
- Linux ``drivers/misc/ioc4.c`` and ``drivers/ide/sgiioc4.c`` at tag
  ``v4.19-rc1``, as software expectations: the general-purpose pins,
  clock calibration and the ATA register layout.
- The Linux IP35 port's ``drivers/tty/serial/ioc4_serial.c`` at
  ``9f613a321ea0``, as a software expectation: the serial arbitration
  idle bit and the port reset sequence.
- Miod Vallat's October 2009 Origin 350 announcement and its OpenBSD
  dmesg, :src:`miod-openbsd-sgi-story Origin 350 support`: IOC4
  revision ``0x53``, four ``ns16550a`` ports with 16-byte FIFOs, a
  DS1742-labelled clock on the byte bus, and no ATAPI support in that
  release.
- The Origin 350 guide :src:`007-4566-001 p. 110-111, 120, 222`, the
  Onyx 350 guide :src:`007-4632-001 p. 29, 50, 58, 63`, the Tezro
  guides :src:`007-4564-001 p. 32` and :src:`007-4643-003 p. 24-25` and
  the Onyx4 guide :src:`007-4634-002 p. 12, 23, 70`: the PS/2
  connectors on the base module.
- The IP35 PROM, :src:`ip35-prom-6.210 IOC4 diagnostics`, run on this
  model, as a software expectation: the PCI status value, the ATA
  register readback and the serial DMA loopback test it performs.

.. _sgi-ioc4-identity:

Identity and placement
======================

The IOC4 is vendor ``0x10a9``, device ``0x100a`` in OpenBSD ``pcidevs``
and Linux ``pci_ids.h``, a *software expectation*. A published Origin
350 dmesg reports it at revision ``0x53``
(:src:`miod-openbsd-sgi-story Origin 350 support`), *observed*. The
model reports that revision for every IO9.

OpenBSD numbers the IOC4 as PCI device 0 on the PIC bus because it adds
one to skip PIC's own configuration slot; Linux would number the same
function device 1. The machine models place it at device 1. That this
is the same slot is *inferred* from the two drivers' configuration
window arithmetic; the IO9 connector wiring itself is *unknown*.

The model reports PCI class ``0x0b4000``: base class ``0x0b``, subclass
``0x40``, programming interface ``0x00``. Linux's IOC4 core driver binds
by an exact 24-bit class match, and its device table lists vendor SGI,
device IOC4, any subsystem, class ``0x0b4000`` with mask ``0xffffff``
(Linux v2.6.30 ``drivers/misc/ioc4.c``), a *software expectation*. The
model's other configuration values are each an *approximation*: interrupt pin 1
(INTA) and one 1 MiB 32-bit memory BAR, sized so the byte-bus select at
``0x80000`` falls inside it. On IP53 boards the ``pci-status-compat``
property sets PCI Status to ``0x0220`` at realize and at every reset.
Status bit 5 is the 66 MHz-capable bit and bits 10-9 are DEVSEL timing,
where ``01`` is medium; both are read-only
(:src:`pci-local-bus-2.2 section 6.2.3`), *established*. The model
keeps them across Command writes and Status write-one-to-clear writes.
The IP35 PROM's diagnostic expects that Command/Status word,
``0x02200000`` (:src:`ip35-prom-6.210 IOC4 diagnostics`), a *software
expectation*. The revision ``0x53`` part's real Status reset value is
*unknown*; a configuration read on such a board before any driver runs
would settle it. Without the property Status reads
zero. Either way QEMU's PCI core also sets Status bit 3, interrupt status,
while INTA is asserted.

The machine models connect the IOC4's PCI interrupt to PIC input 0 and
its four ports to the node's serial lines. The IO9's real interrupt
wiring is *unknown*.

.. _sgi-ioc4-register-map:

Register map
============

Offsets are from BAR 0. Registers outside the UART and timekeeper
windows are 32 bits wide and big-endian, and only aligned 32-bit
accesses are accepted. Any other width fails with ``LOG_GUEST_ERROR``.
OpenBSD uses 32-bit accesses here and bytes in the UART windows,
*software expectation*; the full set of widths the chip accepts is
*unknown*.

.. list-table::
   :header-rows: 1

   * - Offset
     - Name
     - Width
     - Reset
     - Access
     - Notes
   * - ``0x000``
     - DMA error address, low
     - 32
     - ``0``
     - R
     - Valid bit 0, requester bits 4-1, multiple-error bit 5, address
       bits 31-6. Writes log ``LOG_UNIMP``.
   * - ``0x004``
     - DMA error address, high
     - 32
     - ``0``
     - R
     - Address bits 63-32.
   * - ``0x008``
     - Serial interrupt pending
     - 32
     - ``0``
     - R, W1C
     - Eight bits per port. See :ref:`sgi-ioc4-interrupts`.
   * - ``0x00c``
     - Other interrupt pending
     - 32
     - ``0``
     - R, W1C
     - Serial DMA errors and PS/2.
   * - ``0x010``
     - Serial enable set
     - 32
     - ``0``
     - R, W1S
     - Reads return the enable mask.
   * - ``0x014``
     - Other enable set
     - 32
     - ``0``
     - R, W1S
     - Reads return the enable mask.
   * - ``0x018``
     - Serial enable clear
     - 32
     - none
     - W1C
     - Reads log ``LOG_UNIMP`` and return zero.
   * - ``0x01c``
     - Other enable clear
     - 32
     - none
     - W1C
     - As ``0x018``.
   * - ``0x020``
     - Serial and ATA control
     - 32
     - ``0x80``
     - R
     - Serial DMA profile only: bit 7 is the arbitration idle bit.
       Otherwise not modelled.
   * - ``0x024``
     - Miscellaneous control
     - 32
     - clock
     - R
     - Bit 0 reads one for a 66,666,667 Hz UART clock. Writes log
       ``LOG_UNIMP``.
   * - ``0x028``-``0x05c``
     - External interrupt and general-purpose pins
     - 32
     -
     -
     - Not modelled.
   * - ``0x100``-``0x160``
     - ATA task file, control and DMA
     - 32
     -
     -
     - Not modelled, except the ``0x148`` latch on IP53 boards.
   * - ``0x200``-``0x210``
     - PS/2 controller
     - 32
     - ``0``
     - R/W
     - Five registers. See :ref:`sgi-ioc4-ps2`.
   * - ``0x300``-``0x30c``
     - Serial DMA ring bases
     - 32
     - ``0``
     - R/W
     - Low and high words for ports 0-1, then ports 2-3.
   * - ``0x310 + 0x1c*n``
     - Serial DMA port registers
     - 32
     - ``0``
     - R/W
     - Seven per port. See :ref:`sgi-ioc4-serial-dma`.
   * - ``0x380 + 8*n``
     - UART ``n``
     - 8
     - 16550A
     - R/W
     - Byte accesses only.
   * - ``0x80000``
     - Timekeeper window
     - 8
     - device
     - R/W
     - Byte-bus select 0. See :ref:`sgi-ioc4-timekeeper`.

Reads of any other offset log ``LOG_UNIMP`` and return zero; writes log
``LOG_UNIMP`` and are dropped.

The layout above follows OpenBSD ``iofreg.h`` and Linux ``ioc4.h``, a
*software expectation*. The register widths, the reset values and the
handling of the enable-clear reads are each an *approximation*.

.. _sgi-ioc4-uart:

Serial ports
============

Each port is an eight-byte 16550A window at ``0x380 + 8*n``. The
published dmesg reports four ``ns16550a`` ports with 16-byte FIFOs
(:src:`miod-openbsd-sgi-story Origin 350 support`), *observed*. The
model uses QEMU's 16550A with byte lanes in register order.

OpenBSD reads miscellaneous-control bit 0 to choose a 66,666,667 Hz or
33,333,333 Hz UART input clock, a *software expectation*. The model's
``clock-hz`` property selects one of the two; any other value fails
realize. The baud base is the clock divided by 16 and the bit reads back
accordingly. The Origin 350, Onyx 350 and Onyx 4 host machines choose
33,333,333 Hz and the Tezro machines 66,666,667 Hz, an *approximation*:
the boards' real selection is *unknown*.

A UART window accepts only byte accesses. A wider access that reaches the
IOC4 as one transaction fails with ``LOG_GUEST_ERROR`` and has no effect
on the UART. An unaligned wider access that the host bridge splits into
bytes reaches each register in turn; the qtest pins this through a
standalone XBridge, not the PIC the IP53 machines use. The public drivers
use bytes, *software expectation*. The chip's answer to a wider access is
*unknown*.

OpenBSD takes the console UART's address and rate from the firmware's
console record and recognises an IOC4 port by an offset at or above
``0x380``, a *software expectation*. The L1 system controller console is
a separate device (:doc:`l1`).

.. _sgi-ioc4-interrupts:

Interrupts
==========

The serial pending word gives each port one byte at bits ``8*n`` to
``8*n+7``. Linux ``ioc4.h`` names them, from bit 0: transmit ring empty,
receive ring full, receive threshold, receive timer, DCD changed, CTS
changed, UART interrupt and explicit transmit completion, a *software
expectation*. The other pending word holds ATA in bit 0, ATA DMA error in
bit 1, serial DMA errors for ports 0-3 in bits 2-5 and the PS/2
controller in bit 6.

Writing one to a pending bit clears it, and writing one to an
enable-set or enable-clear bit sets or clears that enable, a *software
expectation* from OpenBSD ``iof.c``. OpenBSD masks a pending source,
runs its handler, acknowledges the bit and unmasks it, *software
expectation*. The IOC4 drives one PCI interrupt, asserted while any
pending bit is also enabled, a *software expectation* from the same
driver.

The model's choices for these bits are each an *approximation*:

- A UART interrupt sets its port's bit 6 and also counts as pending for
  as long as the UART holds its interrupt line. Acknowledging the bit
  does not hide a line that is still active.
- Transmit ring empty, receive ring full and the receive threshold are
  levels. Transmit ring empty is set while ring DMA is on and nothing is
  left to send. Receive ring full is set while ring DMA is on and an
  entry waits that the ring had no room for. The receive threshold is
  set while the ring holds at least the threshold, and a zero threshold
  disables it. Writing one to any of them has no effect. The rules are
  those of the shared engine in :ref:`sgi-ioc3-serial-dma`.
- The PS/2 bit latches on each rising edge of the controller's interrupt
  and stays until acknowledged.
- DCD and CTS change bits and both ATA bits are never set.

How the IOC4 interrupt reaches the processor through PIC and Bedrock is
covered in :ref:`sgi-ip35-address-map-interrupt`.

.. _sgi-ioc4-serial-dma:

Serial DMA
==========

IOC4 runs the same serial ring DMA engine as IOC3, described once in
:ref:`sgi-ioc3-serial-dma`: ring entries, the transmit walk, the receive
packer, the receive timer, pause, drain, loopback and the 16550 shadow.
The register layout, ring layout and entry format are those of Linux
``ioc4_serial.c`` and ``ioc4.h``, a *software expectation*. What follows
is where IOC4 differs.

The engine runs only when the ``serial-dma-experimental`` property is
on. Every IP53 machine model sets it. With it off, the engine never
touches a UART: no ring traffic happens, received characters stay in the
UART FIFO and UART transmit and modem-control writes ignore the DMA
state. The registers below still decode and store, and offset ``0x20``
is unmodelled.

Rings
-----

IOC4 has two ring-base pairs where IOC3 has one. Ports 0 and 1 share the
base at ``0x300`` (low word) and ``0x304`` (high word). Ports 2 and 3
share ``0x308`` and ``0x30c``. Bit 0 of the low word selects 4 KiB rings
when set and 1 KiB rings when clear. Each pair's region holds four rings
in order: transmit for the even port, receive for the even port,
transmit for the odd port, receive for the odd port. Linux aligns the
base to the whole region, 4 KiB for 1 KiB rings and 16 KiB for 4 KiB
rings, *software expectation*. The model takes bits 12-31 of the low
word as address and does not enforce the alignment.

Port registers
--------------

.. list-table::
   :header-rows: 1

   * - Offset
     - Name
     - Notes
   * - ``0x310 + 0x1c*n``
     - Control
     - Threshold bits 8-0, drain bit 27, DMA enable bit 28, pause bit
       29, reset bit 31. Bit 16 reads busy while a transmit delay runs
       and bit 30 reads pause state; both are read-only. Other bits are
       stored and log ``LOG_UNIMP``.
   * - ``0x314 + 0x1c*n``
     - Transmit producer
     - Written by software.
   * - ``0x318 + 0x1c*n``
     - Transmit consumer
     - Advanced by the engine. A write stores the pointer field.
   * - ``0x31c + 0x1c*n``
     - Receive producer
     - Advanced by the engine. A write stores the pointer field. Bits
       24-26 do not report the packer's held count, as they do on IOC3.
   * - ``0x320 + 0x1c*n``
     - Receive consumer
     - Written by software. Bit 31 arms the receive timer.
   * - ``0x324 + 0x1c*n``
     - Receive timer reload
     - Bits 11-0, in 16 kHz ticks. Other bits log ``LOG_UNIMP``.
   * - ``0x328 + 0x1c*n``
     - Shadow
     - The 16550 mirror of :ref:`sgi-ioc3-serial-dma`. Writes are
       discarded.

The producer and consumer registers hold byte offsets that advance by
eight and wrap at the ring size.

Differences from IOC3
---------------------

- IOC3 has one ring base for both ports. IOC4 has one per pair.
- The receive producer index does not report the packer's held count.
  Linux's IOC4 register definitions do not name the field, so whether
  the part has it is *unknown*. IOC3's header defines it.
- A failed ring transfer halts the port until a channel reset (control
  bit 31), captures the failing address and raises the port's error
  source in the other pending word (see `DMA errors`_). The header's
  name for these sources is "serial halted by mem err", a *software
  expectation*. Whether an IOC3 port halts is *unknown* and the model
  does not halt it.
- A pointer write with bits outside the pointer field is stored masked
  and logs ``LOG_GUEST_ERROR``. This applies to the transmit producer
  and the receive consumer, whose bit 31 is allowed.
- The model logs ``LOG_GUEST_ERROR`` for these, then lets them take
  effect, because the 16550 orders PIO and DMA characters itself:

  - enabling ring DMA over a misaligned ring base;
  - enabling ring DMA while the UART FIFOs hold PIO data;
  - disabling ring DMA while the port has queued transmit entries,
    held receive data, an armed timer or a pending drain;
  - changing a pair's ring base while either port of the pair has DMA
    enabled or paused, nonzero pointers, or work pending;
  - a PIO write to the transmit holding register while ring DMA is on.

  Linux's driver avoids each of these. The refusals are model policy,
  not hardware, an *approximation*.
- The engine's own guest-error cases apply as on IOC3: undefined
  transmit control bits, and input from the backend in loopback.
- The serial idle bit at ``0x20`` follows the rule of IOC3's arbiter
  idle bit (see below).

Loopback
--------

Linux names shadow bit 28 the loopback bit, a *software expectation*.
Loopback is the 16550's modem-control loop bit, which a PIO write or a
ring modem-control slot sets, and the engine treats it as
:ref:`sgi-ioc3-serial-dma` describes: transmitted characters return to
the port's own receiver one character time later, and input from the
backend is discarded and logged. The received status byte carries the
looped modem lines. The IP35 PROM's serial DMA diagnostic sets loopback
through modem control and compares received status against ``0xc0``
(:src:`ip35-prom-6.210 IOC4 diagnostics`), a *software expectation*.
How the IOC4 itself loops DMA traffic is *unknown*.

Arbitration idle bit
--------------------

The Linux IP35 port's serial driver names bit 7 of the serial control
register at ``0x20`` (``SIO_CR``) the arbitration idle bit, zero while a
serial port's DMA request is active, and polls it after setting a
port's reset bit, a *software expectation*. The model returns ``0x80``
unless the engine is inside a ring transfer at that moment, and zero
during one, an *approximation*. IOC3's chip-control bit 22 follows the
same rule (:ref:`sgi-ioc3-serial-dma`). QEMU completes each transfer
before the access that started it returns, so software always reads
idle. Writes to ``0x20`` log ``LOG_UNIMP``.

Byte order
----------

Ring entries reach memory through the PIC's DMA path, so the byte swap
the PIC applies for the ring address applies to them. The IOC4 adds
none of its own. PIO to the UART windows is in register order. Each is
an *approximation*; the IOC4's own DMA byte order is *unknown*.

.. _sgi-ioc4-dma-errors:

DMA errors
----------

Linux ``ioc4.h`` encodes the requester as ``2*port`` for transmit and
``2*port + 1`` for receive, with the multiple-error and valid flags in
the low error word, a *software expectation*. In the model a failed ring
read or write halts both directions of that port only until a channel
reset, leaves its pointers and held data alone, logs
``LOG_GUEST_ERROR`` and sets the port's bit in the other pending word. The first failure while the valid
bit is clear records the address with its low six bits cleared and the
requester; later failures only set multiple-error. Only a device reset
clears the record. Which failure the chip records, and how software
clears it, are *unknown*. DMA needs PCI bus mastering enabled.

.. _sgi-ioc4-ps2:

PS/2 keyboard and mouse
=======================

OpenBSD ``iockbc.c`` and ``iockbcreg.h`` place five 32-bit registers at
``0x200``: control and status, keyboard receive, mouse receive, keyboard
transmit and mouse transmit, the same layout as the IOC3, a *software
expectation*. A receive word reports up to three bytes in bits 23-0
with valid bits 31-29. Control bits 0 and 1 are the keyboard and mouse
write-pending bits.

The model reuses the IOC3 controller (:doc:`ioc3`) behind an adapter
with these choices, each an *approximation*:

- The registers are big-endian words; the PS/2 byte is the low eight
  bits. The bus byte order of the real IOC4 controller is *unknown*.
- A transmit completes inside the register write, so both write-pending
  bits always read zero, and control writes ignore them.
- Receive words hide bits 28-24, which on the IOC3 hold receive source
  and overflow flags.
- The controller resets to zero.
- Reads of the transmit registers and writes to the receive registers
  log ``LOG_GUEST_ERROR``; such reads return zero.
- Accesses other than aligned 32-bit words fail with
  ``LOG_GUEST_ERROR``.

The Origin 350 and Onyx 350 guides put two PS/2 connectors, driven by
the IOC4, on the serial daughtercard of the base compute module
(:src:`007-4566-001 p. 110-111, 120, 222`,
:src:`007-4632-001 p. 50, 58, 63`), *established*. The Tezro guides
show the same two ports without naming their controller
(:src:`007-4564-001 p. 32`, :src:`007-4643-003 p. 24-25`), so the IOC4
route is *inferred*; on the Onyx 4 the ports themselves are *inferred*
from the Origin 350 guide (:src:`007-4634-002 p. 12, 23`). The model
always attaches a PS/2 keyboard and mouse, an *approximation*: a real
system may have empty connectors, and the stock PROM then reports that
it cannot reach a keyboard, a *software expectation*. The daughtercard
exists only on a base module, and every modelled IO9 is in one.

.. _sgi-ioc4-timekeeper:

Timekeeper window
=================

OpenBSD attaches a DS1742-compatible clock at byte-bus select 0, BAR
offset ``0x80000``, and the published dmesg shows it there
(:src:`miod-openbsd-sgi-story Origin 350 support`), *observed*. The
label does not identify the fitted part. Byte-bus selects 1-3 at
``0xa0000``, ``0xc0000`` and ``0xe0000`` are not modelled.

The IOC4 maps the board's timekeeper as a subregion. A 32 KiB DS1744
device appears whole at ``0x80000``; that is what every IP53 machine
uses (:ref:`sgi-ip53-ds1744`). A 2 KiB DS1742 device exposes only its
eight clock bytes at ``0x87ff8`` (:ref:`sgi-ip53-ds1742`). Without a
timekeeper the window is empty. Both mappings are each an *approximation*; the
IO9's real byte-bus decode is *unknown*.

.. _sgi-ioc4-ata:

ATA
===

Linux ``sgiioc4.c`` maps one channel with task-file registers at
``0x100`` on a four-byte stride, alternate control at ``0x120`` and DMA
registers from ``0x140``, a *software expectation*. The published
announcement states that OpenBSD did not yet support the IO9 ATAPI
interface (:src:`miod-openbsd-sgi-story Origin 350 support`). The model
implements no ATA function: reads return zero and log ``LOG_UNIMP``.

The one exception is ``0x148`` with the ``ata-readback-compat``
property, which IP53 boards set. The IP35 PROM writes each one-bit
value and all ones there and reads each back
(:src:`ip35-prom-6.210 IOC4 diagnostics`), a *software expectation*.
The model keeps a 32-bit latch, zero after reset, with no DMA or
interrupt effect, an *approximation*. Linux names this word as part of
the descriptor pointer, inconsistently; its real meaning and reset
value are *unknown*.

.. _sgi-ioc4-reset:

Reset
=====

No public source gives the IOC4's reset state, so every value here is an
*approximation*. A reset of the IOC4 device, alone or as part of a
system reset, clears both pending and both enable words, the error
address, the ring bases (to 1 KiB rings), every port's DMA registers,
timers, staging and shadow outputs, and the ``0x148`` latch, and
deasserts the interrupt. The PS/2 controller resets to zero. On a
system reset the UARTs also return to QEMU's 16550A reset state: line
control zero, line status ``0x60``, interrupt identification ``0x01``
and divisor ``0x0c``. A reset of the IOC4 device alone leaves the UART
registers unchanged. The PCI Status compatibility value is
restored. The timekeeper keeps its contents.

Both public drivers start by disabling every source and acknowledging
pending bits, which is software initialisation, not a reset value.

.. _sgi-ioc4-migration:

Migration
=========

The device saves its PCI state, the four UARTs, the interrupt words, the
UART clock, the ring bases and sizes, the error address, the ``0x148``
latch and every port's registers, staging, transmit progress and
remaining timer and delay time. A paused port stays paused after
restore; a running one resumes its work once without repeating output.
The serial DMA, PCI status and ATA readback properties must match on
both sides, or restore fails. The PS/2 controller migrates as its own
device. Migration works only between identical builds.

.. _sgi-ioc4-limits:

Model limits
============

- ATA and ATAPI are absent apart from the ``0x148`` latch. No IRIX or
  PROM path to an IO9 optical drive works.
- General-purpose pins, the external interrupt output, byte-bus selects
  1-3 and the PCI error capture for anything but serial DMA are absent.
  Accesses log ``LOG_UNIMP``; reads return zero.
- Carrier-detect and clear-to-send change sources are never raised.
  Control bits other than the threshold, drain, enable, pause and reset
  fields are stored and log ``LOG_UNIMP``.
- Register widths other than those listed fail with
  ``LOG_GUEST_ERROR``: QEMU fails the PCI transaction and the host
  bridge path decides what the processor sees.
- Reset values, timing, the BAR size and interrupt pin are model
  choices, not measurements of a revision ``0x53`` part. The class code is
  the one Linux's driver matches, not a read from a part.
- The PS/2 ports always have a keyboard and mouse attached.

.. _sgi-ioc4-tests:

Tests
=====

``tests/qtest/sgi-ioc4-test.c`` covers PCI identity and reset, both UART
clocks, the interrupt registers, the UART FIFOs, byte-only UART access
and the XBridge split of a wider access, the ``0x148`` latch with its
width checks and migration, the PS/2 adapter and its migration, the
timekeeper window for no clock, the 8-byte clock alias and the 32 KiB
window, and the serial DMA profile: default-off, both PIC swap settings,
bus mastering, control entries, both ring sizes, drain and threshold
as levels, loopback including overrun and discarded outside input, full
rings, the timer, pointer-field checks, error capture, requester ids and
the halt until a channel reset, port reset and isolation, the mode
switches that are logged and then take effect, the idle bit, backend
reconnection and migration of active, paused and delayed ports,
including a profile mismatch.

``tests/qtest/sgi-pic-test.c`` covers the PCI Status compatibility value
and its migration, CPU PIO to an IOC4 behind PIC, and serial DMA, the
interrupt path and pending-state migration through Bedrock.
``tests/qtest/sn-builder-fault-test.c`` and ``tests/unit/test-sn-model.c``
check the IO9 placement records that create the device.
