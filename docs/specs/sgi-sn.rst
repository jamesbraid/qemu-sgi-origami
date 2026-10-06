================================
SGI Scalable Node family machine
================================

SGI shipped two generations of "Scalable Node" system under this
architecture: SN0, built from the Hub ASIC (board IP27, the Origin 200
and Origin 2000), and SN1, built from the Bedrock ASIC (board IP35,
the Origin 300). Both generations address memory and registers the
same way -- a flat physical address space striped by node, with two
attribute bits above the node number selecting the cached, uncached or
special-space window -- and both boot from a stock IP27 or IP35 PROM
with the same container format, entry points and configuration block.
What differs between them is typed: the node ASIC (Hub carries two
CPUs on one processor interface; Bedrock carries four CPUs on two),
the bridge chip behind the node's XIO port (Bridge on SN0,
XBridge on SN1), and the system controller (an entry-level controller
inside the Hub itself, or a module-wide MSC, on SN0; an L1 over a UART
on SN1).

QEMU models the abstract family as ``sgi-sn`` (``TYPE_SGI_SN_MACHINE``,
``hw/mips/sgi/sn-machine.c``), the base every product's machine class
parents. It owns the family's own machine properties and a builder,
``sgi_machine_init()``, that constructs a machine from a compiled
catalogue: a list of ``SGIModelTopology`` entries
(``qapi/sgi-machines.json``, embedded into the emulator and parsed at first
use by ``hw/mips/sgi/sgi-machines.c``) naming every node, module,
fabric link, I/O board and controller a product's configuration ships.
The same compiled document backs the ``query-sgi-machines`` QMP command
(``SGIMachines`` in ``qapi/sgi-machines.json``): the emulator returns its own
embedded copy verbatim, so a package build can read the catalogue back from
the binary it ships instead of carrying a second copy out of band. The
catalogue carries two sections; only ``topologies`` reaches this builder.
``offerings``, one entry per accepted population a launcher may select, is
for packaging and launch tooling outside the emulator, never
read here. Each offering names the ``-M`` value that selects it
(``machine-options``), every writable persistent store the machine has
(``storage``: per-node boot flash ``node<N>-flash``, BASEIO or GIGAchannel
PROM flash ``io<N>-flash``, Timekeeper NVRAM ``nvram<N>`` and its clock
record ``nvram<N>-clock``, each with its exact size and what a new
machine's copy starts from) and the firmware images that initial content
is built from (``init-inputs``).
Four products stand on this base today:
``origin200``, ``origin2000`` and ``onyx2`` (SN0), and ``origin300``
(SN1). ``onyx2`` is Origin 2000 silicon with graphics: the same IP27
node boards, memory and stock PROM, told apart only by the hardware its
topologies fit. ``origin300``'s own product class (``hw/mips/sgi/sn1.c``)
parents ``sgi-sn`` like the others and names the same ``topology=``
contract and compiled descriptor this page describes; only its node
device (``sgi-sn1-node``, ``hw/mips/sgi/sn1-node.c``) is
product-specific, the way each SN0 node's own device is.

.. _sgi-sn-product-family:

Product family
==============

This section is the hardware contract the SN model layers cite for
the products, bricks and cabling they describe. Each fact is rated
*established* (a cited specification or manual, or an identified real
machine), *inferred* (follows from established facts by the stated
step), *approximation* (an emulator value standing in for unknown
hardware) or *unknown*. SGI manuals are cited by document number and
printed page; forum captures by site and thread number. The Origin
3000 bricks, the NUMAlink module and the Origin 350 are reference
models in the catalogue's sources: the emulator does not build those
machines.

.. _sgi-sn-family-table:

Family summary
--------------

.. list-table::
   :header-rows: 1

   * - Product
     - Node board and ASIC
     - CPUs and memory per node
     - Fabric
     - I/O attachment
   * - Origin 2000, Onyx2
     - IP27 (or IP31), Hub
     - 2 processors; 16 main and 8 directory DIMM slots, 8 banks of 64
       to 512 MB, 64 MB to 4 GB, bank 0 required
     - CrayLink: two Routers per module, two modules per rack
     - two Crossbows per midplane; BaseIO in slot IO1
   * - Origin 3000
     - IP35 C-brick, Bedrock
     - 2 or 4 on one or two PIMMs, R12000 360 MHz or R12000A
       400 MHz; 8 DIMM slots, banks 0 to 7, 512 MB to 8 GB
     - NUMAlink 3 through 8-port R-bricks
     - no crossbar in the C-brick; XBridge crossbar in the I-, P- and
       X-bricks
   * - Origin 350
     - IP53, Bedrock
     - 2 or 4 R16000; 8 DIMM slots as four pairs, 512 MB or 1 GB DIMMs,
       1 to 8 GB
     - NUMAlink 3, one external port; a NUMAlink module for 4 to 32
       processors
     - PIC, two PCI-X heads on one widget; factory IO9 card

Sources and ratings, by row:

- Origin 2000 (established): *Origin and Onyx2 Theory of Operations
  Manual*, 007-3439-002, printed pages 35-36 (memory) and 59 (Xpress
  links); *Origin200/2000 IP27 PROM* technical reference, 108-0170-001,
  printed pages 1-1 to 1-4 and 6-3 to 6-6 (Router ports and module
  slots).
- Origin 3000 (established): *Origin 3000 System Architecture*,
  108-0240-002, printed pages 2-4 (processors and PIMMs) and 2-26 to
  2-29 (XBridge ports and the I-brick).
- Origin 350 (established): *SGI Origin 350 Server System User's
  Guide*, 007-4566-001, printed pages 69 (configurations), 114 and 116
  (node board and DIMM pairs).

.. _sgi-sn-family-origin2000-xpress:

Origin 2000 two-module racks and Xpress links
---------------------------------------------

A rack holds two modules (established, 007-3439-002). Within and
between modules, Link 1 runs vertically between the upper and lower
Routers of the same letter, and Link 2 is the Xpress link, which must
cross. Xpress links are only used in systems of 16 or 32 processors
(established, 007-3439-002 printed page 59; the cabling is drawn in
SGI field-service manual 108-0174-001, printed pages 6-35 to 6-48). The ``origin2000-two-module-xpress``
layer records that wiring as a reference configuration the emulator
does not build; its fitted memory is a model choice.

.. _sgi-sn-family-origin3000-cbrick:

Origin 3000 C-brick
-------------------

Each compute node holds one or two PIMMs of two processors each,
R12000 at 360 MHz or R12000A at 400 MHz, with 4 or 8 MB of secondary
cache (established, 108-0240-002 printed page 2-4). The C-brick has no
crossbar of its own: its Bedrock reaches I/O through a Crosstown2 port
into an I-, P- or X-brick's XBridge, whose crossbar section does the
switching (established, 108-0240-002 printed page 2-26). The layer's
C-brick buses -- one SysAD per processor interface and one XIO port
reaching I-brick host ports A and B -- follow from this.

.. _sgi-sn-family-ibrick:

I-brick host ports and base I/O
-------------------------------

The XBridge has eight ports: Crosstown2 ports A and B, Crosstalk ports
8, 9, C and D, and PCI ports E and F (established, 108-0240-002 printed
page 2-26). In the I-brick, compute nodes attach at port A (widget 10)
and port B (widget 11). Port E drives bus 2, two 33 or 66 MHz slots;
port F drives bus 1, three 33 MHz slots, and the IO7 devices
(established, printed pages 2-28 and 2-29, Figure 2-23).

Three owners' IRIX inventories of an Onyx 3000, an Origin 3200 and an
Origin 3400 (Nekonomicon forum 14, threads 16719632, 16723604 and
16720581) print the same base I/O bus at
``/hw/module/<c>/Ibrick/xtalk/15`` (established, three identified
machines):

.. list-table::
   :header-rows: 1

   * - PCI slot
     - Device
     - PCI identity
   * - 1
     - QLogic 2200 Fibre Channel
     - ``1077:2200``
   * - 4
     - IOC3
     - ``10a9:0003``
   * - 5
     - Lucent USB OHCI
     - ``11c1:5802``
   * - 6
     - TI TSB12LV22 IEEE 1394 OHCI
     - ``104c:8009``

They report I-brick board ``030-1557-007`` at revisions A, D and F,
and a second Fibre Channel function at ``xtalk/14/pci/1``. Widget 15
is port F and widget 14 port E, matching the block diagram (inferred).
Unknown: the I-brick XBridge revision, and which host port a booting
I-brick uses; a verbose Origin 3000 PROM boot would settle both, and
none is published.

.. _sgi-sn-family-rbrick:

R-brick and NUMAlink module ports
---------------------------------

The R-brick router has eight ports, 1 to 8. C-bricks must use ports
2 to 5; router-to-router links normally use 1, 6, 7 and 8; Origin 3400
R-bricks disable ports 7 and 8 (established, *SGI Origin 3000 Series
Technical Configuration Owner's Guide*, 007-4311-002, printed pages
36-37; 108-0266-001 printed pages 40-41). The router ASIC itself is not
named in any SGI document found (unknown).

The Origin 300 NUMAlink module uses the same board. Ports 1, 6, 7 and 8
take base modules only; ports 2 to 5 take base or PCI expansion
modules and carry USB to the L2 controller (established, *SGI Origin
300 System with NUMAlink Module User's Guide*, 007-4498-002, printed
pages 4, 10-11, 16 and 93-98). In the tall rack (Figure 4-2) the
cabling runs:

.. list-table::
   :header-rows: 1

   * - Base module position
     - U28
     - U26
     - U24
     - U22
     - U18
     - U16
     - U14
     - U12
   * - NUMAlink module port
     - 3
     - 2
     - 1
     - 8
     - 7
     - 6
     - 5
     - 4

This mapping is established from the manual's figure, and a live
six-chassis rack whose modules at U12, U14, U18, U22 and U24 reported
active ports 1, 4, 5, 7 and 8 (Nekonomicon thread 16727514) agrees
with it. The layer's eight router-port buses carry these port numbers.

.. _sgi-sn-family-origin350:

Origin 350 compute module
-------------------------

.. list-table::
   :header-rows: 1

   * - Fact
     - Rating and basis
   * - Two or four R16000, 4 MB secondary cache each
     - established: 007-4566-001 printed pages 69 and 116
   * - Eight DIMM slots in four pairs, each pair feeding a pair of banks;
       the two DIMMs of a pair match; 1 to 8 GB
     - established: printed pages 114 and 116
   * - One Bedrock, revision 2 at 200 MHz
     - Bedrock established (printed page 69); revision 2 observed in one
       Origin 350 inventory (Nekonomicon thread 16729141)
   * - NUMAlink 3, one external LINK connector, empty in a one-module
       system
     - established: printed page 69 (with or without a NUMAlink module)
   * - PIC: one widget with two PCI-X heads, the second head's registers
       0x800000 above the first; PIC at widget 15
     - two heads and the offset established (OpenBSD/sgi
       ``sys/arch/sgi/xbow/xbridgereg.h`` lines 24-28 and ``xbowdevs``,
       part ``0xd102``, second head ``0xd112``, manufacturer ``0x024``,
       at ``2df91d80c104636deeef8124252ddd5951b55fbf``); widget 15
       observed in one Origin 350 inventory
   * - IO9 in the lowermost slot, on bus 1: IOC4 ``10a9:100a`` at
       configuration device 1, ISP12160 ``1077:1216`` revision 6 at 3,
       BCM5701 ``14e4:1645`` revision ``0x15`` at 4
     - IO9 placement established (printed page 69); device numbers
       observed in every captured IP53 inventory. PIC's configuration
       devices are its internal slots plus one (OpenBSD ``xbridge.c``),
       so IRIX's PCI ids are OpenBSD's devices plus one (inferred).
   * - L1 controller with a display; L2 only with a NUMAlink module
     - established: 007-4566-001, printed pages 68-75

Unknown: the Origin 350's widget identity words for Bedrock and PIC,
and the widget at which Bedrock enters the crossbar on an Origin 350
compute module. One IP53 graphics-blade PROM trace places Bedrock at
widget ``0xb`` (graphica, "SGI O350 Chimera notes",
https://just.graphica.com.au/tips/sgi-o350-chimera-notes/, retained
copy SHA-256
``a839d88fdd9703e8d5ed03c290282d144c9339302cddbfc7d1912a0e04e77f44``,
lines 949-952), which may not transfer to a compute module.

.. _sgi-sn-family-bedrock:

Bedrock revisions
-----------------

Every published IRIX inventory of an Origin 300, Origin 3000, Origin
350, Fuel or Onyx 4 reports the Bedrock (printed as ``HUB``) at
revision 2, 200 MHz (observed, many machines). No capture shows another
revision. The raw register value behind IRIX's revision field is
unknown.

.. _sgi-sn-topology:

Products and topology
=====================

A product's ``topology=`` machine property selects which configuration
of it to build; ``-smp``, ``-m`` and ``-cpu`` then select and validate
a population within that wiring. The compiled descriptor carries more
topologies than this table; these are each product's default and the
Onyx2 deskside:

.. list-table::
   :header-rows: 1

   * - Topology
     - Product
     - Nodes
     - Accepted ``-smp``
     - Accepted ``-m`` per node
   * - ``origin200`` (default)
     - ``origin200``
     - 1
     - 1 or 2
     - 64, 128, 256 or 512 MiB
   * - ``origin200-dual``
     - ``origin200``
     - 2, direct-linked
     - 2+2 (fixed)
     - 64, 128, 256 or 512 MiB, same on both nodes
   * - ``origin2000-rack`` (default)
     - ``origin2000``
     - 1 to 4; two Crossbows, and two Routers from two nodes on
     - 1 or 2 per node
     - 64 MiB (fixed)
   * - ``origin2000-deskside``
     - ``origin2000``
     - 1 to 4; two Crossbows, a Null Router for two nodes, two Routers
       for three or four
     - 1 or 2 per node
     - 64 or 256 MiB
   * - ``onyx2-rack`` (default)
     - ``onyx2``
     - 1 to 4, wired as ``origin2000-rack``
     - 1 or 2 per node
     - 64 MiB (fixed)
   * - ``onyx2-deskside``
     - ``onyx2``
     - 1 or 2, a Null Router for two, one Crossbow
     - 1 or 2 per node
     - 64 MiB (fixed)
   * - ``origin300`` (default)
     - ``origin300``
     - 1
     - 2 or 4
     - 512 MiB (fixed)

``-M origin200,topology=origin200-dual`` selects the second row. Each
topology describes one chassis, and its catalogue record carries a
node-population row for each count of node boards it accepts; the
``nodes`` machine property picks the row, and defaults to the largest,
so ``-M origin2000`` still fits four. ``-M origin2000,nodes=2`` fits node
boards in slots ``n1`` and ``n2`` only, in the order SGI fills them: an
absent node board has no Hub, processors, boot flash or place in the
module controller's arbitration, and the Crossbow port it would use
reads as an empty one. The row also decides which Routers and cables
exist and which XIO slots a board fitted at launch may use; a slot the
row cannot use is refused with the node board it needs. A count no row
accepts is refused with the counts that exist, and a row whose evidence
does not settle its wiring is refused rather than built. ``-smp`` and
``-m`` keep the product's defaults, which fit its most node boards: the
machine cannot tell a default from an explicit value, so a launch with
fewer node boards names both, as ``-M origin2000,nodes=1 -smp 2 -m 64M``
does. Naming
a topology that is not one of a product's own (or any other unknown
name, including ``topology=help``) makes the machine report every
topology that product accepts and exit, since QEMU has no generic help
path for a property value. A topology with more than one node splits
``-smp`` evenly across its nodes when no explicit core/socket count is
given, the way a bare ``-smp 8`` on ``origin2000`` becomes four sockets
of two cores each; naming sockets and cores explicitly must still match
one of the topology's own accepted populations. ``-m`` splits evenly
across nodes and each node's share must be one of its own accepted
sizes -- ``origin200-dual`` accepts the same four per-node sizes
``origin200`` does, so its total ranges from 128 MiB to 1024 MiB, while
``origin2000-rack``'s nodes ship a fixed 64 MiB each and accept no other
size. ``nodes`` is part of the machine configuration: a snapshot restores
only into a machine with the same value. ``origin300`` accepts only R14000 for ``-cpu``; the
other two accept only R10000, the default and the only member of
their own ``valid_cpu_types``.

The abstract base refuses ``-numa`` outright: ``topology=`` already
selects the wiring, and ``-smp``/``-m`` select the population within
it, so a NUMA node list would either repeat that or contradict it.
This refusal, and every other command-line rule described in this
section, applies to every product through the shared builder;
``origin300`` happens to ship only the one population above, so its
own class fixes ``min_cpus``, ``max_cpus`` and ``default_cpus`` to it
rather than accepting a range the way ``origin200`` does.

Every product across both generations refuses ``-kernel``: booting a
guest kernel directly, bypassing the stock PROM, is not a supported
path on any Scalable Node machine in this tree. A guest reaches its
kernel or standalone program through the PROM's own TFTP netboot or a
disk image, the same as real hardware.

.. _sgi-sn-firmware-supply:

Firmware supply
===============

``-bios`` accepts a raw PROM image or SGI's own distribution container,
decoded as :doc:`sgi-prom-image` describes; the machine uses the decoded
payload either way.

``origin200`` and ``origin2000`` take ``-bios`` to supply one PROM
image, copied into every node's boot flash identically; a node whose
flash is instead named explicitly with ``-drive if=pflash,unit=N``
keeps that drive's own content and is skipped when ``-bios`` is
copied in. At least one of ``-bios`` or a per-node pflash drive is
required (a qtest machine that builds no flash-backed node is the one
exception). Absent an image, the machine still builds a working reset
vector: a default template answers the flash's command interface with
a spin-in-place loop, a plausible configuration block and a
factory-fresh environment log, the shape a real chip ships pre-formatted
in rather than merely erased; the three fields a stock PROM's own
configuration block is expected to carry for this product (two flash
timings and its machine-type word) are patched into whichever block is
actually present -- the template's own or a loaded image's --
preserving every other field and the block's stored checksum. Those
three values come from the topology's ``ip27-firmware`` record in
``query-sgi-machines``: machine type 1 with 15 read and 4 write cycles
on Origin 200, and the stock image's own 0, 8 and 1 on Origin 2000 and
Onyx2, which therefore leave a stock block unchanged.

``origin300`` takes ``-bios`` with a stock PROM whose payload is exactly
the size its one node's flash answers for; there is no per-node
override; the image is also rejected outright if it exceeds the flash
part ``sn1_node_create_boot_flash()`` expects. Its boot flash can instead
be backed by a 16 MiB raw ``-drive if=pflash,unit=0`` image of the real
chip's own content, in which case the machine's execution and
maintenance views answer through that storage -- including any write
the guest's own PROM has just made -- rather than the immutable
template ``-bios`` alone produces. When every node's flash is backed this
way, or through its ``node<N>-flash`` property, ``-bios`` may be left
out: the machine boots from the flash, as a machine created by
``qemu-sgi-machine-init`` is started. Without that backing part, the
guest's own environment and self-test writes to its boot flash are
accepted and logged but never persist: a run that only supplies
``-bios`` reaches the L1's own banner and then the IP35 PROM's first
console line ("Starting PROM Boot process") promptly, then repeats
that same sequence indefinitely rather than reaching an interactive
prompt, because nothing carries its own state across the reset the
PROM performs early in that sequence.

.. _sgi-sn-storage:

Writable storage
================

Every writable store the catalogue's ``storage`` list names for a
configuration is bound through the machine property of the same name,
which takes a ``-blockdev`` node name or a ``-drive`` id. The property
stands for the backing device's own ``drive`` property, as ``pflash0``
does on the PC machines, but is applied when the builder creates the
device, since topology= and nodes= decide which devices exist::

  -blockdev driver=raw,node-name=node0-flash,file.driver=file,file.filename=node0-flash.raw \
  -blockdev driver=raw,node-name=node1-flash,file.driver=file,file.filename=node1-flash.raw \
  -M origin2000,topology=origin2000-deskside,nodes=2,node0-flash=node0-flash,node1-flash=node1-flash

Before building anything the machine refuses a property the selected
configuration lists no item for, a name that is no block node or drive,
and a backing whose size is not the item's size.

The machines bind these items today:

- ``node<N>-flash`` on ``origin200``, ``origin2000``, ``onyx2``,
  ``origin300`` and ``fuel``. A Bedrock node's 16 MiB store holds both
  of its 8 MiB flash positions, lower first.
- ``io<N>-flash``, the BASEIO PROM flash of an Origin 2000 or Onyx2
  BaseIO card or an Origin 200 GIGAchannel cabinet, N counting those
  boards in the topology's order.
- ``nvram<N>`` and ``nvram<N>-clock``, each base I/O board's m48t35
  Timekeeper NVRAM and the clock record its model keeps beside it, N
  counting the boards that carry one. A clock record needs its NVRAM
  bound too; an NVRAM bound alone restores its calendar with no time
  added, as before.

The older forms still work but are deprecated, and giving both forms for
one store is refused: ``-drive if=pflash,unit=N`` for node N's flash;
``-drive if=none,id=sgi-baseio-flash<B>`` (B the BaseIO card's I/O board
index) or ``id=sgi-gigachannel-flash<C>`` (C the cabinet) for a BASEIO
PROM flash; and ``-drive if=mtd,index=I`` or ``-drive
if=none,id=sgi-nvram<I>`` with its ``sgi-nvram<I>-clock`` partner for a
Timekeeper, I the I/O board index on SN0 and the node on SN1.

.. _sgi-sn-serial-order:

Serial line order
=================

``origin200`` and ``origin2000`` assign serial backends node-major, in
one fixed six-line block per node: within a node's block, IOC3 port A
(also the firmware console when this is the console node) comes
first, then port B, then the system controller's AUX port, then the
bring-up "junk" UART, then the two write-only lines that record what
each IOC3 port transmitted. The topology's ``console`` field names
which node's block comes first; every other node follows in its
original index order. This is the one rule
``sgi_machine_compute_serial_line()`` implements; no product carries
its own serial-line table. The bring-up UART has no shipped connector
and exists only when the machine's ``junk-uart`` property is set (off
by default); the two
record lines have no connector either and exist only to give a console
consumer that stops reading somewhere to send bytes without blocking
the port they mirror.

``origin300`` assigns its lines directly rather than through this
rule: line 0 is the L1's own UART (the system console -- the L1
forwards it to and from the CPU's own serial channel once the boot
sequence hands off), lines 1 and 2 are the IOC-8 assembly's IOC3 ports
A and B, and lines 3 and 4 are that IOC3's two record lines. It has no
junk-uart property.

The abstract base's other machine properties -- ``debug-leds`` (shows
the firmware's decoded boot-progress indicator, on by default) and
``elsc-dip-switches`` (the eight physical DIP switches a node's
embedded system controller reports to firmware) -- apply to
``origin200`` and ``origin2000`` only; ``origin300`` has neither, and
instead exposes ``chassis-eeprom``, ``board-eeprom`` and
``spd-eeprom``, each a list of files holding one identification record
per file for its L1 to serve.

Expansion L1 serial input
=========================

``controller-brick-serial`` is a machine string list for expansion L1
controllers. Each element is ``controller-index=serial``, using the
zero-based controller index in the selected topology, as
``controller-board-eeprom`` and ``controller-chardev`` do. For example::

  -M origin300,topology=origin300-pci-expansion-experiment,controller-brick-serial.0=1=EXP-A

This sets the expansion controller's existing ``brick-serial`` property
before realization. The serial must contain 1 to 32 bytes. Everything
after the first ``=`` is preserved, including further ``=`` characters.
The machine rejects malformed entries, indices outside the selected
topology, compute controller indices and duplicate controller indices.
Different controllers may have equal serial values.

Omitting an entry preserves that controller's existing serial setting,
including a value supplied with ``-global sgi-l1.brick-serial=SERIAL``.
Without either input, its serial remains absent. Compute L1 controllers
continue to use the global property. An expansion entry overrides the
global value only for its selected controller.

Expansion processor-absence experiment
======================================

The existing typed assembly selection
``controller_startup: bic-functional-experiment`` also selects bounded
processor-absence visibility. Both functional expansion topologies use it.
Assemblies without that selection retain their PI access refusal. The physical
reference composition has no runtime offering.

Only PI0/PI1 CPU-present A/B words, at offsets ``0x40``, ``0x48``,
``0x200040`` and ``0x200048``, are accessible through the addressed BIC's
remote I/O register window. Transfers must originally be aligned eight-byte
reads or writes. Reads use the existing PI handlers with zero configured
processors. Writes retain their ignored hardware-set-register behavior.
Hub-special PI accesses and all other PI registers remain unsupported.

This is a functional approximation: physical BIC PI visibility and reset
values are unknown. It adds no processor, memory or local PI interrupt sink.
NASID resolution, direct-link reachability and completed main power still
control access. Migration requires matching immutable experiment selection.

Expansion configuration storage experiment
==========================================

The ``origin300-pci-expansion-configuration-storage-experiment`` topology
explicitly selects expansion main power and an independent Bedrock flash
attachment. It accepts one caller-owned image, bound by exact module ID::

  -M origin300,topology=origin300-pci-expansion-configuration-storage-experiment,expansion-storage.0=expansion-enclosure=cfg1
  -drive if=none,id=cfg1,file=CALLER.raw,format=raw,readonly=off

``expansion-storage`` is a machine string list. The bounded implementation
accepts one ``module-id=backend-id`` entry. Empty fields, extra separators,
duplicate entries, unselected owners and missing backends fail construction.
The selected module's controller identity is retained independently of its
NASID and compared during migration.

The backend must be unused, writable and a complete regular file opened
through a direct raw-to-file chain. The opened file must contain exactly
16 MiB. Explicit raw offset or size options, filters, snapshots and other
protocols are unsupported. The backend remains reserved by the expansion
assembly; separate raw slices supply two independently stateful 8 MiB CFI
positions. The attachment creates no image or default contents.

Only the expansion RBOOT window is connected. NASID 1 node-special address
``0x23a1e0000`` reads image offset ``0x9e0000``. Its CPU Hub-space reference
uses QEMU's 42-bit carrier ``0x1023a1e0000`` and reaches the same provider.
LBOOT and executable aliases are absent. Main power off refuses guest access while an already accepted
flash timer continues. Power transitions preserve command state; whole
machine reset returns array mode without erasing bytes. Isolated controller
reset preserves flash state. Migration requires the same storage selection,
geometry, module identity and controller identity.

This is an experimental functional attachment. It establishes no physical
configuration-storage chip population, factory contents, diagnostic identity
or successful stock guest discovery. Existing topologies without this
selection require no image and retain failed storage access. The composed
PCI buses are ``expansion0-u0-head-e.0``, ``expansion0-u0-head-f.0``,
``expansion0-u1-head-e.0``, ``expansion0-u1-head-f.0``,
``expansion0-u2-head-e.0`` and ``expansion0-u2-head-f.0``. They remain empty
unless a caller explicitly attaches a supported card. Physical slot and PCI
device-number correspondence is not established.

.. _sgi-sn-board-devices:

Board devices
=============

Base I/O -- the IOC3 plus one or two SCSI controllers, and on the IO-8 a USB
controller, behind an XIO-to-PCI bridge -- is one device, ``sgi-baseio``
(``hw/mips/sgi/baseio.c``), with a preset per shipped board design rather
than a separate class each: ``ip29`` (the Origin 200 System Board's onboard
I/O, on the node's own Bridge), ``baseio`` (the Origin 2000/Onyx2 standard
XIO board, behind a Crossbow), and ``io8`` (the Origin 300 IO-8 assembly's
PCI population -- a QLogic ISP12160, an IOC3 and a Lucent OHCI controller --
placed on the SN1 node's own XBridge head F). A preset name that matches
none of the three is refused outright. IP29's onboard I/O is soldered to the
System Board rather than plugged into a slot, so the Origin 200 node builds
its ``ip29`` instance directly on its own XIO port, with no board
wrapper; the Origin 300 node's ``io8`` instance is placed the same way,
directly on its XBridge head, not through a board class.

``sgi-xio-board`` (``hw/mips/sgi/xio-board.c``) is the abstract base for
a removable XIO board: the unit that plugs into one of a module's XIO
slots. Its concrete classes are ``sgi-xio-baseio`` (a Bridge-based card
built from a ``sgi-baseio`` preset: ``io6``, ``io6g``, ``menet`` or
``mscsi``), ``sgi-pci-expansion`` (a plain Bridge with an empty PCI bus),
``sgi-pci-carrier`` (the Origin 2000 PCI Carrier Assembly, three PCI slots
and its own board record), ``sgi-pci-shoehorn`` (a Bridge carrying one PCI
card), ``sgi-gigachannel`` (the GIGAchannel cabinet's own Bridge),
``sgi-kona`` (the InfiniteReality pipe) and ``sgi-mgras`` (the SI graphics
board, whose widget answers XIO directly with no Bridge). Realizing a
board builds its own widget-facing device, then re-parents the board under
``/machine/module<N>`` so ``info qom-tree`` shows it where the hardware puts
it.

A topology's catalogue places its own boards: the BaseIO in every Origin
2000 and Onyx2, the KONA pipe in the ``onyx2`` topologies (``onyx2-rack``
at IO3, Crossbow 0 widget 15, the slot its Xtown board occupies;
``onyx2-deskside`` at XIO4, which that chassis's Crossbow puts at widget 8)
and the GIGAchannel's cabinet Bridge. Every other XIO slot a topology names
is empty, and a board is fitted there at launch by name::

  -M origin2000 -device sgi-mgras,slot=io3
  -M origin2000 -device sgi-xio-baseio,preset=menet,slot=io5
  -M origin200,topology=origin200-gigachannel \
      -device sgi-xio-baseio,preset=mscsi,slot=io2

``slot=`` takes the slot's IRIX name (``io2`` to ``io12`` on an Origin 2000
module, ``io2`` to ``io6`` in a GIGAchannel cabinet), and the machine
supplies the module, the Crossbow widget and the fabric the board sends
through, the ``sgi-xio-slots`` interface in ``xio-board.h``. It refuses a
slot the topology does not have, one its own board fills, one another board
already holds, and a module the slot is not in. A BaseIO preset fitted by
name takes only ``io1``. ``slot=`` also still takes a Crossbow widget number
with an explicit ``module=`` and ``fabric=``, which the builder and qtest
fixtures use; when that fabric is a slot's own, the board must name that
slot's module and widget. A board fitted at launch whose one-wire serials
are left at the class's shared placeholder takes serials offset by the
slot's position among the machine's XIO slots, so no two boards fitted
this way read the same NIC; the widget alone would not do, since a
module's two Crossbows number their widgets alike (IO5 and IO7 are both
widget 14). A board's id must be unique among the machine's devices.

Every XIO board is named by its IRIX slot: a catalogue board's QOM name and
qdev id, and a launch-fitted board's id unless it is given one, is ``io1``,
``io5`` and so on (``m<N>-io5`` where two modules share the label; the
GIGAchannel's cabinet Bridge is ``io7``). Its PCI bus follows QEMU's
child-bus idiom, ``<id>.0``: a card goes into the carrier in IO2 with
``bus=io2.0``, and ``screendump device=io3`` names an SI board in IO3.

These renames and the stock topologies break saved state from earlier
builds: a default ``-M origin2000`` snapshot carries an ``sgi-mgras``
section the stock machine no longer has, and board ids and bus names
changed. Migration is supported only between identical builds, so no
compatibility shim is kept.

The Onyx2 deskside is a different midplane, not only a different
population. Its two node slots drive Hub slot-identity bits 3 and 2, where
the eight-processor module's four drive 7 to 4, and the stock PROM and IRIX
read those as the deskside's own slot codes and switch to its Crossbow
table: widget 8 ``io4``, 9 ``n2``, A ``n1``, F ``io1``. A catalogue node
record carries ``hub-slot-bits`` where its midplane differs from the
eight-processor module's; the builder hands the Hub the result as its
``slot-bits`` property. The BaseIO-G preset accordingly fits widget F as
well as widget 8.

A Crossbow needs every endpoint present at startup attached before its own
realize builds the routes between them, so the builder links the
topology's own boards before realizing each Crossbow. A board fitted at
launch by slot name realizes after that: its realize claims the empty slot
from the machine, takes the link to that slot's Crossbow port, and as its
last step attaches its widget to the already realized Crossbow, which
brings the link alive. ``origin200`` and ``origin300`` have no Crossbow on
the node -- each places its one Base I/O instance directly on its node's
XIO port, as above.

An XIO port is a QOM interface every chip at one end of an XIO link
implements -- a Hub's or Bedrock's I/O interface, each Crossbow or XBridge
crossbar port, a Bridge, and a board's own widget
(``include/hw/mips/sgi-xio-port.h``). One link type
(``hw/mips/sgi/xio-link.c``) joins two ports and does nothing but hand
each end the other's faces: the processor face for programmed I/O and the
packet face for widget-originated requests, the latter presented at every
widget number when the peer is a leaf. What the far end does with the
destination widget is the peer's own business: a Crossbow routes by it, a
Bridge or a Hub accepts any. A Bridge sends into its link's
widget-addressed fabric, the destination widget in bits 51:48 and the
48-bit XIO address below.

.. _sgi-sn-gigachannel:

GIGAchannel expansion cabinet
-----------------------------

The Origin 200 GIGAchannel is a separate I/O cabinet holding a Crossbow, a
Bridge with four PCI slots and five XIO slots. Fitting it moves the module's
own base I/O behind the cabinet's Crossbow. Two topologies build it:

``-M origin200,topology=origin200-gigachannel``
  One node, with the same ``-smp`` and ``-m`` choices as ``origin200``. The
  machine builds the cabinet's Crossbow with the Hub on widget 10 (link A),
  the IP29 base I/O Bridge on widget 9 and the cabinet's own I/O on widget
  8. The IP29 board reports the Crosstown extension board record beside its
  own.

``-M origin200,topology=origin200-gigachannel-loopback``
  The converted module with a loopback cable instead of a cabinet: the plain
  ``origin200`` wiring, base I/O Bridge directly on the Hub at widget 8, with
  the Crosstown extension board record added.

The cabinet's own I/O is ``sgi-gigachannel``
(``hw/mips/sgi/board-gigachannel.c``). The machine places it, so it is not
``-device``-creatable. It is a Bridge at widget revision 4 carrying the
cabinet interface board's record, ``XBOX_DUALXTOWN``, which the firmware
reports at ``io7``, and the board takes ``io7`` as its id. Its four PCI
slots are Bridge devices 0 to 3 on the bus ``io7.0``, so a PCI card joins
the cabinet with ``bus=io7.0,addr=N``.

The Bridge's external-flash port has two chip selects:

- The base chip select, widget offset ``0xc0_0000``, is where the IP27 PROM
  loads its BASEIO PROM from on a GIGAchannel. The option
  ``-drive if=none,id=sgi-gigachannel-flash0,file=IMAGE,format=raw`` fits a
  flash part there holding ``IMAGE``, a BaseIO flash image. The number counts
  cabinets. With no such drive nothing is fitted: the PROM takes a bus
  error in the window, prints ``Retrying using the IP27 PROM internal copy
  of BASEIO PROM`` and continues from its own reduced copy.
- The second chip select, widget offset ``0xe0_0000``, holds the cabinet
  system controller's 16-bit status word. Both the BASEIO PROM and IRIX read
  it, and neither writes it. The word is the
  ``sgi-gigachannel.system-controller-status`` property, default 0 (no
  redundant power supply, nothing to report), set with ``-global``. For
  example, ``-global sgi-gigachannel.system-controller-status=0x40`` reports
  a redundant supply with redundancy intact. The word reads the same at
  every halfword of the chip select, and writes are logged under
  ``-d unimp`` and dropped.

What is approximated:

- The flash part is the Origin 2000 BaseIO card's, an identity the PROM's
  loader accepts. The part SGI fitted in the cabinet is not known.
- The status word is fixed at start-up. IRIX polls it about once a minute
  and reacts to blower, over-temperature and power-supply events, but the
  model cannot change the word while the guest runs, so none of those
  events can occur. How the real word decodes across its chip select, what
  its upper byte holds and what a write does are not known.
- The board records ride Bridge one-wire lines chosen from where real
  inventories list them, with placeholder barcodes and serials.
- The Crossbow is the Origin 2000 module's Crossbow revision.
- Two-module systems, a cabinet on the slave module and a converted module
  with neither cabinet nor loopback are not built.

Module power and reset
======================

An Origin 2000 module controller carries out ``pwr d``, ``pwr c`` and
``rst`` through its module's power and reset outputs, which the machine
connects. Once every module's power is off the machine powers down; IRIX
6.5.30 sends ``pwr d 1`` to every module at ``shutdown -p``. A module
whose power returns before that power-down is carried out went through
``pwr c``, a cold restart, and the machine resets. A module reset resets
the whole machine. QEMU has no per-module power or reset, so a rack of
several modules approximates both: one module powering off alone is
logged and does not stop the others, and one module's reset or power
cycle resets them all. The module's NMI output is not connected; the
controller model does not implement ``nmi`` and answers it ``err cmd``.

.. _sgi-sn-launch-metadata:

Launch metadata
===============

Each product's catalogue offering carries hints for launch tools: the
console port, the firmware dialogue a launcher drives, the disk bus,
the netboot adapter and the guests and install media it supports. The
machine does not act on them. They are emulator and harness choices, not
hardware facts; where a hardware fact backs a choice, the device and
product sections of these pages state it.

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
     - R10000
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
     - ``none``
     - R10000, R12000

.. _sgi-sn-launch-console:

Console
-------

``ioc3-serial-a`` means the first IOC3 UART on the console base I/O
board; ``l1-uart`` means the L1 system controller's serial line, which
the IP35 PROM's console sits behind. The choice names the port the
stock PROM's own monitor uses on the model; it does not claim which
physical connector an owner must use.

.. _sgi-sn-launch-dialogue:

PROM dialogue
-------------

``ip27-menu`` is the IP27 PROM's maintenance menu and command monitor.
``ip35-l1-prompt`` is the L1 prompt in front of the IP35 PROM, which a
launcher passes through before the PROM's own menu. ``none`` means no
launcher-driven dialogue is defined. These name what a launcher
scripts, not a hardware behaviour.

.. _sgi-sn-launch-disk-bus:

Disk bus and other hints
------------------------

The disk bus is the QEMU SCSI bus id a launcher attaches the boot disk
to: ``scsi.0`` is the first onboard SCSI controller's bus on every SN
product. The netboot adapter names the device the stock PROM's netboot
has been confirmed to drive on the model. The Origin 200 offering also
asks launchers without a display to pass ``debug-leds=off``. Origin 300
launchers support booting an installed IRIX disk and netboot but not an
IRIX installation run; that is a harness limit, not a machine one. The
Onyx2 offerings reuse the Origin 2000 module's dialogue, console and
disk bus, since they are built on the same IP27 node boards and stock
PROM.

The Origin 300 offering lists identity inputs a launcher may supply:
raw chassis, board and SPD EEPROM records, a brick serial and L1 debug
switches. These are model inputs that replace synthetic defaults; the
machine's own identity values are marked synthetic and are not taken
from any real machine.

.. _sgi-sn-fuel-interrupts:

Fuel onboard interrupt routing (approximation)
==============================================

The ``fuel`` machine wires the four interrupt sources on its widget-15
XBridge head (head F) to these XBridge interrupt inputs:

.. list-table::
   :header-rows: 1

   * - Source
     - PCI position
     - XBridge head F input
   * - ISP12160 SCSI, PCI INTA
     - device 1
     - 1
   * - IOC3 SuperIO output (serial, parallel, keyboard)
     - device 4
     - 0
   * - IOC3 Ethernet, PCI INTA
     - device 4
     - 4
   * - OHCI USB, PCI INTA
     - device 5
     - 5

From each input onwards the path is the guest's: XBridge sends the
interrupt write the guest programmed, the crossbar routes it to the
Bedrock's host port (widget ``0xa`` in the model), and the guest's chosen pending
bit, processor mask and interrupt line deliver it.

.. _sgi-sn-fuel-interrupts-limitation:

Limitation
----------

This is a board-wiring approximation. The physical traces from these
devices to XBridge inputs on any Fuel board revision are unknown, and
the model's selected board is ``030-1707-003`` revision G, while the
evidence below comes from a Fuel of unstated revision. Whether the
routing differs between board revisions is unknown. A board schematic
or an interrupt trace on an identified revision G Fuel would settle it.

.. _sgi-sn-fuel-interrupts-evidence:

Evidence
--------

- Miod Vallat, OpenBSD/sgi story, single-page edition,
  http://miod.online.fr/software/openbsd/stories/sgiall.html, retained
  copy SHA-256
  ``48e6ce0c6dd3eb7eeb9546ea77813180100271e8cd060f0742fdddff94f96036``,
  lines 5676-5695: an OpenBSD 4.5-current boot on a Fuel reports
  XBridge revision 2 at widget 15, ``isp0`` at device 1 with ``irq 1``,
  ``ioc0`` at device 4 with ``superio irq 0, ethernet irq 4``, and
  ``ohci0`` at device 5 with ``irq 5``. These are the bridge interrupt
  indices OpenBSD registered handlers on, not measured pins. An earlier
  boot on the same page (lines 5530-5552) reports SuperIO ``irq 6``
  and a lost serial transmit interrupt, so the SuperIO index was not
  fixed across boots.
- OpenBSD/sgi at ``2df91d80c104636deeef8124252ddd5951b55fbf``,
  ``sys/arch/sgi/xbow/xbridge.c`` lines 204-213 and 1220-1288: PCI pin
  A or C maps to the device number, pin B or D to the device number
  XOR 4, and a synthetic onboard-IOC3 pin D selects the first empty
  slot. ``sys/arch/sgi/pci/ioc.c`` lines 391-456 requests pin A for
  Ethernet and that synthetic pin for SuperIO. This explains devices 1,
  4 and 5 mapping to inputs 1, 4 and 5 as driver policy, and SuperIO's
  index 0 as the first empty slot when slot 0 is empty.

Together these are a public software mapping and one machine's
handler report. They are not a measured Fuel trace or proof of
interrupt delivery. The selection of this routing passed an
independent evidence review before it was implemented.

.. _sgi-sn-fuel-interrupts-test:

Discriminating test
-------------------

``tests/qtest/sn1-fuel-test.c`` raises each source on its own and
checks that only its selected head-F interrupt bit is set and that the
interrupt reaches Bedrock, then lowers and acknowledges it:
``fuel/scsi-interrupt``, ``fuel/ioc3-superio-interrupt``,
``fuel/ioc3-ethernet-interrupt`` and ``fuel/ohci-interrupt``, with
``fuel/ioc3-superio-interrupt-reset`` and
``fuel/ioc3-superio-interrupt-migration`` covering reset and migration.
A wrong input number fails these tests. They check the model against
this table; they cannot check the table against a physical board.

Devices
=======

The node ASICs, bridges, controllers and I/O chips this family builds
from have their own pages: :doc:`sgi-hub`, :doc:`sgi-bridge`,
:doc:`sgi-elsc`, :doc:`sgi-ioc3`, :doc:`sgi-qlogic-isp`,
:doc:`sgi-xbridge`.
