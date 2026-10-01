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
(``qapi/sgi-sn.json``, embedded into the emulator and parsed at first
use by ``hw/mips/sgi/sn-catalogue.c``) naming every node, module,
fabric link, I/O board and controller a product's configuration ships.
The same compiled document backs the ``query-sgi-sn-catalogue`` QMP command
(``SGIModelCatalogue`` in ``qapi/sgi-sn.json``): the emulator returns its own
embedded copy verbatim, so a package build can read the catalogue back from
the binary it ships instead of carrying a second copy out of band. The
catalogue carries two sections; only ``topologies`` reaches this builder.
``offerings``, one entry per accepted population a launcher may select, is
for packaging and launch tooling outside the emulator, never
read here.
Four products stand on this base today:
``origin200``, ``origin2000`` and ``onyx2`` (SN0), and ``origin300``
(SN1). ``onyx2`` is Origin 2000 silicon with graphics: the same IP27
node boards, memory and stock PROM, told apart only by the hardware its
topologies fit. ``origin300``'s own product class (``hw/mips/sgi/sn1.c``)
parents ``sgi-sn`` like the others and names the same ``topology=``
contract and compiled descriptor this page describes; only its node
device (``sgi-sn1-node``, ``hw/mips/sgi/sn1-node.c``) is
product-specific, the way each SN0 node's own device is.

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

Firmware supply
===============

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
preserving every other field and the block's stored checksum.

``origin300`` takes ``-bios`` with a raw stock PROM payload of exactly
the size its one node's flash answers for; there is no per-node
override; the image is also rejected outright if it exceeds the flash
part ``sn1_node_create_boot_flash()`` expects. Its boot flash can instead
be backed by a 16 MiB raw ``-drive if=pflash,unit=0`` image of the real
chip's own content, in which case the machine's execution and
maintenance views answer through that storage -- including any write
the guest's own PROM has just made -- rather than the immutable
template ``-bios`` alone produces. Without that backing part, the
guest's own environment and self-test writes to its boot flash are
accepted and logged but never persist: a run that only supplies
``-bios`` reaches the L1's own banner and then the IP35 PROM's first
console line ("Starting PROM Boot process") promptly, then repeats
that same sequence indefinitely rather than reaching an interactive
prompt, because nothing carries its own state across the reset the
PROM performs early in that sequence.

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

Devices
=======

The node ASICs, bridges, controllers and I/O chips this family builds
from have their own pages: :doc:`sgi-hub`, :doc:`sgi-bridge`,
:doc:`sgi-elsc`, :doc:`sgi-ioc3`, :doc:`sgi-qlogic-isp`,
:doc:`sgi-xbridge`.
