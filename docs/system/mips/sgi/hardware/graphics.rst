.. SPDX-License-Identifier: BSD-3-Clause

.. _sgi-graphics:

==========================================================
Graphics boards: IMPACT, VPro, InfiniteReality and PsiTech
==========================================================

Used in :ref:`sgi-ip27` (Origin 200, Origin 2000, Onyx2), :ref:`sgi-ip30`
(Octane, Octane2), :ref:`sgi-ip35` (Fuel) and :ref:`sgi-ip53` (Onyx 350,
Tezro). Four kinds of graphics board are modelled. Three are SGI XIO
widgets that answer the crosstalk widget identity register directly, with
no Bridge or PCI bus between the board and the XIO link. The fourth is a
pair of third-party PCI cards. Each board has its own page with the
register map, behaviour, model limits and tests. This page says which
board goes in which machine and keeps the widget identity table the
machine pages cite. Facts are rated as described in :ref:`sgi-ratings`.

.. list-table::
   :header-rows: 1

   * - Board
     - QEMU device
     - Machines
     - Page
   * - IMPACT (SI, SSI, MXI and the E-series boards), called MGRAS
     - ``sgi-mgras``
     - ``octane``, ``octane2``, ``origin2000-deskside``,
       ``origin2000-rack``, ``origin200-gigachannel``
     - :ref:`sgi-mgras`
   * - VPro (V6, V8, V10, V12), called Odyssey
     - ``sgi-odyssey``
     - ``octane2``, ``fuel``, ``onyx350-compat``,
       ``tezro-tower-multi``; ``octane`` only on request
     - :ref:`sgi-odyssey`
   * - InfiniteReality pipe, called KONA
     - ``sgi-kona``
     - ``onyx2`` and the Origin 2000 and Onyx2 topologies whose
       catalogue offers a pipe
     - :ref:`sgi-kona`
   * - PsiTech RAD1, a PCI card
     - ``psitech-rad1``
     - Origin 200 topologies
     - :ref:`sgi-psitech-rad1`
   * - PsiTech RAD4 and RAD4-KM, PCI cards
     - ``psitech-rad4``
     - Origin 200 topologies
     - :ref:`sgi-psitech-rad4`

The slots each machine offers are on :ref:`sgi-ip30-graphics` and
:ref:`sgi-sn-graphics-pipes`. The pipe's chassis wiring is on
:ref:`sgi-onyx2`.

The O2's display controller, GBE, is not one of these boards and has
its own page (:ref:`sgi-gbe`). No register specification of the three SGI
board families above is public. Almost all of each of their
register and rendering behaviour is an *approximation*
built from public drivers, published inventories and the behaviour of the
stock software, as the board pages state.

.. _sgi-graphics-widget-identities:

Widget identities
=================

Each XIO board answers the widget identification with a manufacturer and a
part number. The values come from public driver tables, a *software
expectation*:

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

The table cites OpenBSD ``sys/arch/sgi/xbow/xbowdevs`` and
``sys/arch/sgi/xbow/impact_xbow.c`` at
``2df91d80c104636deeef8124252ddd5951b55fbf``, and Linux
``arch/mips/include/asm/xtalk/xwidget.h`` at
``2f9060b1db4aa2c21c248e34476d8936a2b69cf6``. OpenBSD matches an
ImpactSR board on exactly the pair ``0x2aa``/``0xc003``
(``impact_xbow.c`` lines 61 and 62).

The widget revision is part of the same register. OpenBSD prints it at
attach (``impact_xbow.c`` line 82), but no public source ties a revision to
a board model or option, so the revision of any particular board is
*unknown*. The identity names a family, not a model.

The devices answer these identities themselves. The ``identity`` records in
the model catalogue restate them and do not drive the register.

.. _sgi-graphics-mgras:

IMPACT
======

The board families, population by ``product`` value, register windows,
raster path and display are on :ref:`sgi-mgras`.

.. _sgi-graphics-odyssey:

VPro
====

The products, placement, registers, command path, memory and display are
on :ref:`sgi-odyssey`.

The V-brick is the graphics enclosure of the Origin 3000 family. It adds a
midplane, an L1 controller and its own routing
(:src:`007-4264-002 printed pages 10, 52 and 54`), *established*. No emulated topology models one, and
no V-brick board revision, widget route, NIC, memory or display option has
been measured. An Origin 300 with a V-brick was never an SGI
configuration.

.. _sgi-graphics-kona:

InfiniteReality
===============

The pipe's identity, products, register map, command grammars, DMA and
display are on :ref:`sgi-kona`.

PsiTech cards
=============

The PsiTech RAD1 (:ref:`sgi-psitech-rad1`) and RAD4 (:ref:`sgi-psitech-rad4`)
are PCI cards, not XIO widgets, so they have no entry in the widget
identity table. Their PCI identities are on their own pages.
