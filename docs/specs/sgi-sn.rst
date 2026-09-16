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
the bridge chip behind the node's crosstalk port (Bridge on SN0,
XBridge on SN1), and the system controller (an entry-level controller
inside the Hub itself, or a module-wide MSC, on SN0; an L1 over a UART
on SN1).

QEMU models the abstract family as ``sgi-sn`` (``TYPE_SGI_SN_MACHINE``,
``hw/mips/sgi/sn-machine.c``), the base every product's machine class
parents. It owns the family's own machine properties and a builder,
``sgi_machine_init()``, that constructs a machine from a compiled
descriptor: an array of ``SGIModelTopology`` entries
(``hw/mips/sgi/sn-model.h``, generated into ``sn-model.c``) naming
every node, module, fabric link, I/O board and controller a product's
configuration ships. Three products stand on this base today:
``origin200`` and ``origin2000`` (SN0), and ``origin300`` (SN1).
``origin300``'s own product class (``hw/mips/sgi/sn1.c``) parents
``sgi-sn`` like the other two and names the same ``topology=``
contract and compiled descriptor this page describes; only its node
device (``sgi-sn1-node``, ``hw/mips/sgi/sn1-node.c``) is
product-specific, the way each SN0 node's own device is.

Products and topology
=====================

A product's ``topology=`` machine property selects which configuration
of it to build; ``-smp``, ``-m`` and ``-cpu`` then select and validate
a population within that wiring. The compiled descriptor carries four
topologies today:

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
   * - ``origin2000-module`` (default)
     - ``origin2000``
     - 4, two Crossbows and two routers
     - 2+2+2+2 (fixed)
     - 64 MiB (fixed)
   * - ``origin300`` (default)
     - ``origin300``
     - 1
     - 2 (fixed)
     - 512 MiB (fixed)

``-M origin200,topology=origin200-dual`` selects the second row; naming
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
``origin2000-module``'s four nodes ship a fixed 64 MiB each and accept
no other size. ``origin300`` accepts only R14000 for ``-cpu``; the
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

Board devices
=============

``sgi-xtalk-board`` (``hw/mips/sgi/xtalk-board.c``) is the abstract
base for a crosstalk board: the removable unit that plugs into one of
a module's XIO slots. Every concrete board takes a ``module`` and a
``slot`` (the crosstalk widget number the module's Crossbow assigns
that position); realizing one builds its own widget-facing device,
then re-parents the board under ``/machine/module<N>`` so ``info
qom-tree`` shows it where the hardware puts it. Four concrete classes
exist:

* ``sgi-ip29-io`` -- the Origin 200 System Board's onboard I/O (a
  Bridge with two ISP1040 SCSI controllers and one IOC3).
* ``sgi-baseio`` -- the Origin 2000 and Onyx2 standard XIO I/O board
  (the same Bridge/SCSI/IOC3 shape as ``sgi-ip29-io``, wired for a
  module's Crossbow instead of a direct link).
* ``sgi-io8`` -- the Origin 300 IO-8 assembly (an XBridge with a
  QLogic ISP12160, an IOC3 and a Lucent OHCI controller on one head).
* ``sgi-pci-expansion`` -- a plain Bridge with an empty, named PCI
  bus, for an Origin 2000 module's optional XIO PCI expansion slot.

The first three are onboard-only (``user_creatable`` is off: a real
system never lets a user plug one into that slot). The shared builder
(``sgi_machine_io_board_type()``) picks the concrete class from the
descriptor's own ``board_kind`` field and builds it for every product;
``sgi-pci-expansion`` is the one ``-device``-creatable board:

.. code-block:: shell

   -device sgi-pci-expansion,module=1,slot=8,fabric=<path>[,...]

A board realizes once machine init has returned, but a Crossbow needs
every one of its endpoints attached before its own realize builds the
routes between them, so a board's realize only validates its
properties, builds its widget and registers itself as pending; a
machine that owns a module's Crossbow creates it unrealized, builds
every board its topology places directly, then attaches every pending
board for that Crossbow and module from a machine-init-done notifier
before realizing the Crossbow itself. ``origin2000`` follows this
ordering for its own two Crossbows; ``origin200`` has no Crossbow (its
one ``sgi-ip29-io`` attaches directly to its node's crosstalk port),
and neither does ``origin300`` (its one ``sgi-io8`` is the fused
XBridge chip itself, sitting directly on Bedrock's crosstalk port), so
both build their one board synchronously instead and never reach the
machine-init-done notifier. ``sgi-pci-expansion`` has no module or
Crossbow on any shipping machine to attach to today; the ordering
contract above is otherwise exercised only by a qtest-only fixture
machine, not by a product.

Devices
=======

The node ASICs, bridges, controllers and I/O chips this family builds
from have their own pages: :doc:`sgi-hub`, :doc:`sgi-bridge`,
:doc:`sgi-elsc`, :doc:`sgi-ioc3`, :doc:`sgi-qlogic-isp`,
:doc:`sgi-xbridge`.
