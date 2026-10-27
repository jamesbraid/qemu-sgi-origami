.. SPDX-License-Identifier: BSD-3-Clause

.. _sgi-dbrick:

===================================
SGI D-brick Fibre Channel enclosure
===================================

The D-brick is the Origin 3000 and Onyx 3000 JBOD enclosure: twelve bays
of dual-ported Fibre Channel disks behind two loop resiliency circuit
(LRC) modules, cabled to a Fibre Channel adapter in an I-brick or
P-brick. ``hw/scsi/sgi-dbrick.c`` models it as the device ``sgi-dbrick``,
which places ``scsi-hd`` disks on a QLogic ISP2200's loop into bays
(:doc:`/system/mips/sgi/hardware/qlogic-isp2200`).

Sources: *SGI Origin 3000 Series Owner's Guide* (007-4240-002), chapter
8 and printed pages 69, 72 and 83; *SGI Onyx 3000 Series Graphics System
Owner's Guide* (007-4264-002), printed pages 9, 62 and 128; *SGI Origin
3000 Series Technical Configuration Owner's Guide* (007-4311-002),
printed pages 48 and 51; *SGI Origin 3000 Server Series System
Architecture* (108-0240-002), printed page 2-34.

What the host sees
==================

.. list-table::
   :header-rows: 1
   :widths: 50 14 36

   * - Fact
     - Rating
     - Source
   * - Twelve bays, four columns by three rows, numbered column/row from
       1/1 at the top left. A fitted brick holds two to twelve disks.
     - established
     - 007-4240-002 pp. 156, 172-173
   * - The disks in bays 1/3 and 4/3 must be present: the operator panel
       uses them as conduits for enclosure information.
     - established
     - 007-4240-002 pp. 159, 166
   * - The brick has no L1 controller, no system-control connection and no
       XIO or NUMAlink port. Its controller is the operator panel's
       enclosure-services microcontroller.
     - established
     - 007-4240-002 pp. 72, 83; 007-4264-002 p. 128
   * - Supported Fibre Channel adapters are single-port, so one adapter
       reaches one LRC input and one loop of a brick.
     - inferred
     - 007-4311-002 p. 51
   * - Every occupied bay is a port on the one attached loop.
     - model choice
     - 007-4240-002 p. 156; 108-0240-002 Figure 2-26
   * - Which loop ID each bay takes, and how the LRC's ID selector
       (settings 1 to 7) sets them.
     - unknown
     - 007-4240-002 p. 168
   * - The disks' worldwide names.
     - per disk
     - each disk's label carries its own (007-4240-002 p. 158)

Each LRC reaches all twelve disks (007-4240-002 p. 156), but the same
guide says each LRC takes six of them as its own loop at startup (p.
168, where the printed sentence is cut off) and names neither the six nor
the split's later behaviour. The model takes the twelve-bay reading
because choosing six would also mean inventing which six; a model of the
startup split needs a port database captured from a real D-brick.

The disks keep their own place on the loop. Each is an ordinary
``scsi-hd`` on the adapter's bus whose ``scsi-id`` is its loop ID and
whose ``port_wwn`` and ``wwn`` are its names, so the D-brick assigns no
loop ID and invents no name; whoever fits the disks chooses them, as the
loop ID rule is unknown. The enclosure is created after its disks and
names them by id::

    -device qlogic-isp2200,id=fc0,...
    -device scsi-hd,id=bay13,bus=fc0.0,scsi-id=1,drive=d13,port_wwn=...,wwn=...
    -device scsi-hd,id=bay43,bus=fc0.0,scsi-id=2,drive=d43,port_wwn=...,wwn=...
    -device sgi-dbrick,id=dbrick0,bay1-3=bay13,bay4-3=bay43

``bay1-1`` to ``bay4-3`` link a bay to its disk. The enclosure refuses
an empty bay 1/3 or 4/3, anything other than a ``scsi-hd`` in a bay, one
disk in two bays, a disk on any bus but an ISP2200's, disks on two
different loops, a disk at a LUN other than 0, and a bay disk whose loop
ID also holds another device, since each disk module is a loop port of
its own. Bays are fixed when the enclosure is created; the device is not
hot-pluggable, and unplugging a bay's disk is not tracked.

Not modelled
============

Enclosure services, the operator panel, LEDs, alarm, key switch,
standby, power and cooling, thermal shutdown, the second LRC, loop
bypass and failover, daisy-chained bricks, and hot swap. No public IRIX
document says that IRIX queries a D-brick's enclosure services, and the
published IRIX inventories list only disks, so the model offers none:
no enclosure port appears on the loop, and an enclosure-services request
sent to a further LUN of a bay's disk finds no logical unit there (CHECK
CONDITION, LOGICAL UNIT NOT SUPPORTED). Adding enclosure services needs
evidence that a supported guest uses them and a public specification of
the panel's enclosure-services part.

The brick has no state of its own to migrate.
