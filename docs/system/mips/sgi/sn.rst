.. SPDX-License-Identifier: BSD-3-Clause

.. _sgi-sn:

==============================================
Scalable Node (SN0 and SN1) machines: overview
==============================================

SGI's Scalable Node machines are cache-coherent NUMA multiprocessors
built from :term:`nodes <node>`. Each node is a board with processors, local memory and
a node controller that joins them to the memory's coherence directory,
to one :term:`XIO` port for I/O and to one network port for other nodes. :term:`SN0`
uses the :term:`Hub` node controller and the :term:`CrayLink` interconnect (*established*,
:src:`007-3439-002 Figure 2-1`). :term:`SN1` uses the :term:`Bedrock` node controller
and :term:`NUMAlink` 3, and describes itself as a distributed shared memory,
global-address-space, cache-coherent design (*established*,
:src:`007-4311-002 p. 9`; :src:`108-0240-002 pp. 2-11 to 2-17`).

QEMU builds both generations from one machine base, ``sgi-sn``
(``TYPE_SGI_SN_MACHINE``, ``hw/mips/sgi/sn-machine.c``). This page
covers what the generations share and the mechanisms the base gives
every product: the catalogue, topology and population rules, graphics
pipes, firmware and writable storage, the serial-line order, board
devices and launch metadata. Each platform's products, wiring and limits
are on its own page: :ref:`sgi-ip27`, :ref:`sgi-ip35` and
:ref:`sgi-ip53`. Chips and boards are on the part pages listed under
:ref:`sgi-sn-devices`.

Facts are rated as described in :ref:`sgi-ratings`.

.. _sgi-sn-product-family:

Product family
==============

.. _sgi-sn-family-table:

Family summary
--------------

.. list-table::
   :header-rows: 1

   * - Product
     - Generation, platform and :term:`node board`
     - Interconnect
     - QEMU
   * - Origin 200
     - SN0, :term:`IP27 <IP number>` platform, IP29 system board
     - one tower, or two joined Hub to Hub by a CrayLink cable
       (*established*, :src:`007-3708-002 pp. 5-6`)
     - ``origin200``
   * - Origin 2000
     - SN0, IP27 platform, IP27 node boards
     - Routers within and between :term:`modules <module>` (*established*,
       :src:`007-3439-002 p. 59`)
     - ``origin2000``, one module
   * - Onyx2
     - SN0, IP27 platform, IP27 node boards
     - as the Origin 2000
     - ``onyx2``, one module
   * - Origin 300
     - SN1, IP35 platform, IP45 board
     - NUMAlink 3, LINK to LINK for two modules or through a NUMAlink
       module (*established*, :src:`007-4365-002 p. 38`;
       :src:`007-4498-002 pp. 10-11`)
     - ``origin300``
   * - Fuel
     - SN1, IP35 platform, IP34 board
     - none in the model, one node
     - ``fuel``
   * - Origin 3200
     - SN1, IP35 platform, C-brick (IP35 board)
     - two C-bricks joined LINK to LINK by NUMAlink 3, no R-brick
       (*established*, :src:`007-4240-002 pp. 124-125`;
       :src:`007-4311-002 p. 87`)
     - ``origin3200`` catalogue entry, a planned target the emulator
       does not build
   * - Origin 3400
     - SN1, IP35 platform, C-bricks
     - NUMAlink 3 through R-bricks (*established*,
       :src:`007-4311-002 pp. 76-78`)
     - none
   * - Origin 3800
     - SN1, IP35 platform, C-bricks
     - NUMAlink 3 through R-bricks (*established*,
       :src:`007-4311-002 pp. 79-80`)
     - none
   * - Onyx 3000
     - SN1, IP35 platform, C-bricks with G-brick or V-brick graphics
       (*established*, :src:`007-4264-002 pp. 8, 10`)
     - as the Origin 3000 servers
     - none
   * - Origin 3900, Onyx 3900
     - SN1, CX-brick with one to four IP53 node boards
       (*established*, :src:`007-4426-001 p. xi`;
       :src:`007-4572-001 pp. 1, 3`)
     - NUMAlink 3 through the CX-brick's own router board
       (*established*, :src:`007-4572-001 pp. 4-5`)
     - ``origin3900`` reference composition for the Origin 3900, never
       compiled
   * - Origin 350, Onyx 350, Tezro, Onyx4
     - SN1, IP35 platform, IP53 board
     - NUMAlink 3 on one LINK connector, and a NUMAlink module for more
       than two modules (*established*, :src:`007-4566-001 p. 69`)
     - ``origin350``, ``onyx350``, ``tezro``, ``onyx4-host``

The Origin 3000 series sizes are 2 to 8 processors in a short rack for
the Origin 3200, 4 to 32 for the Origin 3400 and 16 to 512 for the
Origin 3800 (*established*, :src:`007-4311-002 p. 10`). The Origin 3400
grows to eight four-processor C-bricks and the Origin 3800 to 128
(*established*, :src:`007-4311-002 p. 83`). A CX-brick holds the
equivalent of four C-bricks and one R-brick (*established*,
:src:`007-4572-001 pp. 1, 3`). The Onyx 3000 is *observed* in one
published 12-processor inventory (:src:`nekonomicon-16719632`). No QEMU
machine builds an Origin 3000 :term:`brick`; :doc:`hardware/bricks` describes
each brick and the catalogue layers that record them.

A Hub carries two processors on one processor interface
(*established*, :src:`007-3439-002 Figure 2-1`). A Bedrock carries up to
four on two interfaces (*established*, :src:`108-0240-002 p. 2-12`).
SN0 I/O is a :term:`Bridge`, behind a :term:`Crossbow` where the module has one. SN1 I/O
is an :term:`XBridge`, or a :term:`PIC` on the IP53 board. The system controller is the
:term:`ELSC` or the module system controller on SN0 and an :term:`L1` on SN1. The part
pages rate and cite each of these.

The Onyx2 is Origin 2000 silicon with graphics: the same IP27 node
boards, memory and stock :term:`PROM`, told apart only by the hardware its
topologies fit. The IP53 machines are software profiles of one capture
or manual each, not qualified models of every configuration of the
product (:ref:`sgi-ip35-ip53`). The machine classes are in
``hw/mips/sgi/sn-machine.c`` with the SN0 node in ``hw/mips/sgi/sn0.c``,
and in ``hw/mips/sgi/sn1.c`` with the SN1 node in
``hw/mips/sgi/sn1-node.c``. Every class parents ``sgi-sn`` and takes the
same ``topology=`` contract and compiled catalogue. Only the node device
is generation-specific.

.. _sgi-sn-shared-model:

What the generations share
==========================

Both generations use a flat physical address space striped by node. Two
attribute bits above the node number select the cached, uncached or
special-space window. The address maps are on :ref:`sgi-ip27-address-map`
and :ref:`sgi-ip35-address-map`, each with its own ratings and sources.
Both boot a stock PROM in the same container format, with the same entry
points and configuration block (:doc:`prom-image`).

.. _sgi-sn-catalogue:

The catalogue
-------------

The catalogue is ``hw/mips/sgi/sgi-machines.json``, embedded in the
emulator and parsed on first use by ``hw/mips/sgi/sgi-machines.c``. The
``query-sgi-machines`` QMP command returns the embedded copy verbatim
(``SGIMachines`` in ``qapi/sgi-machines.json``), so a package build can
read the catalogue from the binary it ships. The document has two
sections:

- ``topologies`` are what the builder reads. Each names every node,
  module, fabric link, I/O board and controller of one configuration.
- ``offerings`` are what only launch tools read. An offering is one
  accepted population a launcher may select. It carries the ``-M`` value
  that selects it (``machine-options``), every writable store the machine
  has (``storage``, with each store's size and starting contents) and the
  firmware images that initial content is built from (``init-inputs``).

A topology is a wiring. An offering is a launchable choice of one
topology and one population of it. The machine never reads an offering.

.. _sgi-sn-topology:

Products and topology
=====================

A product's ``topology=`` machine property selects which configuration
of it to build. ``-smp``, ``-m`` and ``-cpu`` then select and validate a
population within that wiring. Topologies are product-scoped:
``origin200`` never builds the ``origin2000`` wiring. The compiled
descriptor carries more topologies than any one launch needs, and each
platform page lists the ones its products accept
(:ref:`sgi-ip27-topologies`, :ref:`sgi-ip35-products`,
:ref:`sgi-ip53-products`). Naming a topology
that is not one of the product's own, or any other unknown name,
including ``topology=help``, makes the machine report every topology
that product accepts and exit, since QEMU has no generic help path for a
property value.

A launch without ``topology=`` builds the product's default topology,
except for ``onyx350`` and ``tezro``. Those two refuse a launch that
does not name one: ``onyx350-compat``, or one of ``tezro-tower-one``,
``tezro-tower-multi`` and ``tezro-rack``. An IP53 machine admits one
resolved tuple of processors, memory and I/O per profile without
``ip53-experimental=on``, and any other tuple needs it
(:ref:`sgi-ip53-serial-profiles`).

Each topology describes one chassis. Its catalogue record carries a
node-population row for each count of node boards it accepts. The
``nodes`` machine property picks the row and defaults to the largest. The
row decides which Routers and cables exist and which XIO slots a board
fitted at launch may use. A slot the row cannot use is refused with the
node board it needs. A count no row accepts is refused with the counts
that exist, and a row whose evidence does not settle its wiring is
refused rather than built. ``nodes`` is part of the machine
configuration: a snapshot restores only into a machine with the same
value. An absent node board has no node controller, processors or boot
flash.

``-smp`` and ``-m`` choose the population:

- Every processor is its own single-core socket. ``-smp N`` alone names
  the count, and ``cores=`` or ``threads=`` is refused. A ``sockets=``
  that differs from ``maxcpus`` is refused. ``-smp N,maxcpus=M`` is
  accepted.
- The topology accepts a set of per-node processor counts, its accepted
  populations. ``-smp N`` picks the one accepted population whose total is
  N (``sgi_machine_find_population_by_total()``). Nothing splits N evenly.
  On a two-node ``origin2000-rack``, ``-smp 3`` builds 2:1, the only
  population of 3. A total that two populations make is refused until
  ``population=`` names one, as per-node counts in node order such as
  ``4:2``. A total no population makes is refused with the totals that
  exist.
- ``-smp`` keeps the product's default, which fits its most node boards.
  The machine cannot tell a default from an explicit value, so a launch
  with fewer node boards names it.
- ``-m`` splits evenly across nodes, and each node's share must be one of
  that node board's accepted sizes. A refused size is reported with the
  per-node sizes that are accepted. Without ``-m`` every node gets the
  topology's default memory.
- A topology whose descriptor gives its nodes different bank totals is
  refused, except an IP53 profile. An IP53 profile fixes each node's
  memory in its descriptor, requires the descriptor's processor
  population, and requires ``-m`` to match the sum.
- ``-cpu`` is checked against the product's valid CPU types: R10000 for
  ``origin2000`` and ``onyx2``, R10000 and R12000 for ``origin200``,
  R14000 for ``origin300`` and ``fuel``, and R16000 for the IP53
  machines.

The abstract base refuses ``-numa`` outright: ``topology=`` already
selects the wiring, and ``-smp`` and ``-m`` select the population within
it, so a NUMA node list would either repeat that or contradict it. Every
product also refuses ``-kernel``. A guest reaches its kernel or
standalone program through the PROM's own TFTP netboot or a disk image,
the same as on real hardware. These rules apply to every product through
the shared builder.

The accepted node counts, populations and memory sizes of each product
are on the platform pages: :ref:`sgi-ip27-topologies`,
:ref:`sgi-ip27-processors`, :ref:`sgi-ip27-memory`, :ref:`sgi-ip35-products`,
:ref:`sgi-ip35-memory`, :ref:`sgi-ip53-products` and
:ref:`sgi-ip53-memory`.

.. _sgi-sn-graphics-pipes:

Graphics pipes
==============

A graphics pipe is one ``-device`` of its family's device type, on the
Scalable Node machines and on ``octane`` and ``octane2``:
``sgi-mgras`` (IMPACT), ``sgi-odyssey`` (VPro), ``sgi-kona``
(InfiniteReality) and ``psitech-rad4`` (PsiTech RAD4). It is placed with
the properties QEMU already has for it. ``slot=`` names an XIO slot by
its label (``io6``) or a crossbar port (``widget-11`` on an Octane,
``widget-13`` on a Fuel, ``widget-12`` on an IP53 machine), and a
PsiTech card is a PCI card placed with ``bus=`` and ``addr=``.
``product=`` names the product, and an InfiniteReality pipe takes its
further settings, ``geometry``, ``rm-type``, ``rm-count``,
``texture-mb`` and ``display``, as properties of the same names::

    -M origin200,topology=origin200-gigachannel -device sgi-mgras,slot=io6,product=si-t
    -M onyx2,topology=onyx2-deskside -device sgi-kona,slot=io4,product=ir2,geometry=ge14-2,display=dg5-2
    -M origin200 -device psitech-rad4,bus=pci,addr=5,product=rad4

``query-sgi-machines`` lists, under each topology's ``graphics`` record,
every slot QEMU can put a pipe in, with a plain description, the
``-device`` properties that reach it, the families it takes, each
family's products with every setting's values and default, the pipe the
machine fits by default, whether a launcher should offer the topology
without a pipe, and the reason for each combination it refuses. It lists
only what QEMU builds: a product or value it does not model appears
nowhere, not even as a refusal. The products, their names and every
setting's values live once in
``hw/mips/sgi/models/graphics-products.yaml``. Each product layer lists
its slots and what each offers, and ``scripts/sgi-model.py`` checks the
rules below before it emits the records:

* a default names a family, product and values its slot offers, and no
  refusal matches it; at most one slot of a topology has one;
* where one slot offers a product or a setting value and another slot of
  the same family does not, that slot carries a refusal with the reason;
* no refusal names a product or value no slot offers;
* a reason is a phrase with no final full stop;
* a slot exists from the node count its Crossbow port's node board needs,
  given as ``requires``.

A topology whose own boards do not suit a pipe says so in
``machine-properties``: the ``-M`` properties a launch that fits any pipe
adds, and the reason. The Origin 2000 lists ``baseio-preset=io6g``. QEMU
does not add these properties itself, because it builds the BaseIO card
while the machine initializes, before any ``-device`` exists, and a
``-device scsi-hd`` needs the card's SCSI bus by then
(:ref:`sgi-ip27-graphics`).

A refusal matches a requested pipe when its family matches, its product
matches if it names one, and every setting it lists has the requested
value. Every slot and setting is ``changeable: false`` until a machine's
graphics can change between boots.

Default pipes follow QEMU's display-adapter convention. A machine that
ships with graphics fits its catalogue default unless a graphics
``-device`` or ``-vga none`` is given. Any graphics ``-device`` replaces
the default, whichever slot it names, so an Octane with
``-device sgi-mgras,slot=widget-9`` leaves ``widget-11`` empty. A
``-device`` that leaves a setting unset takes the slot's default for it.
Each machine's slots and defaults are on :ref:`sgi-ip27-graphics`,
:ref:`sgi-ip35-graphics`, :ref:`sgi-ip53-graphics` and
:ref:`sgi-ip30-graphics`.

``-vga none`` and ``-nodefaults`` skip the default pipe. With no pipe
``-device``, every topology then starts with no graphics. The machine
cannot tell the two options apart, so it refuses neither. The
catalogue's ``headless`` flag only tells a launcher which topologies to
offer without graphics, with the reason in ``headless-refused`` where it
is false. The machine itself does not enforce it. No SGI machine takes a
PC display adapter, so a ``-vga`` value the machine can tell from the
default, such as ``std``, is refused. A machine sees only the adapter
type ``-vga`` leaves behind, and with no ``-vga`` QEMU picks Cirrus
where the binary has it, so ``-vga cirrus`` cannot be told from no
``-vga`` and is taken as the default.

The machine checks each pipe before it realizes, as its pre-plug
handler, so the check sees the device's final property values,
``-global`` included. It fits the default once every ``-device`` exists,
from its machine-init-done notifier. The device is created while the
machine initializes, so it is not taken for a hot-plugged one, and it is
realized only when no ``-device`` turned out to be a pipe. Where a pipe
is a slot of the topology's own I/O board records, as the Onyx2's
InfiniteReality pipe and the Fuel's VPro are, the builder fits nothing
there. The pipe that claims the slot takes the record's port, identity
records and the node controller its packets go to, keeping the QOM name
the builder gave it (``io4``, ``io3``, ``graphics-slot``). A Crossbow
port such a record names starts empty, as an empty XIO slot's does,
until its pipe attaches.

QEMU refuses a pipe with one of these messages, where ``<machine>`` is
the ``-M`` name, ``<t>`` the topology and ``<reason>`` the catalogue's
own text, given where a refusal matches:

.. list-table::
   :header-rows: 1

   * - Case
     - Message
   * - ``-vga`` other than ``none``
     - ``<machine>: -vga <type> is not a display an SGI machine takes; use
       -vga none for no graphics, or a graphics -device such as sgi-mgras``
   * - No slot given
     - ``<machine>: <device> needs slot= naming one of the graphics slots
       of topology '<t>': <slot> (<placement>), ...``, or ``bus= and
       addr=`` for a PCI card
   * - PCI card off every slot
     - ``<machine>: topology '<t>' has no graphics slot at
       bus=<b>,addr=<n>; its graphics slots are <slot> (<placement>),
       ...``
   * - Slot not in the record
     - ``<machine>: topology '<t>' has no graphics slot <slot>; its
       graphics slots are <slot> (<placement>), ...``
   * - Family not taken at the slot
     - ``<machine>: graphics slot <slot> of topology '<t>' takes
       <devices>, not <device>[: <reason>]``
   * - Product not offered at the slot
     - ``<machine>: graphics slot <slot> of topology '<t>' does not offer
       <device> product=<p>; it offers <products>[: <reason>]``
   * - Setting value not offered
     - ``<machine>: graphics slot <slot> of topology '<t>' does not offer
       <device> product=<p> with <setting>=<value>; it offers
       <values>[: <reason>]``
   * - Product setting the slot offers no choice for
     - ``<machine>: graphics slot <slot> of topology '<t>' offers
       <device> product=<p> no <setting> choice, so <setting>=<value> does
       not fit[: <reason>]``
   * - Combination the catalogue refuses
     - ``<machine>: graphics slot <slot> of topology '<t>' does not take
       this <device>: <reason>``, for a combination whose every value is
       offered
   * - Slot the node count does not fit
     - ``<machine>: graphics slot <slot> of topology '<t>' exists from
       nodes=<n>, and this machine fits <m>``
   * - More pipes than the topology builds
     - ``<machine>: topology '<t>' builds at most <n> graphics pipe, and
       <slot> already holds one``
   * - Topology with no graphics record
     - ``<machine>: topology '<t>' has no graphics slots, so it takes no
       <device>``

The machine property ``x-uncatalogued-graphics=on`` skips all of this:
the machine fits no default pipe, accepts any ``-vga`` and checks no
graphics ``-device``. It is for register-contract qtests that use a
machine to carry a board in a slot or combination no SGI system had, such
as the InfiniteReality pipe the KONA qtests fit to an Origin 2000. At
most one pipe is built at once on every topology.
``tests/qtest/sgi-graphics-pipes-test.c`` checks the catalogue
refusals.

.. _sgi-sn-firmware-supply:

Firmware supply
===============

``-bios`` accepts a raw PROM image or SGI's own distribution container,
decoded as :doc:`/system/mips/sgi/prom-image` describes. The machine uses
the decoded payload either way. Each node's boot flash is also a
writable store, bound by ``node<N>-flash`` (:ref:`sgi-sn-storage`). A
node whose flash is bound this way keeps that drive's own content and is
skipped when ``-bios`` is copied in. At least one of ``-bios`` or a bound
flash per node is required. The one exception is a qtest machine that
builds no flash-backed node.

The generations differ when no image is given. An SN0 node with no image
starts from a template the machine patches with the product's values
(:ref:`sgi-ip27-firmware`, :ref:`sgi-prom-image-ip27-values`). An SN1
node has no template: every node needs a real image or a bound flash
(:ref:`sgi-ip35-firmware`, :ref:`sgi-prom-image-ip35-views`).

.. _sgi-sn-storage:

Writable storage
================

Every writable store the catalogue's ``storage`` list names for a
configuration is bound through the machine property of the same name,
which takes a ``-blockdev`` node name or a ``-drive`` id. The property
stands for the backing device's own ``drive`` property, as ``pflash0``
does on the PC machines, but is applied when the builder creates the
device, since ``topology=`` and ``nodes=`` decide which devices exist::

  -blockdev driver=raw,node-name=node0-flash,file.driver=file,file.filename=node0-flash.raw \
  -blockdev driver=raw,node-name=node1-flash,file.driver=file,file.filename=node1-flash.raw \
  -M origin2000,topology=origin2000-deskside,nodes=2,node0-flash=node0-flash,node1-flash=node1-flash

Before building anything the machine refuses a property the selected
configuration lists no item for, a name that is no block node or drive,
and a backing whose size is not the item's size.

.. list-table::
   :header-rows: 1

   * - Store
     - Machines
     - Contents
   * - ``node<N>-flash``
     - all Scalable Node machines
     - Node N's boot flash. A Bedrock node's 16 MiB store holds both of
       its 8 MiB flash positions, lower first.
   * - ``io<N>-flash``
     - IP27 with a BaseIO card or GIGAchannel cabinet
     - The BASEIO PROM flash, N counting those boards in the topology's
       order.
   * - ``nvram<N>``, ``nvram<N>-clock``
     - all Scalable Node machines
     - The Timekeeper NVRAM of each base I/O board that carries one, and
       the clock record its model keeps beside it. A clock record needs
       its NVRAM bound too. An NVRAM bound alone restores its calendar
       with no time added. The part is M48T35-compatible on IP27 and on
       the Origin 300 and Fuel, and DS1744-compatible on the IO9 of the
       IP53 machines (:doc:`hardware/timekeepers`). The IP53 machines
       require both stores and accept neither deprecated form below.
   * - ``module<N>-nvram``
     - IP27
     - The 2048-byte configuration NVRAM of a module's system controller
       (:ref:`sgi-elsc-nvram`), N counting the modules that have one.
       Each Origin 200 module's embedded ELSC has one, and so does each
       Origin 2000 or Onyx2 module's MSC. A bound file's contents are
       used as they are. Only a store with no file starts from the
       controller's first power-up state.
   * - ``<module>-flash``
     - ``origin300`` PCI expansion topologies
     - The 16 MiB configuration storage of expansion module
       ``<module>`` (:ref:`sgi-pci-expansion-storage`). A new machine's
       starts blank.

The older forms still work but are deprecated, and giving both forms for
one store is refused. They are ``-drive if=pflash,unit=N`` for node N's
flash. For a BASEIO PROM flash, ``-drive if=none,id=sgi-baseio-flash<B>``
(B the BaseIO card's I/O board index) or
``id=sgi-gigachannel-flash<C>`` (C the cabinet). For a Timekeeper,
``-drive if=mtd,index=I`` or ``-drive if=none,id=sgi-nvram<I>`` with its
``sgi-nvram<I>-clock`` partner, I the I/O board index on IP27 and the
node on the Origin 300 and Fuel. For an expansion module's configuration
storage, ``expansion-storage=<module>=<drive>``
(:ref:`sgi-pci-expansion-storage`).

.. _sgi-sn-serial-order:

Serial line order
=================

IP27 machines assign serial backends node-major, in one fixed six-line
block per node. Within a node's block, :term:`IOC3` port A comes first (it is
also the firmware console when this is the console node), then port B,
then the system controller's AUX port, then the bring-up UART, then the
two write-only lines that record what each IOC3 port transmitted. The
topology's ``console`` field names which node's block comes first. Every
other node follows in its original index order.
``sgi_machine_compute_serial_line()`` implements this rule. The bring-up
UART has no shipped connector and exists only when the machine's
``junk-uart`` property is set (off by default). The two record lines
have no connector either. They exist to give a console consumer that
stops reading somewhere to send bytes without blocking the port they
mirror.

IP35 machines assign lines in a fixed block of five per Bedrock node, in
node order: the node's L1 UART first, which is the system console and
which the L1 forwards to and from the processor's serial channel once
boot hands off (:ref:`sgi-l1-console`), then
four lines of the node's I/O (``sgi_sn1_serial_line()`` in
``include/hw/mips/sgi/sn-machine.h``). On the Origin 300 and Fuel those
are IOC3 port A, port B and the two record lines. On the IP53 machines
they are the four :term:`IOC4` serial ports (:ref:`sgi-ip35-firmware`). A
one-chassis machine uses lines 0 to 4. ``junk-uart`` has no effect on
IP35.

``debug-leds`` (shows the firmware's decoded boot-progress indicator,
on by default) and ``elsc-dip-switches`` (the eight physical DIP
switches a node's embedded system controller reports to firmware) apply
to IP27 machines. ``debug-leds`` stays inert on IP35, which has no
counterpart of the Hub's trace event. The identification records an
L1 serves come from ``chassis-eeprom``, ``board-eeprom`` and
``spd-eeprom``, each a list of files holding one record per file
(:doc:`hardware/l1`). Expansion module controllers take their own
inputs (:ref:`sgi-pci-expansion-controller-inputs`).

.. _sgi-sn-board-devices:

Board devices
=============

Base I/O is one device, ``sgi-baseio`` (``hw/mips/sgi/baseio.c``), with
a preset per shipped board design rather than a separate class each:
``ip29``, ``io6``, ``io6g``, ``menet``, ``mscsi``, ``io8``, ``io7`` and
``fuel``. ``io7`` is the Origin 3000 I-brick's IOC3, Lucent OHCI
controller and TI TSB12LV22 IEEE 1394 link (:ref:`sgi-sn-family-ibrick`).
Any other preset name is refused. :doc:`hardware/baseio` describes the
presets. The Origin 200's IP29 onboard I/O is soldered to the System
Board rather than plugged into a slot, so the Origin 200 node builds its
``ip29`` instance directly on its own XIO port, with no board wrapper.
The Origin 300 node's ``io8`` instance is placed the same way, directly
on its XBridge head, not through a board class.

``sgi-xio-board`` (``hw/mips/sgi/xio-board.c``) is the abstract base for
a removable XIO board: the unit that plugs into one of a module's XIO
slots. Its concrete classes are ``sgi-xio-baseio`` (a Bridge-based card
built from a ``sgi-baseio`` preset: ``io6``, ``io6g``, ``menet`` or
``mscsi``), ``sgi-pci-expansion`` (a plain Bridge with an empty PCI
bus), ``sgi-pci-carrier`` (the Origin 2000 PCI Carrier Assembly, three
PCI slots and its own board record), ``sgi-pci-shoehorn`` (a Bridge
carrying one PCI card), ``sgi-gigachannel`` (the GIGAchannel cabinet's
own Bridge), ``sgi-kona`` (the InfiniteReality pipe), ``sgi-mgras`` (the
IMPACT graphics board, whose :term:`widget` answers XIO directly with no Bridge)
and ``sgi-odyssey`` (the VPro board). Realizing a board builds its own
widget-facing device, then re-parents the board under
``/machine/module<N>`` so ``info qom-tree`` shows it where the hardware
puts it.

A topology's catalogue places its own boards: the BaseIO in every Origin
2000 and Onyx2 and the GIGAchannel's cabinet Bridge. The ``onyx2``
topologies also record the KONA pipe's slot
(:ref:`sgi-onyx2-graphics-module`), but that slot starts empty and the
pipe is fitted as a graphics pipe (:ref:`sgi-sn-graphics-pipes`). Every
other XIO slot a topology names is empty, and a board is fitted
there at launch by name::

  -M origin2000 -device sgi-mgras,slot=io3
  -M origin2000 -device sgi-xio-baseio,preset=menet,slot=io5
  -M origin200,topology=origin200-gigachannel \
      -device sgi-xio-baseio,preset=mscsi,slot=io2

``slot=`` takes the slot's label (``io2`` to ``io12`` on an Origin 2000
module, ``io2`` to ``io6`` in a GIGAchannel cabinet), and the machine
supplies the module, the Crossbow widget and the fabric the board sends
through, the ``sgi-xio-slots`` interface in ``xio-board.h``. It refuses a
slot the topology does not have, one its own board fills, one another
board already holds, and a module the slot is not in. A BaseIO preset
fitted by name takes only ``io1``. ``slot=`` also still takes a Crossbow
widget number with an explicit ``module=`` and ``fabric=``, which the
builder and qtest fixtures use. When that fabric is a slot's own, the
board must name that slot's module and widget. A board fitted at launch
whose one-wire serials are left at the class's shared placeholder takes
serials offset by the slot's position among the machine's XIO slots, so
no two boards fitted this way read the same NIC. The widget alone would
not do, since a module's two Crossbows number their widgets alike (IO5
and IO7 are both widget 14). A board's id must be unique among the
machine's devices.

Every XIO board is named by its slot label. A catalogue board's QOM name
and qdev id, and a launch-fitted board's id unless it is given one, is
``io1``, ``io5`` and so on (``m<N>-io5`` where two modules share the
label; the GIGAchannel's cabinet Bridge is ``io7``, a board id that has
nothing to do with the ``io7`` preset above). Its PCI bus follows
QEMU's child-bus idiom, ``<id>.0``: a card goes into the carrier in IO2
with ``bus=io2.0``, and ``screendump device=io3`` names an SI board in
IO3.

A Crossbow needs every endpoint present at startup attached before its
own realize builds the routes between them, so the builder links the
topology's own boards before realizing each Crossbow. A board fitted at
launch by slot name realizes after that. Its realize claims the empty
slot from the machine, takes the link to that slot's Crossbow port, and
as its last step attaches its widget to the already realized Crossbow,
which brings the link alive. ``origin200`` and ``origin300`` have no
Crossbow on the node. The ``origin200`` node places its one Base I/O
instance directly on its XIO port, and the ``origin300`` node on its
XBridge head, as above.

An Origin 3000 C-brick has no XBridge of its own. How the builder joins
it to an I/O brick's XBridge, which XIO boards it builds at a cabled
port, and the memory-only node and module-owned L1 a CX-brick needs are
on :ref:`sgi-ip35-brick-composition`. The SN0 deskside's per-node slot
codes are on :ref:`sgi-onyx2-node-slot-codes`.

An XIO port is a QOM interface every chip at one end of an XIO link
implements: a Hub's or Bedrock's I/O interface, each Crossbow or XBridge
crossbar port, a Bridge, and a board's own widget
(``include/hw/mips/sgi-xio-port.h``). One link type
(``hw/mips/sgi/xio-link.c``) joins two ports and does nothing but hand
each end the other's faces: the processor face for programmed I/O and
the packet face for widget-originated requests, the latter presented at
every widget number when the peer is a leaf. What the far end does with
the destination widget is the peer's own business: a Crossbow routes by
it, a Bridge or a Hub accepts any. A Bridge sends into its link's
widget-addressed fabric, the destination widget in bits 51:48 and the
48-bit XIO address below.

Migration is supported only between identical builds.

Module power and reset
======================

Each module's system controller switches the module's power and reset.
On SN0 the machine connects each controller's outputs to the module's
parts. :ref:`sgi-ip27-power` describes the domains and what the model
does, and :ref:`sgi-ip27-power-implementation` how the model sequences
a module start, what an off module's peers see, and how power state
migrates. On SN1 the L1 owns power and reset (:doc:`hardware/l1`).

.. _sgi-sn-launch-metadata:

Launch metadata
===============

Each product's catalogue offering carries hints for launch tools: the
console port, the firmware dialogue a launcher drives, the disk bus,
the netboot adapter and the guests and install media it supports. The
machine does not act on them. They are emulator and harness choices, not
hardware facts. Where a hardware fact backs a choice, the device and
product sections of these pages state it. Each offering also lists every
writable store the machine has and the firmware images that initial
content is built from, described under :ref:`sgi-sn-storage`.

.. list-table::
   :header-rows: 1

   * - Products
     - Console
     - PROM dialogue
     - Disk bus
     - Netboot adapter
     - Default ``-cpu``
   * - ``origin200``, ``origin2000``, ``onyx2``
     - ``ioc3-serial-a``
     - ``ip27-menu``
     - ``scsi.0``
     - ``ioc3-ethernet``
     - R10000, or R12000 with ``processor=r12000-270mhz-4mb`` on
       ``origin200``
   * - ``origin300`` and its variants, ``fuel``
     - ``l1-uart``
     - ``ip35-l1-prompt``
     - ``scsi.0``
     - ``ioc3-ethernet``
     - R14000
   * - ``octane``, ``octane2``
     - ``ioc3-serial-a``
     - ``none``
     - ``scsi``
     - ``ioc3-ethernet``
     - R10000, R12000
   * - ``origin350``, ``onyx350``, ``tezro``, ``onyx4-host``
     - ``l1-uart``
     - ``none``
     - ``scsi.0``
     - ``bcm5701-ethernet`` (``onyx350``, ``tezro``), ``unsupported``
       (``origin350``, ``onyx4-host``)
     - R16000

.. _sgi-sn-launch-console:

Console
-------

``ioc3-serial-a`` means the first IOC3 UART on the console base I/O
board. ``l1-uart`` means the L1 system controller's serial line, which
the IP35 PROM's console sits behind. The choice names the port the stock
PROM's own monitor uses on the model. It does not claim which physical
connector a user must use. :ref:`sgi-sn-serial-order` gives the order
of the lines.

.. _sgi-sn-launch-dialogue:

PROM dialogue
-------------

``ip27-menu`` is the IP27 PROM's maintenance menu and command monitor.
``ip35-l1-prompt`` is the L1 prompt in front of the IP35 PROM, which a
launcher passes through before the PROM's own menu. ``none`` means no
launcher-driven dialogue is defined. These name what a launcher scripts,
not a hardware behaviour.

.. _sgi-sn-launch-disk-bus:

Disk bus and other hints
------------------------

The disk bus is the QEMU SCSI bus id a launcher attaches the boot disk
to. ``scsi.0`` is the first onboard SCSI controller's bus on every
Scalable Node machine, and ``scsi`` is the Octane's. On the Origin 3200
it is the I-brick's Fibre Channel loop, which ``scsi-adapters`` lists as
type ``fc-loop``. The netboot adapter names the device a launcher uses
for the stock PROM's netboot. ``unsupported`` means the offering names
none. The Origin 200
offering also asks launchers without a display to pass
``debug-leds=off``. The Onyx2 offerings reuse the
Origin 2000 module's dialogue, console and disk bus, since they are
built on the same IP27 node boards and stock PROM.

The install media list ``netboot`` where the harness installs IRIX by
booting the miniroot over the netboot adapter from an install server,
the PROM's "Install System Software" from a remote directory. The IRIX
6.5.30 ``prom(1M)`` page documents that menu choice and its network
source for SGI PROMs generally. Every offering of the IP27 products, the
Octane, the Octane2, the Origin 300 and the Fuel lists both
``disk-image`` and ``netboot``. The ``onyx350`` and ``tezro`` offerings
list ``netboot`` alone, and the ``origin350`` and ``onyx4-host``
offerings list none. ``disk-image`` means a launcher can boot an
installed disk image.

The Origin 300 offering lists identity inputs a launcher may supply: raw
chassis, board and SPD EEPROM records, a brick serial and L1 debug
switches. These are model inputs that replace synthetic defaults. The
machine's own identity values are marked synthetic and are not taken
from any real machine.

.. _sgi-sn-devices:

Devices
=======

The node ASICs, bridges, controllers and I/O chips this family builds
from have their own pages: :doc:`/system/mips/sgi/hardware/hub`,
:doc:`/system/mips/sgi/hardware/bedrock`,
:doc:`/system/mips/sgi/hardware/bridge`,
:doc:`/system/mips/sgi/hardware/xbow`,
:doc:`/system/mips/sgi/hardware/xbridge`,
:doc:`/system/mips/sgi/hardware/pic`,
:doc:`/system/mips/sgi/hardware/elsc`,
:doc:`/system/mips/sgi/hardware/l1`,
:doc:`/system/mips/sgi/hardware/router`,
:doc:`/system/mips/sgi/hardware/sn1-router`,
:doc:`/system/mips/sgi/hardware/ioc3`,
:doc:`/system/mips/sgi/hardware/ioc4`,
:doc:`/system/mips/sgi/hardware/baseio`,
:doc:`/system/mips/sgi/hardware/qlogic-isp`,
:doc:`/system/mips/sgi/hardware/gigachannel` and
:doc:`/system/mips/sgi/hardware/bricks`.
