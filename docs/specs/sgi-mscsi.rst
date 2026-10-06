.. SPDX-License-Identifier: BSD-3-Clause

.. _sgi-mscsi:

================================
SGI MSCSI four-channel XIO board
================================

The MSCSI is SGI's XIO SCSI board for the Origin family: one Bridge
with four QLogic ISP1040 SCSI controllers on its PCI bus. QEMU builds it
as the ``mscsi`` preset of ``sgi-baseio`` (``hw/mips/sgi/baseio.c``),
placed in an empty XIO slot. Ratings follow :ref:`sgi-ip30`. The
controllers' register contract is on :doc:`sgi-qlogic-isp` and the Bridge's
on :doc:`sgi-bridge`.

Sources: *Origin2000/Onyx2 Illustrated Parts Catalog* (``web:hmm-408-b``,
printed page 33, "XIO Options"); the one-module Origin 200 GIGAchannel
``hinv -vm`` on the SGI preservation wiki (``web:preterhuman-origin-200``);
an Origin 2000 PROM ``hinv -v`` (``web:nekonomicon-16727716``); an owner's
Origin 2000 ``hinv -m`` (``web:usenet-9uhh6o``); an Octane ``hinv -m`` (``web:usenet-8ns2m5``); Linux
``arch/mips/include/asm/sn/klconfig.h`` at
``linux:db2ddb87143519e20a95aa36c60b36107b736a58``; and the stock PROM
``firmware:ip27-prom-6.156``.

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
     - ``030-0872-004`` PCA MSCSI and ``030-1243-001`` PCA MSCSI B;
       ``030-1281-001`` also prints as MSCSI, at revision D
       (``web:usenet-8ns2m5``). ``030-1243-001`` prints at revision F
       (``web:preterhuman-origin-200``) and revision G
       (``web:usenet-9uhh6o``). How the three parts differ is unknown.
     - established: parts catalogue page 33 for the first two,
       ``web:preterhuman-origin-200`` for the third. A catalogue entry
       for each would settle the difference.
   * - Name in the hardware graph
     - ``mscsi``; ``Location: /hw/module/1/slot/io4/mscsi``,
       ``MSCSI Board: ... part 030-1243-001 rev F``.
     - established, ``web:preterhuman-origin-200`` one-module ``hinv -vm``.
   * - Bridge
     - Revision 3, printed by IRIX as the board revision (``Origin MSCSI
       board, module 1 slot N: Revision 3``) and by the PROM as
       ``ASIC BRIDGE Rev 3, (widget 14)``.
     - established, ``web:preterhuman-origin-200`` and
       ``web:nekonomicon-16727716``.
   * - SCSI controllers
     - Four QLogic ISP1040 at PCI devices 0 to 3, PCI revision 5, vendor
       ``0x1077`` device ``0x1020``; the PROM prints four ``adapter
       PCI-SCSI Rev 5`` at ``pci id`` 0 to 3 under the board's Bridge, and
       IRIX names each ``QL1040B (rev. 2)``.
     - established, ``web:nekonomicon-16727716`` and
       ``web:preterhuman-origin-200``.
   * - Other PCI functions, external flash
     - None.
     - inferred: the PROM lists only the four adapters; no inventory
       mentions a flash part.
   * - Interrupts
     - Each controller's INTA on the Bridge pin matching its device
       number.
     - inferred: the Bridge's ordinary per-slot rule, as on the BaseIO
       and IP29 ISP1040s; no source describes different wiring.
   * - Bus type per channel
     - Single-ended or differential per controller; at least two
       differential channels sit on the MSCSIs.
     - inferred from the wiki inventory, where IRIX prints nine of its
       fifteen QL1040B controllers as differential and at most seven of
       the fifteen are off the MSCSIs. Which controller is which, and how
       IRIX learns it, is unknown; a capture of one MSCSI alone would
       settle it.
   * - Board type in the PROM
     - A record whose part starts ``030-0872-``, ``030-1243-`` or
       ``030-1281-`` is typed MSCSI (board type ``0x22``), which Linux
       names ``KLTYPE_MSCSI`` (an alias of ``KLTYPE_4CHSCSI``).
     - established: Linux ``klconfig.h`` line 358; the prefix table is
       in the stock image, ``firmware:ip27-prom-6.156``.

.. _sgi-mscsi-slots:

Slot rules
==========

On an Origin 2000 the MSCSI fits an XIO slot other than IO2, which the
configuration rules reserve for the PCI module (established,
108-0175-001). On an Origin 200 GIGAchannel any of ``io2`` to ``io6`` takes
it; real systems carry it at ``io2``, ``io4``, ``io5`` and ``io6``
(established, ``web:preterhuman-origin-200``). The board has no fixed
widget: its placement fixes it, and the captures show widget 14 on an
Origin 2000. The model fits it at any XIO widget and does not enforce
the IO2 rule, a stated limitation.

.. _sgi-mscsi-model:

Model choices
=============

- The board-identity record the model presents is part ``030-1243-001``,
  revision ``F`` and name ``MSCSI`` on the Bridge's own one-wire line, with
  Bridge revision 3: the board the one-module GIGAchannel inventory lists
  at ``io4``. The barcode is a placeholder; the per-unit serial comes from
  the catalogue. The record layout is on :doc:`sgi-bridge`. (Approximation:
  one of the three observed parts, chosen because it is the one with a
  complete capture.)
- No IOC3, Timekeeper, serial or parallel port and no flash part: the
  preset refuses a backend for any of them.
- The ISP1040 PCI subsystem ids are the ISP1040 model's own. A PCI
  configuration dump from a real board would settle them (unknown).
- The internal SCSI cabling, external termination and the MSCSI against
  MSCSI B distinction are not modelled.
