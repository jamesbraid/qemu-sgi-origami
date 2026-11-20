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
present, enumerate it, open it, set up a video path and play frames out of
guest memory to a file sink. It has no signal source and no other register
behaviour than what is listed below. Facts are rated as described in
:ref:`sgi-ratings`. No register map is published. Everything here is read from
the behaviour of the shipped driver and video library, so it is a *software
expectation* at best, and where the software does not say, the page says so.

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
   * - 8-byte write at ``0x200010``
     - accepted and dropped; the driver stores a mode word ``0x101``
     - *software expectation*
   * - 8-byte accesses at ``0x201000`` and ``0x202000``, plus the offsets
       in :ref:`sgi-galileo-vgi1`
     - the two DMA channels' registers
     - *software expectation*
   * - anything else in ``0x200000`` to ``0xbfffff``
     - ``0``; the access is logged as unimplemented
     - *unknown*

Without the identification register the driver clears its VGI1 state and
warns "No VGI1 present"; several later driver paths (the media stream
counter and time stamp read, triggers and the forced reset) then
dereference that state, so the model answers it (*software expectation*).

The model wires GIO interrupt line *n* to Bridge interrupt pin *n*
(*approximation*; how the chip routes GIO lines is *unknown*). The driver
allocates lines 4 (DMA), 6 (CC1) and 7; the model raises 4 and 6.

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
(*software expectation*). Without the interrupt the guest's video path setup
blocks. While either bit is set the model pulses GIO line 6 once per field,
every 16683333 ns of virtual time, the NTSC field period (*approximation*; the
real period follows the timing the guest selects and is *unknown*). The fields
sit on a fixed grid of virtual time: enabling joins the grid at its next
field, and a callback that runs late does not delay the field after it,
because video timing runs whether or not its interrupt is enabled
(*inferred*). The DMA verticals below sit on the same grid; one held back for
the guest waits for the next grid field, so the field counter advances once
per vertical and never by two. The pulse is an edge, so one before the Bridge
pin is enabled is lost; whether the line is an edge or a held level is
*unknown*, and the edge is an *approximation*. Clearing both bits, or a reset,
stops it. The interrupt reaches the processor only because the Bridge keeps
its interrupt destination across the link reset the guest sends while it
attaches (:doc:`bridge`).

.. _sgi-galileo-vgi1:

VGI1 DMA channels
=================

Each of the two channels has an 0x1000-byte block in the GIO window, at
window offset 0x1000 for channel 1 and 0x2000 for channel 2. All registers
are 8 bytes wide (*software expectation*).

.. list-table::
   :header-rows: 1

   * - Offset
     - Register
     - What the model does
   * - ``0x000``, ``0x010``
     - setup, interrupt enable
     - stored. A channel runs while setup bits 3:2 are not both clear and
       interrupt enable bit 0 is set (*approximation*: the setup layout is
       *unknown*; the driver writes ``0x9fff0020148c`` and
       ``0x9fff0080184c`` for channel 1, bit 13 more for channel 2, and
       bits 3:2 clear on a stop)
   * - ``0x020``, ``0x030``, ``0x040``
     - horizontal and vertical parameters, line stride
     - stored. The driver writes ``0x05a00110``,
       ``0x00f3001100f30011`` and ``0xb40``; the model reads the line count
       as the sum of the two 16-bit counts at bits 63:48 and 31:16, 243
       twice for NTSC (*inferred*), and the stride as bytes per line
       (*inferred*: four per pixel, 720 pixels; the lines in memory are
       three bytes per pixel, see below)
   * - ``0x100``
     - debug
     - stored (*unknown*)
   * - ``0x200``
     - trigger
     - stored. Writing 2 drops the channel's queued and running request
       (*approximation*: the driver writes 2 then 0 to abort a pending
       trigger; the only trigger event modelled is status bit 4 at a start
       while the register holds 1, see below)
   * - ``0x210``, ``0x220``
     - start offset in the first page, page list address
     - a write of the list address, which the driver makes after the offset,
       queues a request. Bits 39:0 are the GIO address of the page list
       (*software expectation*); the driver also sets bit 48 on the first
       request of a transfer, which the model ignores (*unknown*)
   * - ``0x300``
     - status
     - bits are set by the model and cleared by writing them back
       (*software expectation* for the handler's read and write-back;
       write-one-to-clear is *inferred*)

The status the guest reads has, besides the events in bits 15:0, a line
counter in bits 26:16, the field parity in bit 32 and a free-running 15-bit
field counter in bits 47:33 (*software expectation*: the driver takes the
difference of the counter between two events as the fields elapsed and
treats a step of 2 or more as a missed field, and uses the parity for the
field order). The model advances the counter by 1 per field and toggles the
parity with it; the pair is read as it was at the last event. The line
counter reads the first vparam count at an end of DMA and 0 otherwise
(*approximation*: *unknown* what the real line counter holds). Writing a
status back clears only the events. The counter fields are not events.

The driver checks the order of the events in each channel (*software
expectation*): per request, a vertical with bit 5 (status ``0x21``; the model
adds bit 1 as the descriptor boundary, ``0x23``, and bit 4 when the trigger
register holds 1, ``0x33``, *approximation*), then a plain vertical
(``0x01``), then an end of DMA (``0x08``) as its own interrupt before the next
vertical. An end of DMA that arrives with a vertical (``0x09``), a vertical
while one is in flight, or an end of DMA with no start before it makes the
driver abort and set the channel up again. The model keeps them apart: the end
of DMA is raised half a field after the second vertical, once the guest has
cleared the vertical, and the next vertical is held back until the end of DMA
has been cleared, so a slow guest stretches a field (*approximation*). Bit 8
rides on the end of DMA when the transfer cannot be read (*approximation*: the
error bits' meaning is *unknown*). The end of DMA waits for the vertical to be
cleared even when the guest has masked its enable bit, so ``0x09`` is never
read. Bit 2 and the other bits are never set. GIO line 4 is held high while
any channel has a status bit that its interrupt enable allows, and falls when
the guest has cleared them (*approximation*: the line's type is *unknown*; the
Bridge sends an interrupt on each rising edge).

A request starts on the first field after it is queued, takes two fields
and the next queued request starts on the third (*approximation*: the real
timing follows the signal; a triggered start waits for no event). The transfer
reads the page list: 8-byte entries, the low 32 bits of each a GIO address
shifted right by 14, so a page is 16 KiB (*inferred*: the driver sets the
16 KiB page size in the system mode, and the pages read back as the clip's
frame). Smoke 5.0 queued lists of 86 entries for a 1,399,680-byte frame at
stride 2880 (*software expectation*); the model reads only the entries the
three-byte pixels need, 65 for a 720 by 486 frame that starts near the top
of a page. Bits 63:32 are zero except in the last entry, which the driver
gives ``0xf`` there (*unknown*, ignored). The list may hold up to 2048
entries; a longer link to a further page is not followed. The pages are
read through the Bridge's GIO translation, so the direct map and the
translation entries the guest set apply. The Bridge's byte swapping for
the slot is not applied (*approximation*: the swap setting the driver
chooses is *unknown*).

The model plays every request out. The direction is *unknown*, so input is not
modelled. A frame is the lines of all the vparam counts at the stride. A pixel
is three bytes, B G R, with the lines back to back, so a frame fills three
quarters of the stride's worth of pages. Smoke 5.0's buffers decoded that way,
read from guest memory on the model, give the clip's own picture (*inferred*;
the IRIX 6.5 video library names the packing ``bgr_8_p``, a *software
expectation*). The model converts it to studio-range BT.601 4:2:2
(*approximation*: other formats are not read; the colour space follows the
board's colour space converter, whose settings the model does not read). The
frame is 30000/1001 per second, bottom field first, at 8 bits
(*approximation*: NTSC only). With the ``out-sink`` link it goes to a video
sink (:ref:`sgi-video-io`); without one it is read and dropped.

Smoke 5.0 runs on the model, not on hardware: with a ``Vtr`` line for live
video, Output Clip (SHIFT+F9) and Process reach "Press pen to start". After
the click the driver queues requests back to back, with about 3 percent of
them followed by an abort and a set-up again (the driver's error recovery).
When the guest keeps up the sink gets about 190 black frames of preroll, the
clip's first frame for about 300 frames, then its 20 frames, and Output Clip
ends at the clip's duration. That does not happen reliably: at the default
field period the emulated guest often stops queuing requests during preroll,
the sink gets black frames or the last buffer, and Output Clip ends silently
with the timecode at preroll; with ``field-period-ns=33366666`` the clip ran
to its end once and stalled in other runs, on a heavily loaded host. The
stalls are the emulated processor not taking interrupts for 20 to 340 ms,
measured on the model: the host compacting memory inside a page fault on
QEMU's translation buffer, and the graphics model rasterising a pixel write
while it holds the big QEMU lock. A late field reads to the driver as a missed
field. The default stays one vertical per field: the driver steps its media
clock by the field counter, which advances by one per pulse, so a longer
period is a workaround, not hardware. The shipped IRIX 6.5 video driver uses
16,683,350 ns for an NTSC field (*software expectation*). The model uses
16683333 ns, exactly 1001/30000/2 (*approximation*, as above); the 17 ns
difference does not reach the field counter.

Smoke also logs ``vlSetControl failed (CSC)``, once for the playback input
packing and once for the capture output packing. On the real board too, as
far as the shipped IRIX 6.5 video library goes (*software expectation*):
Smoke asks for packing 32 (``bgr_8_p``) on the colour space converter's
input, the converter's packing list does not include 32, and the video
daemon substitutes the first, ``rgba_8``, returns the error and still
applies it. The model therefore must not accept 32 there. Whether the
memory node's own packing control accepts 32 is *unknown*. On the model
every driver call around it returns success. The converter's tables reach
it through a graphics-board DMA (writes to graphics ``0x81018`` = ``0xc``,
``0x81010`` = ``0x11``, ``0x81000`` = a descriptor, then a poll of bit 12
of ``0x20000``; *software expectation*); the model completes it at once,
with the bit reading clear, and does not deliver the table bytes, so the
converter keeps its power-on table and the model does not apply it to the
frame (*approximation*).

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
board's probe byte reads ``0xff`` (*software expectation*). With it, bit 7
of the graphics board's ``0x071708`` byte reads clear (*inferred*, see
:doc:`mgras`); Smoke's board search needs that. The ``field-period-ns``
property sets the field period, and ``out-sink`` names a video sink for
the frames of :ref:`sgi-galileo-vgi1`. The guest needs the 2 x 50 MiB of low
memory the driver reserves when the machine has more than 1.5 GiB (*software
expectation*); the model does nothing about it.

.. _sgi-galileo-limits:

Model limits
============

Each of these is unimplemented, and an access that reaches one is logged
as such where it can be told apart:

- DMA input, any trigger event besides status bit 4 at a start, a list
  longer than one page, the second channel's use (the driver sets it up and
  never queues on it) and the Smoke restart loop described above.
- The keyer, colour lookup tables and the EDH parts behind I2C. Their
  registers are plain storage (*approximation*).
- A video signal source, the console, genlock and the second video DMA chip.
- The compression board.
- Channel registers other than the ten above, and accesses that are not 8
  bytes.
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

``tests/qtest/galileo-test.c`` runs on the ``octane`` and ``octane2`` machines
and checks the probe and its absent value, the three revisions, the indirect
pairs of every block with their ranges (blender's 16-bit address included),
the VGI1 identification, the two read overrides, the TMI status pulse, the
Bridge identity and slot probes, the field interrupt's cadence, a pulse lost
before the pin is enabled, its stop conditions, a Bridge left in PCI mode, and
reset. It also runs a request through the channel (the event order, the field
counter and parity, the frame in a file sink, an armed trigger), GIO line 4
with its edge, the write-back that clears it and the enable that masks it, an
unreachable page list and the trigger abort. It also checks a held vertical
waiting for the next grid field, the end of DMA never riding a vertical when
the guest masks its enable bit, and a migration between the second vertical
and its end, after which the counter and both timers resume.
