.. SPDX-License-Identifier: BSD-3-Clause

.. _sgi-onyx2:

==========================================
SGI Onyx2 deskside and rack (IP27 board)
==========================================

An Onyx2 is Origin 2000 silicon with graphics. The node boards, Hub,
Crossbow, Router and Bridge are the Origin 2000 parts, built by the
``onyx2`` machine (``hw/mips/sgi/sn-machine.c``) on the same stock IP27
PROM as the other Scalable Node products (:ref:`sgi-sn-product-family`). What differs is
physical: a graphics BaseIO (IO6G) in XIO slot 1, and one or more
InfiniteReality ("KONA") graphics pipes. The deskside and the rack attach
a pipe in two different ways, and the firmware names their slots from two
different tables.

This page is the contract the machine models in ``hw/mips/sgi/models/``
cite for the Onyx2: the chassis and slot map, the widget numbers, the pipe's
identity and board records, and what is still unknown. The pipe's register
behaviour is on :ref:`sgi-graphics`; the BaseIO board and the Crossbow have
their own pages.

Every fact carries one of four ratings:

established
  stated by a cited manual or specification, shown by an identified real
  machine's inventory, or read from the stock firmware image the entry
  names;
inferred
  follows from established facts by a stated step;
approximation
  an emulator value standing in for hardware that is not known;
unknown
  no available source settles it; the entry says what would.

.. _sgi-onyx2-sources:

Sources
=======

SGI manuals, by document number and printed page:

- *Onyx2 Deskside Workstation Owner's Guide*, 007-3454-005.
- *Onyx2 Rackmount Owner's Guide*, 007-3457-005.
- *Origin2000 Deskside Owner's Guide*, 007-3453-002.
- *Origin and Onyx2 Theory of Operations Manual*, 007-3439-002.
- *Origin and Onyx2 Hardware Quick-reference Booklet*, HMQ-380-C
  (``web:origin-onyx2-quick-reference-booklet``).

Captures, each an owner's posted inventory:

- an Onyx2 deskside PROM ``hinv -v`` posted in 2010
  (``web:nekonomicon-16723250``);
- an Onyx2 deskside IRIX ``hinv``, board list and ``gfxinfo -v``
  (``web:irixnet-post-32192``);
- an Onyx2 deskside inventory and board list on SGIstuff
  (``web:sgistuff-collection-2200``);
- an Onyx2 rack IRIX ``hinv -v -m``, board list and ``gfxinfo -v``
  (``web:nekonomicon-16285``).

Public software: the widget identity tables in Linux
(``arch/mips/include/asm/xtalk/xwidget.h``) and OpenBSD's sgi port
(``sys/arch/sgi/xbow/xbowdevs``).

The stock IP27 PROM 6.156 payload (``firmware:ip27-prom-6.156``) is read
by disassembly. A fact read from it names the image address. The IRIX
6.5.30 kernel carries copies of the same tables, byte for byte, which the
author also read; that kernel is not a registered source and the facts
below rest on the PROM alone.

.. _sgi-onyx2-role:

Role
====

The deskside (regulatory model CMN A015) has its own midplane with two node
slots, a Null Router, five XIO slots and four graphics slots. The pipe's
geometry board sits on the midplane and takes XIO slot 4 directly. There is
no Ktown board. The rack (CMN A017) is an ordinary eight-processor compute
module plus a separate graphics module holding up to two pipes. Each pipe
reaches the compute module over a Crosstown cable from an Xtown board in a
compute-module XIO slot to a Ktown board in the graphics module. Established
(007-3454-005 printed pages 1-2 and 8-9; 007-3457-005 printed pages 1, 14,
31 and 48).

.. _sgi-onyx2-identity:

Product identity
================

.. list-table::
   :header-rows: 1

   * - Item
     - Deskside
     - Rack
   * - Midplane board record
     - ``4P1G5_MPLN``, part 013-1839-001 rev F
     - ``8P12_MPLN``, parts 030-1535-001 rev B, 030-0762-006 rev K and
       013-1547-003 rev D in one three-module system
   * - Node boards
     - one or two, up to four processors
     - up to four per compute module; two to eight processors in a rack, up
       to 64 across racks
   * - Node interconnect
     - Null Router, no external ports
     - Star and Rack Routers
   * - Graphics
     - one pipe, one or two RM assemblies
     - one or two pipes per graphics module, up to 16 across racks
   * - Base I/O
     - BaseIO-G (IO6G) in XIO slot 1
     - BaseIO-G (IO6G) in XIO slot 1 of the compute module
   * - Graphics BaseIO records
     - ``BASEIO`` 030-0734-002 with an ``MIO`` 030-0880-003 daughterboard
     - the same two records

Established. The regulatory model, node counts, interconnect, pipe counts
and base I/O come from 007-3454-005 printed pages 1-2, 7 and 24 and
007-3457-005 printed pages 1, 14 and 46; the midplane and BaseIO records come from the three
inventories in :ref:`sgi-onyx2-sources` (``web:irixnet-post-32192``,
``web:sgistuff-collection-2200`` and ``web:nekonomicon-16285`` board
lists). The regulatory number names the enclosure, not the Onyx2: the
Origin 2000 deskside shares CMN A015 (007-3453-002 printed page 1). The
graphics boards are specific to Onyx2 midplanes and may not move to another
system (007-3454-005 printed page 7).

.. _sgi-onyx2-deskside-slots:

Deskside slots
==============

From the rear the deskside midplane carries node slots 2 and 1, four
graphics slots (GE, RM/TM, RM/TM, DG5) and five XIO slots numbered 1, 2, 3, 5
and 6. XIO slot 4 is the graphics board set. Established (007-3454-005
printed pages 8-9 and Figure 2-2, printed page 16; the Null Router is on
printed page 19; 007-3439-002 printed page 57).

XIO slot rules, established (007-3454-005 printed page 24): the BaseIO-G
stays in XIO slot 1; the optional three-slot PCI module goes in slot 2 and a
SCSI board never does; slots 3 and 5 fill first.

The guide also says there is no functional XIO connection below XIO slot 3,
and its Figure 1-4 draws the position under XIO 3 as a solid blank. The
reading is that this one physical position has no connector because the
graphics set uses XIO 4, not that ``io2`` or ``io6`` are dead: a real deskside
runs boards in both. Inferred (007-3454-005 printed pages 8-9;
``web:irixnet-post-32192``).

.. _sgi-onyx2-deskside-widget-map:

Deskside widget-to-slot map
===========================

A deskside has one Crossbow. Firmware names its widgets with a table that
exists only for this chassis:

.. list-table::
   :header-rows: 1

   * - Widget
     - Slot
     - Holds
   * - 8
     - ``io4``
     - the graphics pipe (GE board)
   * - 9
     - ``n2``
     - node board 2
   * - A
     - ``n1``
     - node board 1
   * - B
     - ``io2``
     - XIO slot 2 (PCI module)
   * - C
     - ``io6``
     - XIO slot 6
   * - D
     - ``io5``
     - XIO slot 5
   * - E
     - ``io3``
     - XIO slot 3
   * - F
     - ``io1``
     - BaseIO-G

Established by disassembly (``firmware:ip27-prom-6.156`` table at
``0x1fcd1dd0``; the companion node-slot-to-widget table at ``0x1fcd1ea8`` and
the I/O-slot-to-widget table at ``0x1fcd1f88`` give the same rows from the
other direction). A real deskside's PROM ``hinv -v`` prints ``BASEIO IO
Board, Module 1, Slot io1`` at widget 15, an ``XTALK_PCI`` board in ``io5`` at
widget 13 and ``KONA Graphics Board, Module 1, Slot io4``, matching the
table (``web:nekonomicon-16723250``).

That node board 2 is physically cabled to widget 9 and board 1 to widget A
is inferred from those tables: no capture prints each Hub's widget. The pipe
therefore attaches at widget 8, which differs from every other Scalable
Node map: on an eight-processor module's Crossbow 0, widget 8 is ``io1``.

.. _sgi-onyx2-node-slot-codes:

Deskside node slot codes
------------------------

The deskside's two node slots have their own slot codes, ``0x51`` for ``n1``
and ``0x52`` for ``n2``. The PROM's decoder at ``0x1fc461fc`` turns the low
three bits of the Hub slot-identity register into them: raw 3 gives ``n1`` and
raw 2 gives ``n2``, where an eight-processor module uses raw 7 to 4 for
``n1`` to ``n4``. Only the low three bits decode; raw 0 and 1 decode as
invalid. Established by disassembly (``firmware:ip27-prom-6.156`` address
``0x1fc461fc``). A real deskside's IRIX ``hinv`` prints node memory as
``Module 1/Slot 81`` and ``Slot 82``, the two codes in decimal
(``web:irixnet-post-32192``). Established.

The deskside decode needs bit 0 of the 32-bit word at physical
``0x100000a8`` clear. That word is the configuration-flags field of the boot
flash's firmware configuration block, beside the machine-type field at
``0x90``. With the bit set, raw 3 and 2 decode as ordinary node slots 5 and 6
instead. A deskside's flash must therefore carry the bit clear, and the real
deskside's ``Slot 81/82`` shows it clear. Established by disassembly
(``firmware:ip27-prom-6.156`` address ``0x1fc461fc`` and its caller) and the
capture. The stock image carries the bit clear, so the model needs no
setting. What the bit means otherwise is unknown, and what the deskside
midplane drives on the upper identity bits is unknown. A deskside midplane
drawing, or a capture that prints the identity register, would settle the
second.

.. _sgi-onyx2-two-node:

The two-node deskside
---------------------

No inventory lists the Null Router: neither the PROM ``hinv -v`` nor IRIX's
lists one, where the rack lists ``r1`` and ``r2``. Established
(``web:nekonomicon-16723250``; ``web:irixnet-post-32192``;
``web:nekonomicon-16285``).

The PROM's partner-node probe (``firmware:ip27-prom-6.156`` routine at
``0x1fc470a0``, called at ``0x1fc4ff88``) tries an empty route, then a
one-hop route out of port 4 or 5 chosen by slot parity, for a deskside slot
code. Otherwise it routes through Router port 6. Established by
disassembly. That this models a direct Hub-to-Hub link through the Null
Router is inferred.

In both deskside captures ``n2`` holds NASID 0 and ``n1`` NASID 1
(``web:nekonomicon-16723250``; ``web:irixnet-post-32192``). Established.
What makes ``n2`` NASID 0 is unknown; the PROM's NASID assignment for class 5
slots would settle it. One IRIX ``hinv`` also prints ``IO6prom on Global
Master Baseio in Module 1/Slot io2`` while the BaseIO's hardware-graph path
is ``io1/baseio``; why it says ``io2`` is unknown
(``web:irixnet-post-32192``).

.. _sgi-onyx2-rack-widget-map:

Rack compute module
===================

The compute module is the eight-processor module: four node slots, two
Router slots, twelve XIO slots and two Crossbows. Node 1 activates XIO slots
1 to 6 and node 2 slots 7 to 12. Nodes 1 and 3 reach slots 1 to 6 through
Crossbow 0, nodes 2 and 4 reach slots 7 to 12 through Crossbow 1. The BaseIO-G
stays in slot 1 and the PCI option in slot 2. Established (007-3457-005
printed pages 33-36; HMQ-380-C Tables 41 and 42, printed pages 130-131).

.. list-table::
   :header-rows: 1

   * - Widget
     - Crossbow 0
     - Crossbow 1
   * - 8
     - ``io1`` (BaseIO-G)
     - ``io11``
   * - 9
     - ``n1``
     - ``n4``
   * - A
     - ``n3``
     - ``n2``
   * - B
     - ``io4``
     - ``io8``
   * - C
     - ``io2``
     - ``io10``
   * - D
     - ``io6``
     - ``io12``
   * - E
     - ``io5``
     - ``io7``
   * - F
     - ``io3``
     - ``io9``

Established by disassembly (``firmware:ip27-prom-6.156`` tables at
``0x1fcd1df0`` for Crossbow 0 and ``0x1fcd1e10`` for Crossbow 1). The
booklet's ``xbstat`` table (HMQ-380-C printed page 250) agrees on rows B to
F. It labels widget 8 "BaseIO" only, with no Crossbow 1 entry, and prints
the node rows as "Node 1 or 2" at widget 9 and "Node 3 or 4" at A, where the
PROM puts ``n4`` at 9 and ``n2`` at A on Crossbow 1. The booklet's pairing
reads as loose and the PROM's as the operative naming; that reading is
inferred. Which node board is physically wired to Crossbow 1 widgets 9 and A
is unknown; a midplane drawing would settle it.

.. _sgi-onyx2-graphics-module:

Graphics module
===============

The graphics module holds up to two pipes. Each pipe's boards plug into the
rear of the graphics-module midplane, and the Ktown board sits between the
two board sets. The right-hand pipe, seen from the rear, takes one, two or
four RM/TM assemblies and uses the Ktown's upper connector; the left-hand
pipe takes one or two and uses the lower. Established (007-3457-005 printed
pages 14, 20-22 and 31).

The booklet names a "KCAR midplane" in the graphics troubleshooting chain,
between the Ktown board and the GE14 (HMQ-380-C printed page 251). That this
is the graphics-module midplane is inferred from its place in that chain. No
capture shows a graphics-module midplane board record, so that record is
unknown.

.. _sgi-onyx2-pipe-link:

How a pipe reaches the compute module
=====================================

Each pipe needs its own Crosstown cable from a Crosstown XIO board ("Xtown")
in a compute-module XIO slot to the Ktown board. Established (007-3457-005
printed page 48). The Crosstown board converts the XIO link's on-board
signalling to differential levels for a cable of up to 3 metres, established
(007-3439-002 printed page 73). The Theory of Operations manual calls the
XIO-slot board "the Crosstown (KTOWN) conversion board"; the rack guide and
the captures use KTOWN for the graphics-module board, so this page calls the
XIO-slot board Xtown and keeps KTOWN for the graphics-module end.

The host's path ends at the GE board's XIO-to-graphics ASIC (XG): the
booklet's "XBOW-to-GE14 path" lists the midplane, the XIO board, the
Crosstown cable, the Ktown board, the KCAR midplane and the "GE14 (XGASIC)"
(HMQ-380-C printed page 251; 007-3454-005 printed page 28). Established.
The Xtown and Ktown are therefore link extenders and the XIO widget at the end
of the link is the XG, as on the deskside. That the Xtown and Ktown present
no widget identity of their own is inferred: nothing in the captures or the
firmware tables reads one. Whether they have any register a host can read
is unknown; a rack PROM ``hinv -v`` that prints widget numbers would settle
it.

.. _sgi-onyx2-xtown-placement:

Xtown placement and pipe limits
===============================

A node controls at most one pipe. Established (HMQ-380-C Table 42, printed
page 131). The booklet's recommended Xtown slots (a node count, then pipes,
then slots):

.. list-table::
   :header-rows: 1

   * - Nodes
     - Pipes
     - Xtown slots
     - Crossbow and widget
   * - 1
     - 1
     - any of ``io2`` to ``io6``
     - Crossbow 0, widgets C, F, B, E, D
   * - 2 to 4
     - 1
     - ``io7``
     - Crossbow 1 widget E
   * - 2 to 4
     - 2
     - ``io2``, ``io7``
     - Crossbow 0 widget C, Crossbow 1 widget E
   * - 3 or 4
     - 3
     - ``io2``, ``io7``, ``io8``
     - adds Crossbow 1 widget B
   * - 4
     - 4
     - ``io2``, ``io3``, ``io7``, ``io8``
     - adds Crossbow 0 widget F

The slot columns are established; the widget column follows from the rack
widget map above. The real rack capture has its one pipe at ``io3``,
Crossbow 0 widget F, in a four-node module (``web:nekonomicon-16285``), so
the table is a recommendation and the pipe takes whatever XIO slot its Xtown
occupies. The rack guide draws pipe 0 on the first Crossbow and pipe 1 on the
second (007-3457-005 Figure 1-2, printed page 5). Multirack systems support
up to 16 pipes, and each pipe needs one node board (007-3457-005 printed
pages 1 and 51). Established.

.. _sgi-onyx2-populations:

Node-board populations
======================

Both chassis ship with fewer node boards than they hold, filled from ``n1``.

Deskside: the standard deskside has one node board, the entry level one
processor, and a second board is an option. The first node board always sits
in the right-hand slot, ``n1``. Established (007-3454-005 printed pages 1-2
and 21). With one node board, ``n1`` is the only Hub on the one Crossbow
(widget A), so it controls every widget; that is inferred from its being the
only Hub. Widget 9, where ``n2`` would be, is an empty port. The rack guide
says the Null Router is used in desksides with one or two node boards
(007-3457-005 printed pages 45-47), so a real one-node deskside probably
carries one with its far end empty. That the two are firmware-equivalent is
inferred, and so is the model's one-node row, which fits no link at all.

Rack: a single rack holds two to eight processors, and the booklet lists a
one-node, one-pipe system with node 1 controlling the IO6G and the Xtown in
``io2`` to ``io6`` (HMQ-380-C Table 42). Established. The rack guide names the
Null Router for one or two node boards and the Star Router for a
single-rack Onyx2 with three or four (007-3457-005 printed pages 45-47). A
real rack capture's module 1 carries Standard Routers, with two node boards
and later with four (``web:nekonomicon-16285``): established. With one node
board the model fits no Router; the guide says none is required and no
capture shows one, so that row is inferred. The pipe's Xtown in ``io3`` is
Crossbow 0 widget F, which ``n1`` controls at every count, and one pipe needs
only one node board. The Star Router fit is documented but not modelled.

.. _sgi-onyx2-widget-selection:

How firmware chooses the widget map
===================================

There is no machine-type word for an Onyx2. The PROM routine at
``0x1fc419cc`` (its error text names it as the widget-to-slot routine) picks
a table from the hardware, in this order. Established by disassembly
(``firmware:ip27-prom-6.156`` address ``0x1fc419cc``):

1. Widgets outside 8 to 15 give an invalid slot.
2. The routine reads the local node's slot code. A code of class 5, an
   Onyx2 deskside node slot, selects the deskside table. Only Crossbow 0 is
   legal on a deskside; any other Crossbow prints an error and gives an
   invalid slot.
3. Otherwise a nonzero machine-type word at physical ``0x10000090``
   selects the Origin 200 GIGAchannel table (:ref:`sgi-gigachannel`).
4. Otherwise the Crossbow number picks the Crossbow 0 or Crossbow 1 table of
   the eight-processor module. An Onyx2 rack takes this path and is named
   exactly as an Origin 2000.

No firmware word says "Onyx2": recognition is by slot code. An Onyx2
deskside is the only chassis whose node slots decode to class 5. An Onyx2
rack compute module is an Origin 2000 module; what makes it an Onyx2 is the
hardware in it: a BaseIO-G with MIO, one or more Xtown boards and KONA
widgets behind them. The Origin 2000 deskside is not the Onyx2 deskside: it is
the eight-processor module in a deskside enclosure (007-3453-002 printed
pages 1 and 19-24) and uses the Crossbow 0 and 1 tables. A model therefore
builds an Onyx2 from components: the deskside from its midplane's slot values
and a KONA widget at widget 8, the rack from an Origin 2000 module with
BaseIO-G and KONA widgets on Xtown slots.

.. _sgi-onyx2-pipe-identity:

Pipe identity
=============

The pipe presents XIO manufacturer ``0x2aa`` and part ``0xc102``, named KONA
in Linux and OpenBSD. Established (Linux
``arch/mips/include/asm/xtalk/xwidget.h`` ``WIDGET_KONA_MFGR_NUM`` and
``WIDGET_KONA_PART_NUM``; OpenBSD ``sys/arch/sgi/xbow/xbowdevs`` lines 22 and
42; :ref:`sgi-graphics-widget-identities`). The PROM's graphics classifier
(``firmware:ip27-prom-6.156`` address ``0x1fc4291c``) gives part ``0xc102``
board type ``0x51`` and the name ``kona``, gives other graphics parts their
own types and every unknown graphics part a generic name. Established by
disassembly.

The deskside pipe presents the same identity: its PROM prints ``KONA
Graphics Board, Module 1, Slot io4`` and the classifier names a board
``kona`` only for part ``0xc102`` (``web:nekonomicon-16723250``). The same
deskside's IRIX ``gfxinfo -v`` reports ``Graphics board 0 is "KONAL"
graphics`` with its GE, RM, TM and DG records
(``web:irixnet-post-32192``). Established by capture. That both chassis end
the host path at the same XG ASIC, which accounts for the shared identity,
is inferred.

The widget revision of a real pipe is not captured. The model answers
revision 0 from the device itself; that is an emulator choice, not a decoded
hardware revision.

IRIX accepts a widget as a pipe only if firmware wrote a matching graphics
board record into the node's configuration data: the kernel builds its pipe
list from those records, keyed by module and ``io`` slot, and rejects a
widget with no matching entry. There is no deskside or rack test in that
path; on a deskside the entries come from the deskside table, so the pipe
must be widget 8 for firmware to name its board ``io4`` and for IRIX to find
it. This is a reading of the stock IRIX 6.5.30 kernel, which is not a
registered source here, so it is stated for context and no model entry
rests on it.

.. _sgi-onyx2-one-wire:

One-wire board records
======================

IRIX reads the pipe's board records through the pipe itself.

.. list-table::
   :header-rows: 1

   * - Record
     - Deskside
     - Rack
   * - GE
     - ``GE14-4`` 030-1129-002, or ``GE14-2``
     - ``GE16-4`` 030-1398-001
   * - KT (Ktown)
     - absent; ``gfxinfo -v`` prints ``KT: No NIC serial number available.``
     - ``KTOWN`` 030-1067-001 rev F, under the pipe's slot
   * - RM, TM
     - RM7 030-1054-001, TM7 030-1053-001 per assembly
     - RM9 030-1402-001
   * - DG
     - DG5-2 030-1087-001
     - DG5-2 030-1242-001
   * - BP
     - ``No NIC serial number available.``
     - ``No NIC serial number available.``
   * - Xtown
     - not applicable
     - no record in the board list

Established from the captures (``web:irixnet-post-32192``,
``web:sgistuff-collection-2200``, ``web:nekonomicon-16285``). IRIX lists the
rack's Ktown record at ``/hw/module/1/slot/io3/kona``, the Xtown's XIO slot,
beside the GE record, and lists only the GE record at the deskside's
``io4/kona``. The QEMU device presents its own geometry record whatever part
the model layer names (``hw/misc/sgi-kona.c``); serials are placeholders, so
the part numbers above are the captured identity and the serials are an
approximation.

.. _sgi-onyx2-unknowns:

Unknowns
========

.. list-table::
   :header-rows: 1

   * - Unknown
     - What would settle it
   * - Which upper Hub slot-identity bits the deskside midplane drives
     - A deskside ``hinv`` that prints the register, or a midplane pinout
   * - Why ``n2`` takes NASID 0 on a two-node deskside
     - Disassembly of the PROM's NASID assignment for class 5 slots
   * - Which node is wired to Crossbow 1 widgets 9 and A
     - A midplane drawing or wiring table
   * - Whether the Xtown or Ktown has a host-readable register
     - A rack PROM ``hinv -v`` printing widget numbers, or an ``xbstat``
       capture on a pipe port
   * - The graphics-module midplane's board record
     - A rack IRIX board list that includes the graphics module
   * - Whether user-visible pipe numbers follow the kernel's pipe-list order
     - A two-pipe rack ``gfxinfo -v``

Neither of the project's physical SGI systems is an Onyx2, so a physical
test cannot answer these.
