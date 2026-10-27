.. SPDX-License-Identifier: BSD-3-Clause

.. _sgi-measured-machines:

=================
Measured machines
=================

Some facts in these pages come from physical SGI machines rather than
from a published document. This page describes each such machine and the
conditions of the measurements. The values each read returned are on the
page of the part they belong to, under its Measurements section.

No read here is a reset value. Each shows what one machine answered at the
stated moment, after its firmware had initialised it.

.. _sgi-measured-octane2:

Octane2, IP30 revision B
========================

One Octane2 belonging to the project's maintainer. It ran IRIX 6.5.30m.
IRIX commands ran in a network shell, and register reads used the PROM
Command Monitor's ``dump`` command with the V12 graphics board fitted.
The maintainer ran each session and supplied its full output. Commands and
values are reproduced exactly. Board barcodes, Laser IDs, serial numbers
and MAC addresses are left out.

The register reads are on these pages:

- HEART identification, status, bank descriptors, widget registers and
  request timeout: :ref:`sgi-heart-measurements`
- Crossbow identification: :ref:`sgi-xbow-measurements`
- RAD1 audio configuration: :ref:`sgi-rad1-measurements`
- the processor and FPU identity words the kernel recorded:
  :ref:`sgi-r10000-measurements`
- the Bridge's alternate flash aperture: :ref:`sgi-ip30-flash-wiring`

.. _sgi-measured-octane2-hinv:

Inventory
---------

``uname -R`` printed ``6.5 6.5.30m``. A repeated ``hinv -mvv`` agreed with
the first. The inventory reports:

.. list-table::
   :header-rows: 1

   * - Component
     - Reported value
   * - Processor
     - one 360 MHz IP30 R12000, chip revision 3.5, 2 MiB secondary cache
   * - HEART
     - revision F
   * - IP30 board
     - ``030-1467-001``, revision B
   * - Frontplane
     - ``FP1``, ``030-0891-003``, revision H
   * - Power supply
     - ``PWR.SPPLY.ER``, ``060-0035-002``, revision A, under
       ``/hw/node/xtalk/15/pci/2``
   * - Memory
     - 256 MiB total
   * - Crossbow
     - ASIC revision 1.4
   * - Processor module
     - ``PM10360MHZ``, ``030-1590-002``, revision B
   * - SCSI
     - two integral QL1040B controllers, revision 2; the disk is unit 1
       on controller 0
   * - Onboard I/O
     - IOC3 serial ports ``tty1`` and ``tty2``, parallel ``plp1``,
       Ethernet ``ef0``, RAD audio revision 12.0
   * - Graphics
     - V12 board at widget 11

The inventory shows neither which bay holds the disk nor how controller 0
maps to a PCI device number, and it reports no register values.

.. _sgi-measured-octane2-graph:

Hardware graph
--------------

A read-only ``ls -l`` of the running hardware graph showed SCSI links under
``/hw/node/xtalk/15/pci/0`` and ``/hw/node/xtalk/15/pci/1``. The first
resolves to ``/hw/node/xtalk/15/pci/0/scsi_ctlr/0``, whose ``target``
directory holds ``1``, whose ``lun`` directory holds ``0``; that LUN holds
a ``disk`` directory and a ``scsi`` device node. The installed disk is
therefore PCI device 0, SCSI target 1, LUN 0 as the kernel names it. This
is kernel bookkeeping, not an electrical measurement.

.. _sgi-measured-octane2-prom:

PROM headers
------------

``/usr/sbin/flash -V`` printed the loaded primary PROM header as version
``4.17``, length ``682448`` bytes, ``7`` reloads, and the recovery header
as version ``4.9``, length ``147198`` bytes, with the same programming
time and reload count. ``/usr/sbin/flash -d`` printed nothing. The flash
contents were not dumped or hashed.
