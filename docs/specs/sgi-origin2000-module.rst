.. SPDX-License-Identifier: BSD-3-Clause

.. _sgi-origin2000-module:

==================================================
SGI Origin 2000 module: nodes, Routers, management
==================================================

An Origin 2000 module is the deskside chassis or one rack module: a
midplane with four node-board slots, two Router slots, two Crossbows
and twelve XIO slots, plus the module system controller. This page is
the contract the ``origin2000`` machine models cite for how a module is
populated, how its management bus is wired and how the processor's
reset mode is chosen. Ratings follow :ref:`sgi-ip30`. The Router chip
itself is on :doc:`sgi-router`, the Crossbow on :doc:`sgi-xbow` and the
controller's command protocol on :doc:`sgi-elsc`.

Sources: *Origin2000 Deskside Owner's Guide* (007-3453-002), *Onyx2
Deskside Workstation Owner's Guide* (007-3454-005), *Origin2000
Rackmount Owner's Guide* (007-3456-003), *Origin and Onyx2 Theory of
Operations* (007-3439-002), *Origin200/2000 IP27 PROM Technical
Reference* (108-0170-001), *Origin2000 and Onyx2 Diagnostic Roadmap*
(108-0164-002), the *Hardware Quick-reference Booklet*
(``web:origin-onyx2-quick-reference-booklet``) and two public IRIX
inventories (``web:nekonomicon-16727716``, ``web:nekonomicon-16285``).

.. _sgi-origin2000-module-nodes:

Node boards and slots
=====================

.. list-table::
   :header-rows: 1
   :widths: 46 12 42

   * - Fact
     - Rating
     - Source
   * - Node boards fill N1 first, then N2, N3, N4, counted from right to
       left. A node board carries one or two R10000 processors.
     - established
     - 007-3453-002 printed page 21; 007-3454-005 printed page 21;
       007-3456-003 printed page 20. The booklet's Table 42 also lists a
       three-node placement N1, N2, N4 for three graphics pipes; the
       model supports fill order only.
   * - The deskside is equivalent to one rack module.
     - established
     - 007-3456-003 printed pages 30 and 44.
   * - Node 1 activates XIO slots IO1 to IO6 on Crossbow 0 and node 2
       activates IO7 to IO12 on Crossbow 1. With one node board only
       the first six slots work.
     - established
     - 007-3456-003 printed pages 20-21 and Figure 2-3; 007-3453-002
       printed page 21 (Table 2-2).
   * - With four node boards, N1 controls IO1, IO3 and IO5 and N3 IO2,
       IO4 and IO6 on Crossbow 0; N2 controls IO7, IO9 and IO11 and N4
       IO8, IO10 and IO12 on Crossbow 1.
     - established
     - Booklet Tables 41 and 42, printed pages 130-131.
   * - With three node boards, N1 takes IO1, IO3, IO5 and N3 takes IO2,
       IO4, IO6; N2 takes all of Crossbow 1.
     - inferred
     - The four-node quadrants with N4 absent. The booklet shows the
       controller within a Crossbow is a placement choice, so this is
       SGI's recommendation, not wiring; it has no guest-visible effect
       in the model beyond validating a row.
   * - An absent node board leaves its Crossbow port empty.
     - inferred
     - A Crossbow port with no widget attached reads as empty
       (:ref:`sgi-xbow-link-registers`).

The widgets are Crossbow 0 and 1 at node widgets 9 and A; which node
reaches which Crossbow depends on the slot (the deskside model reaches
Crossbow 0 widget 9 from N1, Crossbow 1 widget A from N2, Crossbow 0
widget A from N3 and Crossbow 1 widget 9 from N4). That map is the
emulator's selection: the Theory of Operations and the booklet give the
widget numbers but no source states the slot-to-widget wiring.

.. _sgi-origin2000-module-routers:

Router fit
==========

.. list-table::
   :header-rows: 1
   :widths: 10 36 10 44

   * - Nodes
     - Fit
     - Rating
     - Source
   * - 1
     - No Router, or the first Router board; the deskside guide says
       both. The model fits none.
     - inferred
     - 007-3456-003 printed page 30 ("no Router is required"); against
       007-3453-002 printed page 21 ("all deskside systems ship with the
       first router board installed") and Figure 2-9 (printed page 22,
       "None" for one node). Which a one-node deskside carried is
       unknown; a one-node rack capture would settle it.
   * - 2
     - Null Router in R1, a passive Hub-to-Hub link (deskside); two
       Standard Routers (rack module).
     - established
     - 007-3453-002 Figure 2-9; ``web:nekonomicon-16285`` lists a
       two-node module with both Routers, R1 active ports 4, 5, 6 and
       R2 port 6 alone.
   * - 3, 4
     - SGI's deskside fit is a Router board in R1 and a Star Router in
       R2 joined by an IR1 jumper. The model fits two Standard Routers.
     - rack established, deskside inferred
     - 007-3453-002 Figure 2-9; 007-3439-002 printed page 57 (3- and
       4-node configurations use a Standard Router with a Star Router);
       ``web:nekonomicon-16727716`` boots PROM 6.156 with "5 objects
       (3 hubs, 2 routers)" on node slots 1 to 3, and its IRIX listing
       shows four Hubs and both Routers each with active ports 4, 5 and
       6. The deskside inference chain: a deskside is one module
       (007-3456-003 page 44); 108-0170-001 section 6.2.1 gives "a
       typical fully-populated single-module system" two Routers
       (printed pages 6-2 to 6-3); and the Standard Router joins 2 to
       32 node boards in 1 to 8 enclosures (007-3439-002 page 57).

The Star Router is a carrier with no Router ASIC. With the Router in R1,
R1 port 5 reaches N1, port 4 N2, port 6 N3 through the midplane, and
port 3 N4 through an external jumper to the Star's one connector, where
its XC chip converts the differential cable signals to single-ended;
ports 1 and 2 stay free (established, 108-0170-001 section 6.2.2,
printed pages 6-3 to 6-4, PDF 97-98; 007-3456-003 printed page 32). The
model does not fit the Star Router: the stock PROM 6.156 discovery
code checks each Hub's partner on the same Crossbow through a fixed
two-Router vector route (``0x56`` or ``0x46``, Table 6-1), and on the
Star wiring that vector reaches a Hub with a route step left over. What
real silicon answers is *unknown*; settling it needs a Hub specification
or a boot log from a three- or four-node Star deskside. Neither of the
project's physical SGI systems is an Origin 2000, so a physical test
cannot answer it.

.. _sgi-origin2000-module-management:

Module management bus
=====================

.. list-table::
   :header-rows: 1
   :widths: 46 12 42

   * - Fact
     - Rating
     - Source
   * - A module has one module system controller (MSC) on one I2C bus
       with a branch to each of the four nodes; the MSC's NVRAM,
       voltage-margin and control block sits on the same bus. The
       diagram shows one MSC, not four, and no private NVRAM copies
       behind the nodes.
     - established
     - 108-0164-002 Figure B-1, printed page B-1 (PDF 55).
   * - The MSC has one RS-232 alternate-console port. A deskside
       attaches a terminal directly; a rack can attach the multimodule
       system controller.
     - established
     - 108-0170-001 section 1.6 (module system controller) and
       007-3456-003; no source gives four ports.
   * - The model therefore builds four node-side PCF8584 adapters, one
       shared bus, one MSC and one shared NVRAM address space.
     - approximation
     - The wiring is the figure above; the number of PCF8584 parts on
       the node side (one per Hub) is inferred from there being four
       masters on the bus. The count of physical EEPROM packages is
       *unknown*.
   * - The MSC answers at I2C address ``0x08``.
     - approximation
     - The model's choice, shared with the Origin 200 controller model
       (:doc:`sgi-elsc`). No public source states the address; a bus
       capture or the controller firmware would settle it.
   * - The management NVRAM is a 2048-byte store at eight consecutive I2C
       addresses from ``0x50``, with 1792 to 2047 reserved for the
       controller.
     - see :ref:`sgi-elsc-nvram`
     - Ratings and sources are on the controller page.

.. _sgi-origin2000-module-number:

Module number
-------------

Each module is assigned a unique module number from 1 to 255; 0 means
none has been assigned. IRIX names devices by module number. The number
is stored in MSC NVRAM, and a backup copy is stored in each node card's
PROM log, from which it is voted if the NVRAM is inaccessible
(established, 108-0170-001 section 1.5, printed page 1-4, PDF 18).
Where inside the NVRAM the number, its validity marker and the
partition number sit has no public source and is *unknown* for this
page; :ref:`sgi-elsc-nvram` says what the model chose and seeds from the
product description's module number. The selected one-module machine
numbers its module 1; that value is a selected installation, not a
hardware reset default.

The per-node PROM logs are separate storage from the shared NVRAM and
from the BaseIO's own non-volatile store.

.. _sgi-origin2000-module-grants:

Processor grants
----------------

The controller's per-processor grant bytes on the shared bus, their
rotation and recovery are described as model behaviour in
:ref:`sgi-elsc-arbitration`. No public source documents the
Origin 2000 controller's grant values, ordering or recovery timer, so
all of that is *unknown* as hardware and an emulator choice as model
behaviour.

.. _sgi-origin2000-reset-mode:

Processor reset mode
====================

The R10000 loads a 25-bit mode word from the system data bus at power-on
or cold reset; it becomes CP0 Config bits 24:0, a soft reset preserves
it, and the primary-cache size fields are hardwired (established,
R10000 User's Manual 2.0, printed pages 160-165 and 256, PDF 190-195 and
286). The mode word's secondary-cache size field (Config bits 18:16)
encodes 1 for 1 MB and 3 for 4 MB (established, same manual, printed
page 165).

SGI's booklet pairs the IP27 node-board assemblies as a 1 MB cache with
a 180 MHz processor and a 4 MB cache with a 195 MHz processor
(established, booklet printed page 93); it documents no 195 MHz and 1 MB
Origin 2000 assembly. The stock PROM 6.156 configuration block carries
mode word ``0x1211a785`` with a 180 MHz processor clock, a 90 MHz Hub
clock and machine type zero, whose secondary-cache size field is 1
(measured from the stock image, ``firmware:ip27-prom-6.156``; the block
layout is on :doc:`sgi-prom-image`). The machine models therefore select
a 180 MHz R10000, 90 MHz Hub and 1 MiB cache and leave the stock word
unchanged. The agreement of a published board pairing with the image's
own record picks a documented real composition; neither alone does.
A 195 MHz part with 1 MB is not selected, because the booklet pairs 195
MHz with 4 MB and the 4 MB encoding would change bit 17 and yield
``0x1213a785``, which does not match the stock image. That the product
shipped with exactly this configuration is not established: it is the
configuration the retained image describes.

That the firmware compares the flash word against CP0 Config at start-up
and reports a mismatch on the node's LEDs is established only from
vendor sources and image decoding this page does not cite, so the
procedure is *unknown* here. Its outcome is visible as a model
requirement: a processor whose Config disagrees with the node's flash
word does not start the firmware, so the model's CPU reset
configuration must equal the flash word (an emulator constraint, with
the discriminating test being a stock PROM boot).
