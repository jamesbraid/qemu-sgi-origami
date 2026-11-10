.. SPDX-License-Identifier: BSD-3-Clause

.. _sgi-gbe:

=====================
GBE: display back end
=====================

Used in :ref:`sgi-ip32` (O2).
GBE is the display controller of the O2. It reads the frame buffer, the
overlay and a table of per-pixel display modes from main memory through
CRIME, turns them into a 24-bit pixel stream with a colour map, a gamma
map and a 32 by 32 hardware cursor, and clocks that stream out to a DAC
and a flat panel. It also holds the video timing generator and the video
capture engine (:src:`sgi-gbe-1.1-spec p. 1`). QEMU models revision 1.1
as ``sgi-o2-gbe`` in ``hw/display/sgi/gbe.c``. The ``o2`` machine
(:doc:`../ip32`) always fits it, with a display, the ``legacy-software``
memory view and a 20 MHz TTL pixel clock. Its register test profile fits
it only with ``gbe11-registers=on``. VICE, the O2's other media engine,
is on :doc:`vice`.

.. seealso::

   Platform pages: :ref:`sgi-ip32`.

Facts are rated as described in :ref:`sgi-ratings`.

.. _sgi-gbe-sources:

Sources
=======

- The *GBE ASIC spec for REV1.1*, 18 July 1996, :src:`sgi-gbe-1.1-spec`.
  Page numbers are PDF pages, which equal the printed "n of 50" numbers.
  It gives the register map, the field layouts and the tile, DID,
  colour-map and cursor formats. It leaves the dot-clock lock thresholds
  as "TBD" and leaves several reset values unspecified.
- The CRIME 1.5 specification, :src:`sgi-crime-1.5-spec`, for the address
  space GBE sees (Table 3-3) and the interrupt bits (Table 5-5). Its
  page numbers are PDF pages.
- The *O2 Technical Report*, :src:`o2-technical-report`, for GBE's place
  in the machine.
- The VICE design specification, :src:`099-0123-003`, only for the
  frame-buffer tile layout it shares with GBE (see :ref:`sgi-gbe-vice`).
- Three public drivers, as software expectations: Linux
  ``drivers/video/fbdev/gbefb.c`` and ``include/video/gbe.h`` at
  ``db2ddb871435``, NetBSD ``sys/arch/sgimips/dev/crmfb.c`` and
  ``crmfbreg.h`` at ``07004b2a9dac``, and OpenBSD
  ``sys/arch/sgi/dev/gbereg.h`` at ``2df91d80c104``.
- Two published O2 boot reports and one published hardware inventory
  (:ref:`sgi-gbe-identity`).


.. _sgi-gbe-identity:

Identity and placement
======================

CTRLSTAT bits 3:0 hold the chip revision and reset to 1 for revision 1.1
(:src:`sgi-gbe-1.1-spec p. 36`), *established*. DEVICE_ID at offset
``0x14`` reads ``0x00000666``, the value the chip also returns on JTAG
(:src:`sgi-gbe-1.1-spec p. 37`), *established*. It is a constant. The
revision is in CTRLSTAT, not in DEVICE_ID.

Published captures of real O2s:

- An OpenBSD 5.2 boot report prints
  ``gbe0 at mainbus0: rev 1, 8MB, 1280x1024 at 8 bits``
  (:src:`dmesgd-2432 gbe0`), *observed*. This is the only published
  read of the CTRLSTAT revision field (OpenBSD ``gbe.c`` prints CTRLSTAT
  bits 3:0).
- A NetBSD 9.99.31 boot report prints ``crmfb0 at mainbus0 addr
  0x16000000`` (:src:`dmesgd-5290 crmfb0`), *observed*.
- A published hardware inventory lists "CRM revision C, GBE revision B"
  (:src:`bukosek-sgi-o2 gfxinfo`), *observed*. How the letter maps to the
  CTRLSTAT revision number is *unknown*. A read of CTRLSTAT on a letter-B
  part would settle it.

All registers and RAMs sit in the 16 MiB physical range
``0x16000000``-``0x16ffffff`` (:src:`sgi-gbe-1.1-spec p. 34`),
*established*. Linux (``gbefb.c`` ``GBE_BASE``) and OpenBSD
(``gbereg.h`` ``GBE_BASE``) use the same base, *software expectation*.
The model maps a 16 MiB big-endian region there.

On the board GBE reads memory over a 64-bit, 133 MHz point-to-point link
to CRIME, and CRIME forwards processor accesses to it
(:src:`sgi-gbe-1.1-spec p. 4-6`), *established*. The model has no link
layer. It calls CRIME's memory view directly for reads (see
:ref:`sgi-gbe-dma`).

Access rules
============

All loads and stores to GBE must be 32 bits wide and uncached. Every
register is read/write, unused bits read as undefined values, and
software must mask them (:src:`sgi-gbe-1.1-spec p. 34`), *established*.
The link carries 24-bit word-aligned addresses and has no flow control for
writes, except colour-map writes, which go through a 64-entry FIFO that
software must not overrun (:src:`sgi-gbe-1.1-spec p. 5, 18`),
*established*.

The model accepts only aligned 4-byte reads and writes. Any other width or
alignment is refused with a bus error and a ``LOG_GUEST_ERROR`` line.
Bits the specification leaves undefined read as zero, a model choice
that is *approximation*. A read of an offset that is inside the 16 MiB
region but has no register returns ``0xffffffff`` and logs
``LOG_GUEST_ERROR``. A write there is dropped and logged the same way.

Colour-map writes take effect at once. The model has no write FIFO, so the
FIFO status register reads zero (see :ref:`sgi-gbe-limits`).

Register map
============

Offsets are from ``0x16000000``. "Stored" lists the bits the model keeps;
the rest read as zero. Resets come from the specification pages given,
*established*, unless the Notes column says otherwise.

.. sgi-registers: base=0x16000000 width=32 expect=0x0:0x3ffffd1
   qemu="-M none -cpu R10000 -display none -device sgi-o2-gbe,id=gbe"
   map=/machine/peripheral/gbe:0:0x16000000
   skiprow="VT_0 to VT_19:per-register" skiprow="CMFIFO:revision-dependent"
   skiprow="VC_2 to VC_8:not-modelled"

.. list-table::
   :header-rows: 1

   * - Offset
     - Name
     - Reset
     - Stored
     - Notes
   * - ``0x000000``
     - CTRLSTAT
     - ``0x03ffffc1``
     - ``0x3fffffc0`` and revision
     - Spec p. 36. Bit 4 is the monitor sense input and has no spec
       reset; the model reads it as 1, so CTRLSTAT reads ``0x03ffffd1``
       after reset.
   * - ``0x000004``
     - DOTCLK
     - ``0x00c00000``
     - ``0x0010ffff``
     - Spec p. 36. Reset value is the model's; see below.
   * - ``0x000008``
     - I2C
     - ``3``
     - bits 1:0
     - Monitor (DDC) bus, spec p. 37.
   * - ``0x00000c``
     - SYSCLK
     - ``8``
     - none
     - Spec p. 37. Writes are never stored.
   * - ``0x000010``
     - I2CFP
     - ``3``
     - bits 1:0
     - Flat-panel bus, spec p. 37.
   * - ``0x000014``
     - DEVICE_ID
     - ``0x666``
     - none
     - Read-only constant, spec p. 37.
   * - ``0x010000``-``0x01004c``
     - VT_0 to VT_19
     - VT_0 ``0x80000000``, others 0
     - 24 bits each; VT_0 bit 31; VT_6 bits 6:0
     - Video timing, spec pp. 38-41.
   * - ``0x020000``-``0x020008``
     - OVR_0 to OVR_2
     - 0
     - OVR_0 bits 13:0; OVR_1 and OVR_2 ``0xffffffe1``
     - Overlay plane, spec p. 42.
   * - ``0x030000``-``0x03000c``
     - FRM_0 to FRM_3
     - 0
     - FRM_0 bits 15:0; FRM_1 bits 31:16; FRM_2 and FRM_3 ``0xffffffe1``
     - Normal plane, spec p. 43.
   * - ``0x040000``, ``0x040004``
     - DID, DID_SHADOW
     - 0
     - bits 16:0
     - DID table pointer, spec p. 44.
   * - ``0x048000``-``0x04807c``
     - WID[0..31]
     - not specified; model 0
     - bits 12:0
     - Window mode table, spec p. 44.
   * - ``0x050000``-``0x0547fc``
     - CMAP[0..4607]
     - not specified; model 0
     - bits 31:8
     - Colour map, spec p. 45.
   * - ``0x058000``
     - CMFIFO
     - 0 at revision 1, 63 at revision 2
     - read
     - Spec p. 45. Free entries; the model's FIFO is always empty. The
       revision 2 value has no public source (`Model limits`_).
   * - ``0x060000``-``0x0603fc``
     - GMAP[0..255]
     - not specified; model 0
     - bits 31:8
     - Gamma map, spec p. 45.
   * - ``0x070000``
     - CRS_0
     - 0
     - all 32 bits
     - Cursor position, spec p. 46.
   * - ``0x070004``
     - CRS_1
     - 0
     - bits 1:0
     - Cursor enable and crosshair, spec p. 46.
   * - ``0x070008``-``0x070010``
     - CRS_CM[1..3]
     - not specified; model 0
     - bits 31:8
     - Cursor colours, spec p. 46.
   * - ``0x078000``-``0x0780fc``
     - CRS_GLY[0..63]
     - not specified; model 0
     - all 32 bits
     - Cursor glyph, spec p. 46.
   * - ``0x080000``, ``0x080004``
     - VC_0, VC_1
     - 0
     - bits 23:0
     - Capture region bounds, spec p. 47.
   * - ``0x080008``-``0x080020``
     - VC_2 to VC_8
     - VC_2 ``0x6`` (``vc_flick`` and ``vc_autofield`` set); VC_3 bit 4
       (``vc_oddeven``) set
     - none
     - Capture control and descriptors, spec p. 47. Not modelled: the
       model reads ``0xffffffff`` and logs.

The specification prints the glyph RAM range as ``0x0780000``-``0x0780FC``
on p. 46 and as ``0x078000``-``0x0780FC`` in the memory map on p. 34. The
memory map is right; the extra zero is a misprint. Its VT_15 table also
labels bits 23:12 ``vt_vcmap_on``, a repeat of the VT_16 name. The model
treats them as the horizontal ``on`` field, an *inferred* reading from
the register's role.

Control registers
-----------------

CTRLSTAT fields (:src:`sgi-gbe-1.1-spec p. 36`), *established*:

- Bits 3:0, chip revision.
- Bit 4, ``sense_n`` from the monitor, read-only.
- Bits 25:6, ten auxiliary pins. Pin *n* has a data bit at ``6 + 2n`` and
  an active-low output-enable bit at ``7 + 2n``. All reset to 1.
- Bit 26, flat-panel half-clock phase. Bit 27, composite sync polarity.
- Bits 29:28, dot-clock source: 0 external TTL, 1 external differential,
  3 internal PLL.

The model stores bits 29:6 and forces bit 0 to 1. It reads bit 4 as 1,
because nothing in the model pulls the sense input low, an *approximation*;
the specification gives no reset for it. For each pin whose output is
disabled (enable bit set) the data bit reads the level on the model's
auxiliary input line instead of the stored bit. Source 2 is undefined; a
write that selects it logs ``LOG_UNIMP`` and is stored.

DOTCLK holds the PLL multiplier M in bits 7:0, the input divider N in
13:8 and the post-scaler exponent P in 15:14, with the out-of-range (OOR) and
out-of-lock (OOL) status in bits 22 and 23
(:src:`sgi-gbe-1.1-spec p. 36`), *established*. The register table leaves
bit 20 blank. The DOTCLK field figure names it BYP, a bypass and power-down
control that powers down the PLL and disables the clock to the pixel
logic (:src:`sgi-gbe-1.1-spec p. 32`), *established* as the spec's
wording. Linux names the bit ``GBE_DOTCLK_RUN`` and OpenBSD
``GBE_DOTCLOCK_RUN``, and both set it to start the clock; NetBSD names it
``BYPASS`` in ``crmfbreg.h`` but sets it to start the clock in
``crmfb.c``, *software expectation*. The drivers' use conflicts with the
spec's name, so the polarity is *unknown*. The model calls the bit RUN and
follows the drivers, an *approximation*. The spec's lock table on p. 32
reads OOR=1, OOL=1 as "PLL in range and phase locked" and OOR=0, OOL=0 as
"out of range and out of lock". The model uses the opposite sense: it
sets both bits for an out-of-range setting and clears both when locked.
Under the spec's table its reset value reads as locked. That polarity is
*unknown* and the model's choice is a model limit, an *approximation*. The two PLL test bits, 25:24, are refused: a write that sets
either logs ``LOG_UNIMP`` and the whole write is dropped, M, N and P
included. See :ref:`sgi-gbe-clock` for the clock model.

The I2C and I2CFP registers drive the monitor and flat-panel buses by
software bit-banging (:src:`sgi-gbe-1.1-spec p. 22-23, 37`),
*established*. Bit 0 is SDA and bit 1 is SDC. NetBSD notes that the
register bits are low-active and inverts them on both write and read
(``crmfbreg.h``, ``crmfb.c``), *software expectation*. The model follows
that polarity: a set bit pulls the line low and a clear bit releases it,
and a read returns the same polarity from the bus. Reset writes 3 to
both registers, which under that polarity drives both lines low. That
differs from a bus whose lines are released at reset, a model limit. The
inverted polarity itself is *inferred*: the spec gives a reset value of 1
for each bit and says the outputs drive external open-collector buffers
(:src:`sgi-gbe-1.1-spec p. 22, 37`). Each register drives a
QEMU I2C bus, named ``i2c`` and ``i2cfp``, through the generic bit-bang
interface. Both buses are empty unless a test attaches slaves, so no
monitor answers DDC reads.

SYSCLK controls the system-clock PLL's test and analog inputs
(:src:`sgi-gbe-1.1-spec p. 37`), *established*. The model always reads 8,
the reset value. A write with any other value in bits 14:8 or 3:0 logs
``LOG_UNIMP`` and is dropped.

Video timing registers
----------------------

Each register packs two 12-bit fields. The ``on`` field is in bits 23:12
and the ``off`` field in bits 11:0, except where noted
(:src:`sgi-gbe-1.1-spec p. 38-41`), *established*.

.. list-table::
   :header-rows: 1

   * - Register
     - Fields
   * - VT_0
     - ``vt_x`` 11:0, ``vt_y`` 23:12, ``vt_freeze`` 31 (reset 1)
   * - VT_1
     - ``vt_maxx`` 11:0, ``vt_maxy`` 23:12
   * - VT_2, VT_3
     - vertical sync (VT_2) and horizontal sync (VT_3), ``on`` and ``off``
   * - VT_4, VT_5
     - vertical blank (VT_4) and horizontal blank (VT_5)
   * - VT_6
     - bit 0 vsync invert, 1 vsync low, 2 hsync invert, 3 hsync low,
       4 composite sync high, 5 composite sync low, 6 ``f2rf`` high
   * - VT_7
     - ``vt_f2rf`` 11:0, ``vt_frmlock`` 23:12
   * - VT_8, VT_9
     - interrupt lines: ``vt_intr1`` 11:0 and ``vt_intr0`` 23:12 in VT_8,
       ``vt_intr3`` and ``vt_intr2`` in VT_9
   * - VT_10, VT_11, VT_12
     - flat-panel horizontal sync, vertical sync and data enable
   * - VT_13, VT_14
     - horizontal and vertical pixel enable
   * - VT_15, VT_16
     - horizontal and vertical colour-map write enable
   * - VT_17, VT_18, VT_19
     - ``startx`` 11:0 and ``starty`` 23:12 presets for the DID, cursor and
       capture counters

Display timing
==============

Counters
--------

The timing block has two 12-bit counters, ``vt_x`` and ``vt_y``. ``vt_x``
counts at the dot clock. ``vt_y`` counts once per ``vt_x`` wrap. Interlaced
timing is not supported; the MACE video-out block makes interlaced output
(:src:`sgi-gbe-1.1-spec p. 19`), *established*. All outputs derive from
the counters. The sync and blank outputs are not used inside GBE and may be
programmed arbitrarily (:src:`sgi-gbe-1.1-spec p. 19`), *established*.

The model runs the counters only while the dot clock produces a rate, VT_1
has both maxima non-zero and ``vt_freeze`` is clear. It advances them in
virtual time and brings them up to date before any register read or write.
The counters run from 0 to ``max - 1`` and then wrap, so VT_1 is the period
of the line and of the frame. The specification says the counter resets
when it equals the maximum (:src:`sgi-gbe-1.1-spec p. 38`), which read
literally gives a period of ``max + 1``. Linux (``gbefb.c``, ``MAXX``
and ``MAXY`` set to ``htotal`` and ``vtotal``) and NetBSD
(``crmfb.c``, ``CRMFB_VT_XYMAX`` set to ``htotal | vtotal << 12``) both
program the totals, *software expectation*. The model follows them. Which
reading the silicon uses is *unknown*; a read of ``vt_x`` at a known
dot-clock count would settle it.

Writing VT_0 or VT_1 reduces the stored counters modulo the new periods.
Writing VT_0 with ``vt_freeze`` set stops the counters.

Sync, blank and flat-panel outputs
----------------------------------

Each ``on``/``off`` pair sets a signal when its counter equals ``on`` and
clears it when the counter equals ``off``. A pair whose fields lie outside
the period, or whose period is zero, logs ``LOG_UNIMP`` once and holds the
signal low until a related register changes. VT_6 inverts and forces the
sync outputs (:src:`sgi-gbe-1.1-spec p. 19, 39`), *established*.

The model exports the outputs as ten named GPIO lines, ``timing``, which a
board can connect:

.. list-table::
   :header-rows: 1

   * - Line
     - Signal
   * - 0
     - vertical sync, from VT_2 and the VT_6 vsync bits
   * - 1
     - horizontal sync, from VT_3 and the VT_6 hsync bits
   * - 2
     - vertical blank, VT_4
   * - 3
     - horizontal blank, VT_5
   * - 4
     - flat-panel horizontal sync, VT_10
   * - 5
     - flat-panel vertical sync, VT_11
   * - 6
     - flat-panel data enable, VT_12
   * - 7
     - stereo goggle control, ``f2rf``
   * - 8
     - CTRLSTAT bit 26, the flat-panel half-clock phase
   * - 9
     - composite sync

The composite sync line is forced high by VT_6 bit 4, else forced low by
bit 5, else the exclusive OR of the two sync lines. CTRLSTAT bit 27 does
not change it. Both are *approximation*. The model has no blank output
that combines VT_4 and VT_5, no DAC data and no pixel clock output.

With VT_6 bit 1 or bit 3 set the model drives the matching sync line low
whatever the invert bit says. The specification gives the invert level
instead (:src:`sgi-gbe-1.1-spec p. 39`).

Frame events
------------

The four vertical events match ``vt_y`` against the four fields of VT_8
and VT_9. When a line starts and its ``vt_y`` equals a field, the model
pulses the matching interrupt output once. An event is a one-shot pulse,
not a level. The specification describes the interrupts as latched in CRIME
and cleared only there (:src:`sgi-gbe-1.1-spec p. 7, 20`),
*established*. A field larger than ``vt_maxy`` never matches.

``vt_y`` equal to the VT_7 ``vt_f2rf`` field toggles the stereo line. A
rising edge on the frame-lock input presets ``vt_y`` to ``vt_frmlock`` at
the next line wrap (:src:`sgi-gbe-1.1-spec p. 19-20, 39`), *established*.
A ``vt_frmlock`` above ``vt_maxy`` logs ``LOG_UNIMP`` and the edge is
ignored. The frame-lock, auxiliary and sense inputs are model GPIO input
lines.

Shadow registers and vertical blank
-----------------------------------

GBE copies OVR_2 to OVR_1, FRM_3 to FRM_2 and DID_SHADOW to DID at the
beginning of blanking (:src:`sgi-gbe-1.1-spec p. 42-44`), *established*.
It reads the overlay and normal tile pointer lists at the beginning of
vertical blanking (:src:`sgi-gbe-1.1-spec p. 14, 17, 19`), *established*.
That the shadow copy uses the same vertical edge, giving software a full
frame to update the pointers, is *inferred*. The specification does not
say which blank edge the shadow copy uses.
The model does all of it, and refetches the DID table, on the first dot of
the line where ``vt_y`` equals the ``vt_vblank_on`` field of VT_4, an
*approximation*.

A write to OVR_1, FRM_2 or DID changes the active copy at once and is
overwritten at the next vertical blank. Linux and NetBSD write the
shadow registers, *software expectation*.

.. _sgi-gbe-clock:

Dot clock
=========

CTRLSTAT bits 29:28 pick the pixel clock: an external TTL input, an
external differential input or the internal PLL. The internal PLL makes

.. code-block:: text

   f_out = f_ref * (M + 1) / ((N + 1) * 2^P)

from the 20 MHz crystal (:src:`sgi-gbe-1.1-spec p. 4, 30-31`),
*established*. M and N are programmed one less than the divider. The
specification states a maximum dot clock of 140 MHz
(:src:`sgi-gbe-1.1-spec p. 1`), *established*, and tabulates the
supported modes up to 1280 by 1024 at 75 Hz with a 135 MHz clock
(:src:`sgi-gbe-1.1-spec p. 32-33`).

The VCO frequency, ``f_ref * (M + 1) / (N + 1)``, must lie in a range for
the PLL to lock. The specification leaves the thresholds "TBD"
(:src:`sgi-gbe-1.1-spec p. 31-32`). Linux's ``gbefb.c`` uses 80 to 220 MHz
with a 20 MHz reference for chip revisions below 2 and 27 MHz otherwise,
*software expectation*.

The model computes the clock exactly with rational arithmetic, so fractions
of a dot are not lost. It treats the Linux range as the lock window, an
*approximation*:

- If the source is the PLL, the clock runs when RUN (bit 20) is set and the
  VCO frequency is in range, and not otherwise.
- A write recomputes the status bits. An out-of-range setting sets both
  bit 22 and bit 23. An in-range setting with RUN clear sets bit 23.
  A running in-range PLL clears both. There is no lock delay and no
  restart control.
- The reference is the ``reference-hz`` device property, default 20 MHz,
  at most 1 GHz.
- If the source is external TTL or differential, the clock is the value of
  the ``external-ttl-hz`` or ``external-differential-hz`` property. RUN
  does not stop it. The default of 0 means no input, and the counters do
  not run. The machine passes ``gbe-external-ttl-hz`` to ``external-ttl-hz``.
- The model does not enforce the 140 MHz limit.

CTRLSTAT resets to select the external TTL input, so a guest that has not
chosen the PLL sees no clock unless the machine supplies one. Linux writes
``0x300aa000`` to CTRLSTAT, which selects the PLL, in ``gbe_reset`` and
sets RUN afterwards (``gbefb.c``), *software expectation*. The reset value
of DOTCLK has both status bits set: M, N and P are zero, so the VCO
frequency is the reference itself, 20 MHz, below the window. The
specification gives no reset for these bits, so the model's reset
value is an *approximation*.

Frame buffer and overlay
========================


.. _sgi-gbe-tiles:

Tiles
-----

Pixels are stored in tiles. A tile is 64 KiB, aligned on a 64 KiB boundary,
128 lines of 512 bytes (:src:`sgi-gbe-1.1-spec p. 13-14`), *established*.
A tile is 512, 256 or 128 pixels wide at 8, 16 or 32 bits per pixel. A
1280 by 1024 frame at 32 bits per pixel is 10 by 8 tiles, with no unused
pixels (:src:`sgi-gbe-1.1-spec p. 13-14`). The tiles of a plane need not be
contiguous in memory. A list of 16-bit entries holds the upper 16 bits of
each tile's physical address. The list is ordered left to right, then top
to bottom. It starts on a 32-byte boundary and must not cross an 8 KiB
boundary. A zero entry means no memory access for that tile and pixels of
value 0 (:src:`sgi-gbe-1.1-spec p. 14, 17`), *established*. The same
tile size appears in the VICE specification (:ref:`sgi-gbe-vice`).

The same specification pages give the plane registers:

- OVR_0 and FRM_0 hold ``rhs`` in bits 4:0, the width of the right-hand
  tile in 32-byte units, and the whole-tile width in bits 12:5.
- FRM_0 bits 14:13 hold the depth: 0 for 8, 1 for 16 and 2 for 32 bits per
  pixel. The overlay is always 8 bits per pixel.
- FRM_1 bits 31:16 hold the height in pixels, shared by both planes.
- OVR_1 and FRM_2 hold the list pointer in bits 31:5 and the DMA enable in
  bit 0. OVR_2 and FRM_3 are their shadows.
- OVR_0 bit 13 and FRM_0 bit 15 reset the plane FIFO. The specification
  marks both "diagnostic bit, do not modify". Linux pulses FRM_0 bit 15
  while it turns the display off (``gbefb.c``), *software expectation*.

At each vertical blank the model reads the list of each enabled plane:
``(whole tiles + 1 if rhs is non-zero)`` entries for each 128 lines of
height, at most 4096. A list that would cross an 8 KiB boundary, a
zero-size plane, a depth field of 3 or an ``rhs`` above 15 logs
``LOG_UNIMP`` and the plane produces zero pixels until the next fetch. A
list read outside the linked memory logs ``LOG_GUEST_ERROR`` and has the
same effect. With the FIFO-reset bit set the model holds the plane: it
fetches nothing and produces zero pixels. Setting the bit also clears the
list, so the plane stays blank until the bit clears and the next vertical
blank refetches it, an *approximation*. Without the display option the
device is register-only and rejects the bit.

Each pixel read is a 512-byte row read from the tile; the model caches the
last row.

Pixel format and order
----------------------

The pixel formats (:src:`sgi-gbe-1.1-spec p. 13`), *established*:

.. list-table::
   :header-rows: 1

   * - Format
     - Layout
   * - RGB8
     - 32 bits, red 31:24, green 23:16, blue 15:8, low byte unused
   * - RGB5
     - 16 bits, red 14:10, green 9:5, blue 4:0
   * - RGB4
     - 16 bits, red 15:12, green 11:8, blue 7:4
   * - RG3B2
     - 8 bits, red 7:5, green 4:2, blue 1:0
   * - I12
     - 12-bit colour-map index
   * - I8
     - 8-bit colour-map index

Components of 2 to 5 bits widen to 8 bits by bit replication
(:src:`sgi-gbe-1.1-spec p. 18`), *established*.

Multi-byte pixels are big-endian in memory. The specification says pixels
appear "in big endian format within the 256 bit memory width, where the
least significant word corresponds to the leftmost screen pixel"
(:src:`sgi-gbe-1.1-spec p. 14`). Read literally for a 32-byte block on a
big-endian bus, that puts the leftmost pixel at the highest address. The
model puts the leftmost pixel at the lowest address, as an ordinary frame
buffer. Which is right is *unknown*; the specification is the only source,
and a captured frame-buffer dump beside the displayed image would settle it.

The pixel-enable window of VT_13 and VT_14 decides which dots take pixels
from the planes. The model reads the planes as one continuous stream
through that window, in raster order. A window whose length differs from
the stored plane width wraps rows within the stream. Outside the window the
output is the colour of pixel value 0 for the DID at that dot. The
model offsets the window by the pipeline delay Linux and NetBSD program
(19 dots before the ``on`` field), an *approximation*.

Display modes and colour
========================

Display ID table
----------------

Each dot has a 5-bit display ID that selects one of 32 window-mode
entries. IDs come from a run-length-encoded table in memory
(:src:`sgi-gbe-1.1-spec p. 15`), *established*. The table is a frame table
and a set of line tables in one 64 KiB block aligned on 64 KiB. The
``did_base`` field of DID (bits 15:0) holds the upper 16 bits of its
address and bit 16 enables it. A disabled table produces ID 0.

A frame table entry is 32 bits, ``block`` in 25:19, ``offset`` in 18:11 and
``yend`` in 10:0. It describes a run of lines that share one line table at
byte address ``did_base + 512 * block + 2 * offset``. A line table entry is
16 bits, ``did`` in 15:11 and ``xend`` in 10:0. A line table must not cross
a 512-byte boundary, and its last entry should have ``xend`` 2047
(:src:`sgi-gbe-1.1-spec p. 15`), *established*.

The model fetches table bytes on demand and caches them until the next
vertical blank. It finds the first frame entry whose ``yend`` is at or
above the line, then the first line entry whose ``xend`` is at or above the
dot. Ends that do not increase, a line table that runs out, a read outside
linked memory or a coordinate above 2047 latches a fault until the next
vertical blank, produces ID 0 and logs ``LOG_UNIMP`` or
``LOG_GUEST_ERROR``. The DID counters start from the VT_17 presets with a
20-dot pipeline offset, which matches the ``htotal - 20`` that NetBSD
programs (``crmfb.c``), an *approximation*.

Window-mode table
-----------------

WID[0..31] hold, per ID (:src:`sgi-gbe-1.1-spec p. 16, 44`),
*established*:

- ``buf`` bits 1:0: 1 lower half, 2 upper half, 3 both.
- ``typ`` bits 4:2: 0 I8, 1 I12, 2 RG3B2, 3 RGB4, 4 RGB5, 5 RGB8.
- ``cm`` bits 9:5: upper five bits of the colour-map index for I8, or the
  direct-colour map for RGB modes when non-zero.
- ``gm`` bit 10: 1 disables gamma.
- ``aux`` bits 12:11: output to the digital pixel bus.

Halves let a 16- or 32-bit plane hold two 8- or 16-bit buffers, for double
buffering. The model supports a half when it is wide enough for the type:
8 bits for I8 or RG3B2, 16 bits for the others. A zero ``buf``, a type of 6
or 7, a plane depth field of 3 or a half too narrow for its type logs
``LOG_UNIMP`` and gives colour 0. The ``aux`` field has no output.

Colour and gamma maps
---------------------

The colour map has 4608 entries of red, green, blue in bits 31:8. I8
indexes it with ``cm`` and the 8-bit pixel. I12 indexes entries 0 to 4095.
For direct-colour types with a non-zero ``cm`` each component is mapped
separately through entry ``cm << 8 | component``, for X direct-colour
visuals. With ``cm`` zero the components pass straight to gamma
(:src:`sgi-gbe-1.1-spec p. 17`), *established*.

The gamma map has three 256-entry tables. Cursor pixels bypass it, overlay
pixels always use it and other pixels use it unless ``gm`` is set
(:src:`sgi-gbe-1.1-spec p. 18`), *established*. Reading the colour map
is a diagnostic operation, because it is single-ported and a read in the
active area disturbs the picture; colour-map writes are drained during
blanking by a 64-entry FIFO (:src:`sgi-gbe-1.1-spec p. 18`),
*established*. The model has neither the artefact nor the FIFO.

The model logs ``LOG_UNIMP`` once per entry when it uses a WID, colour map
or gamma entry that no write has set. Such an entry reads as zero.

Overlay and cursor
==================

Overlay
-------

The overlay is an 8-bit colour-index plane with no display ID. Index 0 is
transparent. Other indexes address colour-map entries 4352 to 4607, the
last 256 entries, and always pass through the gamma map
(:src:`sgi-gbe-1.1-spec p. 14, 17`), *established*. An overlay pixel
replaces the normal-plane pixel. Setting the DMA enable in OVR_1 to zero
stops overlay fetches and yields transparent pixels
(:src:`sgi-gbe-1.1-spec p. 14`).

Cursor
------

The hardware cursor is 32 by 32 pixels with three colours and a
transparent value, or a crosshair (:src:`sgi-gbe-1.1-spec p. 21-22`),
*established*:

- CRS_0 holds ``crs_posx`` in 15:0 and ``crs_posy`` in 31:16. The
  coordinates are signed in the model.
- CRS_1 bit 0 enables the cursor and bit 1 selects the crosshair.
- CRS_CM[1..3] hold the three colours. Glyph value 0 is transparent.
- CRS_GLY[0..63] hold the glyph, 16 two-bit pixels per word, left pixel in
  bits 31:30. Words 0 and 1 are the left and right halves of the top row.
- In crosshair mode the glyph is ignored and colour 1 is used.

The cursor wins over overlay and normal pixels and bypasses gamma
(:src:`sgi-gbe-1.1-spec p. 17-18`), *established*. Software should disable
the cursor while it writes the glyph (:src:`sgi-gbe-1.1-spec p. 22`).

The model places the cursor in the raster from CRS_0, the VT_18 presets and
the colour-map window of VT_15 and VT_16, with a fixed 54-dot horizontal
offset. NetBSD programs ``htotal - 54`` in the VT_18 start field
(``crmfb.c``), *software expectation*. The offset and the (31, 31)
alignment in the specification (:src:`sgi-gbe-1.1-spec p. 21`) are
*approximation*. The model composes the cursor into the pixel row
itself. It does not use the host mouse cursor.

Video capture
=============

GBE can send the displayed picture back to memory as a field of video
for the MACE video-out block. A filter stage reduces the 1280 by 960
active area to PAL or NTSC size with a horizontal, a vertical and an
anti-flicker filter. A DMA stage then writes the field in 64 KiB blocks
listed by a descriptor list in memory, 512 bytes at a time
(:src:`sgi-gbe-1.1-spec p. 23-30`), *established*. Software selects the
area with an inclusive rectangle and the filter with VC_2
(:src:`sgi-gbe-1.1-spec p. 28`).

The model implements only the rectangle:

- VC_0 holds ``vc_left`` in 11:0 and ``vc_right`` in 23:12. VC_1 holds
  ``vc_top`` in 11:0 and ``vc_bottom`` in 23:12
  (:src:`sgi-gbe-1.1-spec p. 47`), *established*. Both reset to 0.
- Each register keeps its low 24 bits independently. The two fields are not
  checked against each other, so an inverted rectangle is kept as written.
  Bits 31:24 read as zero, a model choice.
- Reads and writes of the two registers do no capture, no DMA and no
  display work, and do not raise an interrupt. The saved state includes
  them.
- The model has no filter, no descriptor list and no capture DMA. VC_2
  to VC_8, and the rest of the capture window up to ``0x080fff`` and
  beyond, read ``0xffffffff`` and ignore writes, each with a
  ``LOG_GUEST_ERROR`` line. The VC_3 descriptor-list enable is documented
  as not functional on revision 1.1, and VC_8 is write-only
  (:src:`sgi-gbe-1.1-spec p. 47-48`), *established*.

The specification's memory map lists nine capture registers in a four-word
span (``VC_{0..8}`` at ``0x080000``-``0x08000c``, p. 34) while the
register tables put VC_8 at ``0x080020``. The tables are consistent with
each other and the model follows them.

.. _sgi-gbe-dma:

Interrupts and memory access
============================

GBE has four interrupt sources, matched against ``vt_y`` as above. CRIME
latches them in interrupt bits 16 to 19, described as "GBE Interrupt #0"
to "#3", edge sensitive, and routes them to a processor interrupt line
(:src:`sgi-crime-1.5-spec PDF p. 106-108, 110, 116`), *established*. The model connects
GBE's four one-shot outputs to the CRIME external inputs 0 to 3, which
CRIME maps to bits 16 to 19. The route to a processor line belongs to
CRIME (:doc:`../ip32`).

GBE only reads system memory. Its address space is the 32 bits of its CRIME
link: 1 GB of memory at ``0x40000000`` that CRIME answers, 2 GB reserved at
``0x80000000`` and 1 GB reserved at 0 (:src:`sgi-crime-1.5-spec PDF p. 49-50`
Table 3-3), *established*. The tile pointers and list pointers hold
addresses in that space. The device has a ``dma-view`` property:

- ``crime-1.5`` (default) maps the range ``0x40000000``-``0x7fffffff`` to
  offsets 0 and up of the linked memory, as the table says.
- ``legacy-software`` maps bus addresses ``0x00000000``-``0x0fffffff`` to
  the same offsets, a window for software written against an earlier CRIME,
  an *approximation* with no source for its bounds.

The ``o2`` machine selects ``legacy-software``. The register test
profile selects the view from its ``crime-profile`` option. Both link the CRIME model's decoded memory region. A read outside the
window or the memory logs ``LOG_GUEST_ERROR``, and the plane or table
involved stays blank until the next fetch. The memory must not exceed
3 GiB. The model has no DMA write path, no 16-deep request queue and no
bus ownership token.

Display output
==============

With ``functional-display`` set the device binds a scanout to a QEMU
console. The console size is the colour-map write window: width is the
distance from ``vt_hcmap_off`` to ``vt_hcmap_on`` over the line period, and
height likewise from VT_16 over the frame period. The window is the active
area in the drivers' programming; NetBSD writes ``hdisplay`` and ``vdisplay``
there (``crmfb.c``). Linux programs it the same way, *software expectation*.
The model resolves each dirty line on demand and redraws everything after
any register write and at each vertical blank.

While the selected pixel clock is absent, for example after software
selects the external TTL clock on a board without one, the console shows
black, and the picture returns from the unchanged timing registers when
the clock does. What a monitor shows with no pixel clock is *unknown*, so
this is an *approximation*.

The scanout has no retrace timer of its own. The GBE counters own vertical
blank, shadow copies and list fetches. The model has no monitor, no EDID, no
DAC sense wiring, no retrace-driven monitor power state and no flat-panel
output. These are *approximation*. A device made without
``functional-display`` is a register file: it has no counters, memory
access or console, and rejects the FIFO-reset bits.

The ``o2`` machine always adds the device as ``/machine/gbe`` at
``0x16000000`` with ``functional-display`` and ``external-ttl-hz`` of
20 MHz. Its register test profile (``-M o2,register-test-profile=on``)
has no GBE without ``gbe11-registers``: a load or store at
``0x16000000`` fails with a bus error, and there is no display. With
``gbe11-registers=on`` that profile adds the device as
``/machine/gbe`` at ``0x16000000``. It then requires the eight-bank 1 GiB
memory profile (``bank-profile=8x128m-synthetic`` with ``-m 1G``),
``reset-low-ram-approx=on``, ``prom-private-window=on``,
``prom-sb64-reset=on``, ``rom-image`` settings, and either
``empty-renderer-status=on`` or ``functional-memory-transfer=on``. The
machine prints the full list when one is missing. ``gbe-external-ttl-hz``
needs ``gbe11-registers=on``. ``gbe-one-tile-diagnostic`` is rejected at
start-up.

Reset
=====

Reset restores the values in the register map, stops the counters, clears
the shadow copies, tile lists, DID cache and faults, forgets which table
entries software has written, writes 3 to both I2C registers (see the I2C description above) and
re-evaluates the outputs. The colour, gamma, WID, cursor and capture
registers reset to 0. The specification gives no reset for those tables.

.. _sgi-gbe-vice:

VICE
====

GBE and VICE do not talk to each other. Both read and write the same main
memory through CRIME. The VICE design specification describes a "tiled"
view of memory with 512 by 128 blocks that make 64 KiB pages, and
decomposes its pointer so that software sees raster order while the
hardware crosses pages every 512 locations horizontally and 128 lines
vertically (:src:`099-0123-003 p. 36`), *established*. That is the GBE tile
of :ref:`sgi-gbe-tiles` read as 512 columns of bytes by 128 lines, so a
frame buffer GBE displays can be addressed by VICE through its tile
mode, an *inferred* reading. The specification says nothing about GBE
beyond this. QEMU's VICE model (:doc:`vice`) translates through a tiled index, but the
``o2`` machine does not fit VICE, so the layout GBE shares with it has no
behaviour or test of its own in the machine.

Migration
=========

The device saves version 1 of its state. That covers every register and
table, the fetched tile lists, the DID cache and fetched DID base, the
counter phase, the capture bounds and the I2C pin latches. The saved
``model_config`` byte and the three clock inputs must match on the
destination, because the stream carries equality checks on them. Migration
is only valid between identical builds. It is blocked while an I2C transfer
is in progress on either bus.

Measurements
============

No register of this part has been read on a physical machine for this
project. Published captures are listed under
:ref:`sgi-gbe-identity`.

.. _sgi-gbe-limits:

Model limits
============

- **CMFIFO.** The specification says bits 5:0 are the number of free
  entries of a 64-deep FIFO (:src:`sgi-gbe-1.1-spec p. 45`),
  *established*, and the model reads them that way. It has no FIFO and
  writes drain before the access completes, so the FIFO is always empty.
  At revision 1 the six-bit field wraps 64 free entries to 0, an
  *inferred* reading: 64 does not fit six bits, and the specification
  gives a reset value of 000000. At revision 2 the model reads 63. No
  public source gives a revision 2 reading, so that value is an
  *approximation* and what hardware returns is *unknown*.
  Linux's ``gbefb.c`` waits while ``cm_fifo >= 63``, treating the value
  as occupancy, a *software expectation* that disagrees with the
  specification.
- **Counter period.** VT_1 is the period, not ``period - 1``.
- **Pixel order.** Leftmost pixel at the lowest address. *unknown*.
- **DOTCLK status.** Resets with out-of-range and out-of-lock set, which
  reads as locked under the spec's p. 32 table. The
  lock window, the lack of a lock delay and the missing restart control
  are *approximation*.
- **SYSCLK and PLL test bits.** SYSCLK reads 8 and the PLL test bits are
  refused. Neither is functional.
- **CTRLSTAT sense.** Reads 1.
- **Vertical blank.** The shadow copy, list fetch and DID refetch all
  happen at the first dot of the ``vt_vblank_on`` line.
- **Pipeline offsets.** The pixel-enable lead (19 dots), the 2-dot
  producer extension, the DID lead (20 dots), the DID row (+1) and the
  cursor lead (54 dots) are fixed constants.
- **Sync polarity.** VT_6 low bits ignore the invert bit. Composite sync
  ignores CTRLSTAT bit 27.
- **Outputs.** There is no DAC data, blank, pixel clock or flat-panel data.
  The ``aux`` field of WID is ignored.
- **Auxiliary pins.** The ten pins and their output enables are exposed as
  twenty ``auxiliary`` output lines and ten inputs, with no board wiring.
- **Capture.** Only the VC_0 and VC_1 rectangle is kept.
- **Memory.** No DMA writes. The ``legacy-software`` window is an
  approximation. The memory size limit is 3 GiB.
- **Access.** A wrong width or alignment is a bus error. The model reads an unmapped
  offset inside the 16 MiB region as ``0xffffffff``. Both log
  ``LOG_GUEST_ERROR``. Unsupported fields and malformed tables log
  ``LOG_UNIMP``: PLL test bits, SYSCLK controls, clock source 2, invalid
  timing, plane geometry, uninitialised table entries and unsupported WID
  modes.
- **Revision.** The model is revision 1.1 (CTRLSTAT revision 1). It does not
  model the 27 MHz reference Linux selects for later revisions, except
  through ``reference-hz``.

Tests
=====

``tests/qtest/sgi-gbe-test.c`` is built when ``CONFIG_SGI_O2_GBE``
is set. It uses synthetic fixtures, not captured data. Its cases cover:

- ``/sgi-gbe/coverage/*`` and ``/sgi-gbe/reset``: every indexed table
  at its first and last entry, the timing registers, the scalar resets,
  reset and migration.
- ``/sgi-gbe/capture/*``: the capture bounds. ``registers`` covers storage,
  independence, inverted values, unused bits and reset. ``access`` covers
  refusal of other widths and alignments. ``no-effects`` shows no display,
  DMA, counter or interrupt change and the refusal of VC_2 and up.
  ``migration`` covers the saved bounds.
- ``/sgi-gbe/clock``, ``fractional-clock``, ``clock-pin-phase``,
  ``invalid-timing-reports`` and ``events``: PLL maths, the external
  inputs, the lock window, one-shot events and the frame-lock preset.
- ``/sgi-gbe/frame-lock-outputs``: the output lines and VT_6 controls.
- ``/sgi-gbe/dma-reset/*``: the FIFO-reset bits and plane holds.
- ``/sgi-gbe/multitile``, ``shadow-bounds``, ``descriptor-cache``,
  ``formats``, ``depth-halves``, ``rgb8-geometry-cursor``,
  ``wrapped-producer`` and ``netbsd-vertical-phase``: tile lists, shadow
  copies, pixel formats, half buffers and pipeline phase.
- ``/sgi-gbe/did-*``: DID table parsing, caching and malformed tables.
- ``/sgi-gbe/cursor-*``: cursor bounds and projection.
- ``/sgi-gbe/i2c-*``: idle lines, attached slaves, reset and the
  migration blocker.
- ``/sgi-gbe/dma-address/*``: both address views, their limits and
  interaction with the CRIME memory-transfer banks.
- ``/sgi-gbe/access-unsupported``: refusal of other widths.
- ``/sgi-gbe/board-interrupts``, ``board-mte-scanout`` and
  ``dma-reset/board``: the device in the O2 register test profile, the CRIME
  interrupt bits and the memory-transfer path to the display.

``tests/qtest/sgi-o2-test.c`` covers the machine side.
``o2/gbe11-registers`` checks the absent device by default, the 16 MiB
region, revision and DEVICE_ID, the bus error on wrong widths and a reset.
``o2/gbe-external-ttl/*`` checks the clock property and its use as the
dot clock. ``o2/gbe-one-tile-retired`` checks that the old option is
rejected. The option-dependency checks live in the same file.
