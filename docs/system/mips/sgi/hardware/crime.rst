.. SPDX-License-Identifier: BSD-3-Clause

.. _sgi-crime:

=============================================
CRIME: memory controller and rendering engine
=============================================

Used in :ref:`sgi-ip32` (O2).
CRIME (CPU, Rendering, Interconnect and Memory Engine) is the O2 system
ASIC. It joins the processor to main memory, collects the interrupts of
MACE, GBE, VICE and its own units, keeps a timer and a watchdog, and
contains the rendering engine that draws into main memory
(:src:`sgi-crime-1.5-spec PDF p. 13`; interrupts on PDF p. 106, timers
on PDF p. 112-119, address map on PDF p. 47-52). QEMU models it as
``sgi-crime`` in ``hw/mips/sgi/crime.c``, with state in
``include/hw/mips/sgi/crime.h``. The O2 test profile (:doc:`../ip32`),
a synthetic register test profile, fits it.

The model follows the CRIME 1.5 specification. A second profile,
``legacy-software``, presents the identity published O2 captures show
and accepts a few more register writes. Neither is a revision-matched
description of a shipped chip: which CRIME revision each O2 carries is
*unknown*. The rendering engine is a small functional subset. The ECC
logic, the watchdog, VICE and the error detectors are not modelled.

Facts are rated as described in :ref:`sgi-ratings`.

.. _sgi-crime-sources:

Sources
=======

- The *CRIME 1.5 Specification*, revision 0.3, 2 June 1997,
  :src:`sgi-crime-1.5-spec`. Citations give the PDF page. The printed
  page is the chapter-page number at the foot of each sheet, for example
  PDF p. 114 is printed 5-26. The specification describes CRIME 1.5, not
  1.1, and is marked proprietary and confidential in its own footer.
- Three published captures: an OpenBSD 5.2 boot report of an O2 with a
  195 MHz R10000 (:src:`dmesgd-2432 line 150`), a NetBSD 9.99.31 boot
  report of an O2 with an R12000 (:src:`dmesgd-5290 line 167`) and a
  published hardware inventory (:src:`bukosek-sgi-o2`).
- The *O2 Technical Report*, :src:`o2-technical-report`, for the memory
  sizes the board supports (32 to 256 MB with 16 Mbit SDRAMs, 128 MB to
  1 GB with 64 Mbit SDRAMs).
- Linux ``arch/mips/include/asm/ip32/crime.h``,
  ``arch/mips/sgi-ip32/crime.c``, ``ip32-memory.c`` and ``ip32-irq.c``
  at ``db2ddb871435``, as software expectations.
- NetBSD ``sys/arch/sgimips/dev/crime.c``, ``crimereg.h`` and
  ``crmfbreg.h`` at ``07004b2a9dac``, as software expectations.
- OpenBSD ``sys/arch/sgi/localbus/macebus.c`` at ``2df91d80c104``, as a
  software expectation for how the revision is printed.

Where the specification, the drivers and the model disagree, the page
says so under the register it concerns.

.. _sgi-crime-identity:

Identity and placement
======================

The ID and revision register has the ID value in bits 7:4, reset ``0xb``,
and the revision in bits 3:0, reset 1, so CRIME 1.5 reads ``0xb1``
(:src:`sgi-crime-1.5-spec PDF p. 114`), *established*. The same page
notes that the earlier Petty CRIME had ID ``0xa`` revision 0 and
CRIME 1.1 had ID ``0xa`` revision 1.

Published O2s read differently, *observed*:

- The OpenBSD report prints ``crime rev 10.1``. OpenBSD prints the ID
  nibble and the revision in decimal (``macebus.c`` at ``2df91d80c104``),
  so the low byte is ``0xa1``, *inferred*. The capture is from an older
  OpenBSD release than the source cited, so the print format is assumed
  unchanged.
- The NetBSD report prints ``rev 1.4 (CRIME_ID: 17592186044577)``. That
  value is ``0x1000000000a1``. NetBSD's ``crime.c`` reads all 64 bits,
  takes ID ``0xa`` and prints 1.1, 1.3 or 1.4 from the upper word
  (zero, one or any other value). The same file prints "rev 1.5" for ID
  ``0xb``.
- Linux defines the ID nibble as ``0xa`` and revisions 0x00, 0x11, 0x13
  and 0x14, and has no definition for ID ``0xb``.

No public capture shows an O2 reading ``0xb1``. The specification's
1.5 value and NetBSD's decode of it say nothing about shipped units.

The registers occupy 32 MiB at physical ``0x14000000``
(:src:`sgi-crime-1.5-spec PDF p. 48`), *established*; Linux and NetBSD
use the same base. The model's register window is that 32 MiB, with
the rendering engine at ``0x15000000``. CRIME answers these windows of
the CPU address map (:src:`sgi-crime-1.5-spec PDF p. 47-48`),
*established*:

.. list-table::
   :header-rows: 1

   * - Address
     - Size
     - Window
     - In the model
   * - ``0x00000000``
     - 256 MB
     - Low memory
     - RAM, up to its size (see :ref:`sgi-crime-memory`)
   * - ``0x10000000``
     - 32 MB
     - Frame buffer, linear access
     - not modelled
   * - ``0x12000000``
     - 32 MB
     - Depth buffer, linear access
     - not modelled
   * - ``0x14000000``
     - 32 MB
     - CRIME registers
     - modelled
   * - ``0x20000000``
     - 512 MB
     - High memory alias
     - eight-bank profile only
   * - ``0x40000000``
     - 1 GB
     - Full memory
     - RAM, up to its size
   * - ``0x80000000``
     - 1 GB
     - Memory without ECC checking
     - RAM, up to its size

The specification's table marks the responder of the high memory
alias with a question mark. Its text says the high 512 MB of memory is aliased into the
upper 512 MB of the first gigabyte, and that a memory location must be
reached through only one of the low alias, the high alias or the full
region, or the caches become incoherent
(:src:`sgi-crime-1.5-spec PDF p. 47`). The GBE registers at
``0x16000000`` and the MACE windows belong to other chips
(:doc:`gbe`, :doc:`mace`).

.. _sgi-crime-registers:

Register map
============

The CPU interface and memory controller registers take doubleword
accesses only (:src:`sgi-crime-1.5-spec PDF p. 113, 149`),
*established*. NetBSD's header says the same. The model rejects other
widths, with the exceptions listed under :ref:`sgi-crime-limits`. The
model's registers are big-endian.

.. list-table::
   :header-rows: 1

   * - Offset
     - Name
     - Spec reset
     - Model
   * - ``0x000``
     - ID and revision
     - ``0xb1``
     - ``0xb1``; ``0xa1`` in the ``legacy-software`` profile. Read only.
   * - ``0x008``
     - CPU interface control
     - ``0x20`` + bit 8 (inferred)
     - Reads ``0x120`` (``0x100`` legacy). Writes rejected; legacy
       accepts 0 and ``0x100`` as no-ops.
   * - ``0x010``
     - Interrupt status
     - 0
     - Hardware OR software. Read only.
   * - ``0x018``
     - Interrupt enable
     - 0
     - 32 bits, read/write.
   * - ``0x020``
     - Software interrupt
     - 0
     - Bits 29:28 only (28 to 30 legacy).
   * - ``0x028``
     - Hardware interrupt
     - 0
     - Reads the live state. Writing a zero clears a latched cause.
   * - ``0x030``
     - Watchdog
     - 0
     - Reads 0. Not modelled.
   * - ``0x038``
     - Timer
     - 0
     - 32-bit count, read/write.
   * - ``0x040``
     - CPU error address
     - 0
     - Reads 0 (legacy: the seeded value).
   * - ``0x048``
     - CPU error status, CRIME 1.1 position
     - n/a
     - Legacy only. Reads the seeded value; writing 0 clears it.
   * - ``0x4048``
     - CPU error status, CRIME 1.5 position
     - Bits 2:0 0; bits 8:3 undefined
     - Reads 0 in the 1.5 profile only.
   * - ``0x200``
     - Memory control and status
     - 0
     - Reads 0; see :ref:`sgi-crime-ecc`.
   * - ``0x208`` to ``0x240``
     - Bank 0 to 7 control
     - bank index
     - Bank index, or the written value in the eight-bank profile.
   * - ``0x248``
     - Refresh counter
     - n/a
     - Unsupported.
   * - ``0x250``
     - Memory error status
     - 0
     - Readable in the eight-bank, legacy and transfer profiles.
   * - ``0x258``
     - Memory error address
     - 0
     - Same.
   * - ``0x260``
     - ECC syndrome
     - 0
     - Reads 0 in the transfer profile; otherwise unsupported.
   * - ``0x268``
     - ECC check bits
     - 0
     - Unsupported.
   * - ``0x270``
     - ECC replacement
     - 0
     - One byte, read/write.

The offsets, widths and spec resets come from
:src:`sgi-crime-1.5-spec PDF p. 113-120, 150-155`, *established*. The
reset ``0x120`` of the control register is the spec's reset of the juice
field in bits 7:5 (001) plus the endianness bit 8, which the spec lists
as read-only with no reset value; setting it to big-endian is
*inferred* from the O2 running big-endian.

.. _sgi-crime-profiles:

Profiles and options
====================

Two properties and several options change what the device does. The
O2 test profile (:doc:`../ip32`) exposes ``crime-profile``,
``reset-low-ram-approx``, ``functional-memory-transfer``,
``empty-renderer-status`` and ``prom-id-word`` as machine options of the
same names, sets ``eight-128m-banks`` from its
``bank-profile=8x128m-synthetic`` option, and fixes
``interface-revision`` at 1.

``crime-profile``
  ``1.5`` (default) or ``legacy-software``. Fixed after the device
  exists. The legacy profile reports ID ``0xa1`` with a zero upper
  word, accepts a handful of otherwise rejected writes, answers some
  4-byte reads, and ORs every interrupt cause into CPU line IP2. These
  are *approximation* choices. No public source supports the single
  interrupt line, and the capture of an R12000 O2 shows a non-zero upper
  ID word, so the legacy ID does not match every captured unit.

``eight-128m-banks``
  Presents eight 128 MiB banks, 1 GiB in all, with writable bank
  controls and the high alias at ``0x20000000``. The RAM region must be
  1 GiB. It is a synthetic layout and an *approximation*. Without it the
  RAM region must be 64 or 128 MiB.

``reset-low-ram-approx``
  Needs the eight-bank option. Until the first bank-control write, the
  first 32 MiB of RAM answer at address 0, as if bank 0 were already
  set up. An *approximation*. At reset every bank control selects the
  16 Mbit geometry, which the spec decodes as a 32 MB window at N x
  32 MB for bank N (:src:`sgi-crime-1.5-spec PDF p. 137, 152`). The
  model does not implement 16 Mbit banks, so without this option no RAM
  answers at address 0.

``functional-memory-transfer``
  Turns on the rendering engine subset described under
  :ref:`sgi-crime-renderer`. Needs RAM. Conflicts with
  ``empty-renderer-status``.

``empty-renderer-status``
  Makes one 32-bit read of the renderer status register return
  ``0x1e000000``, the four idle bits set and nothing queued.

``prom-id-word``
  Lets a 4-byte read at offset 4 return the low ID word. A model option
  with no hardware basis; the specification says doubleword access only.

``interface-revision``
  Accepts only 1.

.. _sgi-crime-interrupts:

Interrupts
==========

The status register is the OR of the hardware and software registers.
Status ANDed with the enable register gives 32 interrupts, which CRIME
groups onto the processor's IP2 to IP6 inputs
(:src:`sgi-crime-1.5-spec PDF p. 106-109`), *established*. The routing
is:

.. list-table::
   :header-rows: 1

   * - Bits
     - Source
     - Processor line
   * - 0 to 2
     - MACE video inputs and output
     - IP3
   * - 3 to 5
     - MACE Ethernet, serial and parallel, keyboard, mouse and timers
     - IP2
   * - 6
     - MACE audio
     - IP5
   * - 7
     - MACE PCI bridge errors
     - IP6
   * - 8 to 15
     - MACE PCI inputs: two SCSI, three slots, three shared
     - IP3
   * - 16 to 19
     - GBE
     - IP3
   * - 20
     - CPU interface error
     - IP6
   * - 21
     - Memory error
     - IP6
   * - 22 to 27
     - Rendering engine, edge (22 to 24) and level (25 to 27)
     - IP2
   * - 28
     - Software interrupt 0
     - IP4
   * - 29
     - Software interrupt 1
     - IP5
   * - 30
     - Processor corrected-error input
     - IP6
   * - 31
     - VICE
     - IP2

The spec's table gives bit 16 the same name as bit 19; bit 16 is GBE
interrupt 0. Its prose in sections 5.7.2.2 and 5.7.2.3 swaps bits 20
and 21, while its table and its revision notes keep 20 for the CPU
interface error and 21 for the memory error
(:src:`sgi-crime-1.5-spec PDF p. 109-110, 233`). Linux and NetBSD
agree with the table. Either way both go to IP6, so routing is
unaffected.

Edge-sensitive causes are latched in the hardware register and stay
until software writes a zero to that bit of the hardware register: GBE
bits 16 to 19, rendering-engine bits 22 to 24 and bit 30
(:src:`sgi-crime-1.5-spec PDF p. 110-111, 117-118`), *established*.
Linux acknowledges them that way. The model latches the same set
(``0x41cf0000``). The legacy profile latches bit 31 in place of bit 30
(``0x81cf0000``) and treats bit 30 as a software bit, so a hardware
input on bit 30 is ignored. The legacy latch on bit 31 is an
*approximation*; the spec lists bit 31 as a read-only level.

The software register can emulate any hardware interrupt, with
read/write bits 31:30, 29:28 and 24:0 (:src:`sgi-crime-1.5-spec PDF
p. 117`). The model accepts only bits 29:28, or 30:28 in the legacy
profile. This is narrower than the hardware.

The model drives the five processor lines as levels, high while any of
its causes is enabled and set. The specification has CRIME write the
processor's interrupt register over the system bus on each change
(:src:`sgi-crime-1.5-spec PDF p. 106`); the model's level wiring is an
*approximation* with the same effect on a processor that reads the cause
register. MACE drives bits 15:0 through ``sgi_crime_update_mace_inputs``
and GBE drives bits 19:16. Nothing in the model sets bits 20 to 27, 30
or 31, except an external input line, and the renderer produces no
interrupt. The enable register rejects a write with any of bits 63:32
set; the legacy profile ignores them and logs ``LOG_UNIMP``.

.. _sgi-crime-timer:

Timer and watchdog
==================

The timer is a 32-bit read/write counter, reset 0, clocked at 66.7 MHz
(:src:`sgi-crime-1.5-spec PDF p. 119`), *established*. Linux uses
66,666,500 Hz (``CRIME_MASTER_FREQ``) and NetBSD 66,666,666 Hz. NetBSD
masks the register to 48 bits, which disagrees with the spec. The model
counts at 66,666,666 Hz of virtual time, wraps at 32 bits and starts
from zero at reset. A write sets the count. The 1.5 profile rejects a
write with bits 63:32 set. The legacy profile accepts one and keeps the
low word, and answers a 4-byte read at ``0x03c``.

The watchdog register has a 20-bit value and a timeout flag in bit 20,
counts once per 64 timer ticks, and in CRIME 1.5 resets the machine at
1.0 s with a flag set at 0.5 s (:src:`sgi-crime-1.5-spec PDF p. 118-119`),
*established*. The widths differ in the drivers: Linux uses a 15-bit
value and NetBSD says 33 bits are valid. The model does not implement
the watchdog: reads give 0, the 1.5 profile rejects writes and the
legacy profile accepts a zero write.

.. _sgi-crime-cpu-error:

CPU error registers
===================

The CPU error address register holds address bits 33:2 of a failed
processor access. The status register holds three error bits and
address bits 39:34 (:src:`sgi-crime-1.5-spec PDF p. 120`),
*established*. The spec moves the status register to ``0x4048`` in CRIME
1.5 so that it sits on its own 16 KB page (PDF p. 230, which records the
move from an earlier draft position of ``0x14001048``), and says it was
at ``0x048`` in CRIME 1.1 (:src:`sgi-crime-1.5-spec PDF p. 113`). Linux and
NetBSD read it at ``0x048``, the 1.1 position, *software expectation*.
A Linux kernel at the pinned commit therefore does not find the status
register of the 1.5 profile.

The model has no detector. The registers hold zero unless a test fixture
seeds them. NetBSD also names ``0x050`` as a CPU error enable. The spec
does not map it. The legacy profile accepts a zero write there as a
no-op and logs ``LOG_UNIMP``.

.. _sgi-crime-memory:

Memory controller
=================

Memory is eight slots of SDRAM DIMMs. Two DIMMs make an external bank,
32 MB with 1M by 16 parts or 128 MB with 4M by 16 parts, from 32 MB to
1 GB in all (:src:`sgi-crime-1.5-spec PDF p. 135-136`), *established*.
Software probes the banks and programs eight bank control registers.

Each bank control register has the SDRAM size in bit 8 (0 is 16 Mbit,
1 is 64 Mbit), reserved read-only bits 7:5, and a match field in bits
4:0 that is compared with the request address: for 16 Mbit, bits 4:0
against address bits 29:25, for 64 Mbit bits 4:2 against 29:27
(:src:`sgi-crime-1.5-spec PDF p. 137, 152`), *established*. Reset
values are the bank number. If several banks match, the lowest number
wins. An address that matches no bank sets an invalid-address bit in the
memory error status register (bit 25 for a read, 26 for a write, 27 for
a read-modify-write) and the faulting address in the memory error address register
(``0x258``, bits 29:0, PDF p. 154), and the memory
operation does not happen (:src:`sgi-crime-1.5-spec PDF p. 137,
152-154`), *established*. The spec asks software to place 64 Mbit
banks first. The spec's prose on PDF p. 137 says a 64 Mbit bank compares
request bits 24:22; this page follows the table's 29:27.

Drivers differ from the spec:

- Linux ``ip32-memory.c`` shifts all five match bits left by 25 whatever
  the size, and skips any bank other than 0 whose base is 0
  (``db2ddb871435``).
- NetBSD ``crimereg.h`` names bank 1 at ``0x218``, bank 2 at ``0x210``,
  and so on in swapped pairs, and gives bank 7 the offset ``0x248`` that
  the spec assigns to the refresh counter (``07004b2a9dac``).

The model follows the spec.

The model's behaviour depends on the profile:

- Default: RAM of 64 or 128 MiB answers at ``0``, ``0x40000000`` and
  ``0x80000000`` for its own size only. Bank controls read as the bank
  number and writes are rejected. Memory error registers fault.
- Eight-bank: the RAM region is 1 GiB, split into eight 128 MiB banks
  with fixed backing; bank N is the Nth 128 MiB of the RAM region and
  appears at ``((control >> 2) & 7) * 128 MiB``. Writes with bits 7:5 or
  above bit 8 set are rejected rather than ignored. A bank set to
  16 Mbit is not implemented: accesses to it fail with
  ``LOG_GUEST_ERROR`` and a bus error. An address that no bank decodes
  records the invalid-address bit and the address in the error registers
  and fails with a bus error. It does not raise interrupt bit 21. With
  ``functional-memory-transfer`` or the legacy profile, a write of zero
  to ``0x250`` is accepted and does not clear the status; otherwise it
  is rejected.
  Bit 27 is never set.
  The decoded view also appears at ``0x20000000`` (high 512 MiB), at
  ``0x40000000`` and at ``0x80000000``.

.. _sgi-crime-ecc:

ECC
~~~

The status and control register has an ECC enable bit and a bit that
substitutes the replacement register for the generated check bits; the
replacement register is one byte replicated four times
(:src:`sgi-crime-1.5-spec PDF p. 150, 155`), *established*. The model
keeps no check bits and treats memory as healthy, an *approximation*.
Register ``0x200`` reads 0, or the stored enable bit when
``functional-memory-transfer`` is on, where a write of 0 or 1 is
accepted and the first enable logs a ``LOG_UNIMP`` "healthy-memory"
notice. A write with bit 1 set is rejected. The replacement byte is
stored and read back, and a value above ``0xff`` is rejected. Nothing
uses it.

.. _sgi-crime-renderer:

Rendering engine
================

The rendering engine draws into main memory and is controlled by
registers at ``0x15000000``. The spec groups them into five 4 KB sets:
interface buffer at ``+0x0``, TLB at ``+0x1000``, pixel pipeline at
``+0x2000``, memory transfer engine (MTE) at ``+0x3000`` and status at
``+0x4000`` (:src:`sgi-crime-1.5-spec PDF p. 162-163`), *established*.

.. list-table::
   :header-rows: 1

   * - Offset
     - Registers
     - Source
   * - ``+0x000``
     - Interface buffer data, 128 doublewords
     - PDF p. 163
   * - ``+0x200``
     - Interface buffer address, 128 doublewords
     - PDF p. 164
   * - ``+0x400``
     - Interface buffer control, 32 bits, write only
     - PDF p. 164
   * - ``+0x1000``, ``+0x1200``, ``+0x1400``
     - Frame buffer TLBs A, B, C, 64 entries each
     - PDF p. 164
   * - ``+0x1600``
     - Texture TLB, 28 entries
     - PDF p. 164
   * - ``+0x16e0``
     - Clip ID TLB, 4 entries
     - PDF p. 164
   * - ``+0x1700``, ``+0x1780``
     - Linear TLBs A and B, 16 entries each
     - PDF p. 164
   * - ``+0x2000`` to ``+0x21f8``
     - Pixel pipeline registers (mode, clip, draw, vertices, shading,
       texture, blend, depth, null and flush)
     - PDF p. 165-170
   * - ``+0x3000`` to ``+0x3078``
     - MTE mode, byte mask, stipple mask, clear value, two source and
       two destination addresses, two strides, null and flush
     - PDF p. 183-184
   * - ``+0x4000``
     - Status
     - PDF p. 163

The spec says the host starts a primitive by offsetting its last
register write by a START_OFFSET it does not print. NetBSD ``crmfbreg.h``
defines it as ``0x0800``, *software expectation*, and the model commits
on a write at ``+0x800`` from the register. Linear TLB entries are
32-bit page numbers with bit 31 set, and the frame buffer TLBs hold
16-bit tile entries with bit 15 set, four to a doubleword
(NetBSD ``crmfbreg.h`` and ``crmfb.c``, *software expectation*). The MTE
mode word has the copy flag in bit 11, the stipple enable in bit 10,
the pixel depth in bits 9:8, source type in 7:5, destination type in 4:2
and the ECC flags in bits 1:0 (:src:`sgi-crime-1.5-spec PDF p. 184-185`).

What the model implements depends on the options:

- Default: none of the renderer. A read or write inside
  ``0x15000000`` fails with a bus error and logs ``LOG_UNIMP`` for a
  doubleword access or ``LOG_GUEST_ERROR`` for any other width.
  ``empty-renderer-status`` adds one 32-bit status read.
- ``functional-memory-transfer``: the linear TLBs, the MTE parameter
  registers, a status register that reads idle and empty, and
  synchronous linear fills and copies between linearly mapped RAM
  pages. The transfer is refused unless the byte mask is all ones, the
  destination is a linear TLB, and the source and destination do not
  overlap. In a fill, a destination page whose TLB entry is invalid is
  skipped and logged (``LOG_UNIMP``). A copy that touches one is
  refused.
- ``functional-memory-transfer`` with the ``legacy-software`` profile:
  also the interface buffer RAM, the frame buffer and clip ID TLBs,
  a stored copy of the pixel pipeline registers, tiled and clip ID
  clears, and synchronous two-dimensional fills, copies, stipples,
  alpha-tested draws, indexed uploads and downloads and bounded flat
  triangles, with the drawing state restricted to the combinations the
  code accepts. The status register echoes the interface buffer
  write position with the level always zero.

Anything outside the accepted combinations logs ``LOG_GUEST_ERROR``
naming the reason and stops the virtual machine with an internal error.
The renderer is not a model of the hardware pipeline. Drawing is
instantaneous, the interface buffer never fills, the renderer sets no
interrupt, and texture mapping, fog, blending, the depth and stencil
tests, antialiasing and smooth shading are not drawn. The choice of
which combinations to accept is an *approximation*. How a real engine
treats the rest is *unknown* in this page; the spec leaves parts of it
blank or marked TBD.

.. _sgi-crime-reset:

Reset
=====

Reset clears the interrupt state, enables, software interrupts, timer,
error registers and the ECC replacement byte, empties the renderer
state, and sets each bank control to its bank number. The interrupt
input lines keep their level.

.. _sgi-crime-migration:

Migration
=========

The device migrates as ``sgi-crime`` version 2, with subsections for
transfer, profile, renderer and ECC state. Version 1 streams are
rejected. The eight-bank and reset low-RAM options block migration.
Migration works only between identical builds.

.. _sgi-crime-measurements:

Measurements
============

No register of this part has been read on a physical machine for this
project. The published captures are listed under
:ref:`sgi-crime-identity`.

.. _sgi-crime-limits:

Model limits
============

- The default ID is ``0xb1``, the specification's CRIME 1.5 value. Both
  published captures read ``0xa1`` in the low byte. The legacy profile
  presents ``0xa1`` with a zero upper word, while the R12000 capture's
  upper word is ``0x1000``.
- The software interrupt register accepts fewer bits than the hardware.
  The watchdog, the processor and memory error detectors, ECC, refresh
  and the VICE interrupt are not modelled. Memory error bit 27 is never
  set.
- The control register ignores or rejects every write, so the watchdog
  enable, the reset bits and the juice field cannot be set.
- Registers reject widths other than eight bytes, except in the legacy
  profile, which answers 4-byte reads of the timer low word and the bank
  controls, and with ``prom-id-word``, which answers a 4-byte ID read.
  The renderer's 32-bit registers take 4-byte accesses.
- A rejected access logs ``LOG_GUEST_ERROR`` or ``LOG_UNIMP`` and returns
  a bus error. When ``functional-memory-transfer`` is on or the profile
  is legacy, the same access also stops the virtual machine with an
  internal error.
- The linear frame buffer and depth buffer windows are not modelled.
- The renderer is limited as described under
  :ref:`sgi-crime-renderer`. The spec registers the model does not
  store, including interface buffer control and the diagnostic fields,
  fail or log as shown there.
- Migration is limited as described above.

.. _sgi-crime-tests:

Tests
=====

- ``tests/qtest/sgi-crime-test.c``: ID and control reset values and
  access rules, the ``prom-id-word`` option, timer count, invalid
  access and migration, interrupt routing from MACE, software
  interrupts, reset and migration of pending interrupts, the RAM aliases
  and their holes, populated-bank reset values, views, priority, remap,
  unmatched writes and the reset low-RAM approximation.
- ``tests/qtest/sgi-crime-error-test.c``: seeded error latch clearing,
  validation and migration. It needs the test fixture device.
- ``tests/qtest/sgi-crime-mte-test.c``: linear and tiled memory
  transfers through the MTE and TLB registers.
- ``tests/qtest/sgi-crime-render-test.c``: pixel pipeline state, 2D
  drawing, stipples, alpha, triangles, indexed transfers, the
  interface buffer and refusal of unsupported state.
- ``tests/qtest/sgi-crime-legacy-test.c``: the legacy profile, the
  ECC replacement register, interface buffer and status accesses,
  hard stops and migration.
- ``tests/qtest/sgi-crime-invalid-clear-test.c``: clears that touch
  pages with invalid TLB entries.
