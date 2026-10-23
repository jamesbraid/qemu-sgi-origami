.. SPDX-License-Identifier: BSD-3-Clause

.. _sgi-graphics:

==============================================================
SGI graphics boards: IMPACT (MGRAS), VPro (Odyssey), KONA
==============================================================

Three SGI graphics families attach to the machines this tree models as XIO
widgets, each answering the crosstalk widget identity register directly,
with no Bridge or PCI bus between the board and the XIO link:

- the IMPACT SI board, built around the HQ4 host interface and called MGRAS
  here (``hw/misc/sgi-mgras*.c``, ``-device sgi-mgras``);
- the VPro or Odyssey board, sold as V6 to V12 (``hw/misc/sgi-odyssey.c``,
  ``-device sgi-odyssey``);
- the InfiniteReality pipe, KONA (``hw/misc/sgi-kona*.c``,
  ``-device sgi-kona``), whose chassis wiring is on :ref:`sgi-onyx2`.

This page is the contract the machine models in ``hw/mips/sgi/models/``
cite for those boards. It separates what a public source establishes (the
widget identities, the board families and a handful of register locations
that public drivers use) from what the emulator chose. Almost all of each
board's register and rendering behaviour has no public specification. Where
the models and devices implement it, it is an approximation, labelled so
here and in the device source, and it is exercised by the tests named below.
Nothing in this page describes behaviour a real board is known to have beyond
the public sources cited.

Every fact carries one of four ratings:

established
  stated by a cited specification or manual, or read from public driver
  source that talks to the real board;
inferred
  follows from established facts by a stated step;
approximation
  an emulator choice standing in for hardware that is not known, with its
  limitation stated;
unknown
  no available public source settles it; the entry says what would.

.. _sgi-graphics-sources:

Sources
=======

- OpenBSD with the sgi port kept alive (``openbsd-sgi`` at
  ``2df91d80c104636deeef8124252ddd5951b55fbf``):
  ``sys/arch/sgi/xbow/xbowdevs`` (widget identity table),
  ``sys/arch/sgi/dev/impactreg.h``, ``sys/arch/sgi/dev/impact.c`` and
  ``sys/arch/sgi/xbow/impact_xbow.c`` (the ImpactSR driver),
  ``sys/arch/sgi/xbow/odysseyreg.h`` and ``sys/arch/sgi/xbow/odyssey.c``
  (the Odyssey driver), and the ``impact(4)`` and ``odyssey(4)`` manual
  pages under ``share/man/man4/man4.sgi``.
- Linux at ``2f9060b1db4aa2c21c248e34476d8936a2b69cf6``:
  ``arch/mips/include/asm/xtalk/xwidget.h``.
- *Origin 300 User's Guide*, 007-4365-002, printed page 8 (the rear XIO
  connector), and *Onyx 3000 Series Graphics System Hardware Owner's Guide*,
  007-4264-002, printed pages 10, 52 and 54 (the V-brick).
- The Higher Intellect wiki page on IMPACT graphics
  (``web:preterhuman-impact``).
- Owner captures of a Fuel with a V10 board (``web:tingo-fuel-irix-console``,
  ``web:miod-openbsd-sgi-story``).

.. _sgi-graphics-widget-identities:

Widget identities
=================

Each board answers the XIO widget identification with a manufacturer and a
part number. Established by public driver tables:

.. list-table::
   :header-rows: 1

   * - Board
     - Manufacturer
     - Part
     - OpenBSD ``xbowdevs`` lines
     - Linux ``xwidget.h``
   * - IMPACT SI (HQ4), "ImpactSR"
     - ``0x2aa``
     - ``0xc003``
     - 22 and 40
     - ``WIDGET_IMPCT_MFGR_NUM``, ``WIDGET_IMPCT_PART_NUM``
   * - VPro / Odyssey
     - ``0x023``
     - ``0xc013``
     - 19 and 41
     - ``WIDGET_ODYS_MFGR_NUM``, ``WIDGET_ODYS_PART_NUM``
   * - InfiniteReality / KONA
     - ``0x2aa``
     - ``0xc102``
     - 22 and 42
     - ``WIDGET_KONA_MFGR_NUM``, ``WIDGET_KONA_PART_NUM``

OpenBSD's ``impact_xbow.c`` matches an ImpactSR board on exactly the pair
``0x2aa``/``0xc003`` (lines 61 and 62), and its ``impact(4)`` page says the
driver covers the Indigo2, Octane, Octane 2 and Onyx systems. The widget
revision is part of the same register; OpenBSD prints it at attach
(``impact_xbow.c`` line 82) but no public source ties a revision to a
board model or option. The manufacturer and part values are therefore
established, and the revision of any particular board is unknown.

QEMU's devices answer these identities themselves, from the device, and the
model layers' ``identity`` records only restate them: the catalogue does not
drive the register.

.. _sgi-graphics-mgras:

IMPACT SI board (MGRAS, HQ4)
============================

.. _sgi-graphics-mgras-population:

Board population
----------------

The first modelled population is one textureless SI board: one HQ4 host
interface, one geometry engine (GE11), one raster engine (RE4), two pixel
processors (PP1), 12 MiB of pixel memory, no texture memory and one display
path. OpenBSD's ImpactSR driver names the HQ4 (``impact.c`` lines 34, 78 and
291-292) and its register map has a PP1 select (``impactreg.h`` line 52). The
IMPACT family's configurations, from one geometry and one raster engine up,
are described on the wiki page (``web:preterhuman-impact``), which also
gives the Octane names SI, SI with texture memory (TRAM) and MXI. The exact
chip counts and the 12 MiB figure are an approximation: the smallest board the
stock IRIX software brings up, chosen as the smallest the stock IRIX software
is known to bring up; no public source states the counts. The
board's own part number and revision are not captured for this population, so
the catalogue claims none.

.. _sgi-graphics-mgras-identity:

Identity
--------

The HQ4's XIO identity is manufacturer ``0x2aa``, part ``0xc003``
(:ref:`sgi-graphics-widget-identities`): established. Revision 0 in the
catalogue and in the device is a placeholder, not a decoded revision: unknown.

.. _sgi-graphics-mgras-windows:

Register windows
----------------

OpenBSD's ``impactreg.h`` (lines 23-28, 36-41, 49 and 63) gives the window
offsets its driver uses on the ImpactSR board. Established as a software
expectation, in the sense that a driver that boots real boards reads and
writes these offsets:

.. list-table::
   :header-rows: 1

   * - Function
     - Offset
   * - HQ status
     - ``0x020000``
   * - FIFO status
     - ``0x020008``
   * - Host-bus status
     - ``0x020100``
   * - DMA busy
     - ``0x020200``
   * - Command FIFO
     - ``0x020400``
   * - Command and data FIFO thresholds and delay
     - ``0x040000`` to ``0x040030``
   * - XMAP register block
     - ``0x071c00``
   * - VC3 register block
     - ``0x072000``

The status bits, the command grammar, FIFO fence and completion ordering, the
DMA descriptor and source-mapping format, interrupt allocation, reset order and
retention, the context-switch transaction and the uploaded-program protocol
have no public specification. The device implements the subset the stock IRIX
software needs and the board-level behaviour it observed from the stock
firmware images and the stock graphics software: that is an approximation and
is exercised by ``tests/qtest/sgi-mgras-board-test.c``,
``sgi-mgras-fifo-test.c``, ``sgi-mgras-pixels-test.c``,
``sgi-mgras-irq-test.c``, ``sgi-mgras-gl-test.c`` and
``sgi-mgras-texture-test.c``. Accesses outside the table above stay
observable and are not given invented behaviour. What would settle them: a
published SGI register specification, or a capture from a real SI board.

.. _sgi-graphics-mgras-raster:

Raster, memory and display
--------------------------

The raster path (fills, logic operations, glyph expansion, window-ID and
overlay storage, colour-map resolution, the cursor and the retrace timing)
is an emulator approximation. It is a functional model that renders from
latched guest state, not an electrical or timing model: bit packing, plane
layout, the byte lane of colour-map entries and the exact effect of window
IDs are inferred from what the stock IRIX X server and its diagnostics do, and
are checked only against that software. Block copy has no known payload
grammar and stays inert. The limits and the discriminating tests are named in
``hw/misc/sgi-mgras-raster.c`` and ``hw/misc/sgi-mgras-display.c`` and in
the qtests above. No public document gives the plane or colour-map layout;
a published SI register manual would settle it.

.. _sgi-graphics-odyssey:

VPro board (Odyssey)
====================

.. _sgi-graphics-odyssey-identity:

Identity
--------

The widget identity is manufacturer ``0x023``, part ``0xc013``
(:ref:`sgi-graphics-widget-identities`): established. It names the Odyssey
family, not a model: it does not select V6, V8, V10 or V12, the memory size,
the output option, an ASIC stepping, firmware or the platform software. The
board model and configuration therefore stay separate values: a stock-software
acceptance requirement is that unknown configuration is an attachment failure,
not a silent generic V12.

The device (``hw/misc/sgi-odyssey.c``) takes widget revision 10, 128 MB and
four banks by default, the values a measured V12 answers, and accepts a memory
size and bank count. Its ``profile`` property selects one marketed board. The
IRIX 6.5.30 kernel turns revisions 1 to 3 into BUZZ A and 9 to 15 into BUZZ B,
``gfxinfo`` prints the minor number as the offset less one, and ``hinv`` names
the board from the generation and the size (established from those shipped
binaries):

======= ======== ====== ===== ===== ================ ===================
Profile Revision Memory Banks PB&J  ``gfxinfo`` BUZZ Identity record
======= ======== ====== ===== ===== ================ ===================
v6      2        32 MB  2     1     A.1              ``ODY32``
v8      2        128 MB 4     1     A.1              ``ODY128``
v10     10       32 MB  2     1     B.1              ``ODY32VERSIONB``
v12     10       128 MB 4     1     B.1              ``ODY128VERSIONB``
======= ======== ====== ===== ===== ================ ===================

Published ``gfxinfo`` captures establish A.1 and PB&J 1 on a 32 MB V6, B.1
and PB&J 1 on a 32 MB V10 and on a 128 MB V12 with four banks, and published
``hinv -mv`` captures name the four Octane boards. The V8's revision and banks
are inferred from its generation and size, and the 32 MB boards' two banks
are inferred, since ``gfxinfo`` prints a fixed two-bank line for any 32 MB
board. A second Fuel V10 board reported B.2 (revision 11), so revision is not
fixed per marketed board. Fuel and Tezro boards are other assemblies
(``ASTODY``, ``ASTODYB``), and the profiles' records name the Octane boards.
Display output is not verified.

.. _sgi-graphics-odyssey-attachment:

Attachment
----------

An Odyssey board is a widget on a crossbar port. The V-brick, the graphics
enclosure of the Origin 3000 family, adds a midplane, an L1 controller and
its own routing (007-4264-002 printed pages 10, 52 and 54). The Origin 300's
rear XIO connector joins it to a graphics pipeline (007-4365-002 printed page
8). A direct V12 on the Origin 300's XBridge, with no V-brick, is an emulator
approximation: no published inventory shows that connection, and no V-brick
board revision, widget route, NIC, memory or display option was measured. The Fuel's board is
the widget the system board's XBridge crossbar reaches at port 13: established
by the owner captures (``web:tingo-fuel-irix-console``;
``web:miod-openbsd-sgi-story``).

.. _sgi-graphics-odyssey-pio:

Public PIO baseline
-------------------

OpenBSD's ``odysseyreg.h`` (lines 23-31) maps ``0x00410000`` bytes at
widget-relative offset zero and uses four locations: a command-FIFO status
register at ``0x00001064`` whose bits ``0x00008000`` and ``0x00020000`` are
threshold states, a data-engine status at ``0x0000106c`` (the console waits
for the low seven bits to read zero), an ordered command-FIFO port at
``0x00110000`` and an ordered data-FIFO port at ``0x00400000``. Established as
a software expectation. Every other offset, width, alias and reset value is
unknown. The device implements further registers its IP35 driver was observed
to need (the one-wire access register, the memory-configuration register, the
interrupt registers and a command interpreter); those are approximations,
documented at the top of ``hw/misc/sgi-odyssey.c``, and unmodelled offsets
read zero and log themselves once. A mapped aperture is not evidence of a
linear framebuffer, and the device's own framebuffer layout is an approximation.

.. _sgi-graphics-kona:

InfiniteReality pipe (KONA)
===========================

The pipe's chassis wiring, widget numbers and board records are on
:ref:`sgi-onyx2`; its identity is in
:ref:`sgi-graphics-widget-identities`. The host path ends at the XIO-to-
graphics ASIC (XG) on the geometry board (007-3454-005 printed page 28),
established. The XG's register map, mailbox, interrupt vectors, reset
sequences, DMA packet format, the private IRIX control interface and the board
inventory structure the guest derives from the pipe have no public
specification: unknown. The ``sgi-kona`` device (``hw/misc/sgi-kona.c``)
answers the widget header and classification fields from the device and
describes everything past them as synthetic or approximated: identity serials
and several board-record fields are placeholders, and the command and register
model has no real-device capture behind it. Reads of unimplemented offsets
return zero and writes are accepted. The device is exercised by
``tests/qtest/sgi-kona-read-defense-test.c``. A published InfiniteReality
host-interface manual, or captures from a real pipe, would settle the
unknown parts.
