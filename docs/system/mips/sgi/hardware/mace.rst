.. SPDX-License-Identifier: BSD-3-Clause

.. _sgi-mace:

====================
MACE: I/O controller
====================

Used in :ref:`sgi-ip32` (O2).
MACE is the I/O ASIC of the O2. It holds the PCI host bridge, the Fast
Ethernet MAC, and a peripheral controller that carries the audio
interface, the serial-port DMA channels, the PS/2 keyboard and mouse
ports, the timers and the bridge to a Super I/O part, a real-time clock
and the boot flash (:src:`sgi-mace-2.0-spec p. 8, 181`), *established*. QEMU
models chip revision 2.0 as the PCI host bridge ``sgi-mace`` in
``hw/mips/sgi/mace.c``, with the serial DMA in
``hw/mips/sgi/mace-serial-dma.c``, the PS/2 ports in
``hw/mips/sgi/mace-ps2.c``, the identity line in
``hw/mips/sgi/mace-id.c`` and the audio interface in
``hw/mips/sgi/mace-audio.c``. The Ethernet MAC is a child device on its
own page (:doc:`mace-ethernet`), the clock is the DS1685 model
(:doc:`ds1685`) and the codec is on :doc:`ad1843`. The ``o2`` machine
(:ref:`sgi-ip32`) fits MACE.

Facts are rated as described in :ref:`sgi-ratings`.

.. _sgi-mace-sources:

Sources
=======

- The *MACE I/O ASIC Specification*, chip revision 2.0, 1996-07-16,
  :src:`sgi-mace-2.0-spec`. PDF page numbers equal the printed numbers.
  Several tables in it conflict; the page says where.
- The *CRIME 1.5 Specification*, :src:`sgi-crime-1.5-spec`, for the
  address space MACE's own DMA sees and for the interrupt inputs CRIME
  allots to MACE.
- Linux ``arch/mips/include/asm/ip32/mace.h``,
  ``arch/mips/pci/pci-ip32.c``, ``arch/mips/pci/ops-mace.c``,
  ``arch/mips/sgi-ip32/ip32-platform.c``,
  ``drivers/input/serio/maceps2.c`` and ``sound/mips/sgio2audio.c`` at
  ``db2ddb871435``, as software expectations.
- NetBSD ``sys/arch/sgimips/mace/macereg.h`` at ``07004b2a9dac`` and
  OpenBSD ``sys/arch/sgi/localbus/macebus.c`` at ``2df91d80c104``, as
  software expectations.
- Two published boot reports of real O2s, an OpenBSD 5.2 report
  (:src:`dmesgd-2432`) and a NetBSD 9.99.31 report (:src:`dmesgd-5290`),
  *observed*.
- The Maxim DS2502 datasheet (:src:`maxim-ds2502-datasheet`), for the
  one-wire part the identity line can carry.

.. _sgi-mace-identity:

Identity and placement
======================

MACE 2.0 reports host-bridge revision 1 and MACE 1.0 reports 0
:src:`sgi-mace-2.0-spec p. 165`, *established*. Both captures report a
PCI bridge "mace rev 1" or "macepci0 rev 1" (:src:`dmesgd-2432 macepcibr0`,
:src:`dmesgd-5290 macepci0`), *observed*. The PCI revision register reads
1 in the model.

MACE has 4 MiB of register space at physical ``0x1f000000``, split into
eight 512 KiB blocks :src:`sgi-mace-2.0-spec p. 181`, *established*. The
block bases agree with Linux ``mace.h`` and NetBSD ``macereg.h``, and with
the offsets in both captures, *software expectation* and *observed*. The
model maps one 4 MiB aperture. Registers that exist repeat inside their
blocks in the specification; the model repeats only the ones listed
below.

.. list-table::
   :header-rows: 1

   * - Offset
     - Block
     - In the model
   * - ``0x000000``
     - Future subsystem interface
     - Unmapped. Reads and writes log ``LOG_UNIMP``; reads return 0.
   * - ``0x080000``
     - PCI host bridge registers
     - Modelled (`PCI host bridge`_).
   * - ``0x100000``, ``0x180000``, ``0x200000``
     - Video input 1, video input 2, video output
     - Absent. Same as the future block.
   * - ``0x280000``
     - Ethernet
     - Modelled (:doc:`mace-ethernet`).
   * - ``0x300000``
     - Audio interface, 64 KiB
     - Modelled (`Audio interface`_).
   * - ``0x310000``
     - Peripheral controller control, 64 KiB
     - Ring base, miscellaneous control, interrupt status and mask,
       serial DMA (`Peripheral controller`_). The parallel-port DMA
       registers in the specification (page 1 of the block) are absent.
   * - ``0x320000``
     - PS/2 keyboard and mouse
     - Modelled (`PS/2 ports`_).
   * - ``0x330000``
     - I2C
     - Absent from every machine. A test-only register model exists
       (`I2C bus interface`_).
   * - ``0x340000``
     - UST and compare timers, with the compare registers aliased at
       ``0x350000``, ``0x360000`` and ``0x370000``
     - Modelled (`Timers and UST`_).
   * - ``0x380000``
     - Parallel port, ``0x388000`` ECP registers
     - Absent (`Parallel port`_).
   * - ``0x390000``, ``0x398000``
     - Serial ports 1 and 2
     - Modelled (`Serial ports`_).
   * - ``0x3a0000``
     - Real-time clock
     - Modelled (`Real-time clock`_).

The captures place the serial ports at ``0x390000`` and ``0x398000``, the
clock at ``0x3a0000``, the parallel port at ``0x380000``, the PS/2
controller at ``0x320000``, the audio device at ``0x300000``, the
Ethernet MAC at ``0x280000`` and the PCI bridge at ``0x080000``
(:src:`dmesgd-2432 macebus0`, :src:`dmesgd-5290 mace0`), *observed*.

.. _sgi-mace-interrupts:

Interrupt routing
=================

MACE sends sixteen level signals to CRIME, one per CRIME interrupt slot
:src:`sgi-mace-2.0-spec p. 179`, *established*:

.. list-table::
   :header-rows: 1

   * - CRIME slot
     - Source
     - In the model
   * - 0-2
     - Video inputs and output
     - Never raised.
   * - 3
     - Ethernet
     - Raised by the MAC's interrupt output.
   * - 4
     - Peripheral status bits 16-31: parallel and serial ports
     - Raised while any unmasked bit is set.
   * - 5
     - Peripheral status bits 8-15: clock, PS/2 and timers
     - Raised by any unmasked bit of ``0xeb00``. Poll bits 10 and 12 do
       not count.
   * - 6
     - Peripheral status bits 0-7: audio
     - Raised by any unmasked bit of ``0xff``.
   * - 7
     - PCI error
     - Raised by a master abort when enabled (`PCI host bridge`_).
   * - 8-15
     - PCI interrupt inputs 0-7
     - Inputs 0-2 only (`PCI host bridge`_).

The captures report the Ethernet interrupt as 3, the PS/2 controller as 5,
the serial ports as 4, the audio device as 6, the PCI bridge as 7 and the
two SCSI controllers as 8 and 9 (:src:`dmesgd-2432 macebus0`,
:src:`dmesgd-5290 mace0`), *observed*. The model drives the slots through
CRIME's MACE inputs (:doc:`crime`).

.. _sgi-mace-pci:

PCI host bridge
===============

The bridge is based on PCI 2.1 with the 64-bit extensions, runs at
33.33 MHz and arbitrates up to five masters
:src:`sgi-mace-2.0-spec p. 157`, *established*. It has six 32-bit
registers in the PCI block and no others; reads of unlisted addresses
return all ones and writes are dropped :src:`sgi-mace-2.0-spec p. 161`,
*established*. The registers repeat nowhere.

.. list-table::
   :header-rows: 1

   * - Offset
     - Name
     - Reset
     - Access
     - Notes
   * - ``0x080000``
     - Error address
     - 0
     - R
     - Valid while a flag in bits 16-19 is set.
   * - ``0x080004``
     - Error flags
     - 0
     - R/W
     - See below.
   * - ``0x080008``
     - Control
     - 0
     - R/W
     - See below.
   * - ``0x08000c``
     - Read buffer flush (write), revision (read)
     - 1 on read
     - R/W
     - The write is accepted and has no effect.
   * - ``0x080cf8``
     - CONFIG_ADDRESS
     - 0
     - W
     - Bits 23:2 are stored.
   * - ``0x080cfc``
     - CONFIG_DATA
     - n/a
     - R/W
     - Byte, halfword and word accesses.

Register accesses to the first five registers must be 32-bit and aligned
:src:`sgi-mace-2.0-spec p. 162`, *established*. The model answers another
width with ``LOG_GUEST_ERROR`` and a bus error. The specification says an
illegal size will set the illegal-transaction flag (bit 27) in the error
flags register (:src:`sgi-mace-2.0-spec p. 161`), *established*; the model
does not set it, an *approximation*.

Configuration access
--------------------

The bridge issues configuration mechanism #1 on bus 0, which is hard
wired; subsidiary buses sit below it
:src:`sgi-mace-2.0-spec p. 161`, *established*. The registers are
big-endian, so the byte lane of CONFIG_DATA is mirrored. Linux reads
register ``reg`` of a byte at ``config_data.b[(reg & 3) ^ 3]`` and of a
halfword at ``w[((reg >> 1) & 1) ^ 1]`` (``ops-mace.c``), a *software
expectation*. The model applies the same lane swap.

A configuration access to a device the model has not populated is a master
abort: a read returns all ones for its width, a write is dropped, and the
bridge sets error flag bit 31 and address-valid bit 19 and captures the
address (if bit 19 was clear). The specification also sets bit 20 for a
configuration-space abort and records the address of the first error only
:src:`sgi-mace-2.0-spec p. 162`; the model never sets bit 20.

Linux masks master-abort interrupts around each configuration read, then
clears the flag by writing zero to bit 31 (``ops-mace.c``), *software
expectation*.

Devices exist at PCI device numbers 1, 2 and 3 only. The captures show
the two onboard SCSI controllers at devices 1 and 2
(:src:`dmesgd-2432 ahc0`, :src:`dmesgd-5290 ahc0`), *observed*, and Linux
routes their interrupts for devices 1 and 2 (``irq_tab_mace`` in
``fixup-ip32.c``). The ``o2`` machine
fits an AIC-7880 at each (:doc:`aic7880`).

The model refuses some configuration writes with ``LOG_UNIMP`` and a bus
error. It refuses any write to a base address register or the expansion
ROM address of any device except the first two base address registers of
an onboard AIC-7880. It refuses a COMMAND write that enables memory,
I/O or bus-master access on any device but an onboard AIC-7880. On an
AIC-7880 it refuses COMMAND bits other than I/O enable, memory enable,
bus master and parity error response. These are model limits, not
hardware behaviour, *approximation*.

Error flags
-----------

The bit table gives the flags a value of ``0x6`` at reset (fast
back-to-back capable, DEVSEL medium), while the summary above it gives
0 :src:`sgi-mace-2.0-spec p. 162-163`; the two conflict, *unknown*.
Linux and NetBSD place DEVSEL timing elsewhere. The model resets the
register to 0 and logs ``LOG_UNIMP`` on each read of it, because the
fixed capability bits are not set.

Bits 23 to 31 and bit 4 clear when written as zero and stay when written
as one; clearing bits 28-31 clears the matching address-valid flag in
bits 19-16 :src:`sgi-mace-2.0-spec p. 162`, *established*. The model
implements only bit 31 and its address flag (bit 19). Any write with
bit 31 clear clears both. The other flags are never set, and their
clear behaviour is absent: the interrupt-test bit (25), retry,
parity, target-abort and system-error flags and the read buffer overrun
flag. Linux clears the flags by writing zero at start-up
(``pci-ip32.c``), a *software expectation* the model meets.

Control register
----------------

.. list-table::
   :header-rows: 1

   * - Bits
     - Function :src:`sgi-mace-2.0-spec p. 164-165`
   * - 7:0
     - Interrupt enable for PCI inputs 0-7
   * - 8
     - SERR_N enable
   * - 9
     - Revision 1: arbiter level for REQ_N[6]
   * - 10
     - Parity error response
   * - 11
     - Memory read multiple read-ahead
   * - 14:12
     - Arbiter level for REQ_N[3], REQ_N[4], REQ_N[5]
   * - 15
     - Park on last in use (1) or on host (0)
   * - 23:16
     - Invalidate read buffers on PCI inputs 0-7
   * - 31:24
     - Error interrupt enables; bit 31 is master abort

All bits reset to 0, *established*. Linux writes ``0xff008500`` in
``mace_init`` (``pci-ip32.c``): SERR enable (8), parity response (10),
park on last in use (15) and all eight error interrupt enables (31:24).
The value leaves the eight PCI input enables (7:0) clear, *software
expectation*.
Linux enables each input as a driver claims it, so the value above is its
start-up value, not a final one.

The model accepts a write only if every set bit is in ``0xff0085ff``:
bits 0-7, 8, 10, 15 and 24-31. That includes the Linux value. A write
with any other bit set (9, 11-14 or 16-23) logs ``LOG_UNIMP`` and gets a
bus error, though the specification marks them read/write, an
*approximation*. Parking and read-ahead have no effect in the model.

Interrupts
----------

The bridge has eight external interrupt inputs
:src:`sgi-mace-2.0-spec p. 166`, *established*: input 0 and 1 the SCSI
controller (INTA, INTB), 2 to 4 the INTA of the three expansion slots, and
5 to 7 the INTB to INTD of the slots in a spiral pattern. The table lists
input 1 as the SCSI controller's INTB, but the text above it calls the five
dedicated inputs the INTA of devices 1 to 5, and Linux
(``irq_tab_mace`` in ``fixup-ip32.c``) gives the second SCSI controller
its own input; the model follows the text. Each input is
gated by its control-register enable and reaches CRIME slots 8 to 15
:src:`sgi-mace-2.0-spec p. 179`.

The model wires input 0 to INTA of device 1, input 1 to INTA of device 2
and input 2 to INTA of device 3. All other pins and devices have no
input; a device that raises one gets a ``LOG_UNIMP`` message. Inputs 3 to
7 are absent, an *approximation*. The master-abort flag (bit 31) raises
CRIME slot 7 while the control enable (bit 31) is set. The other error
interrupts are absent.

CPU view of PCI
---------------

The CPU sees the first 32 MiB of PCI memory and I/O in KSEG1 windows and
larger windows above them: a 2 GiB PCI memory window at
``0x2_8000_0000`` and I/O at ``0x1_0000_0000``
:src:`sgi-mace-2.0-spec p. 160`, *established*. Linux
uses ``0x1a000000`` for low memory, ``0x18000000`` for low I/O and
``0x2_8000_0000`` for high memory (``mace.h``, ``pci-ip32.c``), *software
expectation*.

The model maps two windows: a 16 MiB low window at ``0x1a000000`` and a
2 GiB high window at ``0x2_8000_0000``. Both start at PCI address
``0x80000000``. They accept only 1-byte and 4-byte accesses, big-endian
with a byte-lane swap for 1-byte accesses, and fail other widths with a
bus error. The specification draws the low window as 32 MiB. The PCI I/O
windows at ``0x18000000`` and ``0x1_0000_0000`` are absent. Both limits
are each an *approximation*.

Bus-master views
----------------

A PCI master sees the lower 2 GiB of PCI memory as host memory: the low
1 GiB is a byte-swapped view and ``0x40000000`` to ``0x7fffffff`` is a
native view of the same memory :src:`sgi-mace-2.0-spec p. 157, 159`,
*established*. Linux gives PCI devices bus addresses in the low 1 GiB
view (``phys_to_dma`` in ``ip32-dma.c`` masks with ``0x3fffffff`` and adds
``0x40000000`` only for devices that are not on PCI), *software
expectation*.

The model reverses the roles, as an *approximation*. The view at
``0x40000000`` is the one that swaps bytes: with the machine's
``pci-native-word-swap`` option, which the ``o2`` machine turns on, an
access of *n* bytes (*n* of 1, 2 or 4) goes to the address xor ``4 - n``
within the word. The low 1 GiB view is an unswapped alias of the same
memory. Without the option the view at ``0x40000000`` is an unswapped
alias and the low view is refused with a bus error. The byte placement of
short and unaligned transfers is *unknown*; real DMA from a PCI device
on hardware would settle it. A bus-master access below 2 GiB that misses
RAM is refused with ``LOG_UNIMP`` and a bus error.

The bridge's read-ahead buffers (128-byte virtual lines, filled by
memory-read-multiple when control bit 11 is set) are not modelled; reads
go straight to memory, an *approximation* with no guest-visible effect
other than timing.

.. _sgi-mace-peripheral:

Peripheral controller
=====================

All registers of the peripheral controller are 64 bits wide and defined
on 64-bit boundaries :src:`sgi-mace-2.0-spec p. 122`, *established*. Each
functional block starts on a 16 KiB boundary and unused space inside it is
aliased to existing registers. The model decodes the registers below and
no aliases of them. Any other access, or one of a different width,
logs ``LOG_UNIMP``, a read returning 0. The exception is the
miscellaneous control register, where another width gets a bus error
with no log.

.. list-table::
   :header-rows: 1

   * - Offset
     - Name
     - Reset
     - Access
   * - ``0x310000``
     - Ring base and reset
     - ``0x1``
     - R/W
   * - ``0x310008``
     - Miscellaneous control
     - ``0x20``
     - R/W
   * - ``0x310010``
     - Interrupt status
     - 0
     - Mostly R
   * - ``0x310018``
     - Interrupt mask
     - 0
     - R/W
   * - ``0x318000``, ``0x31c000``
     - Serial DMA for ports 1 and 2
     - see `Serial ports`_
     - R/W

Ring base and external reset
----------------------------

Bit 0 (reset 1) holds the external Super I/O part and the other ISA
devices in reset. Bits 31:15 are the 32 KiB-aligned base of the eight
4 KiB ring buffers of the peripheral controller: audio input, audio
outputs 1 and 2, a scratch page and the four serial rings. Bits 14:1
read zero :src:`sgi-mace-2.0-spec p. 123`, *established*.

In the model the register holds ``0xffff8001`` and accepts those bits.
A 0-to-1 transition of bit 0 resets both UARTs. While bit 0 is set, the
UARTs discard received characters, and register accesses to them log
``LOG_GUEST_ERROR``, return 0 and drop writes. The same bit drives the
``isa-reset`` output. A change of bits 31:15 is refused with
``LOG_GUEST_ERROR`` unless the three audio channels and the four serial
channels are all held in reset. The specification does not say when the
base may change, an *approximation*.

Miscellaneous control
---------------------

.. list-table::
   :header-rows: 1

   * - Bit
     - Reset
     - Function :src:`sgi-mace-2.0-spec p. 124`
     - Model
   * - 0
     - 0
     - Flash write enable
     - R/W; drives the output ``flash-write-enable``
   * - 1
     - 0
     - External password-clear input (R)
     - Read-only; set by the ``password-clear-input`` property
   * - 2
     - 0
     - NIC line: 0 holds the line low, 1 releases it
     - R/W; see `Identity line`_
   * - 3
     - 0
     - Current value of the NIC line (R)
     - Read-only
   * - 4
     - 0
     - Red LED: 0 enabled, 1 disabled
     - R/W
   * - 5
     - 1
     - Green LED: 0 enabled, 1 disabled
     - R/W
   * - 6-8
     - 0
     - DP-RAM write enable, UST test, game-port select
     - Writing a one logs ``LOG_UNIMP``; reads zero

Linux writes the LED bits as active low: bit 4 and 5 at 0 light the LED
(``mace.h``), a *software expectation* that agrees with the
specification. NetBSD ``macereg.h`` comments the opposite polarity, and
the two do not both fit the specification. The model stores the bits as
written. The model accepts 64-bit accesses only, unless the
``misc-word-pio`` option is set (`Identity line`_).

Linux names bit 1 "PWD CLEAR jumper detected" (``mace.h``), a *software
expectation* that a jumper exists. Whether the password-clear input is fitted,
and its value, is *unknown* for the O2 board. The machine requires the property
to be given when it loads a boot ROM image.

Interrupt status and mask
-------------------------

Status and mask are 32-bit registers :src:`sgi-mace-2.0-spec p. 125`,
*established*. A source's status bit shows its level, and a source
contributes to CRIME when its status and mask bits are both set. The
bits:

.. list-table::
   :header-rows: 1

   * - Bits
     - Source
     - Type in the specification
   * - 0-7
     - Audio: codec status, volume, input threshold, input FIFO
       overflow, output 1 threshold, output 1 memory error, output 2
       threshold, output 2 memory error
     - R
   * - 8
     - Real-time clock
     - R
   * - 9, 11
     - PS/2 keyboard, mouse interrupt
     - R
   * - 10, 12
     - PS/2 keyboard, mouse poll flag (unmasked)
     - R
   * - 13-15
     - Compare timers 1-3
     - R
   * - 16-19
     - Parallel port: device, DMA context A, context B, memory error
     - R (bit 16 R/W)
   * - 20, 26
     - Serial port 1, 2 device interrupt
     - R
   * - 21, 27
     - Serial TX DMA threshold
     - R
   * - 22, 28
     - Serial TX DMA pair request
     - R/W
   * - 23, 29
     - Serial TX DMA memory error
     - R
   * - 24, 30
     - Serial RX DMA threshold
     - R
   * - 25, 31
     - Serial RX DMA overrun
     - R

The bit order agrees with Linux ``MACEISA_*`` names in ``mace.h``
(*software expectation*), and the NetBSD interrupt masks for the parallel
port, ``0xf0000``, and the serial ports, ``0x3f00000`` and ``0xfc000000``
(:src:`dmesgd-5290 mace0`), *observed*. In the model, bits 16-19 are never
set. A write to the status register clears serial pair-request bits
22 and 28 where it writes zero. A write of ones in bits 13-15 logs
``LOG_GUEST_ERROR`` because compare timers clear through their compare
registers. A write in bits 0-12 or 16-19 logs ``LOG_UNIMP`` and has no
effect. The specification does not say how the read-only bits clear,
*unknown*; the sources of the audio, PS/2 and serial bits clear them.

The poll flags are "unmasked non-interrupt" versions of the PS/2
interrupts. The model sets a poll flag whenever the port's transmit buffer
is empty or its receive buffer is full, whatever the port's interrupt
enables are. A port at idle therefore shows its poll flag set. The
specification's wording does not fix this, an *approximation*.

Timers and UST
==============

The block has one 32-bit counter that increments every 960 ns and three
32-bit compare registers :src:`sgi-mace-2.0-spec p. 147`,
*established*. The period is the PCI clock divided by 32
(:src:`sgi-mace-2.0-spec p. 148`) and Linux uses 960 ns for it
(``MACE_UST_PERIOD_NS`` in ``mace.h``), *software expectation*. The
counter and the compare latches reset to zero. A compare register sets its
interrupt latch when the counter equals the value written, and writing the
register clears the latch :src:`sgi-mace-2.0-spec p. 147`. Software adds a
delta to the current count and truncates the sum to 32 bits.

.. list-table::
   :header-rows: 1

   * - Offset
     - Register
     - Model
   * - ``0x340000``
     - UST counter
     - Read returns the low 32 bits of the elapsed virtual time divided by
       960 ns since reset. A write logs ``LOG_UNIMP``.
   * - ``0x340008``, ``0x340010``, ``0x340018``
     - Compare 1-3, aliased at ``0x350000``, ``0x360000`` and
       ``0x370000`` for each 8-byte offset
     - 64-bit access. A write latches the value, clears the timer's status
       bit and arms the timer for the next time UST reaches it.
   * - ``0x340020`` to ``0x340037``
     - Audio input, output 1 and output 2 timestamp pairs
     - See `Audio interface`_.

The specification calls the counter read-only in its text and read/write in its
register table :src:`sgi-mace-2.0-spec p. 147-148`; the model makes it
read-only, an *approximation*. It does not give the reset value of a compare
register, *unknown*; the model reads 0 and logs ``LOG_UNIMP`` if the register
was never written. The specification does not say whether a compare value equal
to the current count matches at once or only after 2^32 ticks, *unknown*; the
model arms nothing and logs ``LOG_UNIMP``. The model's UST starts at reset. The
register table lists timestamp offsets for video inputs and the video output at
``0x340038`` to ``0x34004f``, which are absent.

Serial ports
============

The two ports are 16C550-compatible UARTs in an external Super I/O part,
which also holds the parallel port
:src:`sgi-mace-2.0-spec p. 126`, *established*. Both captures report
``ns16550a``, with a 16-byte FIFO in the OpenBSD one
(:src:`dmesgd-2432 com0`, :src:`dmesgd-5290 com0`), *observed*. Registers
are bytes at a 256-byte stride: register *r* of port 1 is at
``0x390000 + 0x100 * r``, and port 2 at ``0x398000`` the same
:src:`sgi-mace-2.0-spec p. 133`. Linux uses the same stride and a
1.8432 MHz UART clock (``ip32-platform.c``) and OpenBSD the same clock,
*software expectation*. The specification describes a 22 MHz clock input
with a 6-bit prescaler :src:`sgi-mace-2.0-spec p. 126`, which does not
match; which one is the real clock tree is *unknown*.

The model uses QEMU's 16550A with a 16-byte FIFO and a base clock of
115200 baud, which is a 1.8432 MHz clock, as an *approximation*. Port 1
connects to the machine's first ``-serial`` backend through the
``chardev-a`` property, and port 2 to the second through ``chardev-b``.
The register test profile accepts one host serial backend only. Interrupts set status bits 20 and
26. The specification's extra register at the scratch address when
DLAB is set (the IR encoder select and clock prescaler) is absent: access
logs ``LOG_UNIMP``, reads 0 and drops writes.

Serial DMA
----------

Each port has an optional pair of DMA channels, a transmit and a receive
ring, controlled by eight registers at ``0x318000`` (port 1) and
``0x31c000`` (port 2) :src:`sgi-mace-2.0-spec p. 122`, *established*.
With DMA disabled the port behaves as a plain 16550 and software must not
read the DMA-owned UART registers while DMA is active
:src:`sgi-mace-2.0-spec p. 129`.

.. list-table::
   :header-rows: 1

   * - Offset
     - Name
     - Notes
   * - ``0x00``, ``0x20``
     - TX, RX control
     - Reset ``0x400``; writable ``0x6e0``.
   * - ``0x08``, ``0x28``
     - TX, RX read pointer
     - Bits 11:5.
   * - ``0x10``, ``0x30``
     - TX, RX write pointer
     - Bits 11:5.
   * - ``0x18``, ``0x38``
     - TX, RX ring depth
     - Read only: write minus read, in 32-byte blocks.

The control register has reset in bit 10 (set at reset), DMA enable in
bit 9 and an interrupt threshold in bits 7:5 :src:`sgi-mace-2.0-spec
p. 127`. The threshold values are: 1-3 interrupt at 25, 50 or 75 percent
full for input and below those fill levels for output, 4 empty, 5 not
empty, 6 full, 7 not full, 0 off. A write with bit 10 set resets the
channel and its pointers. The ring holds 4 KiB, 128 blocks, and is full at
127 blocks. The hardware owns one pointer, the read pointer of a transmit
ring and the write pointer of a receive ring, and software the other.

A ring's address is the ring-base register's bits 31:15, the ring ID and
the pointer :src:`sgi-mace-2.0-spec p. 128`. IDs 4 to 7 are the serial
rings: port 1 transmit, port 1 receive, port 2 transmit and port 2 receive.
The ring base register is shared with the audio channels.

The DMA engine moves 64-bit chunks of four control bytes and four data
bytes, big-endian, four pairs per chunk (:src:`sgi-mace-2.0-spec
p. 129-130`). A receive pair has bit 7 valid, bits 3:0 break, framing,
parity and overrun; an invalid pair is control 0 and data ``0xff``. A
transmit pair has command in bits 7:6 (0 invalid, 1 write the transmit
holding register, 2 write the modem control register, 3 delay for *N*
milliseconds) and bit 5 posts an interrupt after the pair. A
modem-status interrupt halts DMA until software handles it.

The model implements the transfers as the specification describes them,
with these particulars:

- The model faults a channel if a ring address is not RAM: it logs
  ``LOG_GUEST_ERROR``, stops the channel and, for transmit, sets the
  memory-error status bit. A receive channel has no memory-error status
  bit in the specification and gets none in the model.
- A receive overrun stops DMA and raises the overrun bit.
- Software writes to a hardware-owned pointer or to a depth register log
  ``LOG_GUEST_ERROR`` and are ignored. A pointer write that advances the
  software-owned pointer past the ring's capacity is ignored.
- With ``crime-profile=legacy-software`` on the machine, a ring address
  below ``0x40000000`` is read as a RAM offset and ``0x40000000`` is added
  to it, an *approximation* with no source.

The channels' view of memory is CRIME's MACE address space: RAM at
``0x40000000`` and a no-ECC alias of it at ``0x80000000``, nothing below
or above :src:`sgi-crime-1.5-spec p. 50`, *established*.

Parallel port
=============

The Super I/O part carries an EPP/ECP parallel port at ``0x380000`` and
``0x388000``, with DMA registers in the peripheral controller
:src:`sgi-mace-2.0-spec p. 122, 133`, *established*. NetBSD attaches
``lpt0`` at offset ``0x380000`` on a real O2 (:src:`dmesgd-5290 lpt0`),
*observed*. The model has no parallel port: accesses to the range log
``LOG_UNIMP`` and read 0, and the ``o2`` machine fits no parallel device
(``no_parallel`` in ``o2.c`` only drops the default parallel port; an
explicit ``-parallel`` is not refused by this machine).

PS/2 ports
==========

Two PS/2 ports carry the keyboard and the mouse; either could be either
device. Each is an 11-bit odd-parity serial port
:src:`sgi-mace-2.0-spec p. 139`, *established*. The registers are 64 bit,
the keyboard at ``0x320000`` and the mouse at ``0x320020``
:src:`sgi-mace-2.0-spec p. 140, 181`.
Both captures report a PS/2 controller at ``0x320000`` interrupt 5
(:src:`dmesgd-2432 mkbc0`, :src:`dmesgd-5290 macekbc0`), *observed*.

.. list-table::
   :header-rows: 1

   * - Offset
     - Register
     - Notes
   * - ``0x00``
     - Transmit buffer (W)
     - Bits 7:0.
   * - ``0x08``
     - Receive buffer (R)
     - Bits 7:0 data, bits 15:8 alias the status register.
   * - ``0x10``
     - Control (R/W)
     - Bits 5:0, reset ``0x10``.
   * - ``0x18``
     - Status (R)
     - Bits 7:0.

Control bits :src:`sgi-mace-2.0-spec p. 141`: 0 inhibit the clock after
transmitting, 1 transmit enable, 2 transmit interrupt enable, 3 receive
interrupt enable, 4 clock enable (reset 1), 5 reset. Status bits
:src:`sgi-mace-2.0-spec p. 142`: 0 external clock, 1 clock enable
output, 2 transmitting, 3 transmit buffer empty, 4 receive buffer full,
5 receiving, 6 parity error, 7 framing error. Linux uses the same bit
positions (``maceps2.c``), *software expectation*. The reset table
gives transmit buffer empty (bit 3) a reset of 0, which is odd for an
empty buffer, *unknown*.

The model is a byte-atomic *approximation*. It has no clock edges and no
shift timing. A transmit write sets the transmit-buffer-full state until
the control register allows the byte out (transmit enable set, clock
enabled, not inhibited, reset clear). The byte goes straight to QEMU's PS/2
keyboard or mouse model, which is the port's peripheral and takes host
input. A byte from the peripheral arrives only when the clock is enabled,
the receive buffer is empty and no byte is waiting to transmit. Every
received frame carries correct odd parity and no framing error. After
reset the status reads ``0x0b``: clock enabled, clock output enabled and
transmit buffer empty. Interrupt output is the transmit-empty state with
the transmit enable bit, or receive-full with the receive enable bit.

The model accepts 64-bit aligned accesses only and logs
``LOG_GUEST_ERROR`` for others and for writes to read-only registers. A
transmit write while a byte is pending, or while reset is set, is
refused the same way. Control bits 6 and 7 are reserved and a write with
them set logs ``LOG_GUEST_ERROR``.

Real-time clock
===============

The ISA interface carries a Dallas DS1687 calendar clock in a 256-byte register
range with an alarm output :src:`sgi-mace-2.0-spec p. 121, 133`, *established*.
Linux maps it at the clock offset with a 256-byte register step
(``ip32-platform.c``), *software expectation*, and OpenBSD attaches a DS1687 at
``0x3a0000`` (:src:`dmesgd-2432 dsrtc0`), *observed*. The model instantiates
the DS1685 model (:doc:`ds1685`), which handles the 256-byte stride of this
window (:ref:`sgi-ds1685-direct`). The clock's interrupt output sets peripheral
status bit 8. The ``drive`` and ``clock-drive`` properties of ``sgi-mace`` pass
through to the clock's backing stores.

Audio interface
===============

MACE carries a serial TDM interface for one AD1843 stereo codec and DMA
channels for one stereo input and two stereo outputs. The codec supplies
the sample clocks :src:`sgi-mace-2.0-spec p. 61`, *established*. This
section covers the MACE side; the codec is on :doc:`ad1843`. The audio
device is at ``0x300000`` in both captures (:src:`dmesgd-2432 mavb0`,
:src:`dmesgd-5290 mavb0`), *observed*, with an AD1843 revision 1 and
interrupt 6 in the OpenBSD report.

The registers are at ``0x300000`` and 64-bit aligned
:src:`sgi-mace-2.0-spec p. 62`. The model repeats them every ``0x80``
bytes through the 64 KiB block, an *approximation*. It also accepts
32-bit accesses at the same addresses: a read of the word at offset 4
returns the register's low half and a read of the word at offset 0
returns zero. A write to the word at offset 0 is dropped. The
specification defines 64-bit access only, so this is an *approximation*.
Other widths and misaligned accesses log ``LOG_GUEST_ERROR``.

.. list-table::
   :header-rows: 1

   * - Offset
     - Register
     - Reset
     - Notes
   * - ``0x00``
     - Control and status
     - ``0x1``
     - Bit 0 codec reset; bit 1 codec present (R); bits 8:2, 15:9 and
       22:16 alias the input write pointer and the two output read
       pointers; bits 24:23 latch the volume inputs, cleared by writing
       zero.
   * - ``0x08``
     - Codec command
     - ``0x10000``
     - Bits 23:17 codec register, 16 read or write, 15:0 data. Sent
       every frame.
   * - ``0x10``
     - Codec status mask
     - 0
     - Bits 15:0.
   * - ``0x18``
     - Codec status word
     - 0
     - Read only; the last status word from the codec.
   * - ``0x20``, ``0x40``, ``0x60``
     - Input, output 1, output 2 ring control
     - ``0x400``
     - Writable ``0x6e0``. Same fields as the serial channels.
   * - ``0x28``, ``0x48``, ``0x68``
     - Read pointers
     - 0
     - Bits 11:5.
   * - ``0x30``, ``0x50``, ``0x70``
     - Write pointers
     - 0
     - Bits 11:5.
   * - ``0x38``, ``0x58``, ``0x78``
     - Ring depth
     - 0
     - Read only.

The table is :src:`sgi-mace-2.0-spec p. 62-64`, *established*, with the
reset values the model uses. The specification gives the command
register's reset as read operation (bit 16 set) and the codec reset bit
as 1. The specification defines a 7-bit codec register address, the AD1843
has 5 bits; the model accepts the field and rejects an address of 32 or
more with ``LOG_UNIMP``. Interrupt cause 0, status word AND mask not
zero, is raised on peripheral status bit 0 :src:`sgi-mace-2.0-spec
p. 63, 125`.

The three rings are 4 KiB and sit in the first three pages of the shared
32 KiB ring block: ring IDs 0 for input, 1 for output 1 and 2 for output
2 :src:`sgi-mace-2.0-spec p. 66`. The control register has the same
reset, enable and threshold fields as the serial channels
(:src:`sgi-mace-2.0-spec p. 64`). The thresholds raise audio interrupt
bits 2, 4 and 6; a DMA fault raises bits 3, 5 and 7, which are fatal until
the channel is reset :src:`sgi-mace-2.0-spec p. 66, 125`. The ring base
register (`Ring base and external reset`_) shares the address space of the
serial channels.

DMA items are 64 bits, four per transaction, with a left word in the high
32 bits and a right word in the low :src:`sgi-mace-2.0-spec p. 65`. An
input item carries each 16-bit sample sign-extended in the high bits of
its word with ``0xff`` in the lowest byte. An output item uses bits
23:8 of each word. Output saturates to ``0xffff`` if bit 31 differs from
any of bits 30:24 :src:`sgi-mace-2.0-spec p. 65`. When the ring has no
data, the interface sends zero samples to the codec.

The model reads a ring entry as one 32-byte block into RAM through the
serial DMA address space and treats a block that is not whole RAM as a DMA
fault.

Timestamps and sample counters
------------------------------

Each channel has a 64-bit register at ``0x340020`` (input), ``0x340028``
(output 1) and ``0x340030`` (output 2) holding a sample-pair counter
(MSC) and the UST value snapped when the counter last changed
:src:`sgi-mace-2.0-spec p. 63, 148`, *established*. MSC counts every
sample pair the codec delivers or requests, even when the ring is full or
empty, except while the channel is in reset. Software may write the
register to zero MSC.

The register table puts UST at the lower offset and MSC 4 bytes above
it, and the paired-read note describes UST in bits 63:32; the note's
prose names the registers the other way round. Linux ``struct timer_reg``
has the UST word first and then the MSC word (``mace.h``), a *software
expectation*. The model returns UST in the high word and MSC in the low
word of a 64-bit read. A write to either half sets that half, so both
the counter and the stamp can be set.

Host audio
----------

The model plays output 1 on the codec's DAC1 line output as 48 kHz
signed 16-bit stereo through one host output voice and takes a single host
input voice at the same format into the codec's AUX1 input. Other codec
outputs are modelled internally and are not played, an *approximation*.
``sgi-mace`` has the standard ``audiodev`` property; the ``o2`` machine
does not set it, so without a backend the codec runs against virtual time
and produces no host sound. The GPIOs ``volume`` (two inputs),
``audio-pwrdwn`` and ``audio-pdmnft`` (inputs) and ``audio-ttl`` (two
outputs) carry the codec's pins to fixtures and the machine.

Identity line
=============

Bit 2 of the miscellaneous control register releases or holds low the
bidirectional line to a one-wire part, and bit 3 reads it
:src:`sgi-mace-2.0-spec p. 124`, *established*. The specification calls
the part a "number in a can" and says the system serial number and
Ethernet address live in the clock module's silicon serial number
:src:`sgi-mace-2.0-spec p. 121`. NetBSD's ``if_mec.c`` notes that on some
machines the DS2502 holding the Ethernet address is on the PCI riser
board, *software expectation*. Whether an O2 board
fits one-wire part on this line is *unknown*.

By default the model has no part on the line: bit 3 reads 0 and the
release bit is only stored. With ``identity-diagnostic=on`` it fits a DS2502
(family code ``0x09``, 48-bit serial, CRC-8 with polynomial
x^8 + x^5 + x^4 + 1) with a 128-byte image from ``identity-eprom-file``, a
serial number from ``identity-test-serial`` and a 7-byte status from
``identity-status``, *established* for the part
(:src:`maxim-ds2502-datasheet p. 3, 18`), an *approximation* for the
board. The model decodes the host's pulse widths in virtual time: a low
of 480 µs up to under 960 µs after a high of at least as long resets the
part; the model answers with presence from 30 µs to 150 µs later; a low of
60 µs to under 120 µs writes a zero; a low of 1 µs to under 24 µs writes
or reads a one. The datasheet's limit for a one is 15 µs; the model widens
it. The model holds the line low from release to the end of a 60 µs slot to
read a zero. After a reset of the device, the first high after a low of at
least 480 µs, of any length, also starts a session. This is a model
choice. The option makes the machine refuse migration and snapshots.

With ``misc-word-pio=on`` (and the identity diagnostic on) the miscellaneous
control register also answers 32-bit accesses: the word at ``0x310008`` is
the high half, always zero, and the word at ``0x31000c`` is the register.
This is an *approximation*.

Reset and migration
===================

Reset sets the ring base to ``0x1``, the interrupt status and mask to 0,
the timers' counter epoch to the current virtual time and the compare latches
to unprogrammed, resets the PCI registers, serial DMA, audio, PS/2 ports and
identity line, holds both UARTs in reset and drives all sixteen CRIME inputs
low. The model resets the miscellaneous control register to ``0x20`` (green
LED off).

``sgi-mace`` migrates as version 1, with the serial DMA, audio and UART
state embedded. The Ethernet MAC, PS/2 ports, identity line and clock are
child devices with their own state. A stream loaded into a different
build is not supported. The identity diagnostic blocks migration.

.. _sgi-mace-i2c:

I2C bus interface
=================

MACE has a two-wire I2C master at peripheral offset ``0x330000``
(the peripheral address map; the register offsets ``0x00`` to ``0x18``
inside the block are in :src:`sgi-mace-2.0-spec p. 149-150`),
*established*. The specification's text says the ASIC contains two I2C
interfaces (p. 149), but its register chapter describes one block of
four registers, which is what the model implements. No machine maps
it: the ``o2`` machine leaves the block absent, and which I2C devices an
O2 board carries is *unknown*. ``sgi-mace`` has a register model of the
master, ``hw/mips/sgi/mace-i2c.c``, which only the qtests can turn on,
with ``-device sgi-mace,i2c-untimed-test-profile=on``. The device refuses
the property outside QTest.

The model has four 64-bit registers: configuration at offsets 0 and 8,
CONTROL at ``0x10`` and DATA at ``0x18``. Only aligned 8-byte accesses
are accepted. Any other width or offset is a bus error with no side
effect. Reading the configuration fails and logs ``LOG_UNIMP`` while a
transfer owns the bus, while the model is busy, while the bus is busy or
when an endpoint cannot do byte-at-once transfers, because the model has
no sampled line state. Otherwise it returns the written bits with both
sampled line inputs (``0x30``) reading released.

CONTROL follows the specification's Table 77 (:src:`sgi-mace-2.0-spec p. 150`),
*established*:

.. list-table::
   :header-rows: 1

   * - Bit
     - Meaning
   * - 0
     - Force idle: a write of 0 forces idle; a read of 1 means not idle.
   * - 1
     - Direction: 1 receive, 0 send.
   * - 2
     - Last byte control (hold): 1 keeps the bus after the byte, 0
       releases it.
   * - 4
     - Transfer busy (read only).
   * - 5
     - Acknowledge not received (read only), the model's NACK status.
   * - 7
     - Bus error (read only), the model's sticky error status.

Bits 3 and 6 are reserved. A configuration write that sets a line
override bit (2 or 3) logs ``LOG_UNIMP``, sets the error status and fails.

The model transfers whole bytes with no wire timing, an
*approximation*:

- A DATA write when the bus is idle sends a START and the address byte,
  and records whether the target acknowledged. Later DATA writes send
  one byte each. A DATA read returns the next received byte, with no
  read-ahead.
- CONTROL's hold bit keeps the bus after a byte. With it clear, the byte
  ends with a STOP. The last byte of a receive is answered with a NACK
  before the STOP.
- An address NACK sets the NACK status. With hold clear the bus is
  released. With hold set the failed selection is kept until software
  ends it. Neither retries the address.
- DATA used against the current direction, or before a target has
  acknowledged, is a protocol error: the model ends the transfer, sets a
  sticky error status, logs ``LOG_GUEST_ERROR`` and returns a bus error.
- Configuration bit 0 requests a local reset of the interface, applied
  at the next byte boundary when a byte is in flight.

The bus behind it, named ``i2c``, takes at most 32 endpoints, added before
the machine starts, and each must declare that it supports byte-at-once
transfers. The only such endpoint is the qtest fixture
``tests/qtest/fixtures/sgi-mace-i2c.c``, which stands for no O2 part.
Migration saves the owned transfer, the pending receive acknowledge and
each endpoint's state, and refuses a stream whose endpoint list differs.

Not modelled: repeated START, clock stretching, arbitration, standard and
fast mode timing, the line override bits, and the sampled line inputs.

.. _sgi-mace-measurements:

Measurements
============

No register of this part has been read on a physical machine for this
project. Published captures are listed under `Identity and placement`_.

.. _sgi-mace-limits:

Model limits
============

- The model has no parallel port, machine I2C interface, video inputs or output,
  PCI interrupt inputs 3 to 7, PCI I/O windows, DP-RAM, game port or UST
  test mode. Accesses log ``LOG_UNIMP`` and read zero.
- The PCI error flags other than master abort, the interrupt-test bit,
  the illegal-transaction flag and the arbiter and read-ahead behaviours
  are absent. The PCI control register refuses writes outside
  ``0xff0085ff`` with a bus error.
- Bus-master DMA by expansion cards is refused. Only the onboard SCSI
  controllers can master the bus, and the low 1 GiB view is an
  approximation (`Bus-master views`_).
- The byte swap of the view at ``0x40000000`` replaces the swap the
  specification puts on the low view.
- Peripheral registers are 64-bit only, except where noted.
- The PS/2 ports have no serial timing and no frame errors.
- The TI Super I/O IR and prescaler register is unsupported. The
  clock-tree of the UARTs is a software view.
- The codec is a functional model; see :doc:`ad1843`.
- Migration works only between identical builds.

Tests
=====

- ``tests/qtest/sgi-mace-test.c`` covers the compare timers (mask,
  acknowledgement, late callbacks, overshoot, migration), the external
  reset, the clock window stride, UART register spacing, interrupt routes
  and receive backends, and the PCI host bridge: reset, configuration lane
  swap, refused BAR and ROM writes, absent devices, control bits, flush,
  master abort, interrupt inputs, expansion pins and migration.
- ``tests/qtest/sgi-mace-ps2-test.c`` covers reset aliases, pending
  transmit, access widths, keyboard and mouse input, clock inhibit,
  interrupt and poll outputs, migration and the board route.
- ``tests/qtest/sgi-mace-serial-dma-test.c`` covers the ring address
  classes and faults, registers, base validation, transmit and receive
  thresholds, packet counts, delay, modem pause, errors, backpressure,
  access widths and migration.
- ``tests/qtest/sgi-mace-audio-test.c`` covers the controller registers,
  the codec model, power timing, thresholds, DMA formats, faults and wrap,
  timestamps, mixing and migration.
- ``tests/qtest/sgi-mace-i2c-test.c`` covers the I2C register slots,
  receive, transmit, NACK, errors, reset, force-idle, an empty bus,
  migration and unrealize.
- ``tests/qtest/sgi-mace-dma-test.c`` covers the bus-master address views
  on 64 MiB, 128 MiB and 1 GiB RAM layouts.
- ``tests/qtest/sgi-mace-id-test.c``, ``tests/qtest/sgi-o2-id-test.c`` and
  ``tests/qtest/sgi-o2-platform-inputs-test.c`` cover the identity line
  and its timing, the password-clear input, the 32-bit miscellaneous
  control access, the clock alarm route and clock storage.
