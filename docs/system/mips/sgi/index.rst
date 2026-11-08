.. SPDX-License-Identifier: BSD-3-Clause

.. _sgi-machine-pages:

SGI machines
============

These pages describe the SGI MIPS machines this tree emulates and the
hardware they are built from. Each platform page covers its machines,
how they boot and what they are missing; the hardware pages describe one
chip or board each, with the evidence behind every modelled behaviour.

.. _sgi-ratings:

How facts are rated
-------------------

Every statement about hardware carries one of these ratings, with a
citation to what supports it. Citations read ``[id, locator]`` and link
to :ref:`sgi-references`.

established
  stated by a cited specification or manual, or read from an identified
  real machine (:doc:`measured-machines`);
observed
  reported by a published capture of a real machine;
software expectation
  what public drivers, the IRIX kernel or the stock :term:`PROM` do or rely on,
  which shows what software needs, not what the hardware is;
inferred
  follows from established facts by a stated step;
approximation
  a model choice standing in for hardware that is not known, with its
  limitation stated;
unknown
  no available source settles it; the entry says what would.

A model run shows what the model does. It is never evidence of what the
hardware does.

Machines
--------

The Scalable Node overview covers what the :term:`SN0` and :term:`SN1` machines share;
each platform page covers its own products.

.. toctree::
   :maxdepth: 1

   sn
   ip27
   ip30
   ip32
   ip35
   ip53
   identity
   measured-machines
   vocabulary

Firmware
--------

.. toctree::
   :maxdepth: 1

   prom-image

Hardware
--------

Each page describes one chip, board or product family and links the
platforms that use it.

Processors and system ASICs
~~~~~~~~~~~~~~~~~~~~~~~~~~~

.. toctree::
   :maxdepth: 1

   hardware/heart
   hardware/hub
   hardware/bedrock
   hardware/xbow
   hardware/bridge
   hardware/xbridge
   hardware/pic
   hardware/router
   hardware/sn1-router
   hardware/crime
   hardware/r10000

I/O and system control
~~~~~~~~~~~~~~~~~~~~~~

.. toctree::
   :maxdepth: 1

   hardware/ioc3
   hardware/ioc4
   hardware/mace
   hardware/baseio
   hardware/ioc3-boards
   hardware/mscsi
   hardware/pci-expansion
   hardware/gigachannel
   hardware/bic
   hardware/bricks
   hardware/l1
   hardware/pbay
   hardware/elsc
   hardware/timekeepers
   hardware/ds1685
   hardware/at29c040a

Network
~~~~~~~

.. toctree::
   :maxdepth: 1

   hardware/tigon
   hardware/xframe
   hardware/bcm570x
   hardware/mace-ethernet
   hardware/dp8384x
   hardware/ics1890

Storage and buses
~~~~~~~~~~~~~~~~~

.. toctree::
   :maxdepth: 1

   hardware/qlogic-isp
   hardware/qlogic-isp2200
   hardware/dbrick
   hardware/aic7880
   hardware/lucent-ohci
   hardware/tsb12lv22

Graphics
~~~~~~~~

.. toctree::
   :maxdepth: 1

   hardware/graphics
   hardware/mgras
   hardware/odyssey
   hardware/kona
   hardware/gbe
   hardware/vice
   hardware/psitech-rad1
   hardware/psitech-rad4

Audio
~~~~~

.. toctree::
   :maxdepth: 1

   hardware/rad1-audio
   hardware/ad1843

References
----------

.. toctree::
   :maxdepth: 1

   references

Glossary
--------

.. toctree::
   :maxdepth: 1

   glossary
