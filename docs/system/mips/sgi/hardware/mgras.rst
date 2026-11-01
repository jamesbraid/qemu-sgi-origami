.. SPDX-License-Identifier: BSD-3-Clause

.. _sgi-mgras:

=======================
IMPACT: graphics boards
=======================

Used in :ref:`sgi-ip30` (Octane, Octane2) and :ref:`sgi-ip27` (Origin
2000, and the Origin 200 with its GIGAchannel cabinet). IMPACT is SGI's
XIO graphics family for the Octane and the Origin 2000, Origin 200 and
Onyx2, sold as SI, SI with texture, SSI and MXI and later as the E-series
ESI, ESI with texture, ESSI and EMXI. Each board presents one XIO widget,
built around the HQ4 host interface, with one or two GE11 or GE12 geometry
engines, one or two RE4 raster engines, two PP1 pixel processors per
raster engine, and on the textured boards one TE1 texture engine and its
TRAM per raster engine. QEMU models the family as ``sgi-mgras`` in
``hw/misc/sgi-mgras.c``, with the raster engine and DMA in
``sgi-mgras-raster.c``, scanout in ``sgi-mgras-display.c``, texture in
``sgi-mgras-texture.c`` and the OpenGL command subset in
``sgi-mgras-gl.c``; ``hw/display/sgi/`` holds the scanout, display-list
work and DMA code it shares with the other SGI graphics heads. The
``product`` property picks the board; the default is ``si``. The
``octane``, ``octane2``, ``origin2000-deskside``, ``origin2000-rack`` and
``origin200-gigachannel`` topologies offer it in their graphics slots
(:ref:`sgi-ip30-graphics`, :ref:`sgi-sn-graphics-pipes`), and the overview
of all three graphics families is :ref:`sgi-graphics`.

.. seealso::

   Platform pages: :ref:`sgi-ip30`, :ref:`sgi-ip27`, :ref:`sgi-sn`.

Facts are rated as described in :ref:`sgi-ratings`. No IMPACT register
specification is public. The model is a functional profile: it gives the
stock IRIX 6.5.30 kernel, X server and OpenGL client and the stock IP30
PROM the registers and command words they use, and draws what those
programs ask for. Most of the board's behaviour here is therefore an
*approximation* or a *software expectation*.

.. _sgi-mgras-sources:

Sources
=======

- SGI's *OCTANE Technical Report*, :src:`octane-technical-report section 4`:
  the chip counts per board (Table 4.1), RDRAM per board, the TE1 and its
  4 MB of TRAM per raster engine, and the even and odd line split between
  two RE4s. It gives no register values.
- The Higher Intellect wiki page on IMPACT, :src:`preterhuman-impact`, for
  the family's configurations and names.
- OpenBSD ``sys/arch/sgi/dev/impact.c``, ``impactreg.h`` and
  ``sys/arch/sgi/xbow/impact_xbow.c`` at ``2df91d80c104``, as software
  expectations: the widget identity, the window offsets, the 64-bit FIFO
  packet, the raster register numbers and names used below (``IR_ALIAS``,
  ``BLOCKXYSTARTI``, ``FILLMODE``, ``PP1SELECT`` and the rest), and the
  console fill and glyph sequences.
- Linux ``arch/mips/include/asm/xtalk/xwidget.h`` at ``2f9060b1db4a`` for
  the widget identity, and ``include/video/newport.h`` at ``db2ddb871435``
  for the VC2, the Newport sibling of the VC3, whose control register
  places its interrupt and video enables at the bits the VC3 uses.
- X.Org's IMPACT driver ``xf86-video-impact`` 0.2.0 at ``b1129f721baa``,
  ``src/impact_regs.h`` and its DMA wait, and Stanislaw Skowronek's Octane
  IMPACTSR framebuffer driver as posted to linux-mips,
  :src:`linux-mips-impactsr-2008`, as software expectations for the DMA
  page list on Octane.
- MAME's Newport model at ``b1da4a1b278c``,
  ``src/devices/bus/gio64/newport.cpp``, for the VC2 timing and
  display-ID table walk the model applies to the VC3.
- Published ``hinv`` and ``gfxinfo`` output of real boards:
  :src:`mimgfx-origin2000-inventory` (SI with texture, GM10 board),
  :src:`usenet-1123184831` (SI with texture, MOT10 board, Origin 200),
  :src:`nekonomicon-17194` (ESI with texture, MOT10 board, Origin 2000),
  :src:`usenet-338549df` (MXI, Octane), :src:`usenet-8nbvii` (a two-GE
  GE12 board with texture, Octane), :src:`usenet-bqfo2p` (ESSI, Octane),
  and the Octane inventories :src:`nekonomicon-16972` (MXI, GM20),
  :src:`nekonomicon-12262` (EMXI, MOT20) and :src:`nekonomicon-16726610`
  (two ESI, MOT10).
- The IRIX 6.5.30 IP27 kernel, X server and IP27 OpenGL client library,
  the ``hinv`` and ``gfxinfo`` programs, and the stock IRIX 6.5.30 Octane
  IDE diagnostic, read as shipped binaries, as software expectations.
- The IP30 PROM, :src:`ip30-prom-4.17 graphics probe`, which names an
  IMPACT board from its serial-number record.
- The GE11 program IRIX 6.5.30 ships, examined as a file for its record
  format only.

No IMPACT board has been measured for this model.

.. _sgi-mgras-products:

Products and population
=======================

The table gives each ``product`` value, the chips it carries, and the
board record the model fits on Octane. Counts for SI, SI with texture,
SSI and MXI are *established* by
:src:`octane-technical-report Table 4.1`, which also gives 12 MB of RDRAM
to SI and 24 MB to SSI. The E-series boards carry the same counts with
later parts; that is *inferred* from the first-generation pattern and the
E-series captures, since no E-series datasheet is public. Board names and
part numbers come from published inventories: GM10 ``030-0938`` on an SI
with texture (:src:`mimgfx-origin2000-inventory`), GM20 ``030-0957`` on
MXI (:src:`nekonomicon-16972`), MOT10 ``030-1241`` on ESI
(:src:`nekonomicon-16726610`) and MOT20 ``030-1240`` on EMXI
(:src:`nekonomicon-12262`), all *observed*. That plain SI uses GM10, SSI
GM20 and ESSI MOT20 is *inferred*. Each revision letter is one a capture
shows; GM10 revision U for plain SI is an *approximation*.

.. list-table::
   :header-rows: 1

   * - ``product``
     - ``hinv`` name
     - GE
     - RE4
     - PP1
     - TE1 and TRAM
     - Board record
   * - ``si``
     - SI
     - 1 GE11
     - 1
     - 2
     - none
     - GM10 ``030-0938-003`` U
   * - ``si-t``
     - SI with texture option
     - 1 GE11
     - 1
     - 2
     - 1, 4 MB
     - GM10 ``030-0938-003`` U
   * - ``ssi``
     - SSI
     - 2 GE11
     - 2
     - 4
     - none
     - GM20 ``030-0957-003`` N
   * - ``mxi``
     - MXI
     - 2 GE11
     - 2
     - 4
     - 2, 4 MB each
     - GM20 ``030-0957-003`` N
   * - ``esi``
     - ESI
     - 1 GE12
     - 1
     - 2
     - none
     - MOT10 ``030-1241-002`` J
   * - ``esi-t``
     - ESI with texture option
     - 1 GE12
     - 1
     - 2
     - 1, 4 MB
     - MOT10 ``030-1241-002`` J
   * - ``essi``
     - ESSI
     - 2 GE12
     - 2
     - 4
     - none
     - MOT20 ``030-1240-003`` G
   * - ``emxi``
     - EMXI
     - 2 GE12
     - 2
     - 4
     - 2, 4 MB each
     - MOT20 ``030-1240-003`` G

The model renders an E-series board exactly as its first-generation
counterpart, an *approximation*: no capture shows GE12, the later PP1 or
MGRAS revision 4 behaving differently to software. Every board scans out
1280x1024, the screen the stock IRIX timing sets up. Which boards also
drive 1600x1200 is *unknown*: the report's feature list gives it to MXI
alone while its Table 4.3 lists SI and SSI visuals at that size. The model
has no mode change. A board whose descriptor named a screen wider than
1280 pixels would be refused at realize.

Published Octane inventories give each IMPACT board, single- or
double-width, one widget and one serial-number record, *observed*
(:src:`nekonomicon-16972`, :src:`nekonomicon-12262`). The model presents
one widget per board.

.. _sgi-mgras-identity:

Identity
========

Widget identity
---------------

The HQ4's widget identity is manufacturer ``0x2aa`` and part ``0xc003``,
the pair OpenBSD's ``impact_xbow.c`` matches and Linux's ``xwidget.h``
names, a *software expectation*
(:ref:`sgi-graphics-widget-identities`). The ``manufacturer``, ``part``
and ``revision`` properties set the word at ``0x000004``, read as
``revision << 28 | part << 12 | manufacturer << 1 | 1``: ``0x0c003555``
with the defaults. Revision 0 there is a placeholder; the production
revision is *unknown*.

The IRIX 6.5.30 kernel takes the HQ revision from bits 31:28 of a 32-bit
read at ``0x000000``, and the stock Octane IDE diagnostic accepts the
board only when that same word carries the part in bits 27:12 and the
manufacturer in bits 11:1, warning that a revision below 2 is a
prototype, *software expectation*. The model answers ``0x000000`` with a
full identification word holding the board's HQ revision, 2 on every
board (``0x2c003555``). Bit 0 set there copies ``0x000004``, an
*approximation*. A 64-bit read of ``0x000000`` returns the word in the
upper half and zero below. Why the two offsets carry different revisions
is *unknown*.

How IRIX tells the boards apart
-------------------------------

The IRIX 6.5.30 kernel builds its board record from these reads, and
``hinv`` names the board from the counts and one E-series marker. All of
this is *software expectation* from the kernel and ``hinv`` binaries:

- ``0x072488``, top byte of a 32-bit read: GE count in bits 1:0, MGRAS
  revision in bits 4:2, product ID in bits 6:5. Bit 7 is not read.
- ``0x072408``, top byte: TRAM per raster path. For product 2 or 3 the RE
  count equals the GE count, and each path reports four TRAMs unless its
  two-bit field (bits 1:0 for path 0, bits 3:2 for path 1) is 3. Product 1
  is a one-RE board with no texture.
- ``0x071308``, byte: the attached monitor's ID in bits 7:4.
- ``0x071708``, byte: CMAP revision in bits 2:0, RA revision in bits 6:4;
  bit 7 clear means a video option is fitted.
- ``0x071d00`` under XMAP index byte 4: the PP1 revision in bits 2:0.
- ``0x02c578``: the RE4 revision in bits 7:4.
- ``0x028004``: after the GE program verifies, the low byte is the GE
  revision; 3 or more marks an E-series board. ``0x028000`` is read first
  and discarded.
- VC3 register ``0x1f``: the VC3 revision in bits 7:6.

``hinv`` then prints SI, SSI, MXI or "SI with texture option" for
one or two GEs with or without TRAM, with an ``E`` prefix when the marker
is set. A GE program that fails verification makes the kernel record GE
revision ``0xff``, so the board prints as E-series, *inferred* from the
kernel and ``hinv``.

The stock IP30 PROM takes its E-series mark from the board's
serial-number record instead: a record named ``MOT`` sets it, one named
``GM`` clears it, and a board with no readable record prints as
E-series (:src:`ip30-prom-4.17 graphics probe`), *software expectation*.

Per-board answers
-----------------

The model answers those reads from a per-board descriptor. "Basis" says
where each board's values come from.

.. list-table::
   :header-rows: 1

   * - ``product``
     - ``0x072488`` byte
     - ``0x072408`` byte
     - GE rev
     - PP1 rev
     - CMAP rev
     - Basis
   * - ``si``
     - ``0x45``
     - ``0x0f``
     - 2
     - 0
     - 3
     - *inferred* from the SI with texture captures with the TRAM taken
       away; no textureless SI has been captured.
   * - ``si-t``
     - ``0x45``
     - ``0x0c``
     - 2
     - 0
     - 3
     - *observed* (:src:`mimgfx-origin2000-inventory`,
       :src:`usenet-1123184831`): product 2, MGRAS revision 1, one GE,
       ``GE11 rev B``, four TRAMs.
   * - ``ssi``
     - ``0x46``
     - ``0x0f``
     - 2
     - 0
     - 3
     - *approximation*: no capture. Product 2 and SI's revisions, though
       every captured two-GE board prints product 3.
   * - ``mxi``
     - ``0x66``
     - ``0x00``
     - 2
     - 0
     - 3
     - *observed* (:src:`usenet-338549df`): product 3, MGRAS revision 1,
       the SI with texture's ASIC revisions.
   * - ``esi``
     - ``0x51``
     - ``0x0f``
     - 3
     - 4
     - 3
     - *inferred* from the ESI with texture capture.
   * - ``esi-t``
     - ``0x51``
     - ``0x0c``
     - 3
     - 4
     - 3
     - *observed* (:src:`nekonomicon-17194`): MGRAS revision 4,
       ``GE12 rev A``, ``PP1 rev E``.
   * - ``essi``
     - ``0x72``
     - ``0x0f``
     - 3
     - 7
     - 4
     - *observed* (:src:`usenet-bqfo2p`): product 3, MGRAS revision 4,
       ``PP1 rev H``, ``CMAP rev F``.
   * - ``emxi``
     - ``0x72``
     - ``0x00``
     - 3
     - 7
     - 4
     - *observed* (:src:`usenet-8nbvii`). That this two-GE GE12 board
       with texture is an EMXI is *inferred* from IRIX's naming rule.

Every board answers HQ revision 2, RE4 revision 2, VC3 revision 0 and RA
revision 0, with ``0x071708`` bit 7 set (no video option), as every
capture prints. Bit 7 of ``0x072488``, never read by IRIX, is 0, and the
``0x072408`` bytes beyond "path present" or "path absent" are an
*approximation*: IRIX maps fields 0, 1 and 2 alike to four TRAMs. Writes
to ``0x072408`` and ``0x072488`` do not change these answers.

``0x071308`` answers monitor ID ``0xf`` in bits 7:4 on every board, what
every decoded Origin capture prints and what selects the kernel's stock
1280x1024 60 Hz timing, *observed* for those captures. The ID belongs to
the monitor, not the board. Bits 2:0 carry the board's CMAP revision
because the stock IDE diagnostic reads them beside ``0x071708`` and fails
a board whose two CMAP revisions differ or fall outside 2 to 3, *software
expectation*. Bit 3 is 0, an *approximation*. A 32-bit read returns the
byte in bits 31:24.

``0x028004`` holds the descriptor's GE revision from reset until the first
HQ query (:ref:`sgi-mgras-hq-commands`), then that query's answer. When a
real board replaces its revision is *unknown*. ``0x028000`` answers 0, an
*approximation*, kept distinct so a guest that took its revision there
would see the difference.

The ESI, ESI with texture, ESSI and EMXI descriptors answer PP1 revision 4
or 7 because the captures print ``PP1 rev E`` and ``H``. The stock IDE
diagnostic reads the same register as the XMAP revision and passes only 0
or 1, so it would fail every E-series board as described here. Which side
is right is *unknown*; ``gfxinfo -v`` and the IDE tests from one E-series
board would settle it.

.. _sgi-mgras-board-record:

Serial-number record
--------------------

The stock IP30 PROM and IDE diagnostic read the board's serial-number
chip through a MicroLAN master at ``0x011098``: one 32-bit store per bit
slot carries the low-drive time in bits 19:10 and the sample offset in
bits 9:2, and the reader waits for bit 1 (done) and takes the wire level
from bit 0, *software expectation*. With ``board-record=on`` the model
fits a DS2505-class record carrying the board name, part and revision
from the table above and completes each slot inside the store, an
*approximation* of the timing. The Octane machines set it, with the
marked synthetic barcode and serial ``EMUG00`` unless
``board-record-barcode`` and ``board-record-serial`` give others. Without
a record, which is the default and the Origin machines' choice, the
register reads ``0x2``: done, wire low. Real Origin boards carry a record
(:src:`mimgfx-origin2000-inventory`), so this is a known gap.

.. _sgi-mgras-registers:

Register map
============

The board decodes a 2 MiB window, big-endian. Offsets below are from its
start. Registers are 32 bits unless the width column says otherwise, and
an aligned 64-bit access to a 32-bit register reads it zero-extended or
writes its low word. Reset is a cold reset of the device. Window offsets
``0x020000`` to ``0x020400``, ``0x040000`` to ``0x040030``, ``0x071c00``
and ``0x072000`` match OpenBSD ``impactreg.h``, *software expectation*.
Every other placement, value and side effect is what the IRIX 6.5.30
kernel, X server, OpenGL client or the stock IDE diagnostic uses, and
what the model answers is an *approximation* unless the notes say
otherwise.

.. list-table::
   :header-rows: 1
   :widths: 14 22 10 10 10 34

   * - Offset
     - Name
     - Width
     - Reset
     - Access
     - Notes
   * - ``0x000000``
     - HQ identification
     - 32, 64
     - see notes
     - R
     - Full identity word with the HQ revision; writes ignored.
   * - ``0x000004``
     - Widget identification
     - 32
     - ``0x0c003555``
     - R
     - From the properties.
   * - ``0x000020``
     - Reserved half
     - 32
     - 0
     - R0, WI
     -
   * - ``0x000024``
     - Widget control
     - 32
     - 0
     - RW
     - Stored, no effect.
   * - ``0x000030``-``0x00003c``
     - Interrupt destination
     - 32
     - 0
     - RW
     - Upper at ``0x30``/``0x34``, lower at ``0x38``/``0x3c``; each pair
       aliases one register. Carries the widget-error interrupt.
   * - ``0x008000``
     - Pre-selection status
     - 32
     - 0
     - R
     - The kernel polls ``& 0x1eff`` for zero before a context selection.
   * - ``0x011000``
     - Context control
     - 32
     - 0
     - RW
     - Stored. A write must precede a context selection.
   * - ``0x011008``
     - Context word
     - 32
     - 0
     - RW
     - Stored.
   * - ``0x011020``
     - Interrupt status
     - 32
     - 0
     - RW
     - Pending sources; written bits are kept apart and read back ORed in.
   * - ``0x011028``
     - Pending clear
     - 32
     - none
     - W1C
     - Also acknowledges the context handshake.
   * - ``0x011030``, ``0x011038``
     - Context words
     - 32
     - 0
     - RW
     - Stored.
   * - ``0x011040``
     - Interrupt enable set
     - 32
     - 0
     - RW, W1S
     - Reads the enable word.
   * - ``0x011048``
     - Interrupt enable clear
     - 32
     - none
     - W1C
     - Clears enables and the model's error latch.
   * - ``0x011050``
     - Second-level enable
     - 32
     - 0
     - RW
     - Setting bit 4 raises the low-water source at once.
   * - ``0x011058``
     - Second-level enable clear
     - 32
     - 0
     - RW
     - A 1 clears that bit of ``0x011050``; clearing bit 4 drops the
       low-water source. Reads the last value written.
   * - ``0x011060``, ``0x011068``
     - HQ interrupt destination
     - 32
     - 0
     - RW
     - Widget in bits 27:24 and address bits 47:32 in bits 15:0 of the
       upper word; address bits 31:0 in the lower.
   * - ``0x011070``
     - HQ interrupt vectors
     - 32
     - 0
     - RW
     - Low-water vector bits 7:0, general 15:8, retrace 23:16.
   * - ``0x011088``
     - Bring-up word
     - 32
     - 0
     - RW
     - Stored.
   * - ``0x011098``
     - MicroLAN master
     - 32
     - ``0x2``
     - RW
     - :ref:`sgi-mgras-board-record`.
   * - ``0x020000``
     - HQ status
     - 32
     - ``0x13``
     - R
     - Bits 0 (raster sync), 1 (idle) and 4. Reads ``0x10`` for at most
       500 ms of continuous display-list work. Writes ignored.
   * - ``0x020008``
     - FIFO status
     - 32
     - 0
     - R
     - Always empty.
   * - ``0x020010``
     - HQ flags set
     - 32
     - 0
     - RW, W1S
     - Reads the flag word.
   * - ``0x020018``
     - HQ flags clear
     - 32
     - 0
     - RW, W1C
     - Reads the flag word.
   * - ``0x020028``
     - Retrace acknowledge
     - 32
     - none
     - W
     - Ignored.
   * - ``0x020038``, ``0x020044``
     - Retrace status
     - 32
     - 0
     - R
     - Read zero.
   * - ``0x020100``
     - Bus status
     - 32
     - 0
     - R
     - Always idle.
   * - ``0x020200``
     - DMA status
     - 32
     - 0
     - R
     - Always idle.
   * - ``0x020400``
     - Normal FIFO
     - 64
     - none
     - W
     - OpenBSD's raster packet (:ref:`sgi-mgras-normal-fifo`).
   * - ``0x028000``
     - GE word 0
     - 32
     - 0
     - R
     -
   * - ``0x028004``
     - GE revision and query answer
     - 32
     - GE rev
     - R
     -
   * - ``0x02c1c4``, ``0x02d1c0``
     - Raster data port
     - 32
     - none
     - R
     - :ref:`sgi-mgras-raster`.
   * - ``0x02c574``
     - RDRAM data
     - 32
     - ``0x8``
     - R
     - 8 for an RE the board has, 0 otherwise.
   * - ``0x02c578``
     - RE4 status
     - 32
     - see notes
     - R
     - ``0x100``, RE4 revision in 7:4, selected RE in 3:2, RE count less
       one in 1:0.
   * - ``0x040000``-``0x040030``
     - FIFO thresholds
     - 32
     - 0
     - RW
     - Thirteen words, stored.
   * - ``0x040068``-``0x040088``
     - Scheduler words
     - 32
     - 0
     - RW
     - ``0x040068``, ``0x040070``, ``0x040078``, ``0x040088``; stored.
   * - ``0x040918``-``0x040928``
     - DMA set-up words
     - 32
     - 0
     - RW
     - Three words, stored.
   * - ``0x050000``-``0x054ffc``
     - HQ program memory
     - 32
     - 0
     - RW
     - 5,120 words; reads return bits 23:0.
   * - ``0x058040``-``0x05804c``
     - GE program ports
     - 32
     - none
     - W
     - :ref:`sgi-mgras-ge`.
   * - ``0x05822c``, ``0x058230``
     - GE read-back
     - 32
     - none
     - R
     - :ref:`sgi-mgras-ge`.
   * - ``0x060008``
     - Bring-up flag
     - 32
     - 0
     - RW
     - Stored.
   * - ``0x061000``-``0x0610fc``
     - Configuration table
     - 32
     - 0
     - RW
     - 64 words, stored; ``0x061038`` is a separate stored word.
   * - ``0x070c30``
     - CMAP selector
     - 16
     - 0
     - RW
     - :ref:`sgi-mgras-display`.
   * - ``0x070d18``
     - CMAP data
     - 32
     - 0
     - RW
     - Auto-increments.
   * - ``0x070d88``
     - VC3 word
     - 8 (write), 32
     - 0
     - RW
     - Stored.
   * - ``0x071208``
     - Display status
     - 8, 32
     - ``0x08``
     - R
     - Fixed.
   * - ``0x071308``
     - Monitor ID, CMAP 0 revision
     - 8, 32
     - see notes
     - R
     - :ref:`sgi-mgras-identity`.
   * - ``0x071708``
     - CMAP and RA revision
     - 8, 32
     - see notes
     - R
     - :ref:`sgi-mgras-identity`.
   * - ``0x071810``
     - DAC address
     - 16
     - 0
     - RW
     - Register number in bits 15:8.
   * - ``0x071811``
     - DAC register number
     - 8
     - 0
     - W
     - Sets bits 15:8 of ``0x071810``.
   * - ``0x071890``
     - Gamma table data
     - 16 (write), 32
     - 0
     - RW
     - The high byte loads the next gamma entry. Reads the last value
       written.
   * - ``0x071910``
     - DAC register data
     - 8
     - see notes
     - RW
     - One byte of the addressed DAC register.
   * - ``0x071988``
     - DAC mode
     - 8, 32
     - 0
     - RW
     - Stored.
   * - ``0x071c08``
     - ``PP1SELECT``
     - 8 (write), 32
     - 0
     - RW
     - :ref:`sgi-mgras-dual-re`.
   * - ``0x071c88``
     - XMAP index
     - 8
     - 0
     - RW
     - Index in bits 6:2.
   * - ``0x071d00``, ``0x071d08``
     - XMAP configuration
     - 32
     - 0
     - RW
     - Per PP1. ``0x071d08`` also takes byte writes. ``0x071d00`` reads
       the PP1 revision under index byte 4 and 1 under index byte 8.
   * - ``0x071d80``
     - XMAP buffer select
     - 32
     - 0
     - RW
     - 32 entries per PP1.
   * - ``0x071e00``
     - XMAP main mode
     - 32
     - ``0x7a4``
     - RW
     - 32 entries per PP1.
   * - ``0x071e80``
     - XMAP overlay mode
     - 32
     - 0
     - RW
     - 32 entries per PP1.
   * - ``0x071f00``
     - PP1 status
     - 32
     - 1
     - R
     - Fixed; writes ignored.
   * - ``0x071f80``
     - PP1 sync
     - 32
     - ``0x101``
     - R
     - Fixed; writes ignored.
   * - ``0x072008``
     - VC3 index
     - 8, 32
     - 0
     - W
     - A 32-bit write takes bits 31:24.
   * - ``0x072038``
     - VC3 index and data
     - 32
     - none
     - W
     - Index in bits 31:24, data in 23:8.
   * - ``0x0720b0``
     - VC3 data
     - 16, 32
     - 0
     - R
     - The indexed register; a 32-bit read puts it in bits 31:16.
   * - ``0x072190``
     - VC3 SRAM data
     - 16, 32
     - 0
     - RW
     - 32K halfwords; the address auto-increments.
   * - ``0x072408``
     - Board version 0
     - 32
     - per board
     - RW
     - Reads the TRAM byte; writes are kept for the timing reference.
   * - ``0x072488``
     - Board version 1
     - 8, 32
     - per board
     - R
     - A byte read returns the byte itself.
   * - ``0x073c08``, ``0x073c88``
     - I2C data and strobe
     - 8, 32
     - 0
     - RW
     - Data is stored; the strobe reads 0 and ignores writes.
   * - ``0x120400``
     - Command port
     - 32, 64
     - none
     - RW
     - :ref:`sgi-mgras-hq-commands`. Reads ``0xf400`` once ready.
   * - ``0x130400``
     - DMA mapping port
     - 32, 64
     - 0
     - RW
     - :ref:`sgi-mgras-dma`. Reads back the last write.
   * - ``0x130500``
     - Context selection
     - 32
     - 0
     - RW
     - :ref:`sgi-mgras-context`.

The widths are the ones the model accepts. Byte reads are accepted only
at ``0x071208``, ``0x071308``, ``0x071708``, ``0x071910``, ``0x071988``,
``0x071c88``, ``0x072488``, ``0x073c08`` and ``0x073c88``; halfword reads
only at ``0x071810``, ``0x0720b0`` and ``0x072190``.

.. _sgi-mgras-interrupts:

Interrupts
==========

The board sends XIO interrupt packets: a raise or lower carrying an
interrupt number, at the widget and address its destination register
names. The IRIX 6.5.30 kernel allocates four interrupts for the board,
packs the low-water, general and retrace vectors into ``0x011070`` and
programs one destination at ``0x011060``/``0x011068``, *software
expectation*. The model follows that packing, an *approximation* for the
routing:

- *Low-water*: raised while pending bit 2 is set, which the model sets as
  soon as ``0x011050`` bit 4 is enabled, since its FIFO is always empty.
  ``0x011040`` does not gate it; the real top-level gate is *unknown*.
- *General*: raised while any of pending bits 19 (context selection), 28
  (context image complete) and 10 (swap reached) is set and enabled in
  ``0x011040``, and lowered when none is. The kernel's handler serves one
  source per call and writes the bit it served to ``0x011028``, so a level
  held while any source is pending is *inferred*.
- *Retrace*: one raise packet per retrace while VC3 register ``0x1e`` bits
  0 and 2 are set, and none to lower it. The IRIX kernel sets bit 0 to
  enable the retrace interrupt, never enables it in ``0x011040`` and never
  acknowledges it on the board, *software expectation*; that bit 0 gates a
  per-frame message is *inferred* from that and the VC2's matching enable
  bit in Linux ``newport.h``.
- *Widget error*: the model's own error latch, pending bit 1, set by any
  access the model rejects, sends a packet to the destination at
  ``0x000030``/``0x000038`` when bit 1 is enabled in ``0x011040``, and
  ``0x011048`` clears it. IRIX's default enable word ``0x3008a200`` leaves
  bit 1 clear.

``0x011040`` sets the enable bits written and ``0x011048`` clears them.
The kernel writes ``0x400`` to ``0x011040`` before a swap and must keep
bit 19 enabled for its next context switch, so a plain store there would
strand the switch; set-and-clear is *inferred* from that, and the stock
IDE diagnostic's register test pairs the two offsets the same way. A
source raised with no destination programmed sends nothing and logs
``LOG_GUEST_ERROR``, once per reset for the retrace. Error sources the
kernel names (FIFO overflow, HQ program error, DMA address error,
privilege violation, DMA access error) are never raised by the model.

.. _sgi-mgras-hq-commands:

HQ4 command port
================

Command words
-------------

Once a context has been loaded (:ref:`sgi-mgras-context`), 32-bit stores
to ``0x120400`` are command words. A header carries a class in bits
31:16, a register or opcode in bits 15:8 and a payload length in bytes in
bits 7:0; the payload follows as whole words, so a 3-byte payload takes
one word. A header with bit 31 set opens an inline data block whose byte
length is bits 19:0, with the first byte's offset in its first word in
bits 23:22. This framing is a *software expectation*: it is what the IRIX
6.5.30 kernel and OpenGL client write and what the stock IDE diagnostic
builds. A 64-bit store is two command words, the high one first. Between
packets the model also takes a 64-bit direct raster record: the value in
bits 31:0 and, from bit 34, a selector whose bits 14:6 name the register
and whose other bits read ``0x60001`` (state) or ``0x70001`` (execute).

The model decodes these classes:

- ``0x0018``/``0x0019`` write raster register ``(class & 1) << 8 | reg``,
  and ``0x001c``/``0x001d`` write and execute it
  (:ref:`sgi-mgras-raster`). The kernel's own literals pair the header
  ``0x00180004 | reg << 8`` with a value, *software expectation*.
- ``0x0008``, ``0x000a`` and ``0x000c`` store DMA descriptor registers
  (:ref:`sgi-mgras-dma`).
- Class 0 opcodes the HQ answers itself: ``0xa2`` (state query, three
  arguments), ``0xa1`` (attribute query, one) and ``0x8b`` (histogram
  query, two) put an answer at ``0x028004`` and set HQ flag 17; ``0x37``
  (swap) raises general source 10; ``0xf2`` (sync request) does nothing,
  since nothing is ever queued ahead of it.
- ``0x000e0400`` sets HQ flag 16, the fence the DMA writers poll; the
  ``0x000e0100`` read fence is consumed quietly.
- The OpenGL client's packets, which the model executes itself
  (:ref:`sgi-mgras-ge`).

The IRIX 6.5.30 client clears flag 17, writes a query and polls the flag
with no bound before reading ``0x028004``; the kernel enables source 10,
writes ``0x00003700`` and sleeps until its handler sees source 10,
*software expectation*. The model sets flag 17 as soon as a query's
arguments arrive and answers from its own GL state or with zero, and it
raises source 10 when it consumes the swap word, since its queue is
always drained; each is an *approximation*, and the board's true answers
are *unknown*. A raster, descriptor or class-0 header with more than 8
payload words, a header with a byte length that is not whole words where
whole words are required, and any unknown class are logged
``LOG_UNIMP`` and their payload is discarded by length, so the next word
parses as a header.

HQ flags
--------

``0x020010`` sets the bits written, ``0x020018`` clears them, and both
read the flag word, *software expectation* from every kernel and client
site. Flag 16 is set by the fence word and by every write-direction DMA
kick, flag 17 by a query; the model sets neither on its own otherwise.
Flag 20 is set and cleared by software only. The model sets flags at
once, an *approximation*: fence completion order is *unknown*.

Status
------

The IRIX kernel and client wait for ``0x020000`` bit 0 to be set before a
pixel read (about 2 s in the kernel, unbounded in the client) and for bit
1 to be set as raster idle (3 s in the kernel, unbounded in ``glFinish``);
no IRIX code waits for either to clear, *software expectation*. X.Org's
driver waits for bit 0 after its DMA without a request, so the bit is
*inferred* to read set whenever the board is idle. The model answers
``0x13`` and, while display-list work is still running, ``0x10`` for at
most 500 ms of continuous work, which stays inside both kernel limits;
when the board clears either bit is *unknown*.

On SN0 machines every processor store the Hub counted on its way to the
board returns one graphics credit to that Hub at once, because the model
takes every word off the port before the store completes
(:ref:`sgi-hub`).

.. _sgi-mgras-normal-fifo:

Normal FIFO
-----------

``0x020400`` takes OpenBSD's raster packet only: a 64-bit write of
``0x0018000400000000`` with the register in bits 48:40, bit 50 for
execute and the value in bits 31:0, *software expectation* from
``impactreg.h``. The model draws OpenBSD's console fill (``IR_ALIAS``
execute with ``FILLMODE`` 0) and its glyph rows (``FILLMODE``
``0x00400018``, ``CHAR_H`` and ``CHAR_L`` executes), an *approximation*
of the pixels. Any other packet or execute is logged ``LOG_UNIMP``; a
write of another width is a guest error.

.. _sgi-mgras-context:

Context switch
==============

The IRIX 6.5.30 kernel hands the pipe to a graphics process in one
transaction, *software expectation* from the kernel binary: it writes
``0x011000``, polls ``0x008000``, writes ``0x4fc`` to ``0x130500``, sleeps
until general source 19, writes 63 32-bit words of the incoming context
to ``0x120400`` with no header or trailer, and sleeps until source 28.
What the board does with the 63 words is *unknown*.

The model's rule, an *approximation* of that expectation:

- A write to ``0x130500`` other than a 32-bit ``0x4fc`` after a
  ``0x011000`` write is a guest error. A valid one starts a transaction
  and raises source 19.
- Writing bit 19 to ``0x011028`` acknowledges it. 32-bit words to
  ``0x120400`` before that acknowledgement are guest errors; after it,
  the next 63 are the context image.
- The 63rd word completes the transaction, raises source 28 and makes the
  port a command port. No closing word is needed.
- 64-bit writes to ``0x120400`` before any selection are ignored.

The model reads only the image words its OpenGL execution needs, to find
the context's GL state and its window and buffers. A cold reset discards
any handshake in progress, including both acknowledgements. The board's
reset retention is *unknown*.

.. _sgi-mgras-dma:

DMA
===

Mapping port
------------

``0x130400`` takes the same 16:8:8 headers as the command port.
Class ``0x48`` writes its payload words into a register file of 80 words
starting at the header's register, *software expectation* from the
kernel. Five mapping slots ``k`` = 0 to 4 sit there: base high at
``0x20 + 2k``, base low at ``0x21 + 2k``, index width at ``0x2b + k``,
page count at ``0x30 + k`` and control at ``0x4b + k``. ``0x000e0100``
closes a kernel sequence and is ignored. Classes ``0x58`` to ``0x5f`` are
handed to the command parser as raster classes ``0x18`` to ``0x1f`` with
their payload, because the stock IDE diagnostic writes raster registers
this way, *inferred*; what bit 22 of the header selects is *unknown*.
Anything else is logged ``LOG_UNIMP`` and discarded by length. 64-bit
writes are two words, high first.

Page lists
----------

A slot's page list starts at ``(base_hi & 0xff) << 32 | base_lo`` with the
bits below the entry size cleared, so the low byte of the high word is the
SN0 node, and holds ``count`` big-endian page-frame numbers of 4 KiB. The
IP27 kernel writes 64-bit entries starting 24 bytes into a page; the IP30
kernel and X server write 32-bit entries starting 12 bytes in, and the
Octane IMPACTSR driver also keeps a 32-bit table
(:src:`linux-mips-impactsr-2008`), *software expectation*. What the board
keys the entry size on is *unknown*, so the machine states it with the
``page-list-entry-bytes`` property: 4 on Octane, 8 elsewhere, an
*approximation* that does not model a guest writing the other layout.
Entries are masked to 28 bits, so a frame number carries the SN0 node in
bits 27:20; that width is *inferred* from node addressing. The transfer's
widget comes from bits 19:16 of the slot's control word, which carries
HEART's widget 8 on Octane and the Hub's on SN0. A count of 0 or over
2^20, or a list that would run past its node's 4 GiB, logs and transfers
nothing.

Image transfers
---------------

Descriptor class ``0x0008`` holds the transfer: pitch at register 4, a
source offset split between registers 5 and 6 that the model adds, the
row count at 7, bytes per row at 8 and the kick at 11; the destination
box is raster registers ``0x046`` and ``0x047``. A store that reaches
register 11 starts the transfer. The kick byte's bits 2:1 name the
mapping slot and bit 3 the direction: clear moves host memory to the
screen, set reads the screen back to host memory, *software
expectation* of what the X server and client send. Each byte offset ``row * pitch + byte`` plus
the source offset walks the slot's page list, 4 KiB to an entry. A kick
naming a slot with no pages logs and moves nothing.

The model runs a transfer to completion inside the store that kicks it.
A write-direction kick then sets HQ flag 16, which the X server polls
before reusing its buffer; a read kick sets nothing. Bit 4 of the kick
byte is set in every X server kick and clear in the OpenGL client's and
the kernel's; the model sends a transfer with bit 4 clear to the pending
OpenGL pixel draw, a texture load or a context restore when one is
waiting, *inferred* from that split, and to the raster engine otherwise.

Display lists
-------------

Descriptor class ``0x0008`` register ``0x0c`` with two words, a byte
offset and a size, executes a display list from the page list in slot 4,
which the IRIX kernel installs for the client's list pool just before,
*software expectation*. The model reads the list through the page list
and follows its chain of ranges and calls with its own parser,
*software expectation* for the client's list format and *inferred* for
the board's order. One execute is limited to 2^20 words, 2,048 ranges and 64 nested
calls, the model's bounds; a list past any of them, or outside the pool,
draws nothing.

A list runs inside the store for the first millisecond of host time and
then in slices on the board's own thread, paced by the virtual clock.
Words written meanwhile to ``0x020400``, ``0x120400`` and ``0x130400``
wait behind it in order. Status, flag, enable, ``PP1SELECT``, XMAP and
scheduler accesses act at once; flag writes and acknowledges also queue a
copy so their final state follows the processor's order. Every other
access waits until the work has finished. That a board runs a list after
the store that started it is *inferred* from the drivers' waits; the
slicing is the model's. Work still running is finished before the machine
stops, so it never migrates. A register access from inside a graphics
head's own work, such as one board's DMA aimed at another's registers, is
refused with a bus error and logged.

.. _sgi-mgras-ge:

Geometry engines
================

The IRIX 6.5.30 kernel loads GE 0 through data ``0x058040`` and control
``0x058044``, and GE 1 through ``0x058048`` and ``0x05804c``, setting
``0x011000`` bits 5:4 to 2 or 3 around each, then verifies each through
``0x05822c`` and ``0x058230``, *software expectation*. In the model a
control write of ``0x200000`` or ``0x200001`` starts a load into that
slot, bit 0 choosing which half of a read pair comes first; any other
control write ends it, and bit 31 rewinds the read pointer. Data writes
append while loading, up to 178,176 words, past which they are guest
errors. Reads of ``0x05822c`` and ``0x058230`` return the stored words in
pairs, ``0x05822c`` advancing the pair, an *approximation* that satisfies
the kernel's check. The stored program is migrated.

The GE11 program IRIX 6.5.30 ships is a flat image of 712,704 bytes:
59,392 records of 12 bytes. A program record holds one 72-bit
instruction as bits 31:0, then bits 63:32, then three zero bytes and bits
71:64; 54,472 of the records are program words in that layout,
a property of the shipped file, a *software expectation*. The upload length the
model accepts is the record count times three words. The instruction set
is *unknown*.

The model never executes the program. It reads the OpenGL client's
command words at the port and draws what OpenGL 1.2.1 says they produce,
an *approximation* of the geometry engines' function and not of their
program; ``sgi-mgras-gl.c`` lists the subset, and a command outside it is
logged ``LOG_UNIMP`` and consumed by length. Load balancing between two
GEs is not modelled: the HQ4 "performs load balancing based on the
filling of the GE11 command FIFOs"
(:src:`octane-technical-report section 4`) and its rule is *unknown*.
GE port accesses after a GL context has been loaded are logged
``LOG_UNIMP`` once, since the model does not save GL state through them.
The ``x-mesa-renderer`` property opts in to a host renderer for those
draws; ordinary drawing is the model's own.

.. _sgi-mgras-raster:

Raster engine
=============

The model keeps 512 raster registers, written by command-port and normal
FIFO records. Executes it implements, with OpenBSD's names where it has
them:

- ``0x040`` and ``0x041``, the point and line executes: a point at, or a
  line to, the execute's own value, with stipple;
- ``0x047`` (``BLOCKXYENDI``): a rectangle fill from ``0x046``
  (``BLOCKXYSTARTI``), coloured from registers ``0x176`` to ``0x178``
  under ``FILLMODE`` ``0x110``, or a clip-ID or overlay fill by its plane
  pointers and masks;
- ``0x070`` and ``0x071`` (``CHAR_H``, ``CHAR_L``): a glyph row or eight
  byte pixels by ``FILLMODE``;
- ``0x141`` (``BKGRD_BA``): two pixels at a running cursor;
- ``0x045`` (``IR_ALIAS``): nothing, unless the transfer control register
  ``0x102`` selects a read through the data port.

Logic operations, the colour masks and the inclusive clip box in
``0x147``, ``0x148`` and ``0x14f`` apply to every paint. These meanings are
*inferred* from what the IRIX 6.5.30 X server writes, not from
hardware. Any other execute is logged ``LOG_UNIMP`` and
leaves the pixels alone. Block copy has no known payload grammar and is
not implemented. Register ``0x161`` (``PP1FILLMODE``, which carries the
logic operation) resets to ``0x0c000000`` and ``0x163`` to all ones,
each an *approximation*.

The X server reads small boxes through the data port: after an
``IR_ALIAS`` execute with ``0x102`` = ``0xa``, each 8-byte unit is read as
``0x02d1c0`` then ``0x02c1c4``, two 4-byte pixels a unit, rows from the
top, *software expectation* for the server's reads and *inferred* for the
layout.
The model advances on ``0x02d1c0`` and returns the latched half at
``0x02c1c4``; other transfer modes read zero and log.

The model's pixel memory is its own layout, an *approximation*: two banks
of 32-bit words at the screen's size, a 24-bit depth plane, a per-pixel
overlay index, a two-bit clip ID that the X server writes over GL windows,
and a per-pixel format tag recording whether a pixel holds a colour index,
a 24-bit colour or a 12-bit window's colour. The real plane packing,
interleave and clear values are *unknown*.

.. _sgi-mgras-tram:

Texture memory
==============

The textured boards add one TE1 and 4 MB of TRAM per raster engine
(:src:`octane-technical-report Table 4.1, sections 4.2.4-4.2.5`),
*established*. No
texture window exists in the aperture: texture registers travel as
raster register records on the command port, *software expectation*
from the kernel and client. The IRIX kernel manages each TRAM path as 256
blocks of 16 KiB, gives each GL context a run of blocks, and saves and
restores another context's blocks as 64 rows of 64 32-bit words through
the mapping port, *software expectation*.

The model keeps one 4 MiB TRAM image in that block shape, and three more
for the other TRAM selects a one- or two-component texture can name. How
the board lays texels into TRAM is *unknown*, so the layout inside a
block is the model's own; what it guarantees is that its saves and
restores invert each other. On MXI and EMXI one image serves both raster
engines, an *approximation* of each path holding its own copy that only
the IDE's per-path TRAM tests could see. A draw reads only blocks a load
or restore has written since reset; elsewhere it draws untextured and
logs. A board with no TRAM keeps no image and ignores texture state.
Filtering, wrap and texture environments follow OpenGL 1.2.1, an
*approximation*.

.. _sgi-mgras-display:

Display and timing
==================

Display path
------------

Each pixel's window ID comes from the VC3's display-ID tables in its
SRAM: register 4 points at the main planes' frame table and register 5
at the overlay's; a frame table holds one word per line pointing at a run
list of 16-bit entries, starting column in bits 15:5 and window ID in
bits 4:0. The layout follows MAME's Newport VC2 model and matches every
table the IRIX X server builds, *inferred*. The window ID indexes the
XMAP main mode, whose bits 9:5 pick a 256-entry colour map; low field 3
shows a 12-bit index, any other an 8-bit one, *inferred* from the X
server's visuals. A nonzero overlay index covers the main planes when the
window's overlay mode has bit 0 set, through the map in bits 7:3. A
window ID whose bit is set in the XMAP buffer select under index 0 shows
bank 1 instead of bank 0, an *approximation* of what the X server
writes. The hardware rule is *unknown*.

The CMAP selector at ``0x070c30`` names a map in bits 4:0 and a first
entry in bits 15:8; each 32-bit write to ``0x070d18`` stores the next
entry, wrapping in the map's 256, and a read returns the current one.
The model reads an entry's colour from bits 31:8, red high, because every
write the IRIX X server makes has a zero low byte, an *approximation*.
The hardware's use of the low byte is *unknown*. The 8,192 entries are what the server addresses; the physical capacity is
*unknown*.

The DAC is addressed by the halfword at ``0x071810`` with the register
number in its high byte, and its data moves a byte at a time through
``0x071910``; a byte write to ``0x071811`` sets the number, *software
expectation* from the stock IDE diagnostic and the X server. The model
answers ID ``0x79`` at register ``0x0300`` and revision 1 at ``0x0b00``,
the pair the diagnostic accepts without warning, and stores every other
register; the part is unidentified. Each write to ``0x071890`` loads the
high byte into the gamma table, red, green, blue, then the next row, and
an address write restarts the load. Scanout ANDs each component with the
pixel mask, register ``0x0400``, before the gamma table: the X server
blanks the screen by writing 0 there and unblanks with ``0xff``, and the
mask is *inferred* to work as a common RAMDAC's. Reset loads an identity
gamma ramp and a mask of ``0xff``, each an *approximation*.

VC3 registers 2 and 3 position the 32x32 cursor at its top-left pixel
plus 31, *inferred* from the X server's writes; register ``0x1d`` bit 1
enables it and register ``0x1e`` bit 2 enables the display. Register 7
loads and reads back the SRAM address.

Retrace period
--------------

When the guest enables the display, the model derives the retrace period
from VC3 register 0's frame table and the DAC PLL: one frame is the sum of
each line sequence's durations (bits 14:8 of each first halfword) times
its repeat count, in VC3 clocks at half the pixel clock, and the pixel
clock is ``33 MHz * (4 * (v + 2) + cntl[7:6]) / ((r + 2) * 2^cntl[5:4] *
2^cntl[2])`` from DAC registers ``0x0c``, ``0x0f`` and ``0x09``. For the
stock 1280x1024 60 Hz timing this gives 894,600 clocks at 53.625 MHz,
about 59.943 Hz. The walk, the clock ratio and the PLL relation are
*inferred* from every timing table IRIX ships, MAME's VC2 model and the
rates SGI's timing compiler records. ``0x072408`` bit 24 selects another
PLL reference that no shipped table uses; the model's 64 MHz for it is a
placeholder, *unknown*.

Deriving the period only at the enable write, restarting the phase only
when the period changes, and running the stock period before any table is
loaded or for a table that gives no frame or one under 1 ms are
each an *approximation*. The last logs ``LOG_GUEST_ERROR`` once per reset. A
real SI's retrace rate under the stock timing would settle the period.

.. _sgi-mgras-dual-re:

Boards with two raster engines
==============================

SSI, ESSI, MXI and EMXI carry two GEs, two RE4s and four PP1s
(:src:`octane-technical-report Table 4.1`). The report says one RE4
processes even lines and the other odd lines
(:src:`octane-technical-report section 4.2.3`), *established*, and the
stock IDE diagnostic's pixel-to-memory mapping puts row ``y`` on RE
``y % 2`` and column ``x`` on PP1 ``x & 1``, *software expectation*.

``PP1SELECT`` at ``0x071c08`` routes the XMAP registers: bit 0 set
reaches every PP1, otherwise bits 2:1 name one, RE 0's two first. The
IRIX kernel writes ``((re << 1) | pp) << 1`` before a per-PP1 sequence
and 1 before writes every PP1 must take, *software expectation*. The
model keeps one XMAP register file per PP1. A read with every PP1
selected answers from PP1 0, a named PP1 the board lacks drops writes
with ``LOG_GUEST_ERROR`` and reads 0, bits 7:3 are ignored, and
``0x071f00`` and ``0x071f80`` are shared fixed values: each
an *approximation*. Scanout reads PP1 0's tables, which software keeps
alike on every PP1.

The kernel selects one RE for direct reads through raster register
``0x112`` bits 1:0, *software expectation*. The model reports that
selection in ``0x02c578`` and answers ``0x02c574`` with 8 for either RE.
IRIX draws and reads pixels through one stream and never addresses one
RE, so the model keeps one set of planes for both and does not split
rows, an *approximation* visible only to the IDE's memory tests. The
second GE takes its program through the second slot and is never
executed, like the first. A two-RE board adds two colour slots that the
OpenGL client's deep visuals draw into.

.. _sgi-mgras-reset:

Reset
=====

A cold reset clears both pixel banks, depth, overlay, clip IDs and format
tags, the HQ and GE program stores, the raster registers, every GL
context, TRAM, the VC3 registers and SRAM, the CMAP, the XMAP files, the
DAC registers, all HQ registers, interrupt state and flags, the context
handshake and any display-list work. It then sets the XMAP main modes to
``0x7a4``, the DAC pixel mask to ``0xff``, an identity gamma ramp,
``0x028004`` to the board's GE revision and ``0x011098`` to done, and the
first retrace falls one stock period later. What a real board keeps
across its cold, warm, link and HQ resets is *unknown*; these values are
each an *approximation*.

.. _sgi-mgras-migration:

Migration
=========

The device state is version 36 and loads only between identical builds.
It carries the pixel planes, the HQ and GE programs, the context
handshake, every GL context with its parser position, the raster and
descriptor registers, the mapping port, the XMAP files of all four PP1s,
VC3, CMAP and DAC state, and the retrace phase as the timer's expiry.
Optional subsections carry the two-RE colour slots, alpha, the
serial-number master, a raster record half-forwarded from the mapping
port, the clip IDs, texture state and TRAM, the latter only once a block
has been written. The retrace period is derived again from the migrated
tables, and travels in its own subsection only when the guest changed
them after its last display enable. Display-list work is finished before
the machine stops, so none is in flight.

.. _sgi-mgras-measurements:

Measurements
============

No IMPACT board has been read for this model. The values above come from
the published captures listed under :ref:`sgi-mgras-sources` and from
IRIX's own decoding.

.. _sgi-mgras-limits:

Model limits
============

- The GE program is stored and read back, never run. The model draws a
  subset of OpenGL from the client's words; anything else is logged
  ``LOG_UNIMP`` and consumed by length.
- Block copy, 1600x1200 and other modes, video options, stereo scanout and
  direct-colour XMAP visuals are not implemented.
- FIFO occupancy, bus status and DMA status always read idle, and HQ
  status reads busy for at most 500 ms of work. The low-water source is
  raised as soon as it is enabled.
- Flags, interrupts and DMA complete inside the store that causes them;
  only display lists run later.
- The E-series boards render exactly as the first-generation ones.
- Both raster engines share one set of planes, and MXI and EMXI share one
  TRAM image. TRAM layout is the model's own.
- Without ``board-record=on`` the serial-number master reads done with no
  chip, unlike a real board.
- An access at a width the model does not accept, or at an offset it does
  not decode, logs ``LOG_GUEST_ERROR``, reads zero, latches pending bit 1
  and sends a widget-error packet if bit 1 is enabled. Unknown raster
  executes, command classes and normal-FIFO packets log ``LOG_UNIMP``.
- Migration works only between identical builds.

.. _sgi-mgras-tests:

Tests
=====

The register-contract qtests run on the default four-node ``origin2000``
with the board in IO3:

- ``tests/qtest/sgi-mgras-board-test.c``: identity, version bytes and the
  ``hinv`` decode for all eight boards (``board-identity/*``), the
  serial-number record, ``PP1SELECT`` routing and two-RE cases, XMAP, VC3
  and DAC register files, the pixel mask, cursor position, status bits,
  bring-up storage and reset of the context handshake.
- ``sgi-mgras-fifo-test.c``: the context selection and image, command
  framing, the normal FIFO's OpenBSD sequences, fills, lines, glyphs,
  logic operations, clipping, CMAP and scanout, flags and FIFO thresholds.
- ``sgi-mgras-irq-test.c``: pending, enable set and clear, the general,
  retrace and widget-error packets, the swap source, the retrace period
  and phase, and graphics credit return.
- ``sgi-mgras-pixels-test.c``: mapping slots, page lists, image transfers
  both ways, the data port, display lists including their slicing, status
  bound and stop, and context eviction.
- ``sgi-mgras-texture-test.c``: texture loads, formats, TRAM save and
  restore and the MXI and EMXI shared-store cases.
- ``sgi-mgras-gl-test.c`` and ``sgi-mgras-gl-draw-test.c``: the OpenGL
  subset, queries and flag 17.
- ``sgi-mgras-migration-test.c``, ``sgi-mgras-migration-gl-test.c`` and
  ``sgi-mgras-migration-glstate-test.c``: migration of the retrace, FIFO
  and GE state, GL contexts and pipeline state.
- ``tests/qtest/octane-mgras-test.c``: the board in each Octane port on
  ``octane`` and ``octane2``, link reset, local DMA with 32-bit page lists
  and interrupt transport.
- ``tests/qtest/sgi-replay-test.c`` replays recorded MMIO sessions from
  ``tests/data/mgras/`` and checks the resulting pixels.

The functional tests ``tests/functional/mips64/test_mgras_desktop.py``
and ``test_mgras_matrix_<product>.py``, one per board, boot IRIX 6.5.30 on
an Origin 2000 to the greeter and desktop and check ``hinv`` and drawing
against colour-statistics references. They need local IRIX assets.
