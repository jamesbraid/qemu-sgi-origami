.. SPDX-License-Identifier: BSD-3-Clause

.. _sgi-mscsi:

================================
SGI MSCSI four-channel XIO board
================================

The MSCSI is SGI's XIO SCSI board for the Origin family: one Bridge with
four QLogic ISP1040 SCSI controllers on its PCI bus. It fits an XIO slot of
an Origin 2000 or Onyx2 module and of an Origin 200 GIGAchannel cabinet.
QEMU builds it as the ``mscsi`` preset of ``sgi-xio-baseio``
(``hw/mips/sgi/baseio.c`` and ``hw/mips/sgi/xio-baseio.c``), placed in an
empty XIO slot. The board has no registers of its own. The Bridge's
register contract is on :doc:`bridge` and the controllers' on
:doc:`qlogic-isp`. Facts are rated as described in :ref:`sgi-ratings`.

.. _sgi-mscsi-sources:

Sources
=======

- *Origin2000/Onyx2 Illustrated Parts Catalog*, "XIO Options":
  :src:`hmm-408-b p. 33`.
- A one-module Origin 200 GIGAchannel ``hinv -vm``,
  :src:`preterhuman-origin-200`.
- An Origin 2000 PROM ``hinv -v``, :src:`nekonomicon-16727716`.
- An Origin 2000 ``hinv -m``, :src:`usenet-9uhh6o`, and an Octane
  ``hinv -m``, :src:`usenet-8ns2m5`.
- A two-module Origin 200 ``hinv -vm``, :src:`nekonomicon-16731033`.
- The Origin 2000 XIO configuration rules, :src:`108-0175-001`.
- The stock IP27 PROM, :src:`ip27-prom-6.156`.
- Linux ``arch/mips/include/asm/sn/klconfig.h`` at ``db2ddb871435``, as
  the public name for the board type.

.. _sgi-mscsi-composition:

Composition
===========

.. list-table::
   :header-rows: 1
   :widths: 24 38 38

   * - Fact
     - Value
     - Rating and source
   * - Part numbers
     - ``030-0872-004`` PCA MSCSI and ``030-1243-001`` PCA MSCSI B.
       ``030-1281-001`` also prints as MSCSI, at revision D
       (:src:`usenet-8ns2m5`). ``030-1243-001`` prints at revisions F
       (:src:`preterhuman-origin-200`), G (:src:`usenet-9uhh6o`) and M
       (:src:`nekonomicon-16731033`). How the three parts differ is
       unknown.
     - *established* for the first two parts (:src:`hmm-408-b p. 33`)
       and for the third (:src:`preterhuman-origin-200`). *Unknown*
       for the difference; a catalogue entry for each would settle it.
   * - Name in the hardware graph
     - ``mscsi``. IRIX prints ``Location: /hw/module/1/slot/io4/mscsi``
       and ``MSCSI Board: ... part 030-1243-001 rev F``.
     - *established*, :src:`preterhuman-origin-200`.
   * - Bridge revision
     - Revision 3 on the rev D, rev F and rev G boards, printed by IRIX
       as the board revision (``Origin MSCSI board, module 1 slot N:
       Revision 3``) and by the PROM as ``ASIC BRIDGE Rev 3, (widget
       14)``. Revision 4 on the rev M boards.
     - *established*, :src:`preterhuman-origin-200`,
       :src:`nekonomicon-16727716` and :src:`nekonomicon-16731033`.
   * - SCSI controllers
     - Four QLogic ISP1040 at PCI devices 0 to 3, PCI revision 5,
       vendor ``0x1077``, device ``0x1020``. The PROM prints four
       ``adapter PCI-SCSI Rev 5`` at ``pci id`` 0 to 3 under the
       board's Bridge. IRIX names each ``QL1040B (rev. 2)``.
     - *established*, :src:`nekonomicon-16727716` and
       :src:`preterhuman-origin-200`.
   * - Other PCI functions, external flash
     - None.
     - *inferred*: the PROM lists only the four adapters and no
       inventory mentions a flash part.
   * - Interrupts
     - Each controller's INTA on the Bridge pin matching its device
       number.
     - *inferred*: the Bridge's ordinary per-slot rule, as on the
       BaseIO and IP29 ISP1040s. No source describes different wiring.
   * - Bus type per channel
     - Single-ended or differential per controller. At least two
       differential channels sit on the MSCSIs.
     - *inferred* from the Origin 200 inventory, where IRIX prints
       nine of its fifteen QL1040B controllers as differential and at
       most seven of the fifteen are off the MSCSIs
       (:src:`preterhuman-origin-200`). *Unknown* for which controller
       is which and how IRIX learns it. A capture of one MSCSI alone
       would settle it.
   * - Board type in the PROM
     - A record whose part starts ``030-0872-``, ``030-1243-`` or
       ``030-1281-`` is board type ``0x22``. Linux names it
       ``KLTYPE_MSCSI``, an alias of ``KLTYPE_4CHSCSI``.
     - *established*: the type name from Linux ``klconfig.h``; the
       prefix table from the stock PROM, :src:`ip27-prom-6.156`.

.. _sgi-mscsi-slots:

Slot rules
==========

On an Origin 2000 the MSCSI fits an XIO slot other than IO2, which the
configuration rules reserve for the PCI module :src:`108-0175-001`,
*established*. On an Origin 200 GIGAchannel any of ``io2`` to ``io6``
takes it. Real systems carry it at ``io2``, ``io4``, ``io5`` and ``io6``
(:src:`preterhuman-origin-200`, :src:`nekonomicon-16731033`),
*established*.

The board has no fixed widget. Its slot fixes it. The captures show
widget 14 on an Origin 2000, *established*
(:src:`nekonomicon-16727716`). The model fits the board at any XIO widget
and does not enforce the IO2 rule, an *approximation*.

.. _sgi-mscsi-model:

Model
=====

``sgi-xio-baseio`` with ``preset=mscsi`` builds the board's own Bridge
and four ``qlogic-isp1040`` functions at PCI devices 0 to 3. Each
controller has PCI revision 5 and the Bridge's per-slot interrupt rule.
The machine's ``baseio-preset`` property refuses ``mscsi``, so the board
cannot replace a module's BaseIO in IO1.

The board-identity record is part ``030-1243-001``, revision ``F`` and
name ``MSCSI`` on the Bridge's one-wire line, with Bridge revision 3. This
is the board the one-module GIGAchannel inventory lists at ``io4``, an
*approximation*: it is one of the observed parts, chosen because its
inventory is complete. The barcode is a marked synthetic placeholder and
the per-unit serial comes from the machine's board catalogue. The record
layout is on :doc:`bridge`.

The model does not present the revision 4 Bridge of the rev M boards. The
rev M board's own part number is the same ``030-1243-001``, so the choice
changes only the revision digit that IRIX and the PROM print.

Model limits
============

- The ISP1040 PCI subsystem ids are the ISP1040 model's own. A PCI
  configuration dump from a real board would settle them, *unknown*.
- Every controller presents whatever bus type the ISP1040 model reports.
  The single-ended or differential split per channel is not modelled.
- The internal SCSI cabling, external termination and the difference
  between MSCSI and MSCSI B are not modelled.
- The board has no IOC3, Timekeeper, serial or parallel port and no flash
  part. The preset refuses a console, record, MediaIO, clock or NVRAM
  backend for any of them instead of dropping it.
- The Origin 2000's reservation of IO2 is not enforced.

Tests
=====

``tests/qtest/sn0-mscsi-test.c`` covers, for a board added with
``-device`` and for boards placed at ``io2`` and ``io4`` of a
GIGAchannel cabinet:

- ``bridge-revision``: the board's Bridge answers at its widget as a
  Bridge, revision 3;
- ``isps-enumerate``: four ``0x1077:0x1020`` functions at devices 0 to 3,
  PCI revision 5;
- ``population``: children ``scsi0`` to ``scsi3`` and no ``scsi4`` or
  ``ioc3``;
- ``not-a-baseio-preset`` and ``refuses-backends-it-cannot-take``: the
  refusals described above;
- ``placed/widgets-answer``: three boards at ``io2`` to ``io4`` answer at
  their own widgets, the cabinet's startup population stays unchanged
  and ``io5`` and ``io6`` stay empty.

``tests/qtest/sn0-bridge-identity-test.c`` reads the board record back
byte for byte for the Bridge identity cases.
