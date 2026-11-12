.. SPDX-License-Identifier: BSD-3-Clause

.. _sgi-xt-digvid:

============================================
XT-DIGVID: DMediaPro DM2 digital video board
============================================

Used in :ref:`sgi-ip30` (Octane2). The DMediaPro DM2 is SGI's uncompressed
high-definition and standard-definition video input and output board for
the Octane2 V10 and V12 workstations (:src:`007-4317-001 xiii`,
*established*), an :term:`XIO` option board that talks to an external SGI
Video Breakout Box (VBOB) over two LVDS cables
(:src:`007-4317-001 30`, *established*). That it presents one
:term:`widget` is *software expectation*, because the driver registers
one widget part (:ref:`sgi-xt-digvid-identity`). Its sibling, the DM3, is
the same product for the Onyx 3000 and Origin 3000
(:src:`007-4317-001 xiii`, *established*); this page covers the DM2 and says
where the DM3 is not covered. No machine in this tree carries the board.

.. seealso::

   Platform pages: :ref:`sgi-ip30`.

   Related parts: :doc:`xbow` (the port the board sits on),
   :doc:`bridge` (the one-wire identity part and the interrupt packet it
   shares with this board's design), :doc:`graphics` (the other XIO option
   boards of the Octane).

This is the hardware reference a model of the board is built from. It
stops at the board's XIO side: the VBOB behind the two cables is a virtual
endpoint that answers the board's control messages (:ref:`sgi-xt-digvid-vbob`),
and the video data path (packers, colour converters, lookup tables, the
video formatters and the link) is not run. Facts are rated as described in
:ref:`sgi-ratings`. The register map is not published. Everything below
the manual's block diagram comes from the behaviour of the shipped
software, so most entries are a *software expectation*, and where
software never reads a value the page says so and gives the value a model
takes as an *approximation*, with what would settle it.

.. _sgi-xt-digvid-sources:

Sources
=======

- *SGI DMediaPro DM2/DM3 Board Owner's Guide*, :src:`007-4317-001`. It
  gives the block structure, the link rates, the cabling, the ``hinv``
  line and the controls and events of the programming interface. It has
  no register map.
- The kernel driver object for the IP30 in SGI's DMediaPro DM2 and DM3
  software 1.1.3, :src:`dm2-driver-ip30-1.1.3`, and the user-space media
  module in the same set, :src:`dm2-library-1.1.3`, as software
  expectations: what the board has to accept for the driver to attach,
  for the module to load the board's FPGAs and for video to run. The
  same set's register diagnostic, :src:`dm2-diagnostic-1.1.3`, tests
  register read-back and gives the storage masks.
- The Discreet Flame 7.6 media module, :src:`dm2-library-flame-7.6`, an
  older build of the same module that Flame uses on the Octane2. Its
  differences from SGI's module that touch the board are in
  :ref:`sgi-xt-digvid-flame`.
- The VBOB's own user's guide is not a source of this page's facts; the
  VBOB side is the virtual endpoint's model choice.

Addresses after a driver or module citation are offsets into the object's
text section (driver) or virtual addresses in the linked library (module
and diagnostic). The IP27 and IP35 builds of the driver carry the same
constants for every point the page states, *inferred* from a
constant-by-constant comparison; the page describes the IP30 build.

.. _sgi-xt-digvid-identity:

Identity
========

.. list-table::
   :header-rows: 1

   * - Field
     - Value
     - Basis
   * - Product names
     - DMediaPro board (DM2, for Octane2); ``hinv`` class "XT-DIGVID
       Multi-standard Digital Video"
     - :src:`007-4317-001 27`, *established*.
   * - ``hinv`` line
     - ``XT-DIGVID Multi-standard Digital Video: controller 0, unit 0,
       version 0x0``
     - :src:`007-4317-001 27`, *established*. The trailing version is not
       the widget revision; the driver never reads a widget revision.
   * - Widget part number
     - ``0xc102``
     - The driver's registration call, :src:`dm2-driver-ip30-1.1.3 0xb8`,
       *software expectation*. The widget identification register's own
       read-back is *unknown*; nothing reads it.
   * - Widget manufacturer number
     - ``682``
     - Same call, *software expectation*.
   * - Widget revision
     - ``0``
     - *approximation*. Nothing reads the revision, so no source gives
       it. A capture of the identification register would settle it.
   * - Board assembly (``Part`` field of the NIC text)
     - ``030-1653-002`` in the model
     - The user-space module parses the ``Part`` field to choose between
       two image names for one FPGA (:ref:`sgi-xt-digvid-fpga-names`),
       :src:`dm2-library-1.1.3 0x4085f0`, *software expectation*. The
       value is an *approximation*: two published Octane2 inventories
       print it, but they are not registered as sources, so no citation
       supports it here.
   * - Slot
     - A :term:`Crossbow` port that takes an XIO option board. Published Octane2
       inventories place it at widget 10 beside a graphics board at 11,
       so the model default is widget 10.
     - *approximation*; the default is a model choice, any option-board
       port works.

The driver reads the board's one-wire identity part through a register at
window offset ``0x9c`` and requires the text ``Name:XT-DIGVID`` in it. It
also accepts the text ``page A data not available`` in its place, which
the driver names a workaround for a failed read,
:src:`dm2-driver-ip30-1.1.3 0x9f0`, *software expectation*. The part
behind the register is a one-wire EPROM of the kind the :term:`Bridge` carries
(:ref:`sgi-bridge-onewire`), *inferred* from the driver using the same
accessor family as the Bridge and :term:`IOC3` one-wire code. The bit layout of
the register is *unknown*: the diagnostic only tests bits 15:2 as plain
storage (:src:`dm2-diagnostic-1.1.3 0x10018808`, *software expectation*).
A model uses the Bridge's one-wire master layout at ``0x9c``, the
fixed-offset record of :ref:`sgi-bridge-onewire` with ``board-name``
``XT-DIGVID``, and a barcode that is a marked synthetic value, each an
*approximation*. A capture of the register while a read runs, or the
accessor routine's source layout, would settle it.

The driver reads the widget status word at ``0x0c`` once at attach and
prints a 400 MB/s warning when bit 5 is set,
:src:`dm2-driver-ip30-1.1.3 0xa7c`, *software expectation*. What bit 5
encodes is *unknown*. A model leaves it clear, an *approximation*; a
capture of the status word on an Octane2 would settle it.

.. _sgi-xt-digvid-window:

Window map
==========

The board answers 16 MiB of its widget space. The driver maps all
16 MiB, :src:`dm2-driver-ip30-1.1.3 0x884`, and the user-space module maps
the same 16 MiB at offset 0 when it loads FPGAs, :src:`dm2-library-1.1.3
0x4165f8`, each a *software expectation*. Every access software makes is a
32-bit word. The driver's register path refuses an offset at or above
``0xc00000``, so nothing is known about the top 4 MiB, *software
expectation*.

Registers sit in 64-bit slots of an 8-byte stride, and the 32-bit
register is the second word of its slot, the word at slot base plus 4.
Every offset in the tables below is that second word, matching the
:term:`Crosstalk` widget block's convention (:ref:`sgi-xbow-registers`); this
reading comes from every software access landing on an odd word,
*software expectation*. The first word of a slot is never accessed,
*software expectation*.

.. list-table::
   :header-rows: 1

   * - Window offset
     - Size
     - What it is
     - Rating
   * - ``0x000000`` to ``0x0000ff``
     - 256 bytes
     - The widget block and the XG's own registers
       (:ref:`sgi-xt-digvid-xg`).
     - *software expectation*
   * - ``0x100000``
     - one word per 16 KiB page
     - The map RAM (:ref:`sgi-xt-digvid-maps`).
     - *software expectation*
   * - ``0x120000``
     - one word
     - FPGA programming and status register
       (:ref:`sgi-xt-digvid-fpga`).
     - *software expectation*
   * - ``0x800000`` to ``0x803fff``
     - 16 KiB
     - The GIF register file: eight 2 KiB blocks, one per FPGA behind the
       GIF (:ref:`sgi-xt-digvid-gif`).
     - *software expectation*
   * - ``0x880000`` to ``0x883fff``
     - 16 KiB
     - Data ports of the eight blocks: the status word at ``0x800004``
       plus a block offset ``B`` has its data port at ``0x880004`` plus
       ``B``, which is the same offset with bit 19 set. The FPGA
       programming path sends configuration bytes there
       (:src:`dm2-library-1.1.3 0x417154`).
     - *software expectation*; that the two are one register seen
       through two addresses is *unknown*.
   * - everything else below ``0x1000000``
     - ``0x0e00000`` and gaps
     - Not touched by any shipped software.
     - *unknown*

A model answers 32-bit accesses at the documented words and refuses other
widths. For any other offset it reads zero and ignores a write, and logs
the access as unimplemented, an *approximation*. What the hardware does
for an unimplemented offset, and for a halfword or byte access, is
*unknown*; a capture would settle it.

Placement on the Crossbow
-------------------------

With a driver tunable at its default of one, attach sets one bit in the
Crossbow's arbitration word for the board's port: bit ``5 + 8*(widget &
3)`` of the register at Crossbow offset ``0x11c``, or ``0x124`` for
widgets 12 to 15 (the IP30 build writes the physical addresses of both),
:src:`dm2-driver-ip30-1.1.3 0xa7c`, *software expectation*. What the bit
means is *unknown*; the board does not see it. The Crossbow model has to
accept the write (:doc:`xbow`).

.. _sgi-xt-digvid-xg:

Widget block and XG registers
=============================

The XG is the board's bus ASIC: it interfaces the widget to the board's
internal 100 MHz bus, holds the map RAM and raises the board's
interrupts (:src:`007-4317-001 6`, *established*). The ASIC's technology
is *unknown*.

.. list-table::
   :header-rows: 1

   * - Offset
     - Name
     - Behaviour
     - Rating
   * - ``0x04``
     - Identification
     - Read only by the diagnostic, which splits it as revision bits
       31:28, part bits 27:12 and manufacturer bits 11:1; the part and
       manufacturer values are the registration pair above. Reads the
       identification word the Crosstalk block defines.
     - *software expectation* for the use; the read-back value is
       *unknown*, and the model's value, revision 0, part ``0xc102``
       and manufacturer ``682``, is an *approximation*.
   * - ``0x0c``
     - Status
     - Bit 5 read once at attach (:ref:`sgi-xt-digvid-identity`). Other
       bits are never read.
     - *software expectation*; the model reads zero, an *approximation*.
   * - ``0x24``
     - Control
     - Read, modified and written back. Attach writes the board's own
       widget number in bits 3:0, the interrupt target widget number in
       bits 25:22 and the constant bits 14 and 9 (``0x4200``), then sets
       bit 17, the XG error interrupt enable, after the interrupt
       destination is programmed. The error handler clears bit 17 and no
       software sets it again.
     - *software expectation*. The field names are *inferred* from the
       Crosstalk block and from the helper names; bits 14 and 9 are
       *unknown*. The model stores all 32 bits.
   * - ``0x34``
     - Interrupt destination, upper
     - Written once: address bits 47:32 in bits 15:0 and the target
       widget in bits 19:16.
     - *software expectation* for the value; the field split is
       *inferred*.
   * - ``0x3c``
     - Interrupt destination, lower
     - Written once: address bits 31:0.
     - *software expectation*.
   * - ``0x5c``
     - Interrupt vectors
     - Written once with video interrupt 0's vector in bits 7:0, video
       interrupt 1's in bits 16:9 and the error interrupt's in bits
       25:18, ORed with ``0x20100`` (bits 17 and 8).
     - *software expectation*. That bits 8 and 17 enable the two video
       interrupts is *inferred* from their position above each
       vector. The Crosstalk block's common role for this slot does not
       apply here.
   * - ``0x64``
     - Configuration
     - Read only. Bits 2:0 are a subsystem number that must read 1, bits
       5:3 a map RAM size code the driver prints as ``code * 32`` K, and
       bits 7:6 a map RAM type: 0 is 32K by 36 bits, 1 is 64K by 18 bits,
       2 is 128K by 9 bits, 3 is invalid.
     - The subsystem value and the type strings are *software
       expectation* (:src:`dm2-diagnostic-1.1.3 0x1000ab00`). The
       mapping of the size code is *unknown*. A model reads ``0x09``:
       subsystem 1, size code 1, type 0, giving 32 K entries; that is an
       *approximation*, and 32 K entries of four bytes fill exactly the
       gap up to ``0x120000``, which makes the choice *inferred* to be
       the real board's.
   * - ``0x6c``
     - Error cause
     - Read, then written back with the value read; the cause bits are
       in the next table. The driver also uses stores of zero to this
       register as a delay (below).
     - Read, write-back and decode are *software expectation*; that a
       write clears the bits written (write one to clear) is
       *inferred*, because a write-back of the read value cannot be told
       from a clear and the diagnostic writes zero to the register.
   * - ``0x74``
     - Water marks
     - Read, then written as: bits 21:16 and 27:22 kept, bits 15:0 set to
       ``0x3f40``, bits 31:28 cleared. The diagnostic tests bits 27:0 as
       storage.
     - *software expectation*; the meaning (graphics FIFO thresholds) is
       *inferred* from the register's name in the diagnostic.
   * - ``0x94``
     - Map RAM error address
     - Read after error cause bits 12 and 13 and printed as five hex
       digits, so the value is a 20-bit address.
     - *software expectation*; what address it holds is *unknown*. The
       model holds the offending entry's byte offset from the map RAM
       base, an *approximation*; a forced map RAM error on a real board
       would settle it.
   * - ``0x9c``
     - One-wire identity register
     - See :ref:`sgi-xt-digvid-identity`.
     - *software expectation*
   * - ``0x10``, ``0x18``, ``0x28``, ``0x40``, ``0x48``, ``0x50``
     - The Crosstalk block's error address, request timeout, error
       command, link configuration and flush slots
     - Never touched by any shipped software.
     - *unknown*; the model stores written values and reads zero at
       reset, an *approximation*.

Stores of zero to ``0x6c`` are the user-space module's only delay: its
wait helper issues one store per 20 delay units (the count is
``(units + 19) / 20``), and every wait on the FPGA load path runs through
it, :src:`dm2-library-1.1.3 0x4164f0`, *software expectation*. A load
issues up to 200,000 of them. A model therefore treats a write of zero to
``0x6c`` as a harmless, cheap, no-effect clear. It must not log it as an
unexpected access.

Error cause bits
----------------

The driver's error handler decodes the bits below and prints a message
per set bit, :src:`dm2-driver-ip30-1.1.3 0x3d180`. The bit numbers are
*software expectation*; the meaning is the message's, *inferred*. The
board's internal bus is the 100 MHz bus of :src:`007-4317-001 6`.

.. list-table::
   :header-rows: 1

   * - Bits
     - Cause
   * - 0
     - internal-bus overlap
   * - 1, 2
     - internal-bus PIO data response overflow, PIO command response
       overflow
   * - 3, 4, 5
     - internal-bus valid while idle, bad command, bad interrupt
   * - 6, 7, 8, 9, 10
     - Crosstalk address, bad command, read request timeout, response,
       request
   * - 11
     - graphics FIFO overflow
   * - 12, 13
     - map RAM parity; map RAM invalid entry (both read ``0x94``)
   * - 14, 15, 16
     - internal-bus command parity, data parity, parity
   * - 17, 18
     - Crosstalk request credit overflow, underflow
   * - 19, 20
     - Crosstalk response credit overflow, underflow
   * - 21, 22
     - graphics cache-line credit overflow, underflow
   * - 23
     - graphics double-word credit overflow
   * - 25, 26
     - command FIFO overflow, output DMA FIFO overflow
   * - 27, 28
     - input DMA response size, input DMA response invalid tag
   * - 29, 30
     - internal-bus write too few, Crosstalk write to a full input FIFO

The handler tests bit 22 twice and prints both the cache-line underflow
and the double-word underflow message for it, and never tests bit 24.
Bit 24 is most likely the double-word underflow bit, *inferred* from the
sequence; what hardware does with it is *unknown*. Bit 31 is not tested.
The handler reads and clears the register in one visit, then clears
control bit 17 (which masks further error interrupts), and nothing
re-enables it, :src:`dm2-driver-ip30-1.1.3 0x3d180`, *software
expectation*.

A model raises one cause: bit 13, when DMA translates through a map RAM
entry that is not valid. It never raises the others, an *approximation*;
raising credit, parity or FIFO errors would need a hardware reference
for when they occur.

.. _sgi-xt-digvid-maps:

Map RAM
=======

DMA from the board never uses a host address directly. The board has its
own flat DMA address space split into 16 KiB pages, and the map RAM holds
one 32-bit entry per page at window offset ``0x100000 + 4*(address >>
14)``. Entries are written with a single 32-bit store each, without
byte swapping, :src:`dm2-driver-ip30-1.1.3 0x2e3ec`, *software
expectation*.

.. list-table::
   :header-rows: 1

   * - Bits
     - Written value
     - Rating
   * - 31:30
     - 0
     - *software expectation*
   * - 29
     - 1 in every valid entry
     - *software expectation*
   * - 28
     - 0 in a valid entry; 1 in an invalidated one
     - *software expectation*
   * - 27:2
     - bits 39:14 of the system DMA address of the page
     - *software expectation* for the field; that it is address bits
       39:14 is *inferred*, because the driver stores the page number the
       kernel's DMA mapping returns shifted down by 14.
   * - 1:0
     - 0
     - *software expectation*; whether hardware gives them any meaning
       is *unknown*.

An invalidated entry is the word ``0x10000000`` for a single page, and
for a run of ``n`` pages the ``i``-th entry is ``0x10000000 | (n - 1 -
i)``, :src:`dm2-driver-ip30-1.1.3 0x2e380`, *software expectation*. Which
bit pattern hardware tests for "invalid", the byte order the board
applies to a map RAM store and whether bits 1:0 or 31:30 are attributes
are *unknown*; nothing reads an entry back.

A model reads an entry as written, treats an entry with bit 29 set and
bit 28 clear as valid and any other as invalid, and sends the resulting
host address as a Crosstalk read or write request through the Crossbow, an
*approximation*. A translation through an invalid entry raises error
cause bit 13 and records the entry's byte offset in ``0x94``
(:ref:`sgi-xt-digvid-xg`). Reset values of the entries are *unknown*; the
model resets them to zero, an *approximation*, which makes them invalid.

Driver conventions a model must keep working:

- A pinned user buffer is mapped as a list of 16 KiB pages with no
  offset in the first page, so the driver is correct only for 16 KiB
  aligned buffers (the page count is ``(size + 16383) >> 14``),
  :src:`dm2-driver-ip30-1.1.3 0x216b0`, *inferred* from the loop.
- A slot's mapping is at most 1024 pages (16 MiB); a larger transfer is
  refused by the driver, :src:`dm2-driver-ip30-1.1.3 0x210e0`, *software
  expectation*.
- After mapping a buffer the driver invalidates the page before it (at
  the buffer's board address minus 16 KiB, when at least 16 KiB) and the
  page after it, :src:`dm2-driver-ip30-1.1.3 0x254e4`, *software
  expectation*. For a full 1024-page buffer in slot 0 of pipe 0 the
  "before" page is the last page of slot 3 of pipe 1, so that entry is
  overwritten; whether any caller fills a slot is *unknown*.

.. _sgi-xt-digvid-fpga:

FPGA programming
================

The board's configuration is eight Xilinx FPGA images, not CPU firmware:
the GIF, a packer, an unpacker, an output and an input colour converter
(OXCC, IXCC), the video input formatter (VIF), the HDOC output formatter
and the CLINK link controller (:src:`007-4317-001 6`, *established* for
the blocks). The user-space module loads all eight when a process opens
the device, and four of them again whenever a path is committed with a
different format, :src:`dm2-library-1.1.3 0x407880`, *software
expectation*. A model does not run the images. It has to behave, at the
registers, so that every check the module makes passes.

The programming register
------------------------

One register, window offset ``0x120000``, programs the GIF directly and
reports configuration status for all eight FPGAs.

.. list-table::
   :header-rows: 1

   * - Access
     - Layout
     - Rating
   * - Read
     - A 16-bit word of DONE and INIT pairs: DONE at the even bit, INIT at
       the odd bit. The GIF is bits 0 and 1, packer 2 and 3, unpacker 4
       and 5, input colour converter 6 and 7, output colour converter 8
       and 9, VIF 10 and 11, HDOC 12 and 13, CLINK 14 and 15. INIT reads
       1 when not asserted: the module's message names it ``INIT low``.
     - *software expectation*, :src:`dm2-library-1.1.3 0x41fca8`; the
       signal polarity is *inferred* from the message.
   * - Write (GIF only)
     - Bits 7:0 a data byte, bit 8 a clock, bit 9 a strobe that is high
       when idle and while data is clocked.
     - *software expectation* for the values; the names are *inferred*
       from the pulse shapes.

Where the DONE and INIT bits of an FPGA sit, which port offset its data
uses, its select word and its image size are fixed by a table in the
module, :src:`dm2-library-1.1.3 0x41fca8`, *software expectation*:

.. list-table::
   :header-rows: 1

   * - FPGA
     - Kind
     - Image bytes
     - Select word
     - Port offset
     - DONE, INIT bits
   * - GIF
     - 1
     - 179233
     - ``0x00``
     - ``0x0000``
     - ``0x0001``, ``0x0002``
   * - Packer
     - 0
     - 104066
     - ``0x01``
     - ``0x0800``
     - ``0x0004``, ``0x0008``
   * - Unpacker
     - 0
     - 104066
     - ``0x02``
     - ``0x1000``
     - ``0x0010``, ``0x0020``
   * - Output colour converter
     - 2
     - 318256
     - ``0x20``
     - ``0x3000``
     - ``0x0100``, ``0x0200``
   * - Input colour converter
     - 2
     - 318256
     - ``0x10``
     - ``0x2800``
     - ``0x0040``, ``0x0080``
   * - HDOC
     - 0
     - 104066
     - ``0x08``
     - ``0x2000``
     - ``0x1000``, ``0x2000``
   * - VIF
     - 0
     - 104066
     - ``0x04``
     - ``0x1800``
     - ``0x0400``, ``0x0800``
   * - CLINK
     - 0
     - 35428
     - ``0x40``
     - ``0x3800``
     - ``0x4000``, ``0x8000``

Each of the seven FPGAs behind the GIF owns the 2 KiB block of the GIF
register file at its port offset, *inferred* from the offsets stepping by
``0x800``. The block layout is in :ref:`sgi-xt-digvid-gif`.

GIF load
--------

All stores are 32-bit and none is followed by a read that the model has
to time, :src:`dm2-library-1.1.3 0x4169c0`, *software expectation*.

.. list-table::
   :header-rows: 1

   * - Step
     - Software does
     - The device must
   * - 1
     - Stores ``0x300``, ``0x100``, ``0x300`` to ``0x120000``, with waits.
     - Treat the strobe's low period (bit 9 low, with the clock bit set) as a
       reset of the GIF configuration: clear GIF DONE, release INIT.
   * - 2
     - Reads the register once: DONE (bit 0) must be 0, or the module
       fails the load.
     - Report DONE 0 after step 1.
   * - 3
     - Waits 8800 delay stores, then reads up to 1000 times, 4000 delay
       units apart, until INIT (bit 1) reads 1.
     - Report INIT 1 no later than the first read after the strobe is
       high again.
   * - 4
     - Stores one clock pair with data 0: ``0x200`` then ``0x300``.
     - Count it as the first clock.
   * - 5
     - For each image byte ``b``, eight times: stores ``b | 0x200``, then
       ``b | 0x300``.
     - Count one clock per pair. The byte is written eight times, not
       shifted, which fits one bit per clock taken in an order the board
       selects; the image's embedded 24-bit length counts 8 * 179233 - 7
       bits, and the module sends 18 more clocks than that count (the
       full total is 1 + 8 * 179233 + 10 = 1,433,875 pairs).
   * - 6
     - Reads the register after the last byte: INIT must still be 1.
     - Keep INIT 1 (no configuration error).
   * - 7
     - Ten more clock pairs with data 0, then up to 10 reads, a wait of
       120 delay units before each, until DONE reads 1.
     - Report DONE 1 once configuration has finished.

The sequence, the counts and the 24-bit length rule are *software
expectation*; that the length field of the image is a bit count the board
consumes is *inferred* (it is 7 below 8 times the byte count in every
shipped image, and the module checks that, :src:`dm2-library-1.1.3
0x417750`). The module also refuses an image whose recorded length
differs from 8 times its size by 9 or more, so the check is in software,
not at the device.

A model completes the GIF load after counting clocks: it sets DONE once at
least ``8 * 179233`` clock pairs have arrived since the strobe, and
holds INIT 1 throughout, an *approximation*. The board's real DONE
condition (the header's length count, a CRC, a start-up sequence) is
*unknown*; the configuration port's logic is in no shipped software, and
a capture of DONE against a short or over-long stream would settle it.
A model does not parse the image. The model accepts a stream of any
length without failing, so a bad image is never rejected by the board, also
an *approximation*.

Loading the other seven
-----------------------

The seven FPGAs behind the GIF load through the GIF's register file and
need the GIF configured first, :src:`dm2-library-1.1.3 0x416e00`,
*software expectation*. With ``sel`` the select word, ``B`` the port
offset and ``D``, ``I`` the DONE and INIT bits of the table:

.. list-table::
   :header-rows: 1

   * - Step
     - Software does
     - The device must
   * - 1
     - At first load only: reads ``0x120000``; GIF DONE (bit 0) must be 1.
     - Report GIF DONE 1 after a GIF load.
   * - 2
     - Stores ``sel | 0x80``, ``sel``, ``sel | 0x80`` to ``0x80000c``,
       with waits of 300 and 400 delay units.
     - Treat the low period of bit 7 with select ``sel`` as a reset of
       that FPGA: clear its DONE, release its INIT.
   * - 3
     - At first load only: reads ``0x120000``; ``D`` must be 0.
     - Report DONE 0 after step 2.
   * - 4
     - Waits 8800 delay stores. At first load, reads up to 1000 times
       until ``I`` is 1; on a reload waits 200,000 delay stores and does
       not read.
     - Report INIT 1.
   * - 5
     - Per image byte, kind 0 FPGAs: reads ``0x800004 + B`` up to 10 times
       with no wait until bit 7 reads 1, then stores the byte (as a word)
       to ``0x880004 + B``. Kind 2 FPGAs (the colour converters): stores
       the byte to ``0x800004 + B`` with no read.
     - Report bit 7 (READY) as 1 on every read of a kind 0 port. Accept
       the store.
   * - 6
     - Before the final byte, at first load: ``I`` must still read 1.
     - Keep INIT 1.
   * - 7
     - At first load: reads ``0x120000`` up to 10 times with a wait of 100
       delay units until ``D`` reads 1. On a reload waits 1000 delay
       stores and does not read.
     - Report DONE 1 after the last byte.

A model sets an FPGA's DONE once it has received the table's byte count
for that FPGA since the last reset pulse, and keeps READY at 1, an
*approximation*; what READY means electrically and how long it stays low
after a byte are *unknown*. Kind 2 sends no READY poll, and whether
those two FPGAs report READY is *unknown*. Both are settled by the
receiving logic or a capture.

Reloads (a path committed with a different format) read nothing from
``0x120000``; success is the fixed waits plus the byte handshake above,
so the model must accept a reload at any time after the first load, with
the registers of the loaded FPGAs and the GIF unchanged except for the
FPGA's own block.

Load order and reset
--------------------

At open the module loads the GIF, issues a driver reset with argument 3,
loads CLINK, HDOC, OXCC, UNPACKER, VIF, PACKER and IXCC in that order, issues
a second driver reset with argument 4, then resets the CLINK UART by
storing 1 then 0 to the CLINK UART reset register,
:src:`dm2-library-1.1.3 0x407880`, *software expectation*. The driver
handler for the reset calls the GIF initialisation (:ref:`sgi-xt-digvid-gif`)
when bit 1 of the argument is clear, and bit 2 selects its re-initialise
flag. Argument 3 has bit 1 set and so skips it; argument 4 reaches it with
the flag set, which is the path that re-runs the GIF initialisation after
the FPGA loads, :src:`dm2-driver-ip30-1.1.3 0xe428`, *software
expectation*. GIF
initialisation reads GIF DONE (bit 0 of ``0x120000``) and the GIF
identification word, which is why the GIF image must have loaded before
everything else, :src:`dm2-driver-ip30-1.1.3 0x599c`, *software
expectation*.

.. _sgi-xt-digvid-fpga-names:

Image selection
---------------

The images are Intel HEX files whose byte totals equal the table's sizes.
Which file each FPGA gets depends on software state, not on the device:
the packer and unpacker image per pixel packing and sampling, the HDOC
image by the ``Part`` field (the standard variant for part 1653 with a
dash number below 3, a fast variant for dash number 3 or later or for any
other part number), :src:`dm2-library-1.1.3 0x4085f0`, *software expectation*. All
of them are the same size per FPGA, so a model does not distinguish them.
Image names, bitstream internals and the FPGA part numbers are *unknown*
and not needed.

.. _sgi-xt-digvid-gif:

GIF register file
=================

The GIF is the FPGA that holds the DMA engines, the local bus controller
and the PIO unit, and that controls all other FPGAs
(:src:`007-4317-001 6`, *established*). Its register file is at window
offset ``0x800000``. Following the convention of
:ref:`sgi-xt-digvid-window`, ``0x800000 + R + 4`` is register ``R``, and
``R`` is a multiple of 8. The file is a set of 2 KiB blocks, *software
expectation* (inferred from every offset sitting in its block):

.. list-table::
   :header-rows: 1

   * - Block base ``R``
     - FPGA
   * - ``0x0000``
     - GIF core and the two DMA pipes
   * - ``0x0800``
     - Packer
   * - ``0x1000``
     - Unpacker
   * - ``0x1800``
     - VIF
   * - ``0x2000``
     - HDOC
   * - ``0x2800``
     - Input colour converter
   * - ``0x3000``
     - Output colour converter
   * - ``0x3800``
     - CLINK

The block order is not the module's programming order, so a block's base
cannot be derived from the programming number.

Reads and writes before the GIF is configured are *unknown*. A model
answers them as an unimplemented offset (reads zero, writes ignored,
logged) until GIF DONE is set, an *approximation*.

Reset and storage
-----------------

Hardware reset values of every GIF register are *unknown*: software
never reads a register before writing it, except for the two
read-modify-write cases the page names and the identification words. A
model resets every register of the file to zero, an *approximation*, and
treats each register below as plain storage limited to its mask, except
where the page states a behaviour. The storage masks come from the
diagnostic's read-back test, which writes six patterns and compares
``read & mask`` with ``write & mask`` (:src:`dm2-diagnostic-1.1.3
0x1000cd08`, *software expectation*): the masks tell which bits store,
and a mask of zero means the diagnostic does not test the register, not
that it is read-only. A model implements a mask by ignoring writes to
other bits and reading them as zero, an *approximation*. No GIF register
has a read-to-clear or write-one-to-clear use anywhere in the shipped
software.

Core and the two pipes
----------------------

Two pipes, one per direction, each with its own state record, four DMA
slots and a status ring (:ref:`sgi-xt-digvid-dma`). **Pipe 0** is video
output (host memory to board, driven through HDOC) and **pipe 1** video
input (board to host memory, through VIF), *inferred* from the driver's
own names for the two paths and from the XG's input DMA error causes, which
name the engine that reads host memory. The pipe registers are:

.. list-table::
   :header-rows: 1

   * - Window offset
     - Pipe 0 (out)
     - Pipe 1 (in)
     - Behaviour
   * - DMA control
     - ``0x800014``
     - ``0x80003c``
     - See :ref:`sgi-xt-digvid-control`. Storage mask: pipe 0
       ``0x1e7fe0``, pipe 1 ``0x7fe0``, for the bits the diagnostic can
       store (the valid, abort and interrupt enable bits are outside it).
   * - Start address 1
     - ``0x80001c``
     - ``0x800044``
     - 24-bit offset inside the slot window, written with a value below
       ``0x1000000``, mask ``0xfffff8``.
   * - Start address 2
     - ``0x800024``
     - ``0x80004c``
     - As start address 1. That it addresses the second field of an
       interlaced frame is *inferred*.
   * - Video control
     - ``0x80002c``
     - ``0x800054``
     - 26 bits, mask ``0x3ffffff``: bits 11:0 and 23:12 are two counts of
       the raster geometry and bits 24 and 25 are flags.
   * - Video control 2
     - ``0x800034``
     - ``0x80005c``
     - 12 bits, *inferred* from the diagnostic's mask.
   * - Status ring pointer reset
     - ``0x800074``
     - ``0x800064``
     - A store of 0 restarts the pipe's status ring at entry 0
       (:ref:`sgi-xt-digvid-ring`).
   * - Pipe reset
     - ``0x80007c``
     - ``0x80006c``
     - A store of 0 at stream start; the completion counter restarts
       (see below).

Which pipe owns each of the paired words at ``0x64``, ``0x6c``,
``0x74`` and ``0x7c`` is given by three code sites that agree,
:src:`dm2-driver-ip30-1.1.3 0xbfe4`, *software expectation*; the VBOB
write handler's timeout path uses the same selection. Their meaning is
*unknown*, *software expectation* for the stores.

The raster registers are written by raster configuration with formulas
from the video timing: video control is ``(a & 0xfff) | ((b << 12) &
0xfff000) | (c << 24) | (d << 25)`` where ``c`` is set when a 16-bit
field of the pipe record is zero and ``d`` when another is non-zero,
:src:`dm2-driver-ip30-1.1.3 0x7d9c`, *software expectation*; what the
counts and flags mean is *unknown*.

Other core words:

.. list-table::
   :header-rows: 1

   * - Window offset
     - Name
     - Behaviour
     - Rating
   * - ``0x800004``
     - Core reset and enable
     - Stored 0 when the GIF is re-initialised. Plain storage.
     - *software expectation*; bit meaning *unknown*.
   * - ``0x80000c``
     - FPGA select
     - See :ref:`sgi-xt-digvid-fpga`. Select bits 6:0, bit 7 the
       strobe.
     - *software expectation*
   * - ``0x800084``
     - GIF identification
     - Read only. Bits 31:8 read ``0x474946`` ("GIF") and bits 7:0 are the
       GIF version, printed by the driver. Any other upper value fails
       attach.
     - *software expectation*. The version is *unknown*: a model reads
       ``0x47494601``, an *approximation*, since only the upper 24 bits
       are tested.
   * - ``0x80008c``
     - UST and media stream counter
     - Stored 0 immediately before the host UST is sampled. The role is
       the board's own timestamp counter, *inferred* from the pattern and
       the driver treating it as the board's own clock. A model keeps it as storage and
       does not count.
     - *software expectation* for the store; *approximation* for the
       model.

Packer, unpacker, VIF, HDOC and the colour converters
-----------------------------------------------------

The user-space module writes the following constants at open, one set per
pipe, :src:`dm2-library-1.1.3 0x4087e0`, *software expectation*. With
``v`` equal to ``0x1000`` for pipe 1 and ``0x1200`` for pipe 0:

.. list-table::
   :header-rows: 1

   * - Window offset
     - Value
     - Pipe
   * - ``0x800844``, ``0x80084c``
     - ``v``, ``0x1000``
     - 1 (packer block)
   * - ``0x800854``
     - ``0xd00``
     - 1
   * - ``0x800814``, ``0x800804``, ``0x80080c``
     - ``0x1f0008``
     - 1
   * - ``0x800824``
     - ``0x1``, mask ``0x5``
     - 1
   * - ``0x801044``
     - ``v``
     - 0 (unpacker block)
   * - ``0x801004``
     - ``0x160008``
     - 0
   * - ``0x80104c``
     - ``0xd00``
     - 0
   * - ``0x80100c``
     - ``0x32001f``
     - 0
   * - ``0x80101c``
     - ``0x0fffe0f9 | (flag << 28)``, mask ``0xffd``
     - 0

The role of each bit is *unknown*; the values do not depend on the pixel
packing. A model stores them as the mask allows and does not interpret
them.

The VIF block (video input) is programmed at path commit, :src:`dm2-library-1.1.3
0x409e50`, *software expectation*:

.. list-table::
   :header-rows: 1

   * - Window offset
     - Register
     - Mask
     - Written by software as
   * - ``0x801804``
     - control
     - ``0x3fff``
     - ``(x & 1) | (flag ? 0x10 : 0x20) | (flag << 6 & 0x40) | (w << 11 &
       0x800) | (nz << 12 & 0x1000) | 0x700``
   * - ``0x80180c``
     - enable
     - ``0x3``
     - the pipe 1 video-control shadow; bit 0 is the video enable, set at
       the first input buffer and cleared at the end
   * - ``0x801814``
     - scratch
     - none
     - written by the diagnostic between its register tests; the module
       never writes it
   * - ``0x80182c``
     - input mux
     - ``0xff``
     - ``(a & 7) | (b << 3 & 0x18) | ((c == 10) << 5) | (d << 7 & 0x80)``
   * - ``0x801884``, ``0x80188c``
     - active video, fields 0 and 1
     - 22 bits
     - two 11-bit line numbers
   * - ``0x801894``
     - lines per frame
     - none
     - total lines minus one (1124 for a 1125-line format)

``flag`` is 1 for standard-definition timing and ``w``, ``nz``, ``x``, ``a``
to ``d`` are format-table fields; the meaning of every bit is *unknown*.

The HDOC block (video output) is programmed at open and at path commit,
:src:`dm2-library-1.1.3 0x4087e0`, *software expectation*. Its 25
registers and the library's formulas, with the diagnostic's masks:

.. list-table::
   :header-rows: 1

   * - Window offset
     - Register
     - Mask
     - Software writes
   * - ``0x802004``
     - status
     - ``0x3ffffff``
     - read-modify-write: bit 24 set before the other HDOC registers are
       written and cleared after; a path-built word with bit 17 set last
       (cleared instead on a board whose part number is 1653 with a
       dash number of 3 or more)
   * - ``0x80200c``
     - enable
     - ``0x1``
     - the pipe 0 video-control shadow; bit 0 is the video enable
   * - ``0x802014``
     - format
     - ``0x7fffff``
     - ``((total lines - 1) << 12 & 0x7ff000) | ((samples per line - 1) &
       0xfff)``
   * - ``0x80201c``
     - colour converter read enable
     - ``0xffffff``
     - ``((active width + 41) & 0xfff) | 0x29000``
   * - ``0x802024``
     - horizontal blanking codes
     - ``0xffffff``
     - ``((active width + (flag ? 4 : 8)) << 12 & 0xfff000) | (flag ? 2 : 4)``
   * - ``0x80202c``, ``0x802034``, ``0x80203c``, ``0x802044``, ``0x80204c``
     - active video and vertical blanking for each field, and field
     - ``0x3fffff``
     - two 11-bit line numbers
   * - ``0x802054``
     - colour converter read enable 2
     - ``0xffffff``
     - two 12-bit fields
   * - ``0x80205c``
     - timecode
     - ``0x7fffff``
     - not written by the module
   * - ``0x802064``
     - position re-sync
     - ``0x7fffff``
     - read once at open; written with a 12-bit sample position and an
       11-bit line position
   * - ``0x80206c``
     - position re-sync window
     - ``0x7fffff``
     - the same position moved by 7 samples
   * - ``0x802074``
     - data read enable
     - ``0xffffff``
     - ``((active width + (flag ? 1 : 2)) & 0xfff) | (flag ? 0x1000 :
       0x2000)``
   * - ``0x80207c``, ``0x802084``
     - FIFO write enables
     - ``0xffffff``
     - not written by the module
   * - ``0x8020b4`` to ``0x8020ec``
     - test patterns, default Y and C
     - none
     - diagnostic only

The position re-sync register is the one HDOC read the module makes: it reads the
value, passes the position fields to its driver, and then writes
back. A model keeps both as storage with the 23-bit mask; a read returns
the last write or zero at reset, an *approximation*. The timecode, FIFO
and diagnostic registers have names and masks (*software expectation*)
and no use.

The diagnostic's layout (:src:`dm2-diagnostic-1.1.3 0x10018808`,
*software expectation*) puts control 0 (mask ``0x1ff``), control 1 (mask
``0xff``), a lookup table control (mask ``0x1f``) and a lookup table data
register at ``0x802804``, ``0x80280c``, ``0x802814`` and ``0x80281c`` for
the input converter, and at ``0x803004``, ``0x80300c`` (mask ``0x3ff``),
``0x803014`` and ``0x80301c`` for the output converter. Those offsets
overlap the module's layout, so the two are separate address spaces
selected by the loaded image.

The colour converters have two register layouts, one per image. The
module loads the "programmable" images and writes the first layout below.
The diagnostic loads two other images and tests the second. A model
implements the first, an *approximation*, because the module is the
software that runs the board. Which layout belongs to which image is
read from who loads it, so it is *inferred*.

.. list-table::
   :header-rows: 1

   * - Window offset
     - Input converter (module)
     - Output converter (module)
   * - ``0x802814`` to ``0x80282c``
     - red, green, blue, alpha offsets (full 32 bits)
     - at ``0x803014`` ... ``0x80303c``: red, green and blue offsets as
       low 16 bits and bit 16 in pairs, then ``around`` at ``0x803044``
   * - ``0x802834``
     - misc: ``(x == 0) | (x << 8 & 0x100)``
     - 8-bit packing at ``0x80304c``: 1 for 8-bit components
   * - ``0x80283c``, ``0x802844``
     - alpha
     - decimation at ``0x803054``
   * - ``0x80284c``, ``0x802854``, ``0x80285c``
     - coefficient low 16 bits, bit 16, index (latches on the index write)
     - the same triple at ``0x80305c``, ``0x803064``, ``0x80306c``

Each coefficient set is ten matrix words and seven filter taps (``0x285,
-200, 0x68, -59, 0x21, -17, 6``), each written low, high, index, with the
indices ``16 + i/3 + 4*(i%3)`` for the matrix's first nine words (a 3 by
3 matrix transposed), 28 for the tenth and ``i`` for tap ``i``
(:src:`dm2-library-1.1.3 0x41a3b0`, *software expectation*). That the
index write latches the pair is *inferred* from the order. A model
stores the writes and applies no colour conversion.

Lookup tables load through the control register at the block base
(``0x802804`` input, ``0x803004`` output) and a data register eight bytes
above. The module writes control 8, then control 2, 4 or 1 for component
0, 1, 2, then 8192 writes of the 32-bit entry to the data register, then
control 0, :src:`dm2-library-1.1.3 0x4070f0`, *software expectation*.
Whether the data register increments an index and what the control
values mean are *unknown*. A model accepts the 8192 writes in the
sequence, keeps nothing, and applies no table.

The CLINK block holds:

.. list-table::
   :header-rows: 1

   * - Window offset
     - Register
     - Behaviour
   * - ``0x803864``
     - pattern generator low, mask ``0xffff``
     - Written ``0xc3f7`` when the board runs without a VBOB.
   * - ``0x80386c``
     - pattern generator high, mask ``0xff``
     - Written 37 (``0x25``) in the same case.
   * - ``0x803874``
     - sync, mask ``0x1ff``
     - Diagnostic only.
   * - ``0x8038e4``
     - UART reset, mask ``0x1``
     - Written 1 then 0 at open and at path open.
   * - ``0x803804`` to ``0x80385c``
     - twelve transmit halfwords, slot stride 8
     - :ref:`sgi-xt-digvid-vbob`.
   * - ``0x8038ec``
     - transmit doorbell
     - :ref:`sgi-xt-digvid-vbob`.
   * - ``0x803884`` to ``0x8038dc``
     - twelve receive halfwords
     - :ref:`sgi-xt-digvid-vbob`.
   * - ``0x8038f4``
     - receive release
     - :ref:`sgi-xt-digvid-vbob`.

All rows are *software expectation*. When the board runs with the
VBOB support variable set, the module programs the pattern generator to
produce an on-board 50 MHz clock instead of taking the link's, which says
the board can run with no VBOB, *software expectation*; it has no source
or sink for video then.

.. _sgi-xt-digvid-dma:

DMA
===

Two pipes, each with four DMA slots and one status page. The driver
keeps a 16 MiB DMA mapping per slot, so a slot is a window of the board's
DMA address space 16 MiB wide, entered into the map RAM as a list of 16 KiB
pages (:ref:`sgi-xt-digvid-maps`), :src:`dm2-driver-ip30-1.1.3 0x15878`,
*software expectation*. The window of slot ``k`` is:

.. list-table::
   :header-rows: 1

   * - Pipe
     - Slot ``k`` window
     - Map RAM entries
   * - 1 (in)
     - ``k * 0x1000000``, up to ``0x3ffffff``
     - ``k * 1024`` to ``k * 1024 + 1023``
   * - 0 (out)
     - ``0x4000000 + k * 0x1000000``, up to ``0x7ffffff``
     - ``4096 + k * 1024`` and up

The formulas are *software expectation*; that the offset programmed in
the start-address registers is an offset inside the slot's window, and
that the window is implied by which slot's valid bit is raised, is
*inferred* from the 24-bit range of the start addresses equalling 16 MiB.

.. _sgi-xt-digvid-control:

DMA control word
----------------

The control word is ``0x800014`` (pipe 0) or ``0x80003c`` (pipe 1). The
driver keeps a shadow of every bit it keeps and rewrites the whole word,
:src:`dm2-driver-ip30-1.1.3 0x255ac`, *software expectation*.

.. list-table::
   :header-rows: 1

   * - Bits
     - Meaning
     - Rating
   * - 3:0
     - Per-slot valid bits. A write with a bit set arms the slot. The
       valid bits are never kept in the shadow, so they are write-one
       latches and two writes in a row with one bit each leave both
       slots armed.
     - Never kept in the shadow, *software expectation*. That they are
       latches is *inferred*; it is the single inference the start
       order rests on, and the reset value, the read-back and the effect
       of writing 0 to a valid bit are *unknown*. A capture of a running
       board's control word would settle it.
   * - 4
     - Abort. Set by the abort path: the shadow with bit 4 ORed in.
     - *software expectation*
   * - 16
     - Interrupt enable. Written with the valid bit at the start of a
       transfer and cleared at the end.
     - *inferred* from the driver's trace names; it is outside the
       diagnostic's store masks.
   * - 12, 10, 9
     - Set and cleared by raster configuration.
     - *software expectation*; roles *inferred*.
   * - other kept bits
     - Initial shadow ``0xea040`` (pipe 0), ``0xa040`` (pipe 1).
     - *software expectation* for the values, which the driver writes at
       GIF initialisation; they are driver-chosen, not reset values.

A model keeps bits 3:0 as latches that clear on slot completion or abort,
holds the abort bit, and reads the written word back for the bits in the
diagnostic's mask. The valid-latch behaviour, the clear on completion and
treating a valid bit that arrives while the slot is armed as a no-op are
an *approximation*. The model does not gate pipe interrupts on bit 16, and
stores it, an *approximation* (:ref:`sgi-xt-digvid-int`).

Start order
-----------

The driver streams buffers through the four slots in turn,
:src:`dm2-driver-ip30-1.1.3 0x1d758`, *software expectation*:

1. At stream start it stores 0 to the pipe reset word and sets control
   bit 16 (interrupt enable) with the shadow.
2. For each buffer it maps the pages into the slot's window and guard
   pages (:ref:`sgi-xt-digvid-maps`).
3. On the input pipe, for the first buffer, it sets bit 0 of the VIF
   enable (``0x80180c``) before arming the first slot. On the output pipe
   it arms the slot first and then sets bit 0 of the HDOC enable
   (``0x80200c``), which presumably fills the output FIFO first;
   that reason is *unknown*.
4. It arms the slot by one control write: ``(1 << slot) | shadow |
   0x10000``. In a replicate-a-field mode it holds the first slot, and when
   a second is pending writes two words in a row, each with one valid bit,
   to arm both. The flags that choose the mode are *unknown*.
5. At the end it clears the enable bit, stops interrupts, and, on the
   pipe on the IP27 and IP35 builds, stores the abort bit when it leaves
   with an error and the board reported none. The IP30 build does not
   store the abort there; whether that is deliberate is
   *unknown*. The abort bit is still reachable on the IP30 through the
   abort and signal-loss paths.

The driver never reads a board register to learn that a slot finished: it
sleeps on a per-slot semaphore that the interrupt side posts from the
status ring (:ref:`sgi-xt-digvid-ring`). A model completes a slot by
writing a status entry with the slot's DONE bit.

The data transfer
-----------------

The extent of a slot's transfer, the line order, the interleave and the
packing are fixed by the raster registers and the loaded packer and
unpacker, and none of them is interpreted by this page's sources; the
video path is not run. A model completes an armed slot on a field
boundary of its pipe, validates the map RAM entry for the page that holds
the slot's first start address, and transfers no payload bytes, an
*approximation*. Output frames are discarded and input buffers keep their
previous content. What a slot transfer reads and writes would be settled
by decoding the raster registers against the manual's packing tables
(:src:`007-4317-001 5`, :src:`007-4317-001 6`). The model's field period is that of the timing
the virtual VBOB was configured with, and 1001/60000 seconds when none has
been, also an *approximation*.

.. _sgi-xt-digvid-ring:

Status ring and interrupt acknowledgement
=========================================

The board reports progress by writing records into a ring in host memory;
it does not use registers for the cause of an interrupt,
:src:`dm2-driver-ip30-1.1.3 0x34b9c`, *software expectation*.

Each pipe has a ring of 16 entries of 0x80 bytes (2048 bytes) in one
host page. The ring's board DMA address is fixed at ``0x8000000`` for
pipe 0 and ``0x9000000`` for pipe 1; no register carries the address, so
the board takes it as built in and relies on the map RAM, which the driver
programs: one entry at ``0x108000`` (pipe 0) or ``0x109000`` (pipe 1), the
entry for the page before and the page after invalidated. That the
addresses are built in is *inferred* from the absence of any register
that would hold them. The driver does not check that the ring page is
16 KiB aligned, so a model must place the entry so the ring starts at the
page's low 2 KiB, *inferred*.

.. list-table::
   :header-rows: 1

   * - Entry offset
     - Content
     - Rating
   * - ``0x00``
     - A 32-bit word whose bits 23:0 are the board's media stream
       counter, a field counter that wraps at 24 bits. The driver extends
       it to 64 bits and requires bits 31:24 to read zero.
     - *software expectation* for the use; the name is *inferred*.
   * - ``0x04``
     - 32-bit flag word, bit 0 VALID; the other bits are in the next
       table.
     - *software expectation*
   * - ``0x08`` to ``0x7f``
     - Copied by the driver and never read.
     - *unknown*; a model writes zero.

The producer's protocol, from the driver's consumer, is: write the entry,
setting VALID last, advance to the next entry, wrapping from 15 to 0. The
driver reads an entry by atomically clearing VALID, copying the 0x80 bytes
and handling the flags; it stops at the first entry without VALID. The
hardware's head position is invisible to software, and its action on a
full ring, an entry that still has VALID set, is *unknown*. A model
writes entries in order, restarts at entry 0 when a pointer reset word
(``0x800064`` for pipe 1, ``0x800074`` for pipe 0) is written, and, when
the next entry still has VALID set, holds the pending entry until the
host clears it and raises the interrupt then, an *approximation*; a
capture of the ring under overrun would settle it. A model also writes
word ``0x00`` before the flag word, so a consumer that clears VALID and
copies sees whole entries.

.. list-table::
   :header-rows: 1

   * - Bit
     - Name
     - Used by the driver
   * - 0
     - VALID
     - entry present
   * - 1
     - flag 1
     - passed to completion as a boolean; meaning *unknown*
   * - 2
     - input off
     - pipe 1: input signal absent
   * - 3
     - input sync-code error
     - pipe 1: logged and ignored
   * - 4
     - overflow
     - pipe 0: a transfer error; pipe 1: sequence lost, logged
   * - 5
     - input change
     - pipe 1: input state changed
   * - 6
     - field parity
     - field parity, compared with the low bit of the driver's field
       counter at a field interrupt
   * - 7
     - field interrupt
     - field interrupt: counters, host timestamp, timestamp event
   * - 8 to 11
     - slot done 0 to 3
     - slot completed
   * - 12 to 15
     - slot aborted 0 to 3
     - slot aborted
   * - 16, 17
     - current-slot 0 and 1
     - named by the driver's dump, never tested
   * - 18
     - link transmit done
     - VBOB message acknowledge
   * - 19
     - link receive done
     - VBOB message received
   * - 20 to 31
     - none
     - not tested

The bit numbers, the table and the driver's actions are *software
expectation*, the names' hardware meaning is *inferred*, and flag 1, field
parity and current-slot are *unknown*. The driver handles slot-done and slot-aborted bits in
slot order starting at its own counter, so the board must report
completions in the order the slots were armed. A model reports slot done for
the slot it completes, field parity as the low bit of the number of field
interrupts it has raised on the pipe, flag 1 as 0 and the current-slot bits as
0, an *approximation*.

When the input pipe's signal changes the driver reads two bits: input change
with input off is a lost signal, input change alone a gained one, input off
alone is logged while DMA runs. The output pipe has no sync flag,
*software expectation*. A model raises input change|input off when the
virtual endpoint's input signal disappears and input change when it appears,
an *approximation* (:ref:`sgi-xt-digvid-vbob`).

.. _sgi-xt-digvid-int:

Interrupts
----------

The board raises three interrupts through the widget: error, video 0 and
video 1. Video 0 belongs to pipe 1 (in) and video 1 to pipe 0 (out),
:src:`dm2-driver-ip30-1.1.3 0x3dce8`, *software expectation*. The driver
programs the vectors and the destination address into ``0x5c``, ``0x34``
and ``0x3c`` (:ref:`sgi-xt-digvid-xg`), and writes no register to
acknowledge a video interrupt and reads no register to learn its cause.
It clears the in-service state in memory, so the interrupt is an ordinary
Crosstalk interrupt packet cleared by the host's interrupt controller,
*inferred*. The error interrupt reads and clears ``0x6c`` and masks itself
through control bit 17 (:ref:`sgi-xt-digvid-xg`).

A model sends the interrupt as a write of the vector to the destination
address, as the Bridge sends its interrupts (:ref:`sgi-bridge-interrupts`),
an *approximation*: the packet format on the board is *unknown*. It raises
a video interrupt for a pipe each time it writes a status entry for it,
when the vector word's enable bit for that pipe (bit 8 for video 0, bit 17
for video 1) is set, and raises the error interrupt for a non-zero cause
when control bit 17 is set, each an *approximation*. A transaction capture
of the widget would settle the packet and the gating.

.. _sgi-xt-digvid-vbob:

VBOB control channel
====================

All the host sees of the cables is a message channel and nothing else,
:src:`dm2-library-1.1.3 0x4105f0`, *software expectation*. The cables A
(transmit) and B (receive) run at seven times the video clock, 189 to
519.75 MHz for 27 to 74.25 MHz video, and the CLINK FPGA holds the link
control and status logic (:src:`007-4317-001 30`, *established*;
:src:`007-4317-001 6`, *established*).

Transmit
--------

The host writes a 24-byte message as twelve 32-bit stores to window
offset ``0x803804 + 8*k`` (``k`` from 0 to 11). Word ``k`` holds
bytes ``2k`` and ``2k+1`` as bits 15:8 and 7:0, zero extended. It then
stores 1 to the doorbell at ``0x8038ec``,
:src:`dm2-driver-ip30-1.1.3 0xb530`, *software expectation*. The driver
holds one lock per pipe and waits up to 250 clock ticks for the ack, then
resets the pipe's status ring pointer and its page, and returns an error
that the library retries once.

The device must raise link transmit done (:ref:`sgi-xt-digvid-ring`) after
the doorbell, within the 250 ticks, in the ring of the pipe the message
names. Which ring the real board uses, and whether the ack comes from the
CLINK transmitter or from the VBOB, are *unknown*. A model acks in the
ring of the pipe in byte 1 of the message at the next opportunity, an
*approximation*; a capture with the VBOB unplugged would settle it.

Receive
-------

A frame from the VBOB is twelve 32-bit words at ``0x803884 + 8*k``, the
low 16 bits of each holding two message bytes high then low, with
link receive done set in the ring. The driver reads the words, stores 1 to
``0x8038f4`` to release the buffer, and posts the frame as an event,
:src:`dm2-driver-ip30-1.1.3 0x35c00`, *software expectation*. The device
must not deposit a second frame until the host has stored 1 to
``0x8038f4`` (*inferred*). Which pipe's ring carries the flag is
*unknown*; a model uses the ring of the frame's pipe byte, an
*approximation*.

Frame format
------------

Every message builder fills a zero-initialised 24-byte frame,
:src:`dm2-library-1.1.3 0x40f130`, *software expectation*:

.. list-table::
   :header-rows: 1

   * - Byte
     - Content
   * - 0
     - count of bytes that follow (the last used index)
   * - 1
     - pipe: 0 output (cable A), 1 input (cable B)
   * - 2
     - register number; bit 7 set means a write
   * - 3 and up
     - data

That byte 1 selects a cable is *inferred*.

Messages the virtual endpoint accepts
-------------------------------------

Set messages are acknowledged with link transmit done only. Get messages are
answered with a frame. Module function addresses refer to
:src:`dm2-library-1.1.3`, *software expectation* for every row.

.. list-table::
   :header-rows: 1

   * - Reg
     - Dir
     - n
     - Body
     - Endpoint behaviour (*approximation* where it chooses a value)
   * - ``0x20``
     - W
     - 4
     - byte 3 = 1, byte 4 = 2 if the argument is non-zero else 1
     - Store the TV standard.
   * - ``0x21``
     - W
     - 3
     - byte 3 = ``8 | code`` in the high nibble when the second argument
       is 0, the low nibble when non-zero. Codes: internal 0, jack A 1,
       jack B 2, analog 3.
     - Store the source for each nibble. The second message overwrites the
       whole byte. Which nibble belongs to which pipe is *unknown*.
   * - ``0x22``
     - W
     - 18
     - timing record: byte 3 = 6 for SD (525 or 625), else 5; bytes 4 to
       17 are line and sample counts; byte 18 = 4 for a 74.18 MHz pixel
       clock, 5 for 74.25 MHz, 1 otherwise.
     - Store the timing and take the field period from it.
   * - ``0x24``, ``0x25``
     - W
     - 4
     - bytes 3 and 4 are routing codes 1 to 10; pipe 1 uses ``0x24``,
       pipe 0 ``0x25``
     - Store; no effect.
   * - ``0x25``
     - R
     - 3
     - byte 3 = route code
     - Reply with byte 0 non-zero and byte 4 = the stored routing code
       for the pipe.
   * - ``0x26``
     - W
     - 3
     - byte 3 = ``0x80`` if the argument is non-zero else 0
     - Store the sync-state enable.
   * - ``0x26``
     - R
     - 2
     - none
     - Reply with the sync-state frame below.
   * - ``0x27``
     - W
     - 3
     - byte 1 = 1; byte 3 = ``0x80`` if non-zero else 0
     - Store the input-signal enable.
   * - ``0x27``
     - R
     - 2
     - byte 1 = a context flag
     - Reply with the input-signal frame below.
   * - ``0x28``
     - W
     - 3
     - byte 3 = low nibble (argument != 1) | high nibble (jack control !=
       1)
     - Store; no effect.
   * - ``0x40``
     - R
     - 2
     - byte 1 = index
     - Reply with 24 bytes; no library path parses the reply.
       The model returns a frame whose byte 0 is 1, byte 1 is the
       index, byte 2 is ``0x40`` and the rest is zero.
   * - ``0x47``
     - W
     - 7
     - bytes 3 to 7 are the ASCII text ``RESET``
     - Reset the endpoint's state to power-on (internal genlock, no
       input signal, default routing).

The routing codes, the TV standard values and the info reply's content
are *unknown*; the board's side of the cables and the VBOB's firmware are
not described by any source this page uses. The reply frames follow.

A reply frame is matched to nothing: any received frame becomes the
answer to a pending get, the module waits 3 seconds (the unit is
*inferred*), and a reply whose byte 0 is zero counts as a failure. A
model answers within the same operation as the doorbell, so a get
completes promptly and the 3 second wait never fires, an *approximation*.
Frames the endpoint sends unrequested while a get is pending corrupt the
get; the model sends none while a get is outstanding, which is a
*software expectation* of the library's receive path
(:src:`dm2-library-1.1.3 0x4103a0`).

The sync-state reply (message ``0x26`` read): byte 3 bit 0 is genlock
LOCKED (1) or UNLOCKED (0) for the module's two defined selectors; byte 4,
5 or 6 is the detected timing code, depending on the selector; byte 3 bit
3 or bit 2 means "illegal combination", :src:`dm2-library-1.1.3
0x414cb8`, *software expectation*. The input-signal reply (message
``0x27`` read) carries six detected timing codes in bytes 4 to 9, one per
jack or pipe slot, :src:`dm2-library-1.1.3 0x40ee90`, *software
expectation*; what each slot means is *unknown*. The detected timing is
a wire code mapped to an ML timing number:

.. list-table::
   :header-rows: 1

   * - Wire
     - Timing
     - Wire
     - Timing
   * - 0
     - unknown
     - 9
     - 1080 29.97p
   * - 1
     - none
     - 10
     - 1080 30p
   * - 2
     - unknown
     - 11
     - 1080 23.98p
   * - 3
     - 525
     - 12
     - 1080 24p
   * - 4
     - 625
     - 13
     - 1080 23.98 PsF
   * - 5
     - 720 59.94p
     - 14
     - 1080 24 PsF
   * - 6
     - 720 60p
     - 15
     - 1080 25p
   * - 7
     - 1080 59.94i
     - 16
     - 1080 50i
   * - 8
     - 1080 60i
     - 17
     - 1035 59.94i

Codes 18 and above map to unknown, and no code maps to 1080 25 PsF, which
is a selectable source timing, :src:`dm2-library-1.1.3 0x4077c0`,
*software expectation*. The standard timings the board supports are those
of the manual's Table 1-1, "Supported Video Timings"
(:src:`007-4317-001 4`, *established*); the HD genlock input and output
pairs are Table 5-3 (:src:`007-4317-001 71`, *established*).

Genlock and lock reporting
--------------------------

Genlock lets the VBOB lock the output timing to an external reference:
SD output locks to a 525 or 625 reference on the VBOB's SD video or SD
genlock input; HD output locks to a supported HD reference on the HD
video input or HD genlock input (:src:`007-4317-001 70`, *established*). The
genlock source is one of digital jack A, jack B, analog or internal,
sent to the VBOB with message ``0x21``. The lock detector and the
generator are in the VBOB, which holds a genlock FPGA; no register on the
board is named for genlock, :src:`dm2-library-1.1.3 0x40ff20`, *software
expectation*. The module reports it to applications as an unlocked,
locked or unknown state and as an error status of none, no signal, unknown
signal, illegal combination or timing mismatch, derived from the sync-state
reply: ML timing none gives no signal, unknown gives unknown signal,
otherwise an illegal-combination bit gives illegal combination, else none;
timing mismatch is not derived from the reply,
:src:`dm2-library-1.1.3 0x40ce60`, *software expectation*. Lock change is
reported as the events sync lost and sync gained, and an input timing
that differs from the requested one produces the same events or an
error before a transfer starts (:src:`007-4317-001 70`, :src:`007-4317-001
82`, *established*).

Unrequested frames carry changes: a frame with byte 2 equal to ``0xa6``
or ``0xa7`` and byte 3 bit 7 set is consumed by the module's receive
thread and never answers a get, :src:`dm2-library-1.1.3 0x40cad0`,
*software expectation*. ``0xa6`` is a sync change: for one selector the
module reads byte 5 as the timing and byte 3 bit 0 as gained, for another
byte 4 and bit 1; a gained bit set posts a sync-gained event, a clear
bit reads the error status and posts a loss. ``0xa7`` is an input signal
change: the module reads byte 7 or 6, or byte 5 or 4, by selector, and
posts an input-changed event if the timing differs from its stored one.
That a set bit means signal good is *inferred*; which selector value
serves which pipe and format family is *unknown*.

The virtual endpoint's behaviour:

- It holds a genlock source per nibble and a detected input timing per
  jack slot, each a property, and a flag for each of four reference
  signals present, an *approximation*.
- Internal genlock is always locked, with the detected timing equal to the
  output timing configured by message ``0x22``.
- An external source is locked when a reference of a timing compatible
  with the configured output timing is present on that jack, and
  unlocked with the no-signal error otherwise. "Compatible" is the
  manual's list of supported genlock input timings (:src:`007-4317-001
  70`), with 525 and 625 valid only as a reference on the HD genlock input
  and never on the HD video input.
- When the locked state changes it sends an ``0xa6`` frame with byte 3 =
  ``0x80`` plus both bit 0 and bit 1 set when locked and neither when not,
  byte 2 = ``0xa6`` and bytes 4 and 5 both holding the detected timing
  code, so either selector reads the same answer. When an input signal
  appears or goes it sends an ``0xa7`` frame with byte 3 = ``0x80`` and
  bytes 4 to 7 all holding the detected timing code.
- It answers the sync-state reply with byte 3 bit 0 the lock state and
  bytes 4 to 6 the detected timing, and the input-signal reply with the
  six slots filled from the properties.
- With the default of no reference signals and no input signal, the
  output pipe runs from the internal timing and the input pipe produces
  no field interrupts.

All of it is an *approximation*: the real VBOB's reply layouts beyond the
bytes the library reads, its lock time and its notification rules are
*unknown*. The VBOB's own firmware (flashed by SGI's flash tools over the
same channel) and the manual's cabling table (:src:`007-4317-001 30`)
would settle them; the flashing protocol is not supported.

.. _sgi-xt-digvid-flame:

Discreet Flame's module
=======================

Discreet's Flame 7.6 OCT2 ships a DM2 module that is an older build of
SGI's module and drives the same board. It uses the same register
windows, the same eight FPGA records (byte for byte), the same GIF load
sequence and the same one 16 MiB mapping at offset 0 only while loading,
:src:`dm2-library-flame-7.6 0x5454500`, *software expectation*. The
differences that reach the device are:

- During a non-GIF FPGA load Flame's module waits 200 delay units
  between status polls where SGI's polls ten times back to back, so a
  model that reports READY at once satisfies both,
  :src:`dm2-library-flame-7.6 0x5454bb0`, *software expectation*.
- Flame's module loads a colour converter image chosen from a table at
  each path commit, instead of one image at open followed by register
  writes, so the colour converter FPGAs are loaded at any path commit,
  :src:`dm2-library-flame-7.6 0x5454940`, *software expectation*. That it
  therefore does not program the converters' coefficient registers is
  *inferred* from the missing messages and tables. A model that accepts a colour
  converter reload at any time, as it must for the VIF, HDOC, packer and
  unpacker, serves both.
- Flame's module does not issue the driver command that returns the
  board's NIC text; its image choice does not depend on the ``Part``
  field, *software expectation*.
- Both modules register the same widget part and manufacturer.

Which of Flame's device libraries opens a DM2 is *unknown*.

.. _sgi-xt-digvid-model-choices:

Model choices
=============

Every value below is the model's choice where no source gives one. Each is
an *approximation*; the last column says what would settle it.

.. list-table::
   :header-rows: 1

   * - Choice
     - Model value
     - What would settle it
   * - Widget revision, identification read-back
     - revision 0, part ``0xc102``, manufacturer ``682``
     - a capture of ``0x04``
   * - Status word, bit 5
     - clear
     - a capture of ``0x0c`` on an Octane2
   * - Reset value of every register not listed
     - zero
     - a power-on capture of the register file
   * - Configuration word ``0x64``
     - ``0x09`` (subsystem 1, 32 K entries, type 0)
     - a capture of ``0x64`` and the size-code table
   * - Map RAM entry reset, valid test, byte order
     - zero (invalid); valid is bit 29 set and bit 28 clear; stores read
       back as written
     - a forced map RAM error and a read of an entry
   * - Map RAM error address
     - the offending entry's byte offset from the map RAM base
     - a forced error and a read of ``0x94``
   * - One-wire identity register
     - the Bridge's one-wire master layout at ``0x9c``, a record named
       ``XT-DIGVID`` and the part ``030-1653-002``
     - the accessor's bit layout, a captured NIC read
   * - Unimplemented offsets and non-word accesses
     - read zero, write ignored, logged
     - a capture of a stray access
   * - GIF load completion
     - DONE after ``8 * 179233`` clocks since the strobe, INIT held 1,
       no parsing of the stream
     - the configuration port logic, a capture of DONE
   * - Other FPGA load completion
     - DONE after the table's byte count since the select pulse, READY
       always 1
     - the same capture with a short stream
   * - GIF identification version
     - ``0x47494601``
     - a capture of ``0x800084``
   * - Access to the GIF file before GIF DONE
     - read zero, write ignored, logged
     - a capture
   * - Control word valid bits
     - latches that clear on slot completion or abort; a valid bit for an
       armed slot is a no-op
     - a capture of the control word on a running board
   * - Interrupt gating
     - a status entry raises its pipe's video interrupt when the
       vector word's enable bit is set; control bit 16 is stored and
       not used; the error interrupt needs control bit 17
     - a transaction capture of the widget
   * - Interrupt packet
     - a write of the vector to the destination address
     - a transaction capture
   * - Slot completion and payload
     - one slot per field boundary in arming order; the first start
       address's page is checked against the map RAM; no payload bytes
     - decoding the raster registers against the packing tables
   * - Field period
     - that of the timing the endpoint was given, else 1001/60000 s
     - the manual's timing tables and a capture
   * - Status entry body and flags
     - word 0 the 24-bit field counter, flag 1 as 0, field parity the low bit of the
       field-interrupt count, current-slot 0, bytes 8 to 127 zero
     - a capture of the ring
   * - Ring overrun
     - hold the pending entry until the host clears VALID
     - a capture of the ring under overrun
   * - Ring pointer reset words
     - store of 0 restarts the ring at entry 0
     - a capture of the first status write after a reset
   * - Pipe reset words ``0x80006c``, ``0x80007c``
     - restart the completion counter
     - the interrupt service's use of the counter
   * - Transmit ack
     - a link transmit done entry in the ring of the message's pipe, in the
       same operation as the doorbell
     - a capture with the VBOB unplugged
   * - Receive ring and the frame release
     - the ring of the frame's pipe byte; one frame at a time until the
       host stores 1 to ``0x8038f4``
     - a capture of a frame's ring
   * - VBOB reply layouts, info reply and routing codes
     - as the message table
     - the VBOB's firmware messages
   * - Genlock lock rule and notifications
     - as :ref:`sgi-xt-digvid-vbob`
     - the VBOB firmware messages or a captured lock change
   * - Error causes raised
     - bit 13 only
     - a hardware reference for when each fires
   * - Colour converter registers
     - the programmable layout; writes stored, no conversion
     - a capture with the other image loaded
   * - Lookup table loads
     - 8192 writes per component accepted and dropped
     - the table maths and a read of the data register

Unknowns that block parts of the model
--------------------------------------

Two kinds of unknown would stop a model from being complete rather than
merely approximate.

- **The video data path.** The packings, colour-space conversions, lookup
  tables, the slot's transfer extent and the HDOC and VIF behaviour are
  all behind the FPGA images, and no shipped software states them. The
  model completes DMA slots without payload, so a guest that captures
  video gets unchanged buffers, and one that plays back loses the
  frames.
- **The VBOB side of the link.** The messages the module sends are
  covered, but the VBOB's replies beyond the bytes the module reads, its
  lock behaviour and the meaning of the routing codes are in the VBOB's
  firmware, which no source here describes.

.. _sgi-xt-digvid-limits:

Model limits
============

The page states these as unsupported, each *unknown* or outside scope:

- The DM3 for the Onyx 3000 and Origin 3000. The manual covers both
  boards (:src:`007-4317-001 xiii`), and the Origin builds of the driver
  carry the same constants, but the widget numbers, the host interrupt
  wiring and the Origin-only tunables (hub I/O credit counts) are not
  described here.
- The 400 MB/s link-speed distinction (status bit 5).
- Execution of the FPGA images, the video formatters, the packers, the
  colour converters, the lookup tables and the LVDS link.
- The VBOB's firmware, its flashing protocol and its other front-panel
  inputs and outputs (genlock outputs, RS-422, general-purpose I/O).
- The diagnostic's second colour converter register layout.
- Map RAM parity errors and every other XG error cause except the
  invalid-entry error.
- The kernel's simulation-mode ioctls (Discreet's driver routes them to a
  stub), SGI's driver log and custom-format commands.
- UST accuracy: the board's media stream counter is a field counter, the
  UST stamp at the field interrupt is the host's clock, and the board's
  own UST counter at ``0x80008c`` is storage.
