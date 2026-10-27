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
  what public drivers, the IRIX kernel or the stock PROM do or rely on,
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

.. toctree::
   :maxdepth: 1

   sn
   ip27
   ip30
   ip32
   ip35
   measured-machines
   prom-image
   vocabulary
   references

Hardware
--------

.. toctree::
   :maxdepth: 1
   :glob:

   hardware/*
