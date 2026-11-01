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
point in its boot that the part page states, after its firmware had
initialised it.

.. _sgi-measured-octane2:

The reference Octane2
=====================

The reference Octane2 is one physical Octane2, measured for these pages.
Wherever a page says "the reference Octane2", it means this machine:

.. list-table::
   :header-rows: 1

   * - Part
     - Fitted
   * - System board
     - IP30 ``030-1467-001``, revision B
   * - HEART
     - revision F (widget revision field 6)
   * - :term:`Crossbow`
     - ASIC revision 1.4 (widget revision field 5)
   * - Onboard :term:`Bridge`
     - widget 15, revision field 4 (D)
   * - Processor
     - one 360 MHz R12000, chip revision 3.5, 2 MiB secondary cache
   * - Memory
     - 256 MiB in one bank
   * - Graphics
     - V12 (VPro) at widget 11
   * - Operating system
     - IRIX 6.5.30m
   * - Boot :term:`PROM`
     - ``PROM Monitor SGI Version 6.5 Rev 4.17 IP30 Jan 20, 2004 - 64 Bit``

No expansion Bridge, PCI card cage or second :term:`XIO` board is fitted. The
Crossbow reports links alive only at widgets 8, 11 and 15
(:ref:`sgi-xbow-measurements`), and the running kernel's hardware graph
lists widgets 0, 8, 11 and 15 (:ref:`sgi-measured-octane2-graph`).

Register reads used the PROM :term:`Command Monitor`'s ``dump`` command after the
firmware had initialised the machine and stopped at its menu, so no value
is a reset value. A ``dump -w`` read is one aligned 32-bit load and a
``dump -d`` read one aligned 64-bit load, through the uncached processor
segments. No command wrote a register. IRIX commands ran as root at the
serial console, after a normal boot from the
same PROM. Each session's full output was kept, except where a part page
says only the value was reported. Commands and values are reproduced
exactly. Board barcodes, Laser IDs, serial numbers, the system
identifier, network addresses and MAC addresses are left out.

The register reads are on these pages:

- HEART identification, status, mode, error words, MicroLAN, bank
  descriptors, interrupt registers, counter and compare, processor ID and
  widget registers: :ref:`sgi-heart-measurements`
- Crossbow identification, widget 0 registers, per-link status,
  auxiliary status and control, and every reset fence:
  :ref:`sgi-xbow-measurements`
- the Bridge's identification, status, control, direct map, device,
  interrupt, error and time-out registers and both flash selects:
  :ref:`sgi-bridge-measurements`
- the QL1040B configuration headers: :ref:`sgi-qlogic-isp-measurements`
- the onboard :term:`IOC3`'s PCI configuration header, interrupt, chip control,
  pin, MII and Ethernet registers: :ref:`sgi-ioc3-measurements`
- the PHY address and status word the PROM last read:
  :ref:`sgi-ics1890-measurements`
- RAD1 audio configuration header and UST counter:
  :ref:`sgi-rad1-measurements`
- the V12's widget identification, memory configuration and command
  FIFO status: :ref:`sgi-odyssey-measurements`
- the processor and FPU identity words the kernel recorded in its
  hardware inventory, reported from a user-space
  inventory query without a transcript: :ref:`sgi-r10000-measurements`
- the PROM's environment variables: :ref:`sgi-ds1685-measurements`

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
     - V12 board at widget 11, ``ODY128VERSIONB``, ``030-1611-001``,
       revision C
   * - FPU
     - R12010, revision 0.0
   * - PCI functions
     - ``0x1077:0x1020`` at PCI slots 0 and 1, ``0x10a9:0x0003`` at slot
       2 and ``0x10a9:0x0005`` at slot 3

``hinv -c disk`` repeats the SCSI lines. The inventory shows neither which
bay holds the disk nor how controller 0 maps to a PCI device number, and
it reports no register values.

``gfxinfo -vv``, with no X server running, printed:

.. code-block:: text

   Graphics board 0 is "ODYSSEY" graphics.
           Managed (":0.0") 1280x1024
           BUZZ version B.1
           PB&J version 1
           128MB memory
                   Banks: 4, CAS latency: 3
            Monitor 0 type: Unknown

``sysconf`` reported ``PROCESSORS R12000 3.5``, ``NUM_PROCESSORS 1`` and
a 16 KiB page size. It printed no cache line or block size.

The PROM Command Monitor's own ``hinv -v`` printed:

.. code-block:: text

                      System: IP30
                   Processor: 360 Mhz R12000, with FPU
        Primary I-cache size: 32 Kbytes
        Primary D-cache size: 32 Kbytes
        Secondary cache size: 2 Mbytes
                 Memory size: 256 Mbytes
                     Network: ef0 ethernet (100/10 base-T)
                   SCSI Disk: scsi(0)disk(1)
                       Audio: RAD Audio Processor
                    Graphics: ODYSSEY

The PROM inventory names no ASIC revision.

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

``ls -l /hw/node/xtalk`` lists widgets ``0``, ``8``, ``11`` and ``15``.
Widget 8 holds a ``heart`` link to ``/hw/node`` and widget 11 the graphics
widget's node. ``/hw/node/xtalk/15/pci`` lists device directories ``0`` to ``4``
beside ``controller``, ``direct``, ``io`` and ``mem`` nodes. Devices 0 to 3
match the four PCI functions the inventory names. The inventory names no
function at device 4 and the listing of that directory's contents was not
captured, so what the kernel attached there is *unknown*. Linux and
OpenBSD both route the onboard IOC3's second interrupt to Bridge pin 4,
the first device number without a function (:doc:`hardware/ioc3`), which
would explain a kernel entry there; no read here confirms it.

.. _sgi-measured-octane2-prom:

PROM headers
------------

The Command Monitor's ``version`` command printed
``PROM Monitor SGI Version 6.5 Rev 4.17 IP30 Jan 20, 2004 - 64 Bit``.
``/usr/sbin/flash -V`` printed the loaded primary PROM header as version
``4.17``, length ``682448`` bytes, ``7`` reloads, and the recovery header
as version ``4.9``, length ``147198`` bytes, with the same programming
time and reload count. ``/usr/sbin/flash -d`` printed nothing. The flash
contents were not dumped or hashed. The flash reads that show where the image sits and how the boot select
repeats are under :ref:`sgi-bridge-octane2-flash`.

.. _sgi-measured-octane2-counter:

Timed reads
-----------

Two reads were timed against a host clock. The HEART counter advanced
375,407,256 ticks over 30.03 s of host time, 12.5012 MHz
(:ref:`sgi-heart-octane2-counter`). The two RAD1 UST reads were several
seconds apart, within about 8 s by the HEART counter
(:ref:`sgi-rad1-octane2-ust`). The timed interval was bounded by the
keystrokes that sent the commands, which limits the counter rate to
about 0.1%.
