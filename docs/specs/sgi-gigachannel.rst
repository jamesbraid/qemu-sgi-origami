.. SPDX-License-Identifier: BSD-3-Clause

.. _sgi-gigachannel:

=====================================================
SGI Origin 200 GIGAchannel expansion cabinet (XBOX)
=====================================================

The GIGAchannel is a separate I/O cabinet for the Origin 200. It holds one
Crossbow, one Bridge with four PCI slots and five XIO slots. Two XIO cables
join it to one Origin 200 CPU module. The upgrade removes the module's
passive XTalk jumper and fits a Crosstown adapter in its place, so after
conversion the module's own Hub reaches its own IP29 Bridge only through the
cabinet's Crossbow. The node therefore cannot boot without either the cabinet
or a loopback cable (007-3708-002 printed pages 19-24 and 63).

This page is the hardware contract the machine models cite for the cabinet:
its identity, composition, cabling, link assignment and what firmware and
IRIX report. What the emulator builds, and where it approximates, is in
:ref:`sgi-sn-gigachannel`. SGI's own names are GIGAchannel (the product),
XBOX (the chassis model and board-record prefix) and Crosstown (the cable
adapters). The cabinet is distinct from the PCI shoehorn, a one-slot XIO PCI
adapter, and from the Origin 2000 PCI carrier.

Facts are rated established, inferred, approximation or unknown, as on
:ref:`sgi-onyx2`. A fact read from the stock IP27 PROM payload names the
image address and cites ``firmware:ip27-prom-6.156``.

.. _sgi-gigachannel-sources:

Sources
=======

- *Origin200 and Origin200 GIGAchannel Owner's Guide*, 007-3708-002.
- *Origin200 and Origin200 GIGAchannel Maintenance Guide*, 007-3709-002.
- *Origin200 GIGAchannel Upgrade Installation Instructions*, 108-0187-001.
- Real ``hinv -vm`` inventories on the Higher Intellect wiki's Origin 200
  page (``web:preterhuman-origin-200``); a 2003 comp.sys.sgi.marketplace
  owner post (``web:usenet-pigot-3edc9a71``); the 2012 forum thread
  *Building an Onyx 200* (``web:nekonomicon-16726704``); and a two-module
  inventory with a cabinet on each module (``web:nekonomicon-16731033``).
- The stock IP27 PROM 6.156 payload (``firmware:ip27-prom-6.156``), measured from the image itself.

Public Linux, OpenBSD and NetBSD trees hold no GIGAchannel support: a search
for the ``XBOX`` name over them returns only unrelated drivers.

.. _sgi-gigachannel-identity:

Product identity
================

.. list-table::
   :header-rows: 1

   * - Item
     - Value
     - Rating
   * - Product name
     - Origin 200 GIGAchannel expansion cabinet
     - established (007-3708-002 Table 1-1, printed page 5)
   * - Cabinet interface board
     - 030-1252-001, silkscreen "XBOX XTALK INTERFACE"; board record
       ``XBOX_DUALXTOWN``, rev F in every capture
     - established (``web:preterhuman-origin-200`` inventories;
       ``web:usenet-pigot-3edc9a71``)
   * - CPU-side Crosstown board
     - 030-1264-001, "PCA XBOX EXTENSION BOARD O200"; board record
       ``XBOX_EXT`` rev C (``2G_MPLN`` rev B in the upgrade guide's example)
     - established (108-0187-001 Table 1-1, page 1-2;
       ``web:preterhuman-origin-200``; ``web:usenet-pigot-3edc9a71``)
   * - Cabinet midplane
     - 030-1380-002, "PCA XBOX MIDPLANE O200"
     - inferred as a part identity: the number comes from seller listings
       that are not registered sources; no capture shows its board record
   * - Cabinet Bridge revision
     - Bridge revision 4, as printed (``ASIC BRIDGE Rev 4, (widget 8)``)
     - established (108-0187-001 page 3-8; ``web:nekonomicon-16726704``).
       The letter that revision corresponds to is not printed anywhere
       and is not claimed
   * - Upgrade kits
     - ``HU-CONNECT-XBOX`` (module hardware), ``561-CHAS-XBOX`` (cables)
     - established (108-0187-001 Table 1-1)

The Crosstown board records and the cabinet record are human-readable
fields in ``hinv -vm``, not raw one-wire bytes. The owner post adds ``Group
ff Capability ffffffff Variety ff`` for each record. Established in three
specimens.

.. _sgi-gigachannel-composition:

Physical composition
====================

.. list-table::
   :header-rows: 1

   * - Assembly
     - What it carries
     - Rating
   * - Crosstown board in the CPU module
     - replaces the XTalk jumper; connectors XIO A and XIO B
     - established (007-3708-002 printed pages 19-24)
   * - Interface board 030-1252-001 in the cabinet
     - two external XIO connectors, a card-edge connector to the midplane,
       link and data status LEDs for each of XIO A and XIO B
     - established (007-3709-002 printed pages 60-65)
   * - Midplane 030-1380-002
     - the XIO slots, the interface board and the PCI backplane plug into it
     - established (007-3709-002 printed pages 107-123)
   * - Crossbow
     - in the cabinet
     - established that it is there: the owner's block diagram places it in
       the cabinet (007-3708-002 Figure 1-5, printed page 7) and a builder
       reports the base I/O feed passing "through the Gigachannel's XBOW and
       back" (``web:nekonomicon-16726704``). That it sits on the midplane is
       inferred
   * - Cabinet Bridge
     - drawn between the midplane and the PCI backplane
     - established as present (007-3708-002 Figure 1-5; 108-0187-001 page
       3-8); that its board is the midplane is inferred, since no photograph
       shows it elsewhere
   * - PCI backplane
     - four PCI slots, three full-length and one half-length
     - established (007-3708-002 printed pages 19-24; 007-3709-002 printed
       page 23)
   * - XIO slots
     - five, physical numbers 11 to 15, customer-installable
     - established (007-3708-002 Table 1-1; 007-3709-002 printed pages 33-41)

The cabinet carries no register block that any source describes: a model is
a composition of existing devices with product-specific wiring and
identity records.

.. _sgi-gigachannel-topology:

Cabling and link assignment
===========================

Two XIO cables join the module's XIO A to the cabinet's XIO A and its XIO B
to the cabinet's XIO B; both are required. Loopback mode joins the module's
own XIO A to its XIO B with one cable and runs without a cabinet. The
cabinet may attach to the master or the slave module; one cabinet cannot be
shared by two modules and cabinets cannot be daisy-chained. Established
(007-3708-002 printed pages 19-24, 63 and 109-110). Each cable carries one
complete Crosstalk link, one joining the module's Hub to the cabinet and
the other its IP29 Bridge: inferred from the Crosstown board replacing a
jumper that joined Hub to Bridge. Which cable carries which link is
unknown.

.. list-table::
   :header-rows: 1

   * - Crossbow link
     - Endpoint
     - Software location
     - Rating
   * - 8
     - cabinet Bridge, four PCI slots
     - ``io7``, board ``xbox_dualxtown``
     - established: ``PCI_XIO ... Slot io7`` and ``ASIC BRIDGE Rev 4,
       (widget 8)`` (108-0187-001 page 3-8; ``web:nekonomicon-16726704``)
   * - 9
     - the module's IP29 Bridge (base I/O)
     - ``io1``, board ``MotherBoard``
     - established by one capture: ``BASEIO ... Slot io1``, ``ASIC BRIDGE
       Rev 4, (widget 9)`` (``web:nekonomicon-16726704``). The upgrade
       guide's example listing conflicts: it shows ``BASEIO ... Slot
       MotherBoard`` with ``ASIC BRIDGE Rev 3, (widget 8)`` beside the
       cabinet's own widget 8 (108-0187-001 page 3-8). Two Bridges cannot
       share widget 8 on one Crossbow, so the guide's example reads as a
       composed listing, and the model follows the capture; the
       base I/O Bridge revision (3 or 4) is unknown
   * - A
     - the module's Hub
     - the node's own slot (``n1``)
     - inferred: no capture prints the Hub's port; the PROM names link A the
       node's own slot in the same way as its Origin 2000 table
       (``firmware:ip27-prom-6.156`` widget-to-slot routine at
       ``0x1fc419cc``)
   * - B
     - optional XIO board
     - ``io2``
     - established: a Fibre Channel XIO board at ``io2`` reports widget 11
       (``web:nekonomicon-16726704``)
   * - C, D, E, F
     - optional XIO boards
     - ``io3``, ``io4``, ``io5``, ``io6``
     - established from the stock PROM image: ``firmware:ip27-prom-6.156`` Origin 200
       table at ``0x1fcd1e30`` (widgets 8 to F are ``io7``, ``io1``, node
       slot 1, ``io2``, ``io3``, ``io4``, ``io5``, ``io6``), selected when
       the machine-type word at physical ``0x10000090`` is nonzero; real
       boards sit at these names (``web:preterhuman-origin-200``)

Physical XIO slots 11 to 15 correspond, as a set, to links B to F and
positions ``io2`` to ``io6``; the one-to-one mapping is unknown. The four
cabinet PCI slots are labelled 1 to 4 in the owner's guide figure
(007-3708-002 Figure 1-5), and IRIX numbers PCI slots from 0
(007-3708-002 printed page 135). That they are Bridge
devices 0 to 3 is inferred from hardware-graph paths of the form
``io7/xbox_dualxtown/pci/0`` (``web:nekonomicon-16731033``).

In a two-module system with one cabinet on module 1, module 1's base I/O
moves to ``io1`` behind the Crossbow and module 2's stays directly on its
Hub. Established for the module-1 case (``web:preterhuman-origin-200``;
``web:nekonomicon-16726704``). A two-module system with a cabinet on each
module reports the same layout on each. Established (``web:nekonomicon-16731033``).
That capture prints no widget numbers, so it does not settle the link rows. How
IRIX treats a single cabinet on the slave module alone is unknown.

.. _sgi-gigachannel-firmware-view:

What firmware and IRIX report
=============================

Each item is real-hardware output unless it names another source.

- **Board records.** ``hinv -vm`` lists the cabinet under
  ``/hw/module/1/slot/io7/xbox_dualxtown`` as ``XBOX_DUALXTOWN`` part
  030-1252-001 rev F, and lists ``XBOX_EXT`` part 030-1264-001 rev C beside
  the ``IP29`` record under ``/hw/module/1/slot/io1/MotherBoard``. Established
  in three specimens (``web:preterhuman-origin-200``;
  ``web:usenet-pigot-3edc9a71``). The records ride one-wire lines whose
  exact wiring is unknown; see :ref:`sgi-gigachannel-record-wiring`.
- **Base I/O location.** With a cabinet the base I/O is
  ``/hw/module/1/slot/io1``; a plain Origin 200 reports ``slot/MotherBoard``.
  Established by one capture (``web:nekonomicon-16726704``); the upgrade
  guide's example listing disagrees (see the link table above).
- **Bridge inventory.** The PROM's ``hinv -v`` prints ``PCI_XIO Origin 200 IO
  Board, Module 1, Slot io7`` with ``ASIC BRIDGE Rev 4, (widget 8)``. The
  Crossbow itself does not appear as an inventory line in any capture.
  Established (108-0187-001 page 3-8; ``web:nekonomicon-16726704``).
- **I/O PROM load message.** On GIGAchannel systems the PROM prints
  ``Loading BASEIO prom from XBOX Flashprom....``; Origin 2000 systems print
  ``Loading BASEIO prom ....``. Established
  (``web:nekonomicon-16726704``).
- **Part-number recognition.** The PROM classifies a board whose identity
  part number starts ``030-1250-`` or ``030-1252-`` as the inventory type
  that IRIX prints as ``PCI_XIO``. Established from the stock PROM image
  (``firmware:ip27-prom-6.156`` classifier at ``0x1fc41538``, which compares
  nine-character prefixes). The part prefix ``030-1264-`` is in a separate
  string list in the same routine; its use is unknown.
- **Cabinet detection.** The PROM's cabinet detection (``firmware:ip27-prom-6.156``
  routine at ``0x1fc484c0``, called from ``0x1fc527c0``) takes a Crossbow
  whose widget-0 part number reads ``0x0000`` as the cabinet. Established from the stock PROM image.
- **Loopback inventory.** In loopback the PROM inventory matches the
  pre-upgrade one except for the added ``030-1264-001`` record: base I/O at
  ``Slot MotherBoard`` and ``ASIC BRIDGE Rev 3, (widget 8)``. Established
  (108-0187-001 pages 2-9 to 2-11).

.. _sgi-gigachannel-record-wiring:

How the cabinet record reaches firmware
=======================================

How the PROM reaches the ``XBOX_DUALXTOWN`` record, and which one-wire line
carries it, is unknown: no source states the wiring, and the part
(030-1252-001) is described only by photographs of a board with two heatsinked
ASICs whose markings are not legible. The model therefore presents the record
without claiming a wiring: it rides the cabinet Bridge's own one-wire line,
the line the firmware names ``io7`` from, and the ``XBOX_EXT`` record rides
the IP29 Bridge's own line, because inventories list it beside the IP29
record. These are emulator choices. They are approximations, with
placeholder barcodes and serials, and a capture of the raw one-wire bytes
from a real cabinet would settle them.

.. _sgi-gigachannel-unknowns:

Unknowns
========

.. list-table::
   :header-rows: 1

   * - Unknown
     - What would settle it
   * - Which XIO cable carries the Hub link and which the Bridge link
     - A cabinet-side wiring drawing, or a capture that prints the cabinet
       Crossbow's ports
   * - Physical XIO slots 11 to 15 against links B to F
     - An inventory from a cabinet that prints widget numbers for each slot
   * - The cabinet Crossbow's revision
     - A cabinet PROM ``hinv -v`` or ``xbstat`` capture
   * - The cabinet's flash part and system-controller status word
     - Cabinet-side documents or a status-word capture; see
       :ref:`sgi-sn-gigachannel`
   * - A single cabinet on the slave module alone
     - An inventory of such a system
   * - Raw one-wire line of each board record
     - A one-wire bus capture from a real cabinet
