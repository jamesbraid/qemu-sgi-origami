.. SPDX-License-Identifier: BSD-3-Clause

.. _sgi-galileo:

====================================
Galileo: Octane Digital Video option
====================================

Used in :ref:`sgi-ip30` (Octane with IMPACT graphics). The Octane Digital
Video option is a video board in the Octane's :term:`XIO` module, and
Galileo is the family name of its video chips. The model is revision 1.5,
the one ``hinv`` prints as ``GALILEO15``. Its registers sit in an option
window of the IMPACT graphics board, and its DMA and interrupts go through
a :term:`Bridge` that runs in GIO mode (:ref:`sgi-galileo-sources`). The
``octane`` and ``octane2`` machines take it with ``-device
sgi-galileo,slot=widget-10``; no catalogue entry or default fits it.

.. seealso::

   Platform pages: :ref:`sgi-ip30`.

   Related parts: :doc:`mgras` (the graphics board that carries the
   registers), :doc:`bridge` (the chip the GIO side reuses),
   :doc:`xt-digvid` (the other Octane video board).

The model is at the level where IRIX and the video layer see the option
present, enumerate it, open it and set up a video path. It runs no video:
no signal source or sink, no DMA data and no other register behaviour than
what is listed below. Facts are rated as described in :ref:`sgi-ratings`.
No register map is published. Everything here is read from the behaviour
of the shipped driver and video library, so it is a *software expectation*
at best, and where the software does not say, the page says so.

.. _sgi-galileo-sources:

Sources
=======

- The shipped IRIX 6.5 video driver and video library, and the Discreet
  Smoke 5.0, Flame 7.6 and Flint 8.3.4 video layers, for what they do to
  the hardware. Every *software expectation* below comes from them.
- *Indigo2 IMPACT Video Programmer's Guide*, 007-3182-003, for the names
  of the alpha blender (p. 2), crosspoint (p. 72), CC1 (pp. 25, 72), VGI1
  memory nodes (p. 25), colour-space converter (p. 103) and texture map
  interface (p. 123). That guide covers the Indigo2 part, so the Octane
  parts' use of these names is *inferred*.
- Linux's SN inventory header, ``include/asm-ia64/sn/invent.h``, which
  defines ``INV_VIDEO_MGRAS`` as "Galileo 1.5 video" (the linux-2.5.0
  snapshot in the Linux history set, line 505; the Linux/MIPS 2.4 tree has
  it at line 506).
- *OCTANE Digital Video and OCTANE Compression Installation Guide*,
  007-3466-001, for the inventory line (Table 1-3, p. 35) and the XIO
  slot layout (pp. 13-14). Public Usenet posts with the board's name and
  inventory lines are cited in the identity table. The guide has no
  register map.

.. _sgi-galileo-identity:

Identity
========

.. list-table::
   :header-rows: 1

   * - Fact
     - Value
     - Rating
   * - Board name line
     - ``GALILEO15 Board: part 030-0931-003 rev C``, as ``hinv`` printed it
       in Usenet posts ``a43mev$8cn@netaxs.com`` (comp.sys.sgi.hardware,
       2002) and ``aud1v6$slq$1@news.lucky.net`` (comp.sys.sgi.admin, 2002)
     - *observed*
   * - Inventory line
     - ``Digital Video: unit 1, revision 4.2, TMI: revision 2, CSC:
       revision 1``, as ``hinv`` printed it in the post
       ``aud1v6$slq$1@news.lucky.net`` and in 007-3466-001 Table 1-3; the
       driver builds it from the revisions below
     - *observed*
   * - Board revision byte
     - ``0x24``
     - *inferred*
   * - TMI revision
     - ``2``
     - *inferred*
   * - CSC revision register
     - ``0`` (the driver adds one)
     - *inferred*
   * - Product ID
     - ``0x11``
     - *software expectation*
   * - Graphics board
     - one IMPACT (MGRAS) board must be present
     - *software expectation*

.. _sgi-galileo-gio:

GIO-mode Bridge
===============

The driver's two attach paths split one widget part number by the Bridge's
mode: the part ``0xc002`` from manufacturer 54 with status bit 5 clear
belongs to the video driver, and the same part with the bit set is a PCI
Bridge (*software expectation*). The model runs its own :term:`Bridge`
core with the strap clear (*approximation*: the chip's other GIO-mode
differences are *unknown*). The driver reads the product ID of four GIO
slots at word offset 4 of each slot window, at window offsets ``0x200000``,
``0x600000``, ``0x700000`` and ``0x800000`` (*software expectation*).

.. list-table::
   :header-rows: 1

   * - Window offset
     - Read, 32 bits
     - Rating
   * - ``0x200004``
     - low 7 bits ``0x11``: the board's product ID
     - *software expectation*
   * - ``0x600004``, ``0x700004``, ``0x800004``
     - ``0``: no board
     - *software expectation*
   * - ``0x200000``, 8 bytes
     - the same register as a 64-bit read: bits 6:0 must be ``0x11``, and
       bits 15:8 are the VGI1 revision, which the model returns as 0
       (*software expectation* for the test and the revision's place,
       *approximation* for the value)
     - *software expectation*
   * - ``0x200020``, 8 bytes
     - the chip ID, read and not checked; the model returns 0
     - *software expectation*
   * - 8-byte writes at ``0x200010`` and ``0x201000``
     - accepted and dropped; the driver stores a mode word ``0x101``
       and a zero channel setup word
     - *software expectation*
   * - anything else in ``0x200000`` to ``0xbfffff``
     - ``0``; the access is logged as unimplemented (the rest of the DMA
       engine's register file lives here)
     - *unknown*

Without the identification register the driver clears its VGI1 state and
warns "No VGI1 present"; several later driver paths (the media stream
counter and time stamp read, triggers and the forced reset) then
dereference that state, so the model answers it (*software expectation*).

The model wires GIO interrupt line *n* to Bridge interrupt pin *n*
(*approximation*; how the chip routes GIO lines is *unknown*). The driver
allocates lines 4 (DMA), 6 (CC1) and 7; line 4 is never raised.

.. _sgi-galileo-option-window:

Option window
=============

The registers are bytes in the graphics board's window, at the graphics
base plus the offsets below (*software expectation*; what the window is
on the hardware is *unknown*).

.. list-table::
   :header-rows: 1

   * - Offset
     - What
     - Rating
   * - ``0x61000``
     - the probe stores ``0x4210`` (32 bits); the graphics board's existing
       configuration storage takes it
     - *software expectation*
   * - ``0x70008``
     - byte, reads ``0x11`` when the option is fitted
     - *software expectation*
   * - ``0x72800``
     - blender block (alpha blender)
     - *software expectation*
   * - ``0x73000``
     - CC1 block (CC1, crosspoint and input/output control)
     - *software expectation*
   * - ``0x73400``
     - control block (the board's own, with the TMI pair)
     - *software expectation*
   * - ``0x73800``
     - CSC block (colour-space converter)
     - *software expectation*

In a block, direct register *n* (0 to 7) is the byte at ``block + n * 0x80
+ 8``. An indirect register is reached through two adjacent direct
registers, the index and then the data: direct 1 and 2 for CC1, direct 0
and 1 for the control block and CSC, and control direct 6 and 7 for the
TMI. The blender takes a 16-bit address, the low byte in direct 3 and the
high byte in direct 4, and then data at direct 1 for the file of
registers 0 to 38 or at direct 2 for the second file of registers 0 to 3
(*software expectation*). The indirect ranges are CC1 0 to 80, CSC 0 to 5, blender
0 to 38, control 0 to 39 and 240 to 255, and TMI 0 to 149 (*software
expectation*, from the driver's range checks). Other offsets in a block,
other widths, and indirect numbers outside the ranges are logged and read
as zero, and writes to them are dropped (*approximation*).

The model stores a written byte and reads it back, for every register of
the five blocks, except as follows (*software expectation* for why, and
*approximation* for the model's values):

- Control indirect 37 bit 7, the I2C busy flag, reads 0. The driver polls it
  after every I2C byte.
- Control indirect 17 bits 0, 4 and 6 read 0. The driver's timer polls them
  for a pending field event; the model raises no such event, so they never
  read set (*approximation*).
- Control direct 2 reads ``0x24``, the board revision, and takes no write
  (*inferred*). The driver's reset writes 1 to control direct 3 first, which
  the model stores.
- TMI indirect 147 reads 2 and CSC direct 3 reads 0, and neither takes a
  write (*inferred*).
- TMI indirect 129, the TMI status register, has bit 1 set on every second
  read, which is what the driver's texture-transfer start waits for (it
  waits for the bit to read 1 and then 0, and gives up after a bounded
  number of reads otherwise). Only a texture transfer reads it; the model
  starts no transfer (*software expectation* for the wait,
  *approximation* for the pulse).

.. _sgi-galileo-interrupts:

Field interrupts
================

Window enable and frame writes wait for CC1 field interrupts. The driver
sleeps for the number of fields named by CC1 indirect 38 bits 7 and 5 if
either is set, and the attach sequence leaves the register at ``0xa2``
(*software expectation*). Without the interrupt the guest's video path
setup blocks. While either bit is set the model pulses GIO line 6 once per
field, every 16683333 ns of virtual time, the NTSC field period
(*approximation*; the real period follows the timing the guest selects and
is *unknown*). The pulse is an edge, so one before the Bridge pin is enabled
is lost; whether the line is an edge or a held level is *unknown*, and the
edge is an *approximation*. Clearing both bits, or a reset, stops it.

.. _sgi-galileo-model:

Model
=====

``-device sgi-galileo,slot=widget-10,id=galileo`` fits the board in an
Octane option port, beside an ``sgi-mgras`` board; the board is not fitted
by default. That the board sits in widget 10 is an *approximation*; the
installation guide puts option boards in XIO slots B, C and D and does not
name widget 10 (pp. 13-14). It
carries a Bridge with the board's name line. Its registers need a graphics
board: the ``graphics`` link names one, and without it the model takes the
machine's only ``sgi-mgras``, which must already exist, so a second MGRAS or
a video board listed first needs the link. Without the option, the graphics
board's probe byte reads ``0xff`` (*software expectation*). The ``field-period-ns`` property
sets the field period. The guest needs the 2 x 50 MiB of low memory the
driver reserves when the machine has more than 1.5 GiB (*software
expectation*); the model does nothing about it. The Octane's graphics
board does not report the video option bit in its revision byte
(*unknown*).

.. _sgi-galileo-limits:

Model limits
============

Each of these is unimplemented, and an access that reaches one is logged
as such where it can be told apart:

- DMA: the data engine's register file and descriptors, and GIO line 4.
- The keyer, colour lookup tables and the EDH parts behind I2C. Their
  registers are plain storage (*approximation*).
- A video signal source or sink, genlock and the second video DMA chip.
- The compression board.
- The VGI1 channel registers at ``0x1010``, ``0x1200``, ``0x1300``,
  ``0x2010``, ``0x2200`` and ``0x2300`` that the driver touches at attach:
  writes are dropped and reads return 0.
- CC1 indirect 38 bit 1 is stored and has no effect; only bits 7 and 5 start
  the field interrupt.
- On hardware the TMI and blender 16-bit writes go through the graphics
  board's FIFO; the model takes them as option window byte writes
  (*approximation*).
- The private ioctl set beyond what the start-up writes; the register
  values written there come from the guest's own parameters.

.. _sgi-galileo-tests:

Tests
=====

``tests/qtest/galileo-test.c`` runs on the ``octane`` and
``octane2`` machines and checks the probe and its absent value, the three
revisions, the indirect pairs of every block with their ranges (blender's
16-bit address included), the VGI1 identification, the two read overrides, the TMI status
pulse, the Bridge identity and slot probes, the field interrupt's
cadence, a pulse lost before the pin is enabled, its stop conditions, a
Bridge left in PCI mode, and reset.
