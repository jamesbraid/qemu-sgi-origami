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
``include/hw/mips/sgi/crime.h``. The ``o2`` machine (:doc:`../ip32`)
fits it with the ``legacy-software`` profile, the eight-bank memory and
the functional memory transfer engine. VICE, whose interrupt CRIME
collects, has its own page (:doc:`vice`).

.. seealso::

   Platform pages: :ref:`sgi-ip32`.

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
   * - ``0x058``
     - Error register without a modelled source
     - n/a
     - Reads 0 in the legacy profile, with ``LOG_UNIMP``; otherwise
       unsupported.
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
     - Reads 0 in the legacy and transfer profiles, and logs
       ``LOG_UNIMP`` in the legacy one; otherwise unsupported.
   * - ``0x268``
     - ECC check bits
     - 0
     - Reads 0 in the legacy profile, with ``LOG_UNIMP``; otherwise
       unsupported.
   * - ``0x270``
     - ECC replacement
     - 0
     - One byte, read/write.

The offsets, widths and spec resets come from
:src:`sgi-crime-1.5-spec PDF p. 113-120, 150-155`, *established*. The
reset ``0x120`` of the control register is the spec's reset of the coherent
read limit field in bits 7:5 (001) plus the endianness bit 8, which the spec lists
as read-only with no reset value; setting it to big-endian is
*inferred* from the O2 running big-endian.

.. _sgi-crime-profiles:

Profiles and options
====================

Two properties and several options change what the device does. The
``o2`` machine (:doc:`../ip32`) fixes ``crime-profile=legacy-software``,
the eight banks, ``reset-low-ram-approx``, ``functional-memory-transfer``
and ``prom-id-word``. Its register test profile exposes ``crime-profile``,
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
  Makes the model return ``0x1e000000`` for one 32-bit read of the
  renderer status register, the four idle bits set and nothing queued.

``prom-id-word``
  Lets a 4-byte read at offset 4 return the low ID word. A model option
  with no hardware basis; the specification says doubleword access only.

``interface-revision``
  Accepts only 1.

.. _sgi-crime-address-spaces:

Address windows and aliases
===========================

CRIME makes the first-level decode of a processor access and forwards it
to a subsystem. CRIME, GBE, MACE and VICE then each decode further inside
their own window (:src:`sgi-crime-1.5-spec PDF p. 47`), *established*.
The windows are in the address table under :ref:`sgi-crime-identity`.

The processor has a 40-bit physical address space. The base I/O
resources are reached through KSEG1, and the first 32 MB of PCI memory
and of PCI I/O space are aliased into KSEG1, so a typical configuration
reaches all its PCI resources there. The full 32-bit PCI spaces need
translation buffer entries
(:src:`sgi-crime-1.5-spec PDF p. 47`), *established*. The specification
describes the memory windows this way, each *established*
(:src:`sgi-crime-1.5-spec PDF p. 47`):

- The full 1 GB is directly reachable at ``0x40000000``, and the first
  256 MB is aliased to KSEG0, so the operating system can map its own
  structures without translation buffer entries. Reaching the full 1 GB
  takes entries.
- The high 512 MB is also aliased into the upper 512 MB of the first
  1 GB of address space (``0x20000000``), an alternative to the
  1.5 GB to 2 GB range of the full window that gives more compact page
  tables.
- The frame buffer and depth buffer windows give linear access to the
  2K by 2K, 32-bit frame buffer and depth buffer that the rendering
  engine currently maps.

The no-ECC window at ``0x80000000`` lets the processor read any location
without ECC checking or correction. The specification gives two uses: reading the
raw contents of memory in an ECC error fixup routine without disabling
the ECC logic for other DMA, and reading data the rendering engine wrote
without raising ECC errors. Only checking and correction are
suppressed. ECC is still generated for a write through the window, and a
write of less than a doubleword still makes a read-modify-write memory
transaction (:src:`sgi-crime-1.5-spec PDF p. 47`), *established*. The
model keeps no check bits, so it treats the no-ECC window as a data alias
of the full-memory window, an *approximation*: a value written through
any of the low, full or no-ECC views reads back through every other view
that decodes the same bank (``sgi-crime-test.c``, cases
``ram-aliases-holes`` and ``populated-banks-views``).

.. _sgi-crime-dma-spaces:

DMA address spaces
==================

VICE, GBE and MACE reach memory through CRIME, and each is restricted to
system memory (:src:`sgi-crime-1.5-spec PDF p. 49-50`), *established*.
The specification gives each client its own address table
(Tables 3-2 to 3-4 on the same pages), each row *established*. The
VICE table also has a first row, 960 GB reserved from
``0x10_0000_0000``, which the table below leaves out:

.. list-table::
   :header-rows: 1

   * - Client
     - Start address
     - Size
     - Function
   * - VICE
     - ``0x00000000``
     - 1 GB
     - reserved
   * - VICE
     - ``0x40000000``
     - 1 GB
     - memory, CRIME
   * - VICE
     - ``0x80000000``
     - 1 GB
     - memory without ECC checking, CRIME
   * - VICE
     - ``0xc0000000``
     - 61 GB
     - reserved
   * - GBE
     - ``0x00000000``
     - 1 GB
     - reserved
   * - GBE
     - ``0x40000000``
     - 1 GB
     - memory, CRIME
   * - GBE
     - ``0x80000000``
     - 2 GB
     - reserved
   * - MACE
     - ``0x00000000``
     - 1 GB
     - reserved
   * - MACE
     - ``0x40000000``
     - 1 GB
     - memory, CRIME
   * - MACE
     - ``0x80000000``
     - 1 GB
     - memory without ECC checking, CRIME
   * - MACE
     - ``0xc0000000``
     - 1 GB
     - reserved

VICE generates only the low 32 bits of a DMA address and zeroes the top
eight of its 40. It still decodes the full 40-bit address of a processor
access to its own 16 MB window (:src:`sgi-crime-1.5-spec PDF p. 49`),
*established*. GBE's table has no no-ECC window, so only VICE and MACE
reach it (:src:`sgi-crime-1.5-spec PDF p. 49-50`), *established*. The model's GBE
and MACE views of memory are described on :doc:`gbe` and :doc:`mace`, and
VICE's on :doc:`vice`.

PCI masters reach memory through MACE with two aliases. Both PCI tables
place native-endian memory at ``0x40000000`` and reverse-endian memory
at 0, and send them to CRIME. In the reverse-endian alias a transfer of
less than 64 bits has its bytes swapped to the opposite of the memory
system's order (:src:`sgi-crime-1.5-spec PDF p. 51-52`, Tables 3-5 and
3-6), *established*. The memory-space table (Table 3-6) labels its 2 GB
local region "Local PCI I/O", as the I/O table does. Its introductory
text calls the region local PCI memory space, so the label is read as a
copy of the I/O table's, *inferred*. MACE should cache references to memory
made through PCI memory space, and there is no hardware coherence between
that cache and processor accesses, so a driver must flush, invalidate and
keep processor and device access to a cached DMA buffer apart in time. It
should not cache references made through PCI I/O space
(:src:`sgi-crime-1.5-spec PDF p. 51-52`), *established*. Which byte
placement the model gives each alias is described on :doc:`mace`.

.. _sgi-crime-interconnects:

Links to MACE and GBE
=====================

CRIME's input/output unit joins the chip to MACE through one link and to
GBE through another. It turns DMA requests from the two chips into
requests to the memory controller, performs processor reads and writes
on the links, and receives interrupts that the chips send as transactions
(:src:`sgi-crime-1.5-spec PDF p. 55`), *established*. The model takes
MACE's 16 interrupt causes as input lines that each hold a level
(``sgi-crime-test.c``, case ``mace-level-routing``), not as interrupt
packets with write enables, an *approximation*. GBE's memory access and
four interrupt outputs are described on :doc:`gbe`.

MACE link
~~~~~~~~~

The MACE link is a half-duplex, synchronous bus 32 bits wide that
transfers at twice the internal clock rate, and so carries one 64-bit
logical word each clock. It carries command, address and data on the same signals,
sub-block transfers of 1 to 8 bytes under a byte mask, block transfers of
1 to 32 doublewords, split reads and routing tags in transfer headers
(:src:`sgi-crime-1.5-spec PDF p. 55-56, 66`), *established*. A doubleword
crosses the 32-bit bus as two words, bytes 0 to 3 first. All data on the
link is big-endian and the link cannot switch byte order
(:src:`sgi-crime-1.5-spec PDF p. 66-67`), *established*. Two token
signals pass bus ownership, and CRIME owns the bus at reset and power-up
(:src:`sgi-crime-1.5-spec PDF p. 66`), *established*. The specification
discourages sub-block DMA as very inefficient, since the memory system
protects 64 bits at a time with ECC
(:src:`sgi-crime-1.5-spec PDF p. 56`), *established*.

A transfer is a header doubleword with a 4-bit command, a count of
doublewords in bits 52:48 (one less than the number sent or requested), a byte mask
in bits 47:40, a 6-bit source tag in bits 37:32 and a 32-bit byte
address in bits 31:0. Unused fields are zero
(:src:`sgi-crime-1.5-spec PDF p. 70-78`), *established*. A set mask bit
marks a byte as not valid, and the least significant bit, header bit 40,
masks byte 7 in big-endian numbering (:src:`sgi-crime-1.5-spec PDF p. 74`),
*established*. The transfer types are:

.. list-table::
   :header-rows: 1

   * - Type
     - Command
     - Doublewords
     - Use
   * - null
     - ``0000``
     - 1
     - idle fill
   * - interrupt update
     - ``0001``
     - 1
     - 16 write-enable bits in 31:16 and 16 interrupt bits in 15:0
   * - write block
     - ``0010``
     - 2 to 33
     - header and 1 to 32 data doublewords
   * - write sub-block
     - ``0011``
     - 2
     - header and one masked data doubleword
   * - read block request
     - ``0100``
     - 1
     - asks for 1 to 32 doublewords
   * - read sub-block request
     - ``0101``
     - 1
     - asks for one masked doubleword
   * - read data response
     - ``0110``
     - 2 to 33
     - returns the data to the tag's source; an error bit set means the
       request was invalid, for example a bad address

The rows come from :src:`sgi-crime-1.5-spec PDF p. 70-78`, each
*established*.

The flow rules, each *established* (:src:`sgi-crime-1.5-spec PDF p. 67-69`):

- Transactions from CRIME to MACE enter each MACE subsystem's queue in
  the order issued, and those from one subsystem reach CRIME in order.
  Transactions from different subsystems are ordered by arbitration, and
  nothing orders a CRIME-initiated transaction against a MACE-initiated
  one.
- The initiator keeps the count of its outstanding transactions. A DMA
  read is acknowledged when its response has arrived in the subsystem.
- CRIME issues no more than two outstanding PIO writes and one
  outstanding PIO read, and issues nothing while a read is outstanding.
  A subsystem stores two PIO writes, or a write and a read. PIO writes to
  different subsystems may complete out of order, and two to the same
  subsystem complete in order. The specification states these limits for
  CRIME as a whole and sizes each subsystem's store to match; it does not
  say whether CRIME counts them per subsystem.
- MACE has one interrupt packet outstanding at a time. An interrupt
  packet bypasses memory writes still pending in CRIME, so software
  that takes an interrupt as the end of a DMA must first flush those
  writes. The specification says a single PIO read does this.

GBE link
~~~~~~~~

The GBE link is a point-to-point, burst link: 64 data lines that toggle
at 133 MHz and are demultiplexed inside each chip to 128 bits at 66 MHz,
for a peak of 1 GB/s (:src:`sgi-crime-1.5-spec PDF p. 79-81`),
*established*. It differs from the MACE link in being 64 bits wide on
the pins and in having no acknowledge pins: flow control is implicit in
the data lines (:src:`sgi-crime-1.5-spec PDF p. 79-81`), *established*. Every
transfer is a 128-bit header, with a command in bits 127:124, a size in
123:119, 32 bits of data in 63:32 and a 32-bit address in 31:0, followed
by 128-bit data cycles. Data is big-endian. The sender drives null
commands whenever it has nothing to send, including during reset. CRIME
is the sender at reset, and GBE becomes the sender, by driving its token
output high and waiting for CRIME's to fall, when programming asks it to.
The current sender sends every pending command before it releases the bus
(:src:`sgi-crime-1.5-spec PDF p. 81, 84-85`), *established*.

.. list-table::
   :header-rows: 1

   * - Sender
     - Command
     - Meaning
   * - CRIME
     - ``0001``
     - PIO write: 32 bits of data to a word-aligned 24-bit GBE offset,
       with address bits 31:24 ignored
   * - CRIME
     - ``0010``
     - PIO read request: one 32-bit read at a word-aligned 24-bit offset
   * - CRIME
     - ``0011``
     - DMA read data: a count of 256-bit words follows the header, in
       the order GBE asked for them
   * - CRIME
     - ``0100``
     - DMA write done: a GBE write has reached memory
   * - GBE
     - ``0101``
     - PIO read data: 32 bits, with the address returned for debugging
   * - GBE
     - ``0110``
     - DMA read request: a 256-bit-aligned address and a count of
       256-bit words
   * - GBE
     - ``0111``
     - DMA write data: a header and twice the count of data cycles
   * - GBE
     - ``1000``
     - interrupt: the low 4 address bits select the sources

Each row is *established* (:src:`sgi-crime-1.5-spec PDF p. 81-84`). The
interrupt command is a one-shot event that CRIME must latch. A mask bit of
1 sets the matching CRIME interrupt, a 0 leaves it alone, and CRIME can
only clear it, by the software write described under
:ref:`sgi-crime-interrupts` (:src:`sgi-crime-1.5-spec PDF p. 84`),
*established*. Only one PIO read is outstanding at a time. PIO writes have no
flow control, so GBE must accept them at the full 66 MHz rate, except
colour map writes, which go through a 64-deep FIFO that software must
throttle (:src:`sgi-crime-1.5-spec PDF p. 81-82, 84`), *established*.
CRIME's memory transaction queue holds up to 16 requests, reads and
writes together, and it returns read data in the order GBE asked for it
(:src:`sgi-crime-1.5-spec PDF p. 83`), *established*. A DMA write goes
to a write buffer in CRIME and its memory request is queued once the
last data has arrived (:src:`sgi-crime-1.5-spec PDF p. 83, 86`),
*established*.

The specification disagrees with itself on the limits. One page says CRIME
has a single DMA write buffer and GBE must wait for the write-done command
before it issues another write; the next page says there are two write
buffers and GBE may have two writes pending. The summary on a third
page allows one outstanding DMA write and eight outstanding reads or
writes in all, against the 16-entry read queue, and calls all DMA
512-byte aligned where the text says 256-bit aligned
(:src:`sgi-crime-1.5-spec PDF p. 82-84`). The right values are *unknown*.
The same summary lists a rule of no 8K DRAM page crossings
(:src:`sgi-crime-1.5-spec PDF p. 84`), *established* as a constraint on DMA
over the link.

.. _sgi-crime-cpu-interface:

Processor interface
===================

CRIME supports one R10000 or R12000 processor, with VICE on the same
system bus. CRIME 1.5 supports no coherent I/O and carries no ECC
protection on the processor data bus
(:src:`sgi-crime-1.5-spec PDF p. 91, 97`), *established*. The only
external request CRIME starts is an interrupt request
(:src:`sgi-crime-1.5-spec PDF p. 93`), *established*.

The specification gives CRIME's response to each processor request, each
*established* (:src:`sgi-crime-1.5-spec PDF p. 93`):

- A noncoherent block read and any doubleword, word or partial-word read
  are acknowledged with data.
- Block writes and doubleword, word or partial-word writes are accepted.
- An upgrade is treated as an ordinary read and acknowledged with data.
- An eliminate request is not supported. CRIME gives no response, and
  whether it drops or queues the request is left to the implementation,
  with no adverse effect. The processor is meant to have eliminate
  requests disabled at initialisation.
- A coherent block read, shared or exclusive, is answered with data
  below an address limit and with an error completion response above it.

The limit is a workaround for speculative loads by the R10000. A coherent
read above the limit gets the error response: CRIME does not read memory
and returns no data. A noncoherent block read is served from any address.
CRIME decodes address bits 39:23 for this, so any bit set in 39:26 puts
the address above every limit
(:src:`sgi-crime-1.5-spec PDF p. 93, 95-96`), *established*. Bits 7:5 of
the control register set the limit in 4 MB steps from 8 MB to 32 MB
(:src:`sgi-crime-1.5-spec PDF p. 115`), *established*:

.. list-table::
   :header-rows: 1

   * - Bits 7:5
     - Coherent reads above this address get the error response
   * - 000
     - none are served: every coherent read gets it
   * - 001
     - 8 MB (the value the specification gives as current)
   * - 010
     - 12 MB
   * - 011
     - 16 MB
   * - 100
     - 20 MB
   * - 101
     - 24 MB
   * - 110
     - 28 MB
   * - 111
     - 32 MB

The control register's other fields, each *established*
(:src:`sgi-crime-1.5-spec PDF p. 114-115`):

.. list-table::
   :header-rows: 1

   * - Bits
     - Field
     - Access
     - Reset
   * - 13
     - R5000 processor-bus check control, not used in CRIME 1.5
     - read/write
     - 0
   * - 12
     - CRIME check control for the same, not used in CRIME 1.5
     - read/write
     - 0
   * - 11
     - cold reset: 1 resets the system with a power-on reset; set by
       software or the watchdog, cleared by hardware
     - read/write
     - 0
   * - 10
     - soft reset of the processor, set by software or the watchdog and
       cleared by hardware; not used in CRIME 1.5
     - read/write
     - 0
   * - 9
     - watchdog timer enable
     - read/write
     - 0
   * - 8
     - endianness, 1 for big
     - read
     - not applicable
   * - 7:5
     - coherent read limit, above
     - read/write
     - 001
   * - 4:0
     - reserved
     - read
     - 0

The model reads this register as ``0x120`` in the 1.5 profile, the reset
of bits 7:5 and the endianness bit set (see :ref:`sgi-crime-registers`),
*inferred*, and as ``0x100`` in the legacy profile, so bits 7:5 read 000,
an *approximation*. The 1.5 profile refuses a write of bit 9 or bit 11 (``sgi-crime-test.c``, case
``reset-access``). The legacy profile accepts a write of 0 or ``0x100``,
which changes nothing, and refuses a write of any other single bit
(``sgi-crime-legacy-test.c``, case ``controls``).

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
*approximation*; the spec lists bit 31 as read-only, a registered copy of
the VICE interrupt pin (:src:`sgi-crime-1.5-spec PDF p. 109, 116`).

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

Delivery and acknowledgement
~~~~~~~~~~~~~~~~~~~~~~~~~~~~

When one of the grouped interrupts changes, CRIME writes the
processor's interrupt cause register over the system bus. The write
carries the new level of one line in the low data bits and a write-enable
mask in bits 20:16, so it changes that line and leaves the others
(:src:`sgi-crime-1.5-spec PDF p. 106, 109`), *established*:

.. list-table::
   :header-rows: 1

   * - Line
     - Cause register bit
     - Data bit
     - Write-enable bit
   * - IP2
     - 10
     - 0
     - 16
   * - IP3
     - 11
     - 1
     - 17
   * - IP4
     - 12
     - 2
     - 18
   * - IP5
     - 13
     - 3
     - 19
   * - IP6
     - 14
     - 4
     - 20

The rows come from :src:`sgi-crime-1.5-spec PDF p. 109`, each
*established*. CRIME cannot disable a line as a group; the processor's own
masks do that. The specification names the lines IP3 to IP7 in saying so,
which reads as a slip for the five lines of its own table, *inferred*
(:src:`sgi-crime-1.5-spec PDF p. 110`).

MACE sets bits 15:0 by sending CRIME a 16-bit value with a 16-bit mask.
Software cannot write those bits, and clears the cause by the procedure in
MACE's own registers. GBE sets bits 19:16 with four set strobes, each
asserted for one clock in CRIME, and software clears a GBE bit by writing
zero to it in the hardware register. VICE's interrupt is bit 31, a
registered copy of its interrupt pin. Software cannot write it, and clears
the cause inside VICE. The rendering engine sets bits 27:22
(:src:`sgi-crime-1.5-spec PDF p. 109-110`), *established*.

An edge cause stays in the hardware register after its source goes away
until software writes zero to its bit. If no other cause is on that line,
CRIME then writes the processor to drop the line, and if the source is
still asserted CRIME interrupts the processor again at once. A level cause
is not latched: CRIME raises the line when the source asserts and drops
it when the source de-asserts
(:src:`sgi-crime-1.5-spec PDF p. 111-112`), *established*. The model
latches again at once when an input is still high after a clear, and a
write of ones to the hardware register clears nothing. A zero written to
a level cause's bit does not clear it while its input stays high. This
holds for every hardware cause in both profiles (``sgi-crime-legacy-test.c``, cases
``source-routing`` and ``functional-source-routing``). A level cause falls
with its input (``sgi-crime-test.c``, case ``mace-level-routing``).

In the hardware register the specification marks bits 30, 29:28, 24:22
and 19:16 read/write and every other bit read-only. The status register
is read-only, and the enable and software registers are read/write. All
four reset to 0 (:src:`sgi-crime-1.5-spec PDF p. 116-118`),
*established*. The model refuses a write to the hardware register with
any of bits 63:32 set in the 1.5 profile (``sgi-crime-test.c``, case
``reset-access``). The specification's prose says the rendering engine's
bits 27:22 clear when software removes the cause, while its table lists
bits 24:22 as writable. The two are consistent if only the edge bits are
latched, *inferred* (:src:`sgi-crime-1.5-spec PDF p. 109, 117-118`).
The table notes that bit 30 was formerly a third software interrupt that
CRIME 1.1 did not use, and adds "we think"
(:src:`sgi-crime-1.5-spec PDF p. 106`). Whether CRIME 1.1 used it is
*unknown*; the legacy profile's software bit 30 is consistent with the note.

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

Control register bit 9 enables the watchdog. Control bit 11 requests a
power-on reset, is set by software or by the watchdog, and is cleared by
hardware (:src:`sgi-crime-1.5-spec PDF p. 114-115`), *established*. In
CRIME 1.1 the first expiry, at 0.5 s, started a soft reset and the second,
0.5 s later, a hard reset. CRIME 1.5 sets only the flag at the
first expiry and always cold-resets at the second, as it has no warm
reset (:src:`sgi-crime-1.5-spec PDF p. 118-119`), *established*.

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
register of the 1.5 profile. Linux's handler prints the error address and
status and then writes zero to the status register (``crime.c`` at
``db2ddb871435``), *software expectation*.

In the status register bit 2 is the CPU illegal address flag. Bits 1 and
0 were the VICE and CPU write parity flags, which the specification says
are no longer supported now that the processor bus carries no check
bits. Bits 8:3 hold address bits 39:34, new in CRIME 1.5 "if it can be
implemented", with no reset value given. Hardware sets the flags and
software clears them, and each reads and writes with reset 0. The address
register records the address of a processor data parity error or an
invalid address (:src:`sgi-crime-1.5-spec PDF p. 120`), *established*.

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
banks first. Its prose on PDF p. 137 says a 64 Mbit bank compares
request bits 24:22 and the table says 29:27. They are the same bits:
a client's request address is 25 bits (*inferred* from the figure), the
processor address shifted
down by five, and the figure on PDF p. 138 puts the external bank select at
processor bits 29:27 and internal request bits 24:22 for a 64 Mbit bank,
and at 29:25 and 24:20 for a 16 Mbit bank
(:src:`sgi-crime-1.5-spec PDF p. 125, 138`), *established*. The same figure
puts the internal bank select, the SDRAM chip's own bank bit, at processor
bit 26 for 64 Mbit and bit 24 for 16 Mbit.
In 64 Mbit mode selector bits 1:0 read and write but take no part in the
match (:src:`sgi-crime-1.5-spec PDF p. 152`), *established*. The model
stores them and places a bank by bits 4:2 alone (``sgi-crime-test.c``, case
``populated-priority-and-remap``).

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
  to ``0x250`` is accepted; otherwise it is rejected. See
  :ref:`sgi-crime-memory-errors` for what the write does.
  Bit 27 is never set.
  The decoded view also appears at ``0x20000000`` (high 512 MiB), at
  ``0x40000000`` and at ``0x80000000``.

.. _sgi-crime-memory-layout:

Memory arrangement and timing
~~~~~~~~~~~~~~~~~~~~~~~~~~~~~

A front-only DIMM carries eight 16-bit data chips, 128 bits, and one
16-bit ECC chip. Two make an external bank of 256 data bits and 32 ECC
bits. Each SDRAM chip has two internal banks, so a system has at most
8 external and 16 internal banks, and the controller keeps four pages
open at a time (:src:`sgi-crime-1.5-spec PDF p. 136`), *established*.
The specification recommends that external bank 0 is always filled and that
the banks go in decreasing density
(:src:`sgi-crime-1.5-spec PDF p. 137-138`), *established*.

A request whose address matches no bank still passes through the pipeline,
but the controller drops every bank select so the memory operation does
not happen, and it captures the error and interrupts if enabled. The
rendering engine can also mark a request as made through an invalid
translation buffer entry, which drops the selects the same way
(:src:`sgi-crime-1.5-spec PDF p. 137`), *established*. The model's handling
of an invalid entry is under :ref:`sgi-crime-renderer`.

The memory clients are GBE, MACE, VICE, the rendering engine, the
processor and refresh. The arbiter hands out the slots of a 64-slot round
in these shares, and the specification works out each client's worst-case
slot interval from them (:src:`sgi-crime-1.5-spec PDF p. 133-135`),
*established*:

.. list-table::
   :header-rows: 1

   * - Client
     - Share of slots
     - Longest request in a slot
     - Worst-case slot interval
   * - GBE
     - 1/2
     - 16 words, no page crossing
     - 0.50 us
   * - MACE
     - 1/4
     - 8 words, no page crossing
     - 1.00 us
   * - VICE
     - 1/8
     - 8 words, one page crossing
     - 2.00 us
   * - rendering engine
     - 1/16
     - 8 words, no page crossing
     - 4.00 us
   * - processor
     - 1/32
     - 8 words, no page crossing
     - 8.00 us
   * - refresh
     - 1/64
     - 2 rows
     - 16.00 us

Each row is *established* (:src:`sgi-crime-1.5-spec PDF p. 133-135`). A
slot may also hold one read-modify-write from MACE, the rendering engine,
the processor or VICE. A memory word is 256 bits, *inferred* from the 256-bit external bank and
the slot bandwidths (:src:`sgi-crime-1.5-spec PDF p. 134-136`).

After reset the controller runs the SDRAM initialisation, a precharge of all
banks and a mode set of CAS latency 2, burst length 1 and sequential
order, and software must leave memory alone for at least 120 us so
that 8 refresh cycles happen. The SDRAM needs 4096 refresh cycles every 64 ms.
A timer asks for a refresh every 27 us, and each refresh precharges all
banks and refreshes two rows
(:src:`sgi-crime-1.5-spec PDF p. 141-142`), *established*. The refresh
counter register at ``0x248`` holds an 11-bit read/write count with no
reset value given (:src:`sgi-crime-1.5-spec PDF p. 152`), *established*;
the count's units and its relation to the 27 us timer are *unknown*.

.. _sgi-crime-memory-errors:

Memory error registers
~~~~~~~~~~~~~~~~~~~~~~

The memory error status register has these bits, all set by hardware and
cleared by software, read/write and reset 0
(:src:`sgi-crime-1.5-spec PDF p. 152-153`), *established*:

.. list-table::
   :header-rows: 1

   * - Bits
     - Meaning
   * - 27
     - invalid address during a read-modify-write
   * - 26
     - invalid address during a write
   * - 25
     - invalid address during a read
   * - 24
     - ECC read error during a read-modify-write
   * - 23
     - ECC read error
   * - 22
     - multiple hard errors
   * - 21
     - hard error
   * - 20
     - soft error
   * - 19
     - reserved
   * - 18, 17, 16, 15
     - access by the processor, VICE, GBE, the rendering engine
   * - 14:8
     - rendering engine source ID
   * - 7
     - MACE access
   * - 6:0
     - MACE source ID

Each row is *established* (:src:`sgi-crime-1.5-spec PDF p. 153`). The
specification does not define a hard or a soft error. Reading a soft
error as a corrected single-bit error and a hard error as an uncorrected one
fits the ECC text, *inferred* (:src:`sgi-crime-1.5-spec PDF p. 130, 144`).
Linux's header defines the same fields (``crime.h`` at ``db2ddb871435``).
Its handler treats a hard or multiple error as fatal and a soft error as
non-fatal, and writes zero to the status register after printing it
(``arch/mips/sgi-ip32/crime.c`` at ``db2ddb871435``), each a *software
expectation*. A
memory error sets the memory error bit in the hardware interrupt register.
The address register holds address bits 29:0 of the error, read-only,
reset 0 (:src:`sgi-crime-1.5-spec PDF p. 152, 154`), *established*.

The registers update as follows, each *established*
(:src:`sgi-crime-1.5-spec PDF p. 145-146`). The first hard error updates the
status and address registers. A second hard error only sets the multiple
hard errors bit. A first soft error with no hard error updates both, and
so does an invalid address with no ECC error. An ECC error also captures
its syndrome. The status identifies the client that made the request and,
for the rendering engine and MACE, the message it sent.

The model sets only bit 25 for a failed processor read and bit 26 for a
failed write, and no other bit, and records the failed address (case
``memory-error-status-write`` in ``sgi-crime-test.c``, in the 1.5 and
legacy profiles). It sets none of the client bits, the multiple-error bit
or the ECC bits, an *approximation*.
A write of zero to the status register depends on the profile, each an
*approximation* of the specification's software clear:

- The 1.5 profile without ``functional-memory-transfer`` refuses it.
- The 1.5 profile with ``functional-memory-transfer`` accepts it and
  changes nothing, so a set bit stays set.
- The legacy profile, with or without ``functional-memory-transfer``,
  accepts it and clears the status, and keeps the address.

Case ``memory-error-status-write`` in ``sgi-crime-test.c`` asserts the
first, second and third of these (the legacy profile without the
engine), and ``sgi-crime-error-test.c`` case ``clear-isolation`` and
``sgi-crime-mte-test.c`` case ``healthy-memory`` assert the legacy
profile with the engine. A nonzero write to the status register and any
write to the address register are refused in all three configurations of
``memory-error-status-write``.

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

The code is single-error-correcting and double-error-detecting. Eight
check bits cover each 64 data bits, so a 256-bit memory word has four
identical generate, correct and detect units. The controller generates ECC on
every write and, on a read, forms a syndrome from the data and the stored check
bits. A single-bit error is corrected. A multiple-bit error interrupts the
processor if the memory interrupt is enabled, and the client receives the
incorrect data with an error indication. An ECC error needs ECC enabled
(:src:`sgi-crime-1.5-spec PDF p. 130, 144-145`), *established*. A client can
turn detection and correction off for one read, which forces the syndrome to
zero. Generation on writes is unaffected
(:src:`sgi-crime-1.5-spec PDF p. 144`), *established*.

The syndrome register (``0x260``) and the check-bit register (``0x268``)
each hold four byte lanes, bits 7:0 for memory word bits 63:0 up to bits
31:24 for bits 255:192, read-only with reset 0
(:src:`sgi-crime-1.5-spec PDF p. 154`), *established*. The specification's
text says the check-bit register holds the bits read from memory after any
read, and its tables call the register the generated check bits
(:src:`sgi-crime-1.5-spec PDF p. 144, 150, 154`). Which it holds is *unknown*.

A client that writes less than 64 bits makes the controller read the
word, merge the new bytes under the client's mask, generate ECC for the
result and write both back. The mask goes to memory unaltered, so the
merged data does not rewrite the bytes the client did not write. A
read-modify-write cannot scrub memory
(:src:`sgi-crime-1.5-spec PDF p. 145`), *established*.

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

- The default ID is ``0xb1``, the specification's CRIME 1.5 value
  (:src:`sgi-crime-1.5-spec PDF p. 114`), an *approximation* for a shipped
  O2: both published captures give ``0xa1`` in the low byte. The legacy profile
  presents ``0xa1`` with a zero upper word, while the R12000 capture's
  upper word is ``0x1000``.
- The software interrupt register accepts fewer bits than the hardware.
  The watchdog, the processor and memory error detectors, ECC, refresh
  and the VICE interrupt are not modelled. Memory error bit 27 is never
  set.
- The control register ignores or rejects every write, so the watchdog
  enable, the reset bits and the coherent read limit field cannot be set.
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
  interrupts, reset and migration of pending interrupts, the memory error
  status write rule, the RAM aliases and their holes, populated-bank reset values, views, priority, remap,
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
