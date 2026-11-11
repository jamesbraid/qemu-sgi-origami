.. SPDX-License-Identifier: BSD-3-Clause

.. _sgi-bricks:

======================================================
Bricks: Origin 3000 enclosures and the NUMAlink module
======================================================

Used in :ref:`sgi-ip35`, whose ``origin3000`` machine builds the Origin 3200,
Origin 3400, Onyx 3000 and Onyx 3400 from these bricks, and :ref:`sgi-ip53`,
which keeps them as reference compositions with the IP53 module layers.

.. seealso::

   Platform pages: :ref:`sgi-ip35`, :ref:`sgi-ip53`.

The Origin 3000 and Onyx 3000 generation builds a system from separate
rack-mounted enclosures called bricks, joined by NUMAlink 3 and Crosstown2
cables. This page covers the C-, CX-, D-, G-, I-, IX-, N-, P-, PX-, R-,
V- and X-bricks and the Origin 300 NUMAlink module: what each is, what it
holds, how it connects, and how much of it the model catalogue builds. The
catalogue is the set of YAML layers under ``hw/mips/sgi/models/layers/``
and the compiler in ``scripts/sgi_model/``. Ten bricks have a
module layer there: the C-brick (``modules/c-brick.yaml``), CX-brick,
D-brick, G-brick with its pipe, I-brick, N-brick, P-brick, R-brick,
V-brick and X-brick, and so does the NUMAlink module
(``modules/numalink-module.yaml``). The IX- and PX-brick have no layer.
The machine builder builds both as :term:`PIC` enclosures
(:ref:`sgi-sn-family-ixpx`).

``-M origin3000`` builds four catalogued systems: the Origin 3200
(:ref:`sgi-sn-origin3200`), the Origin 3400 with four C-bricks on one
R-brick, the Onyx 3000 with a V-brick (:ref:`sgi-sn-family-vbrick`) and
the Onyx 3400 with InfiniteReality3 (:ref:`sgi-sn-onyx3400-ir`). The
other compositions that hold bricks are hardware-only reference models,
which the compiler refuses to compile. Facts are rated as described in
:ref:`sgi-ratings`.

.. list-table::
   :header-rows: 1

   * - Brick
     - Role
     - Catalogue
   * - C-brick
     - Compute node (one :term:`Bedrock`)
     - Layer, built in four systems
   * - CX-brick
     - Up to four compute nodes and a router
     - Layer, reference composition, L1 profile
   * - D-brick
     - Fibre Channel disk enclosure
     - Layer, reference composition
   * - G-brick
     - InfiniteReality graphics enclosure
     - Layer, built in the Onyx 3400 with InfiniteReality3
   * - I-brick
     - Base I/O (:term:`XBridge`, PCI, IO7)
     - Layer, built
   * - IX-brick
     - Base I/O (three PICs, PCI-X, IO9)
     - Builder PIC enclosure, no layer
   * - N-brick
     - Passive C-brick to graphics pipe connector
     - Layer, built in the Onyx 3400 with InfiniteReality3
   * - P-brick
     - PCI expansion (three XBridges)
     - Layer, reference composition
   * - PX-brick
     - PCI-X expansion (three PICs)
     - Builder PIC enclosure, no layer
   * - R-brick
     - Eight-port NUMAlink router
     - Layer, built in two systems
   * - V-brick
     - VPro graphics enclosure
     - Layer, built in the Onyx 3000
   * - X-brick
     - Legacy :term:`XIO` expansion (one XBridge)
     - Layer, reference composition
   * - NUMAlink module
     - Eight-port router for Origin 300 base modules
     - Layer, reference composition

.. _sgi-bricks-sources:

Sources
=======

- The *Origin 3000 Series Owner's Guide*, :src:`007-4240-002`, for the
  enclosures, rear panels and L1 behaviour of the C-, D-, I-, P-, R- and
  X-bricks.
- The *Origin 3000 Series Technical Configuration Owner's Guide*,
  :src:`007-4311-002`, for configuration limits, port rates and the
  appendix of dimensions.
- The *Origin 3000 System Architecture* manual, :src:`108-0240-002`, for
  the Bedrock, the XBridge port list and the block diagrams of the I-, P-,
  R- and X-bricks. The *Origin 3000 Series Internal Technical
  Configuration Manual*, :src:`108-0266-001`, repeats and extends it.
- The *Onyx 3000 Series Graphics System Hardware Owner's Guide*,
  :src:`007-4264-002`, for the G-, N- and V-bricks and the graphics
  cabling rules.
- Addenda for later bricks: :src:`007-4572-001` (CX-brick),
  :src:`007-4604-001` (IX-brick), :src:`007-4605-001` (PX-brick) and
  :src:`007-4424-001` (N-brick).
- The *Origin 300 System with NUMAlink Module User's Guide*,
  :src:`007-4498-002`, for the NUMAlink module, and the router service
  procedures :src:`108-0301-001` for the R-brick's port switch.
- Published IRIX hardware inventories of three Origin 3000 class machines,
  cited in :ref:`sgi-sn-family-ibrick` and on :ref:`sgi-sn-family-bedrock`. No inventory of a CX-, D-, G-, IX-, N-, P-,
  PX-, V- or X-brick is published, so every statement about those bricks
  is a product description.
- Public driver source as software expectations: Linux
  ``include/asm-ia64/sn/ksys/l1.h`` in ``linux-history`` at
  ``381ab1f53816`` and OpenBSD ``sys/arch/sgi/sgi/ip27.h`` and
  ``sys/arch/sgi/xbow/xbow.c`` at ``2df91d80c104``.

Related pages are :doc:`bedrock`, :doc:`xbridge`, :doc:`pic`,
:doc:`sn1-router`, :doc:`l1`, :doc:`pci-expansion` and
:doc:`/system/mips/sgi/sn`.

.. _sgi-bricks-fabric:

How bricks connect
==================

Two cable types join bricks.

NUMAlink 3
  Joins a C-brick's ``LINK (NI)`` connector to an R-brick port, or in an
  Origin 3200 to another C-brick (*established*,
  :src:`007-4240-002 pp. 121, 124-125`). It runs at 1.6 GB/s in each
  direction (*established*, :src:`007-4311-002 pp. 23, 47`).

Crosstown2 (XIO)
  Joins a C-brick's ``XIO (II)`` connector to an I/O or graphics brick
  (*established*, :src:`007-4240-002 p. 124`). The rating is 1.2 GB/s in
  each direction (*established*, :src:`007-4311-002 pp. 23, 47`). The
  I/O bricks also list 800 MB/s, and the guides disagree on which brick
  uses which figure, so the port speed and its power-on default are
  *unknown*. A log of an L1 ``ioport`` command or a register read on a
  real brick would settle it.

The C-brick has no crossbar. Its Bedrock reaches I/O through a Crosstown2
port of an I/O brick's crossbar (*established*,
:src:`108-0240-002 p. 2-26`). The cabling rules the guides give, which the
catalogue's composition rules can express and the compiler checks, are:

- A G-brick connects to a C-brick only through an I-, X- or N-brick. A
  V-brick connects to a C-brick directly (*established*,
  :src:`007-4264-002 pp. 8, 10, 62`).
- A system holds G-bricks or V-bricks, not both (*established*,
  :src:`007-4264-002 pp. 10, 21`).
- A C-brick on an R-brick must use ports 2 to 5 (*established*,
  :src:`007-4311-002 p. 36`).
- An Origin 3200 holds one or two C-bricks, at most two I/O bricks of
  which one is an I-brick, and no R-brick (*established*,
  :src:`007-4311-002 p. 87`).
- The D-brick is not on the NUMAlink or XIO network. It hangs off a Fibre
  Channel adapter in an I- or P-brick (*established*,
  :src:`007-4240-002 p. 168`).

The management network is separate. Each brick except the D-brick has an L1
controller, and an optional L2 controls the L1 controllers over USB
(*established*, :src:`007-4240-002 pp. 69, 72`). The model's L1 is
described in :doc:`l1`.

.. _sgi-bricks-cbrick:

C-brick
=======

The C-brick is the compute node: one Bedrock, one or two processor
interface modules (PIMMs) of two processors each, and eight DIMM slots
(*established*, :src:`108-0240-002 pp. 2-2, 2-4`). The processors are
R12000 at 360 MHz or R12000A at 400 MHz with 4 or 8 MB of secondary cache
per processor (*established*, :src:`108-0240-002 p. 2-4`), and the later
internal manual adds a 500 MHz R14000 option (*established*,
:src:`108-0266-001 p. 28`). The 500 MHz option is offered
(:ref:`sgi-sn-cbrick-r14000-500mhz`), and a 600 MHz R14000 PIMM is captured as
well (:ref:`sgi-sn-cbrick-r14000`). Local memory is 512 MiB to 8 GiB in banks 0 to
7, two banks per DIMM pair, with the two DIMMs of a pair matching
(*established*, :src:`108-0240-002 pp. 2-5, 2-6`). The guides disagree
on the bank count, the minimum size and the cache size, and the layer
records none of them.

Connections:

- One rear ``LINK (NI)`` NUMAlink 3 connector, to an R-brick or, in an
  Origin 3200, to another C-brick.
- One rear ``XIO (II)`` Crosstown2 connector, to an I-, P- or X-brick or,
  for graphics, to a V-brick.
- An L1 controller on the motherboard with a front display, a DB-9 console
  port and a USB port for an L2 (*established*,
  :src:`007-4240-002 pp. 115-116, 121`).

Published inventories of an Origin 3200 and an Origin 3400 report the
Bedrock as ``Hub`` revision 2 at 200 MHz and the :term:`PROM` as 6.210 on every
C-brick (*observed*; see :ref:`sgi-sn-family-bedrock`). That the C-brick
and Origin 300 Bedrocks are the same silicon is *inferred* from the
matching reports. Whether ``Hub revision 2`` is the value of the Bedrock's
revision register is *unknown*. A revision register read on a C-brick
would settle it.

Software expectations: OpenBSD reads bits 15:12 of the :term:`Hub` board
identification word as a product code, with 0 for a C-brick (``ip27.h``),
and names the Bedrock widget part ``0xc110``, manufacturer ``0x036``
(``xbowdevs``). The L1 brick-type character for an IP35 module is ``C``,
0x43 (Linux ``l1.h`` line 263).

Catalogue: ``modules/c-brick.yaml``, on the ``sn1`` architecture and the
``ip35`` assembly. It describes the chassis (L1 brick type ``C``), a Bedrock
with two processor interfaces of two CPUs each, widget part ``0xc110``,
manufacturer ``0x036``, revision 2 at 200 MHz (the values above), two
R12000-family CPU components, eight DIMM slots as one memory component, one
``SysAD`` bus and one ``XIO (II)`` connector with its ``xio-ii`` bus. The
layer asserts no CPU clock or cache size. The processors are placed on the
SysAD bus of processor interface 0 only, so the layer shows two of the four
possible CPUs. The layer has the ``LINK (NI)`` connector, which a
composition joins to another C-brick or to an R-brick port, and an L1
component. It holds no memory fit. The widget part and manufacturer
are software expectations, not reads from a C-brick, so the Bedrock identity
is an *approximation*.

.. _sgi-sn-family-origin3000-cbrick:

C-brick in the catalogue
------------------------

The C-brick has no crossbar of its own. Its Bedrock reaches I/O through
a Crosstown2 port into an I-, P- or X-brick's XBridge, whose crossbar
section does the switching (*established*, :src:`108-0240-002 p. 2-26`).
It has one rear ``XIO (II)`` connector, which joins it to an I-, P- or
X-brick (*established*, :src:`007-4240-002 p. 124`). The layer's
C-brick buses, one SysAD per processor interface and the ``xio-ii`` bus
from the Bedrock to that connector, follow from this. Which port of the
far brick's XBridge the cable reaches is the composition's choice, not
the C-brick's.

The eight DIMM slots form four pairs. Banks 2n and 2n+1 sit on pair n
and both DIMMs of a pair match, so the two banks of a pair are fitted
alike; pairs may differ (*established*,
:src:`108-0240-002 pp. 2-5, 2-6`). The Origin 3200 catalogue entry fixes
each C-brick at the fit a posted Origin 3200 inventory reports
(*observed*, :src:`nekonomicon-16723604`): four 400 MHz R12000s with
8 MB secondary caches on two PIMMs and 4096 MB in eight 512 MB banks.
Resolution refuses any other processor or bank population for that
product. How the shared builder joins a C-brick to an I/O brick's XBridge
is on :ref:`sgi-ip35-brick-composition`.

The entry is the ``origin3000`` machine, topology ``origin3200``. Its
C-brick values the sources do not settle are model choices:

* The board-identification word is ``0x2``. Product code 0 in bits 15:12
  is a C-brick (*software expectation*, OpenBSD ``ip27.h``). The low nibble 2
  selects the stock IP35 PROM 6.210's configuration row for a 400 MHz,
  8 MB fit, and the other bits read zero (*approximation*: no C-brick
  has been read).
* The flash configuration block carries machine type 0 and that row's
  word ``0x5414a785``. Each processor's reset mode is the word's low 25
  bits (*approximation*, as the Origin 300's R14000 values are taken
  from its own row).
* The FPU identification is ``0x0935``, from the R12010 revision 3.5 the
  inventory prints (*inferred*).
* The L1 is the ``cbrick`` profile, named by its slot: 001c04 and
  001c07. The two C-brick L1s are peers over the NUMAlink cable.
* With no XIO cable, a C-brick's II link reads down (*inferred*, as the
  Bedrock II model already takes it).

.. _sgi-sn-origin3200:

Origin 3200 machine
-------------------

``-M origin3000`` builds two C-bricks joined LINK (NI) to LINK (NI) and
an I-brick on c0's ``XIO (II)`` connector at XBridge port A, with 8
R12000s (``-cpu R12000 -smp 8``) and 8192 MB. The I-brick's XBridge is
``/machine/module2/xbridge``. Its heads E and F are widgets 14 and 15 of
c0, and its IO7 is ``/machine/module2/io7`` on head F. The I-brick's L1
is ``/machine/module2/l1``, the ``ibrick`` profile named 001i14, reached
from c0's L1 over the XIO cable as an in-process call. It holds the
IO7's Ethernet address, the machine's ``mac``. The I-brick's Fibre
Channel card is ``/machine/module2/io7/fibre-channel``, a
``qlogic-isp2200`` at head F device 1 with its pin A on head pin 1. Its
private loop is the SCSI bus ``scsi.0``, the boot disk bus. Each disk
there is a loop port at the loop ID its ``scsi-id`` gives and needs the
``port_wwn`` and ``wwn`` names that port has, which the launcher
chooses.

Model limits:

* No internal FC disk is built with the machine. Disks join the loop
  only as the launcher attaches them, and which of the brick's two drive
  bays a disk stands for is *unknown*.
* The IO7's IEEE 1394 link controller carries the model's lone PHY with no
  connected port, because the fitted PHY part is unidentified
  (*approximation*); no 1394 bus, CD-ROM or packet DMA is modelled. The
  stock PROM writes one base into both of that controller's BARs; the
  model answers OHCI registers at that shared base (*approximation*),
  since TI does not say which decoder answers.
* The IO7's time-of-day part is unidentified, so its :term:`IOC3` carries the
  Origin 300's M48T35-compatible Timekeeper as a compatibility choice
  (*approximation*), with blank storage on every start.
* Head E's PCI slots are empty. The I-brick's XBridge revision is the
  model's.

.. _sgi-sn-cbrick-r14000:

C-brick with 600 MHz R14000 processors
--------------------------------------

``-M origin3000,topology=origin3200,processor=r14000-600mhz-8mb``,
``topology=origin3400,processor=r14000-600mhz-8mb`` and
``topology=onyx3000,processor=r14000-600mhz-8mb`` are the Origin
3200, Origin 3400 and Onyx 3000 entries with every C-brick fitted with
four 600 MHz R14000s and 8 MB secondary caches (``-cpu R14000``, processor
``r14000-600mhz-8mb``). Everything else, the bricks, the cabling, the
memory and the I/O, is the R12000 entry's. The Onyx 3400 with
InfiniteReality3 is not offered with this processor, and no other clock,
cache size or part is. The 500 MHz option is a separate entry
(:ref:`sgi-sn-cbrick-r14000-500mhz`).

The processor is a PIMM SGI documents and inventories capture. Established
(*established*):

* The model fits 600 MHz R14000 processors with 8 MB secondary caches at
  300 MHz, ``Tap 0xa``, at processor revisions 2.4 and 2.3 on PIMM part
  numbers ``030-1734-001`` and ``-002``, *inferred*: no registered
  source holds the record this rests on, and the size would be the
  inventory tool's reading of the processor's Config0 field. No SGI document gives a 600 MHz C-brick PIMM
  size or part number. A 4 MiB C-brick PIMM is unseen and not offered; the
  PROM's other 600 MHz words (2 and 4 MiB) belong to its Origin 300 and Fuel
  rows, not to the C-brick class.
* SGI's L3 Controller release notes add L1 support for a 600 MHz R14KA
  PIMM and place it in the Origin 3000 and Origin 300 systems.
* The stock IP35 PROM 6.210 carries a class-0 configuration row for
  600 MHz and 8 MB, word ``0x549cab85``, reached by board-identification
  low nibble 3. Its clock, cache and tap fields are the ones the
  inventories print.

The model's values:

* The flash configuration block carries machine type 0 and ``0x549cab85``,
  and the board-identification word's low nibble is 3 (*inferred*: the
  C-brick word selecting the PROM row the captured PIMMs match; no C-brick
  flash block or register has been read).
* The reset-mode word is the low 25 bits, ``0x009cab85``, and Config0 reads
  ``0x6c9cab85`` at reset (*inferred*: no C-brick Config0 has been read;
  the model's choice of this word is an *approximation*). Its fields are
  EC 5 (the 200 MHz Hub clock from 600 MHz), SC 3, SS 4, so 512 KiB shifted
  left by 4 is the 8 MB cache, SB 1, BE 1 and K0 5. What bit 23 does is
  *unknown*; software may rewrite K0 (not traced). The stock PROM writes
  the flash word to Config0 and checks only that the low 16 bits read back
  (``0xab85``); it never reads SysClkDiv or SCClkDiv, and its printed
  speeds come from the flash block. The generator holds both the cache size
  and the flash word's low 25 bits to the SS field.
* PRId is ``0x0f24`` (*inferred* for the part revision, not read on a
  C-brick): Linux prints ``PrId 0x00000f24`` on Origin-class machines and
  IRIX ``hinv`` prints 2.4 on IP35. The captured parts print 2.4 and 2.3 and
  the model uses 2.4; 2.3 (``0x0f23``) is *inferred*. The revision follows
  the part, not the clock.
* The FPU identification is the R14000 type's ``0x00000900`` (*inferred*: no raw
  FIR has been read on any R14000 or on an IP35), not a value built from the revision. IRIX ``hinv``
  prints the FPU revision as the CPU's because the kernel copies PRId into
  the FPU entry, which does not test the FIR; a raw ``0x0900`` would print
  as an R14010 revision 0.0, as IP30 listings do. The FPU revision line
  is an *approximation*.
* The cache runs at 300 MHz, the processor clock over two, as captured. The
  Hub stays at 200 MHz (*approximation*).

Not modelled: the 600 MHz part ``030-1823-001`` that later L1 firmware
recognizes, whose processor type (R16000 or R14000A), cache size, tap and
C-brick status are all *unknown*.

.. _sgi-sn-cbrick-r14000-500mhz:

C-brick with 500 MHz R14000 processors
--------------------------------------

``-M origin3000,topology=origin3200,processor=r14000-500mhz-8mb``,
``topology=origin3400,processor=r14000-500mhz-8mb`` and
``topology=onyx3000,processor=r14000-500mhz-8mb`` are the Origin
3200, Origin 3400 and Onyx 3000 entries with every C-brick fitted with
four 500 MHz R14000s and 8 MB secondary caches (``-cpu R14000``, processor
``r14000-500mhz-8mb``). Everything else is the R12000 entry's. The Onyx 3400
with InfiniteReality3 is not offered with this processor, and no other
clock, cache size or part is. No topology mixes 500 MHz R14000 and 400 MHz
R12000 C-bricks.

The processor is a PIMM SGI documents. Established (*established*):

* The C-brick owner's guide lists a 500 MHz R14000 PIMM with 8 MB per
  processor (:src:`108-0266-001 p. 28`). The L3 Controller 1.7 release notes
  add L1 support for the 500 MHz R14K PIMM ``030-1520-002``, the R14Ka one
  ``030-1799-001``, and for mixing them in one C-brick.
* The stock IP35 PROM 6.210 carries a class-0 configuration row for 500 MHz
  and 8 MB, word ``0x549ca985``, reached by board-identification low
  nibble 1 or 6. Nothing separates the two nibbles.
* A public Origin 3400 inventory lists eight C-bricks with 24 processors
  printed as 500 MHz R14000 at processor revision 1.4 and eight as 400 MHz
  R12000, all with 8 MB secondary caches. The print has no board lines,
  so it gives no PIMM part number, cache clock or tap.

The model's values:

* The flash configuration block carries machine type 0 and ``0x549ca985``
  (*software expectation* as the PROM's row for this part; the owner's
  guide and the data sheet give an 8 MiB part, *established*), and the board-identification
  word's low nibble is 1 (*approximation*: no C-brick flash block or
  register has been read, and 6 reaches the same row).
* The reset-mode word is the low 25 bits, ``0x009ca985``, and Config0 reads
  ``0x6c9ca985`` at reset (*inferred*: no C-brick Config0 has been read;
  the model's choice of this word is an *approximation*). Its fields are
  EC 4 (the 200 MHz Hub clock from 500 MHz), SC 3, SS 4, so 512 KiB shifted
  left by 4 is the 8 MB cache, SB 1, BE 1 and K0 5. What bit 23 does is
  *unknown*; software may rewrite K0 (not traced). The stock PROM writes
  the flash word to Config0 and checks only that the low 16 bits read back
  (``0xa985``); it never reads SysClkDiv or SCClkDiv, and its printed
  speeds come from the flash block. The generator holds both the cache size
  and the flash word's low 25 bits to the SS field.
* PRId is ``0x0f14`` (*inferred* for the part revision, not read on a
  C-brick): Linux prints ``PrId 0x00000f14`` on Origin-class machines and
  IRIX ``hinv`` prints 1.4 on IP35, but no raw PRId has been read from a
  C-brick. The revision follows the part, not the clock: the 1.4 here is the
  captured part's, and 500 MHz Origin 300 and Fuel listings print 2.4 and
  2.3.
* The cache runs at 250 MHz, the processor clock over two, and the PROM's
  cache line prints ``Tap 0xa`` (*inferred*: the same revision and clock
  print both on Onyx2 boards, and no C-brick line is held). The PIMM's
  board lines are *unknown*. The Hub stays at 200 MHz (*approximation*).
* The FPU identification is the R14000 type's ``0x00000900`` (*inferred*;
  no FIR has been read on an IP35), not a value built from the 1.4
  revision. IRIX ``hinv`` prints the FPU revision as the CPU's because the
  kernel copies PRId into the FPU entry, which does not test the FIR; a raw
  ``0x0900`` would print as an R14010 revision 0.0, as IP30 listings do. The
  FPU revision line is therefore an *approximation* of the printed
  1.4.

A mixed system (400 MHz R12000 and 500 MHz R14000 C-bricks together, as the
public inventory records) is not offered. A product's ``cpu_selection``
names one processor for every C-brick, and the machine takes one ``-cpu``
type for all its nodes, so mixing needs a per-brick selection in the
generator and a per-node CPU type in the machine.

.. _sgi-sn-two-partitions:

Two partitions of an Origin 3400
--------------------------------

``-M origin3000,topology=origin3400-two-partitions`` is the smallest
partitioned system the configuration guide lists as valid: two C-bricks on
router ports 2 and 3 of one R-brick, an I-brick on each C-brick, and the
partition IDs 1 and 2 held by the two C-brick controllers. A C-brick's
controller answers its ID to the stock PROM's partition request and prints
it in the ``brick`` line; the R-brick prints ``N/A`` and refuses ``brick
partition``.

*Provisional:* an I-brick reports its C-brick's ID and refuses ``brick
partition``. The partitioning datasheet and the L1 datasheet disagree on
whether an I/O brick holds a partition of its own or acquires its C-brick's,
and a separate research item will settle which. The model follows one reading
(the I-brick follows its C-brick and refuses a set) until then.

Both partitions boot in one QEMU process, so they share one machine clock,
one disk-image namespace and one set of host backends, but each C-brick has
its own console (the first is ``l1``, the second ``l1-node1``) and its own
I-brick, so each partition has the system disk and console path the guide
requires. Booting each partition's operating system from its own installed
disk is not set up by the catalogue; a disk is bound per I-brick.

The product combination, the IDs 1 and 2 and the absent L2 are
approximations: the configuration guide says two one-C-brick partitions
are a valid configuration, not that a two-C-brick system ships with an
R-brick, and it names no ID for a particular brick. With no L2 the
controller answers 0 to the partition-record request, so the PROM never
fetches a record and takes the ID from the controller alone.

.. _sgi-sn-cbrick-r16000:

C-brick with 800 MHz R16000 processors
--------------------------------------

``-M origin3000,topology=origin3200,processor=r16000-800mhz-8mb``,
``topology=origin3400,processor=r16000-800mhz-8mb`` and
``topology=onyx3000,processor=r16000-800mhz-8mb`` are the Origin 3200,
Origin 3400 and Onyx 3000 entries with every C-brick fitted with four
800 MHz R16000s and 8 MB secondary caches (``-cpu R16000``, processor ``r16000-800mhz-8mb``).
Everything else, the bricks, the cabling, the memory and the I/O, is the
R12000 entry's.

The 800 MHz R16000 is a real SGI part: an irixnet forum sale of an
"SGI Fuel 800Mhz R16K" and sgidepot's Fuel page, which lists Fuel
processors from R14K/500 (2 MB L2) to R16K/900 MHz (8 MB L2) at 600, 700
and 800 MHz, both establish it. Both are Fuel sources, so they show the
part, not a C-brick fitted with it, and no capture of an Origin 3000
C-brick with one exists; SGI's own Origin 3000 data sheet of that time
lists an R16000 only for the Cx-brick. These offerings are therefore an
*approximation*: the values below are model choices, each marked with
what it rests on, and the option does not claim to be what a shipped
C-brick presents.

* The clock is the part's (established for the Fuel). The cache is 8 MB, which every C-brick PIMM held in
  captures carries, rather than the 4 MB of some 800 MHz boards (inferred).
* The reset-mode word is ``0x009caf85``, the 800 MHz, 8 MB word the
  IP53 Onyx 350 entry carries, so the processor is the same
  ``r16000-800mhz-8mb`` there. Its SS field, bits 18:16, selects the 8 MB
  cache, and the generator holds the word to the cache size (inferred).
* The flash configuration block carries machine type 0 and the stock IP35
  PROM 6.210's 800 MHz, 8 MB word for the C-brick class, ``0x569caf85``,
  and the board-identification word's low nibble is 5, which reaches that
  row (approximated). That row's cache tap reads ``0xb`` where every
  captured 800 MHz R16000 board prints ``0xa``; a word with the ``0xa``
  tap exists in the PROM only for another machine type, and which a real
  C-brick PIMM used is unknown.
* The PIMM part number, the processor revision and the FPU revision are
  unknown. The Hub stays at 200 MHz (inferred).
* What the guest sees is the model's, not a C-brick's. ``hinv -v`` prints
  ``800 MHz R16000 Rev 3.0``, ``R16010FPC Rev 3.0`` and ``Secondary Cache
  8MB 400MHz Tap 0xb``. The revisions come from the QEMU R16000's identity
  words, and most captured 800 MHz boards print 2.2; the tap is the
  PROM row's, not a measurement.
* The Onyx 3400 with InfiniteReality3 is not offered with this processor,
  and no other clock, cache size or part is. The 600 MHz R14000 PIMM is a
  separate entry (:ref:`sgi-sn-cbrick-r14000`).

.. _sgi-sn-origin3200-two-ibricks:

Origin 3200 with an I-brick on each C-brick
-------------------------------------------

``-M origin3000,topology=origin3200-two-ibricks`` is the ``origin3200``
brick set with a second I-brick on the second C-brick's ``XIO (II)``
connector. It is model only: no source documents it and no inventory shows
it, and the display name and description say so. The guide allows two I/O
bricks of which one is an I-brick (:src:`007-4311-002 p. 87`) but names no
second I-brick. Each I-brick keeps widgets 14 and 15 and is told apart by
its host C-brick's module, as the Origin 300 two-module inventories name
theirs (*inferred*). The second I-brick's rack slot (U11) and its port A
are model choices (*approximation*).

The stock IP35 PROM 6.210 finds 2 nodes and 8 CPUs and passes its
diagnostics, and ``hinv -v`` lists an ``IBRICK Bridge`` pair under each of
modules ``001c04`` and ``001c07``. The PROM names a node by its base I/O
identity when the node's own is unreadable, so two I-bricks must not share
one: the builder gives each I-brick L1 its module index as ``identity-index``
whenever a topology has more than one, and the topology gives each I-brick a
distinct synthetic XBridge and IOC3 serial. Without that the PROM prints the
same hub NIC on both nodes and finds one object. What a real second I-brick
or a dual-hosted one shows is *unknown*.

.. _sgi-sn-onyx3400-ir:

Onyx 3400 with InfiniteReality3 machine
---------------------------------------

``-M origin3000,topology=onyx3400-ir`` builds the Figure 1-10 brick set
of the Onyx 3000 graphics guide (:src:`007-4264-002 p. 27`) as a
catalogue entry, ``onyx3400-ir``. The ``onyx3400`` composition stays the
reference of the same brick set. The machine has four C-bricks on ports
2 to 5 of one R-brick, an I-brick on c0's ``XIO (II)`` connector, an
N-brick whose first three chips carry c1 to c3, and two G-bricks. It has
16 R12000s, four per C-brick, and 16384 MB. The stock PROM 6.210 finds 4
nodes, 16 CPUs and one router, and its diagnostics summary reads ``ALL
DIAGS PASSED``. Its ``hinv -v`` lists the four node boards, the
I-brick's two heads, the router, an XBow for each C-brick and, for the
pipe that is fitted, a ``KONA Graphics Board`` under the module of the
C-brick that reaches it.

All four pipes are cabled: the I-brick's second port (port B) carries
pipe 0 of G-brick 0, and the N-brick's first three chips carry pipe 1 of
G-brick 0 and both pipes of G-brick 1. The N-brick's fourth connector
pair is empty.

Choices the sources do not settle, each an *approximation*:

* One R-brick, not Figure 1-10's two. The routed Origin 3400 drawing of
  the technical configuration guide (:src:`007-4311-002 p. 55`) has one,
  with the C-bricks on ports 2 to 5. The R-brick presents all eight
  ports, and its L2 controller and USB hub traffic to the C-brick L1s
  are not modelled. The C-brick L1s are not peers of each other.
* The C-bricks are fitted like the Origin 3200's: four 400 MHz R12000s
  with 8 MB caches and 4096 MB. The C-brick layer carries both PIMMs
  unconditionally, so Figure 1-10's two-processor C-bricks are not
  expressible.
* The N-brick's XBridge presents the revision the model's XBridge
  presents, as the I-brick's does.
* Module numbers of the bricks.
* The I-brick and N-brick controllers' attached-brick replies, and the
  N-brick's type letter (:ref:`sgi-l1-origin3000`).
* Each N-brick chip presents its two PCI heads, widgets 14 and 15, with
  nothing behind either, to the C-brick cabled to it. The N-brick
  Overview (revision 10/24/01, printed page 3) calls the chip's ports E
  and F logically disconnected. The heads are there because the stock
  PROM 6.210 records an N-brick board (type ``0x74``, Linux
  ``KLTYPE_NBRICK``) for a C-brick's crossbar only from heads that
  decode, and IRIX 6.5.30, like Linux's ``io_xswitch_widget_init``,
  looks for an I/O brick board on the node or its crossbar peer when it
  brings up a crossbar widget, so a pipe behind the N-brick needs one,
  *software expectation*. A widget scan of a real N-brick would settle
  which heads answer.

Model limits:

* The machine fits up to four ``sgi-kona`` pipes, one per pipe slot.
  The first, on I-brick port B, is the default. Each G-brick's pipe 1
  takes one or two raster managers and its pipe 0 one, two or four. Each
  C-brick's L1 knows its chip's G-brick whether or not a pipe is fitted.
* The router's ports 1, 6, 7 and 8 are uncabled, and the PROM prints
  ``discover_object: vector read 0x2 LRI failed`` and an EEPROM vector
  read error against the router.

.. _sgi-bricks-cxbrick:

CX-brick
========

The CX-brick is a high-density compute brick: one to four IP53 node
boards, one router board and one power entry module, equal to four
C-bricks and one R-brick (*established*, :src:`007-4572-001 pp. 1, 3`).
It needs IRIX 6.5.18 or later. Each node board holds one Bedrock, zero or
four processors with 8 MB of secondary cache each, and up to eight DIMMs
for 1 to 8 GB of local memory, with a pair rule like the C-brick's. A
zero-processor board is a supported memory node (*established*,
:src:`007-4572-001 pp. 3-4, 13`). The brick holds up to 16 processors and
32 GB.

Connections (*established*, :src:`007-4572-001 pp. 4-5, 10-12`):

- A router board with one router ASIC, called the same ASIC as the
  R-brick's, four internal NUMAlink 3 links to the node boards and four
  external NUMAlink 3 connectors, labelled NUMAlink 1 Port A, 8 Port H,
  7 Port G and 6 Port F. Ports A and F go to one neighbouring CX-brick.
  Ports G and H go to R-bricks in router-based systems.
- Four external XIO connectors labelled XIO 0 to XIO 3, each belonging to
  the node board of the same number, for I/O and graphics bricks.
- One L1 controller on the router board, one display, one DB-9 console
  and one USB L1 port. One module ID and one partition ID belong to the
  brick, not to a node (*established*, :src:`007-4572-001 pp. 5, 10`).

The CX-brick shares the IP53 label with the Origin 350 node board, but
that does not make them the same board (*inferred*). Which router link
each node uses, how the nodes appear in a hardware inventory, node
numbering, the processor clock and the node board part number are
*unknown*. A CX-brick inventory would settle them.

.. _sgi-sn-family-cxbrick:

The *Partitioning Guide for Systems That Contain Cx-bricks* covers the
Origin 3900 and gives a partition of one CX-brick with an I- or IX-brick
(*established*, :src:`007-4426-001 pp. xi, 3, 6, 18`).

Catalogue: the ``cx-brick`` layer and the ``origin3900`` composition.
They record one module with four Bedrock nodes, an eight-port router
linked to each node over a midplane link, and each node's XIO connector
on its own Bedrock's ``xio-ii`` bus. The composition cables an I-brick to
node board 0's XIO connector at its port A, a choice of the composition
(*approximation*).

``-M origin3000,topology=origin3900-ip53,nodes=4 -cpu R16000`` builds it. No
source read fixes the CX-brick's processors, node board part, router
ports, NASIDs, widget numbers or L1 type character, so the model chooses
each, an *approximation*:

* Each node board is fitted as an Origin 350 IP53 is: four 700 MHz
  R16000s, here with 8 MB caches, and 2048 MB.
* Node board *n* takes router port *n* + 2.
* The L1 reports the C-brick's type character (:ref:`sgi-l1-cxbrick`).
* Board *n*'s link reports the brick's slot plus *n* and a serial of its
  own, because the stock IP35 PROM tells nodes apart by those, *software
  expectation*.

The node board has its own board kind, ``ip53-cx``. Like the C-brick's
IP35 board it has no XBridge and takes an XIO cable. Like the Origin
350's IP53 it carries R16000s, but it has none of the Origin 350's PIC,
IO9 or runtime profile. On the model the stock IP35 PROM 6.210 reports
4 nodes, 16 CPUs, 8192 MB and 1 router and passes its diagnostics.
The Origin 350 layers in
``assemblies/ip53.yaml`` and ``modules/ip53-origin350-module.yaml`` do not
transfer: that module has two or four processors, a PIC at widget 15 and
an IO9, and the CX node board has none of these.

.. _sgi-sn-cxbrick-memory-node:

CX-brick with a memory-only node board
--------------------------------------

``-M origin3000,topology=origin3900-memory-node -cpu R16000`` is the
``origin3900-ip53`` brick set with node board 3 fitted with no processors. The
addendum allows a brick to mix four-processor and zero-processor boards
(*established*, :src:`007-4572-001 p. 13`), and the zero-processor board
keeps its Bedrock, eight DIMM slots, router link and XIO connection
(*established*, :src:`007-4572-001 p. 4`). The brick has 12 processors
(``-smp 12``) and boards 0 to 2 are as ``origin3900``'s. Board 3 has
2048 MB and no processor, with its processor slots and SysAD bus left out
of the ``cx-brick-memory-node`` layer, and the catalogue gives it
``role: memory``. A brick of four such boards is not offered: it must be
paired with another CX-brick (*established*, :src:`007-4653-001 pp. 47,
64`).

What the stock IP35 PROM 6.210 does with it is observed on the model, not
established for a CX-brick (*inferred*). The PROM reports 12 CPUs on 4
nodes, finds the board as a hub with no processor, and its KLCONFIG exists
afterwards: ``hinv -v`` lists ``IP35 Node Board, Module 001c10`` with its
Bedrock and 2048 MB and no processor. The board's local-block registers
answer the master's vector requests with nothing executing on it. With the
PROM's verbose switch (L1 debug switches ``0x104``) the console shows
``> Headless Hub: index 4``, ``Initializing headless node at nasid 3`` and
``Headless node at NASID 3 module 001c10``; without it only the
``Intializing any CPUless nodes`` line prints. The master marks the board
by setting bit 38 of its scratch 1. A vector read of that one register that
fails (the test property ``x-test-refuse-vector-read`` on the board's
Bedrock) makes the PROM print ``Vector read error`` and ``*** Headless HUB
inaccessible`` and carry on to the menu.

The summary's ``DIAG RESULTS`` heading is followed by nothing, where the
four-compute-board ``origin3900`` prints ``ALL DIAGS PASSED``. The PROM
causes this from valid state, a *software expectation* drawn from analysis
of the stock PROM 6.210 setup code. For a hub with no processors whose I/O probe finds nothing, the setup
code reads the node's I/O link status register: with bits 13:12 at 0 or
1 it records diagnostic value ``0x9b``, a link-down result, on the
hub's KLCONFIG component, and with 2 or 3 it records zero. The summary
prints ``ALL DIAGS PASSED`` only when no node has a problem flagged, and
a nonzero hub result flags one without printing a line for it, so the
section stays empty. Why nodes with processors never take that read is
*unknown*.

The model matches that account. Board 3 has no I/O brick, so its link
status reads bit 13 clear (bits 13:12 read 0) and the master records
``0x9b`` there; every node with processors records zero. Board 0, cabled
to the I-brick, reads 2. Changing only the hub's diagnostic value,
after the node is initialized and before the BASEIO PROM starts, moves
the summary: zero on board 3 gives ``ALL DIAGS PASSED``, and ``0x9b`` on
node 2 of a four-compute-board ``origin3900`` leaves it empty. The qtest
``sn/builder/origin3900/memory-node-ii-link`` checks the two link
values.

Limits:

* Whether a real memory-only board prints nothing under ``DIAG
  RESULTS`` depends on what an uncabled I/O link reads on hardware. The
  model reads bit 13 clear with no XIO cable, *inferred* and unmeasured;
  if that holds, the empty section is what the real PROM prints too
  (*inferred*). The model does not raise the link to get
  ``ALL DIAGS PASSED``, since nothing shows a real uncabled link reads
  up. Settled by the link status of a real node with no I/O brick, or a
  console log from a real Origin 3900 with a memory-only board.
* The board's NIC reads zero until the master initializes it; whether a
  real board reads one is *unknown*.
* Every other fact about the board (its part number, Bedrock revision and
  router port) is the ``origin3900`` choice, an *approximation*.

.. _sgi-bricks-dbrick:

D-brick
=======

The D-brick is a 4 U Fibre Channel disk enclosure for JBOD storage
(*established*, :src:`007-4240-002 p. 155`). It holds twelve bays in a grid
four wide by three high for hot-pluggable dual-ported 3.5-inch disks, two to
twelve of them fitted, with filler modules in empty bays. The disks in bays
1/3 and 4/3 must be present because the operator-panel firmware uses them to
reach the disks (*established*, :src:`007-4240-002 pp. 156, 166, 172-173`).
The rear holds two 550 W power and cooling modules, one operator-panel
module and two loop resiliency circuit (LRC) modules.

Connections:

- Each LRC module has a gigabit interface converter input that cables to a
  Fibre Channel adapter in an I- or P-brick, a DB-9 output to daisy-chain a
  second D-brick and an ID selector with settings 1 to 7 (*established*,
  :src:`007-4240-002 pp. 167-168`).
- The twelve disks connect to two loops, one per LRC. Which bays one LRC
  presents is *unknown*: the owner's guide says all twelve in one place
  and six per loop at start-up in another.

The D-brick has no L1 controller and no NUMAlink or XIO connection
(*established*, :src:`007-4240-002 pp. 69, 72`; the L1 brick-type table on
p. 235 has no D). That it has no module number is *inferred* from the rack
numbering on p. 13. A microcontroller on the operator-panel module, which
the guides call the enclosure system interface (ESI) module, monitors
power, cooling and temperature over an I2C bus (*established*,
:src:`007-4240-002 p. 163`). No public IRIX document says a guest queries
the ESI.

Catalogue: the ``d-brick`` layer, a twelve-bay Fibre Channel enclosure
with no L1, and the ``origin3200-dbrick`` composition, which is a
reference because the Fibre Channel cable to the D-brick is stated and
not built. The device is on :doc:`dbrick`. The host side of the loop is
the QLogic ISP2200 adapter (:doc:`qlogic-isp2200`).

.. _sgi-bricks-gbrick:

G-brick
=======

The G-brick is the Onyx 3000's InfiniteReality graphics enclosure: an 18 U
brick with one or two pipes, a Ktown2 interface board, a midplane, a power
supply and an L1 controller. It holds no processors or memory
(*established*, :src:`007-4264-002 pp. 10, 35`). Each pipe is a geometry
engine board (GE16-4), one or more raster manager boards (RM10 for
InfiniteReality3, RM11 for InfiniteReality4) and a display generator (DG5).
The brick has 11 rear board slots: Ktown2, six for pipe 0 and four for
pipe 1. Pipe 0 takes one, two or four raster managers and pipe 1 one or
two (*established*, :src:`007-4264-002 pp. 39-43`). The brick plugs
directly into 220 V and works without the power bay.

Connections:

- Ktown2 has two Crosstown2 connectors, the top for pipe 0 and the bottom
  for pipe 1 (*established*, :src:`007-4264-002 pp. 39, 62`).
- Each connector cables to an I-, X- or N-brick, which in turn cables to a
  C-brick (*established*, :src:`007-4264-002 pp. 8, 10, 62`). Which XBridge
  port of an I- or X-brick faces the Ktown2, and that the XBridge forwards
  between its two Crosstown2 ports, are *inferred* from the cabling
  figures and *unknown* as widget numbers.
- The L1 reaches the L2 over USB. Every documented G-brick configuration
  has an L2 (*established*, :src:`007-4264-002 pp. 63-68, 127-131`).

Two guides disagree on three G-brick L1 duties: voltage margining,
temperature monitoring and ID PROM reads. Those are *unknown*.

Catalogue: the ``g-brick`` layer, with the Ktown2 board and its two
Crosstown2 connectors, and ``g-brick-ir3-pipe``, one pipe with a GE16-4,
one RM10 and a DG5-2. The Ktown2 is a link extender with no widget
identity a host can read, so the pipe's XG ASIC is the widget on the I/O
brick's bus. The InfiniteReality pipe itself is the ``sgi-kona`` device
(:doc:`kona`). The Onyx 3400 with InfiniteReality3 builds two G-bricks
cabled through the N-brick (:ref:`sgi-sn-onyx3400-ir`).

.. _sgi-bricks-ibrick:

I-brick
=======

The I-brick is the base I/O brick. It holds one XBridge with eight ports:
Crosstown2 ports A (10) and B (11), :term:`Crosstalk` ports 8, 9, C and D, and PCI
ports E and F (*established*, :src:`108-0240-002 pp. 2-26 to 2-29`). In
the I-brick only A, B, E and F are wired. Ports A and B face compute
nodes. Port E drives bus 2, two slots at 33 or 66 MHz. Port F drives bus
1, three 33 MHz slots, and the on-board IO7 devices (*established*,
:src:`108-0240-002 pp. 2-28, 2-29`).

The IO7 holds an IOC3 (Ethernet, serial and clock), a Lucent USB OHCI
controller and a TI TSB12LV22 IEEE 1394 controller. A CD-ROM sits behind
the 1394 controller (*established*, :src:`007-4240-002 p. 130`). Published
inventories of an Onyx 3000, an Origin 3200 and an Origin 3400 print the
same base I/O bus with a QLogic 2200 Fibre Channel function at PCI slot 1,
the IOC3 at 4, the USB controller at 5 and the 1394 controller at 6
(*observed*; the table is in :ref:`sgi-sn-family-ibrick` below). The bus is
``xtalk/15`` in those inventories, so head F is widget 15 and head E widget
14 (*inferred* from the block diagram).

Connections:

- Rear connectors ``XIO 10`` and ``XIO 11``, the XBridge's ports A and B
  (*inferred* from the matching numbers). Either can go to a C-brick, and
  the second can go to another C-brick to make a dual-ported I-brick
  (*established*, :src:`007-4240-002 pp. 132-133`).
- Two real-time interrupt connectors for graphics (*established*,
  :src:`007-4240-002 p. 133`).
- An L1 controller. The L1 tells the two host ports apart as indices 1
  and 2 (*established*, :src:`007-4240-002 p. 106`).

Which host port a booting I-brick uses, the XBridge revision, and the
mapping from physical slot to PCI device are *unknown*. A verbose
Origin 3000 PROM boot would settle the first two.

Catalogue: ``modules/i-brick.yaml``. It holds the brick, an XBridge
(two heads) with its crossbar section, connectors for ports A (widget
``0xa``) and B (widget ``0xb``), and four PCI components on bus 1 (head F)
at the devices above: ISP2200 at 1, IOC3 at 4, USB OHCI at 5 and
TSB12LV22 at 6. Bus 2 (head E) has no device. The layer attaches the
C-brick at port A (*approximation*: port A is the documented
compute-node port, and the real choice is unknown). Port B is declared
and left uncabled. The component types are the ones modelled elsewhere:
:doc:`ioc3`, :doc:`lucent-ohci`, :doc:`tsb12lv22`, :doc:`qlogic-isp2200`
and :doc:`xbridge`. The I-brick's own L1, real-time interrupt connectors
and CD-ROM are not described.

.. _sgi-sn-family-ibrick:

Base I/O bus in published inventories
-------------------------------------

Inventories of an Onyx 3000, an Origin 3200 and an Origin 3400 print the
same base I/O bus at ``/hw/module/<c>/Ibrick/xtalk/15`` (*observed*,
three identified machines; :src:`nekonomicon-16719632`,
:src:`nekonomicon-16723604` and :src:`nekonomicon-16720581`):

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

They report I-brick board ``030-1557-007`` at revisions A, D and F, and
a second Fibre Channel function at ``xtalk/14/pci/1``. Widget 15 is port
F and widget 14 port E, matching the block diagram (*inferred*). The
``io7`` preset of ``sgi-baseio`` (:doc:`baseio`) carries the IOC3, USB
and IEEE 1394 devices at head F devices 4, 5 and 6.

.. _sgi-bricks-ixbrick:

IX-brick
========

The IX-brick is a 4 U base I/O enclosure, "an improvement on the I-brick":
12 PCI or PCI-X slot positions as six buses of two, with a preinstalled
IO9 card in bus 1 slot 1, leaving 11 for cards. It adds PCI-X, an optional
second pair of serial ports and, on the Onyx 3000, a four-port USB card in
bus 6 slot 2 (*established*, :src:`007-4604-001 pp. 1-3, 10-12`). Three
PIC ASICs provide two Crosstown2 ports and the six buses. As drawn, the
middle PIC faces the host ports and has one link to each of the other two
(*established*, as drawn, :src:`007-4604-001 p. 4`). Each bus runs at its
own speed and a PCI card in a bus forces it to PCI mode.

The IO9 carries an external SCSI connector, an internal SCSI port for the
two front drive bays, a Gigabit Ethernet connector, RTI and RTO connectors
and, through daughtercards, two or four RS-232 ports (*established*,
:src:`007-4604-001 pp. 3, 5, 10`). A DVD-ROM is in the front bay. Its chips
are a QLogic SCSI controller, a Broadcom BCM5701 and an :term:`IOC4`. That the
QLogic part is an ISP12160 is *inferred* from the Origin 350's IO9, which
carries one (:ref:`sgi-ip53-io-profile`).

Connections: rear connectors ``XIO 10`` and ``XIO 11``, each to one
compute brick, the second optionally to another; an L1 controller with a
display (*established*, :src:`007-4604-001 pp. 6, 9-10`).

An Origin 350 guest names its own compute module ``IXbrick`` in the
hardware graph. That module has a Bedrock and one PIC, and is not this
brick. Widget numbers of the PICs, PCI device numbers per slot, and the
PIC revisions are *unknown*. An inventory of a standalone IX-brick would
settle them.

Catalogue: no layer. The machine builder builds the enclosure from the PIC
(:doc:`pic`), IOC4 (:doc:`ioc4`) and BCM5701 (:doc:`bcm570x`)
(:ref:`sgi-sn-family-ixpx`).

.. _sgi-bricks-nbrick:

N-brick
=======

The N-brick is a 2 U connector brick for Onyx 3000 systems with no I/O
needs. It replaces up to four I- or X-bricks and joins up to four C-bricks
to up to four graphics pipes. It holds a printed circuit board with the
port connectors, a power board, an L1 controller with a display and hot-swap
fans (*established*, :src:`007-4264-002 pp. 9, 58-60`;
:src:`007-4424-001 p. 1`). The rear panel has four connectors labelled
``C-brick 0`` to ``C-brick 3`` and four labelled ``Graphics pipe 0`` to
``Graphics pipe 3``, each rated 800 MB/s in each direction and each with a
green hardware LED and a yellow software LED. The ``C-brick`` connectors
cable to C-brick ``XIO (II)`` connectors. The ``Graphics pipe`` connectors
cable to G-brick Ktown2 boards.

The guides name no ASIC in the brick, so it is *inferred* that it is passive
and each C-brick connector passes through to a pipe connector unmodified.
That connector ``C-brick n`` pairs with ``Graphics pipe n`` is *inferred*
from the drawing. Public OpenBSD names a board type for the N-brick and
never probes behind it (``xbow.c``). The link speed, the fan count (text
says two, the figure shows three) and how the L1 reaches four C-bricks are
*unknown*.

Catalogue: the ``n-brick`` layer holds four XBridges, one per board,
each joining one C-brick connector to one graphics pipe connector, with
no PCI and no XIO slots. The layer puts the C-brick on port A and the
pipe on port B of every chip, as the I-brick layer puts its C-brick on A
(*approximation*; which port faces the C-brick is *unknown*). The
real brick's L1 hears any of the four C-bricks, which no controller
record can describe, so the model gives the brick no L1. The Onyx 3400
with InfiniteReality3 builds it (:ref:`sgi-sn-onyx3400-ir`).

.. _sgi-bricks-pbrick:

P-brick
=======

The P-brick is the PCI expansion brick: twelve hot-pluggable 3.3 V or
universal PCI slots as six buses of two, at 33 or 66 MHz and 32 or 64 bits
(*established*, :src:`108-0240-002 pp. 2-30, 2-31`;
:src:`007-4311-002 pp. 31-33`; :src:`007-4240-002 pp. 140-142`). It holds
three XBridge ASICs. U0 serves buses 3 and 4 and is also the Crosstown2
interface for U1 (buses 1 and 2) and U2 (buses 5 and 6), joined by two links
to each. Two Crosstown2 ports, A (10) and B (11), run at 1.2 GB/s each
direction through host interface cards. The brick is 4 U and draws +48 V at
about 225 W (*established*, :src:`108-0266-001 pp. 36-37`).

The guest sees one crossbar and six bridge widgets, each numbered by the U0
port its chip hangs off: widget ``0x8`` bus 2, ``0x9`` bus 1, ``0xc`` bus
5, ``0xd`` bus 6, ``0xe`` bus 4 and ``0xf`` bus 3 (*inferred* from the
figure, a public Linux widget table and one inventory in a public post;
no P-brick inventory exists). Ports ``0xa`` and ``0xb`` face the hosts.

Connections: rear ``XIO 10`` and ``XIO 11`` to compute bricks, the second
optionally to a second compute brick; an L1 on a separate power board; a
serial ID EEPROM on the PCI motherboard.

The XBridge revisions, mode straps, what a host interface card contains and
the Crosstown2 power-on speed are *unknown*. The Origin 300 PCI expansion
module has the same three-XBridge wiring but a Bedrock interface card and
a NUMAlink connector, and is not the P-brick.

Catalogue: the ``p-brick`` layer and the ``origin3200-pbrick``
reference composition, described below. The nearest built device is the
Origin 300 expansion module in :doc:`pci-expansion`, on :doc:`xbridge`.

.. _sgi-sn-family-pbrick:

P-brick chips and links
-----------------------

The three chips are joined by four XIO links, two from U0 to each of the
others: U0 D to U2 D, U0 C to U2 8, U0 9 to U1 8 and U0 8 to U1 D
(*established*, :src:`108-0240-002 pp. 2-30, 2-31`, Figure 2-24). The
``p-brick`` layer records the chips, their ports, the six buses and the
twelve slots, and lists one XIO cable per link. ``origin3200-pbrick``
hangs the brick off the second C-brick at U0 port A. Port A is a choice of
the composition (*approximation*). The first C-brick carries the I-brick,
as in ``origin3200``.

The machine builder checks the four links like any other cable, then
refuses to build them, so no P-brick is built. The XBridge model takes one
host port, and a downstream chip's mode is not given by any source cited
here (*unknown*). Also *unknown* are whether U1 and U2 present each head
on its own link, which head rides which link, the PCI device number of
each slot and the slots' interrupt wiring. The widget numbering above
stays *inferred* for that reason. A document that names the downstream
chips' mode straps, or a P-brick widget inventory, would settle these.

The layer records the brick's L1 type letter as ``P`` (``0x50``, Linux
``ksys/l1.h``), which IRIX 6.5.30 and the stock IP35 PROM 6.210 name a
P-brick with module class ``p``, expecting a 600 MHz link, *software
expectation*. Nothing is built from it.

.. _sgi-bricks-pxbrick:

PX-brick
========

The PX-brick is the PCI-X successor to the P-brick: a 4 U enclosure with
twelve slots as six buses of two, each taking PCI or PCI-X cards at 33, 66,
100 or 133 MHz (*established*, :src:`007-4605-001 pp. 1, 8-9`). Three PIC
ASICs provide the buses and two Crosstown2 ports. As drawn, the two
external ports attach to the middle PIC, which has one link to each of the
others (*established*, as drawn, :src:`007-4605-001 p. 3`). The addendum
lists InfiniteReality pipe connectivity as an enhancement but describes no
connector, card or cable for it (*established*, p. 1), so that attachment
is *unknown*.

Connections: rear ``XIO 10`` and ``XIO 11`` (the second for an optional
dual-port arrangement), an L1 controller with a display, three hot-swap
fans, a slot power LED and a fault LED per slot.

How many links join the middle PIC to each other PIC, the PIC widget
numbers, head offsets and slot wiring are *unknown*. The Origin 350 PIC
contract does not transfer to this three-PIC enclosure.

.. _sgi-sn-family-ixpx:

IX-brick and PX-brick PIC enclosures
------------------------------------

Catalogue: no layer; the machine builder builds both enclosures.

Both bricks hold three PICs. The middle one serves buses 3 and 4, meets
the two Crosstown2 connectors on its ports A and B, and joins the lower
PIC (buses 1 and 2) and the upper PIC (buses 5 and 6) by one link each
(*established*, :src:`007-4604-001` Figure 2 and :src:`007-4605-001`
Figure 2). The model builds the middle PIC's integrated PXBOW as the
enclosure crossbar, with the host on port 0xa, the lower PIC on 0xc,
the upper on 0xd and the middle PIC's own heads on 0xf. Links present,
alive at startup and restored by a link reset are those four. That port
reading follows the XBridge port letters as hex digits and the public
Linux Altix brick table (*approximation*, not a measured board). Every
PIO, DMA and interrupt packet crosses the crossbar: no PIC has a direct
Bedrock peer. Each packet enters at its PIC's port and reaches the
Bedrock as a packet from that widget, so the Bedrock's inbound access
register governs it per PIC. Interrupt packets go to the host port the
cable meets, with the level in destination bits 63:48.

All six heads run ``pic-rev3-enclosure-experimental``: the PIC register
layout at revision 3, configuration device 0 reserved, the direct 32-bit
DMA window and the ``DIR_MAP`` field rule, and none of the IP53
compatibility behaviours (no held-input reassertion, no IP35 PROM reset
values or masks, no 32-bit control writes, no direct64 decode). Head 0
drives the lower-numbered bus. INTA of configuration device *d* drives
interrupt input *d* - 1; INTB to INTD reach no input and are logged. A
failed DMA send is logged by the PIC from its own result.

Model limits:

* Only single attachment is built: a cable to port 0xb, a cable from an
  Origin 350 compute module and a board on any other crossbar port are
  refused.
* Neither brick has an L1, and no on-board device is fitted. The
  IX-brick's IO9 waits on contracts for its IOC4 port presence, its
  QLogic part and its BCM5701 identity, so its bus 1 slot 1 is empty and
  logged as such.
* No catalogued machine builds either brick, because no reviewed
  configuration attaches one to a C-brick. The builder test builds them.

.. _sgi-bricks-rbrick:

R-brick
=======

The R-brick is the NUMAlink router: a 2 U brick with one SGI router ASIC
with eight NUMAlink 3 ports, a power board, an L1 controller and a USB hub
(*established*, :src:`007-4240-002 pp. 191-194`; :src:`007-4311-002 p. 36`).
It has no processors, memory or I/O. The ASIC is an arbiter and a crossbar
between ports, and no message leaves by its entry port (*established*,
:src:`108-0240-002 pp. 2-19 to 2-22`). A message is one or more 128-bit
micropackets, with four virtual channels per port. The architecture manual
gives the routing-table structure (a 16-entry local table and a 32-entry
remote table) but no register addresses.

Ports are lettered A to H on the rear and numbered 1 to 8 (*established*,
:src:`007-4240-002 pp. 194-196`):

- Ports 2 to 5 (B to E) carry USB through the cable. A C-brick must use
  one of these.
- Ports 1, 6, 7 and 8 (A, F, G, H) are for router-to-router links, though
  a router link may use any port.

One design fills four roles by placement: ordinary router (four C-brick
ports, four router ports), MetaRouter (systems above 128 processors) and
repeat router (above 256). Origin 3400 R-bricks ship with ports 7 and 8
disabled, a setting kept in the controller's non-volatile memory and changed
with L1 commands and a 48-hour authenticator (*established*,
:src:`108-0301-001 pp. 3-2, 3-3, 3-7 to 3-10`;
:src:`007-4311-002 pp. 36-37`; :src:`108-0266-001 pp. 40-41`;
:src:`108-0240-002 p. 2-20`). How the
controller makes the ASIC refuse two ports is *unknown*.

Catalogue: ``modules/r-brick.yaml``. It describes the chassis, one :term:`SN1`
router component with eight ports, an L1 and a USB hub, the eight port
connectors, each with a cable bus a composition attaches the far end to,
and a fabric bus for each. The router's ``node_ports`` names ports 2 to 5.
Resolution refuses a node cabled to any other port, and the builder
refuses such a cable in a topology it is handed, with the message "node
'<id>' is cabled to router '<id>' port <n>; it takes a node only on
ports 2, 3, 4, 5". A cable to another router may use any port. The layer
sets no L1 brick-type character: the router L1 profile answers ``R``
(``0x52``) for every router's controller, which is what the stock IP35
PROM 6.210 and IRIX 6.5.30 expect, *software expectation*, not a capture
of a fitted R-brick. It does not model the disabled-port variant. The SN1 router
device that the Origin 300 routed topology builds is described in
:doc:`sn1-router`.

The ``origin3400`` composition is the guide's 8- to 16-processor
interconnect drawn with one R-brick (:src:`007-4311-002 p. 55`): four
C-bricks on ports 2 to 5 and an I-brick on the first C-brick's
``XIO (II)`` connector at XBridge port A. Its composition rules follow
Appendix B (p. 89): one to eight C-bricks, one or two R-bricks and at
least one I-brick among at most eight I/O bricks. Every Origin 3400
bundle ships two R-bricks (pp. 76-78), which this composition does not
show. It is an emulation target. Each C-brick takes the Origin 3200 fit:
four 400 MHz R12000 processors with 8 MB secondary caches and 4096 MB in
eight 512 MB banks. The bundles ship C-bricks without memory and the
customer chooses the DIMMs, so the memory is a model choice
(*approximation*), as are the brick slot numbers (the guide places no
brick of this system in a slot) and the synthetic identities. Being
routed, the composition assigns no master or slave role to a C-brick. The
Origin 3200 forbids an R-brick.

.. _sgi-sn-origin3400-two-routers:

``-M origin3000,topology=origin3400-two-routers`` is the same four
C-bricks on the two R-bricks every bundle ships: C-bricks 0 and 1 on ports
2 and 3 of the first, C-bricks 2 and 3 on ports 2 and 3 of the second, and
two cables between the routers, port 1 to port 1 and port 6 to port 6. The
guide reserves ports 1, 6, 7 and 8 for R-brick links
(:src:`007-4311-002 pp. 36-37`) and draws two cables between the two
R-bricks of its 32-processor interconnect (:src:`007-4311-002 p. 56`) but
numbers no ports, so the port pairing and the C-bricks' ports are
model choices (*approximation*). The R-bricks take rack slots 19 and 25,
where the published Origin 3400 inventory places its two. The catalogue
expresses each router-to-router cable as a product bus that both routers
join at their port number, and each R-brick instance omits its own port 1
and port 6 buses (``omit_buses``), so no router port is on two cables.

The stock IP35 PROM 6.210 finds 6 objects (4 hubs, 2 routers), 16
processors, 16384 MB and both routers, passes its diagnostics and reaches
the menu. Before it hands any hub its node number it programs each
router's scratch 0 and protection configuration and then all eight links'
128 local and 16 global entries, with the next router's turn on the links
its hubs' traffic enters by (:ref:`sgi-sn1-router-routing`). Each router's
L1 serves the R-brick board record; without it the PROM prints an EEPROM
vector read error for that router. IRIX 6.5.30 boots from the Origin
3200's installed disk to a login, and ``hinv -v`` lists a router under
modules ``001c04`` and ``001c10`` with active ports 1, 2, 3 and 6.

.. _sgi-sn-origin3800:

``-M origin3000,topology=origin3800`` is an Origin 3800 of one compute rack
at 32 processors: eight four-processor C-bricks, four on ports 2 to 5 of
each of two eight-port R-bricks, the routers joined by two cables, port 1 to
1 and 6 to 6, and an I-brick on the first C-brick. The guide establishes the
shape: a compute rack holds up to eight C-bricks and three R-bricks, every
32-processor bundle ships two R-bricks, the interconnect drawing shows ten
cables (the eight C-brick cables and two more), and the bandwidth table
joins the routers with two cables unless the high-bandwidth option is bought
(:src:`007-4311-002 pp. 56, 79-80, 93`). It numbers no ports. The port
assignment is the preliminary cabling draft's (*inferred*; the draft is
preliminary and labels this shape a 3600 Series system). The draft's rack
units give the C-bricks module numbers 10, 13, 16, 21, 24, 29, 32 and 35 and
the R-bricks 19 and 27. The I-brick is model-only (*approximation*): a real
Origin 3800 puts its I-bricks in a separate I/O rack, and which C-brick XIO
port that rack's cables reach is unknown. Each C-brick is fitted as the
Origin 3400's is, with 4096 MB.

``topology=origin3800-high-bandwidth`` adds the four-cable fit, which also
joins ports 7 to 7 and 8 to 8. That the extra cables use equal port numbers
is the draft's table (*inferred*) and the fit is not an orderable option.

.. _sgi-sn-rack-numbers:

A topology may number its racks with ``machine.emulation.rack_numbers``, a
list of ``{component, number, reason}`` entries that name rack chassis
components. A module takes the number of its nearest numbered chassis
ancestor, and ``initial_module_numbers`` stays the bay. That a brick's L1
keeps a rack number beside its bay, and that the PROM turns the pair into a
module identifier, is *established* by a replay of the PROM's encoder. The
model checks these rules only when racks are numbered:

* The number is one of the 64 the encoder accepts: 001 to 008, 011 to 018,
  021 to 028, 031 to 038, and 101 to 104, 111 to 114 and so on to 171 to
  174 (*established*, same replay). The L1 stores other numbers and the
  PROM then fails to build an identifier for that brick.
* The bay is 1 to 39, the range the Origin 300 L1 package's ``brick slot``
  command accepts (*established* for that package; which L1 package an
  Origin 3800 uses is *unknown*). A (rack, bay) pair is unique, so two
  racks may both have a bay 10. A Cx-brick's router port takes its bay plus
  four, so the pair is claimed at both bays, and a Cx-brick whose router
  bay would pass 39 is rejected (*approximation*: the offset is the L1
  model's board numbering, not a captured hardware fact).
* Every numbered rack holds a module and every module sits in a numbered
  rack. This is a rule of the model's schema (*approximation*).
* For the Origin 3800, a compute rack (number below 100) holds only C- and
  R-bricks, at most eight C-bricks and two R-bricks, and an I/O rack
  (number 100 and up) holds only I-, P-, X- and D-bricks, at most eight.
  The rack contents and the I/O-rack limit of eight are *established* by
  the cabling guide's rack tables. A shipped compute rack takes up to three
  R-bricks; the two-R-brick cap is a model cap, because the third is the
  MetaRouter slot, whose wiring the PROM accepts is *unknown*.

Topologies that number no rack keep the module number rules above
unchanged: unique across the whole system, 1 to 255. No shipped topology
numbers a rack yet, and the compiled catalogue carries no rack field for
any of them. A numbered topology's module records carry ``rack``. The
machine builder does not read it yet, so every L1 answers rack 1. The
catalogue's QAPI module record has no ``rack`` field, so a topology that
numbers a rack cannot be catalogued until the builder stage adds it.

Model limits of a routed Origin 3000 build:

* The L2 controller, which a rack holding an R-brick requires
  (:src:`007-4311-002 p. 42`), is not modelled, nor is the R-brick's USB
  hub traffic to the C-brick L1s. Running without them is a limit, not a
  claim that the hardware does.
* The router presents all eight ports, not the six an Origin 3400
  enables.
* Its forwarding is the routed Origin 300 experiment's
  (:doc:`sn1-router`), which no R-brick register contract confirms. The
  table shapes, the destination ID mapping and whether an entry names an
  absolute port or a turn are *unknown*.

.. _sgi-bricks-vbrick:

V-brick
=======

The V-brick is the enclosure for VPro (Odyssey) graphics on the Onyx 3000,
sold as InfinitePerformance. It holds one or two pipes. Each pipe is one
V12 board occupying two of the four board slots, and a midplane XBridge is
the interface between the brick's two ports and the V12 boards
(*established*, :src:`007-4264-002 pp. 10, 52, 54`). The brick has its own
L1 controller, takes 48 VDC from the power bay and cables directly to a
C-brick (*established*, :src:`007-4264-002 pp. 8, 52, 55, 128-129`). Each
pipe needs a dedicated C-brick, and a system holds V-bricks or G-bricks,
not both (*established*, :src:`007-4264-002 pp. 10, 19, 21`).

Connections:

- Two rear connectors, ``XIO 10`` and ``XIO 11``, labelled Port A and
  Port B in the block diagram and rated 800 MB/s. One cables to a C-brick;
  with one pipe fitted the second may cable to another C-brick.
- The CX-brick's addendum names the V-brick as compatible with its four
  XIO connectors (*established*, :src:`007-4572-001 pp. 2, 5, 10`).
- A host interface board sits between the connectors and the XBridge
  (*established*, as drawn). Its function is *unknown*.
- The four slot links are *inferred* to be the XBridge's Crosstalk ports
  8, 9, C and D, one per slot.

No V-brick has been measured or inventoried. The board revision, the V12
widget number and the link rate (800 MB/s here, 1.2 GB/s for the same
chip's other cabling) are *unknown*.

Catalogue: the ``v-brick`` layer, a midplane XBridge between the two
rear XIO ports and four V12 board slots; the composition places the V12
boards. :ref:`sgi-sn-family-vbrick` describes the Onyx 3000 that builds
it.

.. _sgi-sn-family-vbrick:

Onyx 3000 V-brick machine
-------------------------

The ``onyx3000`` topology (``-M origin3000,topology=onyx3000``) puts the
I-brick on the first C-brick and a one-pipe V-brick on the second, with
the cable on the midplane XBridge's port A and the V12 on its Crosstalk
port 8. Both ports are the composition's choices (*approximation*).
Which connector is port A, which Crosstalk port serves a slot, and so
the V12's widget number, are *unknown*, and a PROM inventory or hardware
graph of an Onyx 3000 with a V-brick would settle them. The V-brick
shares the X-brick's chassis, whose slot map puts slots 1 and 2 at
Crosstalk ports 8 and 12, so one V12 is *inferred* to sit at 8 or 12.

The V-brick has an L1 (``brick-type=vbrick``, :ref:`sgi-l1-origin3000`),
reached from the C-brick its cable joins. It answers ``X`` with the
X-brick's power and logic board records, *inferred*: the V-brick is an
X-brick chassis and the L1 firmware takes the letter from the logic
board's name, and no V-brick L1 reply has been captured. Its XBridge
answers at both PCI heads, widgets 14 and 15, with nothing on them, as
every XBridge does (:src:`108-0240-002 p. 2-26`), *established*. IRIX
6.5.30 needs an I/O brick record for each crossbar widget it sets up, the
V12 included, and the stock PROM builds those records only from live
heads, *software expectation*.

No source fixes an Onyx 3000 C-brick's processors, caches or memory, so
each C-brick takes the Origin 3200 C-brick's fit (four 400 MHz R12000s
with 8 MB caches and 4096 MB), a model choice. The V12 is fitted by
default and ``-vga none`` leaves its port empty. On the model the stock
PROM lists the V12 as a graphics board in the second C-brick's module,
builds the V-brick's I/O records from its heads and diagnoses all four
processors on each node. For a brick that answers ``X`` it expects the
400 MHz link the C-brick's L1 reports, which agrees with the guide's
800 MB/s port rate (:src:`007-4264-002 p. 54`). No IRIX boot with the
V12 fitted is recorded for this V-brick identity. A two-pipe V-brick, whose second pipe needs a second
C-brick on the other Crosstown2 port, is refused: the midplane XBridge
takes one host (:ref:`sgi-ip35-brick-composition`).

.. _sgi-bricks-xbrick:

X-brick
=======

The X-brick is the legacy XIO expansion brick. It holds one XBridge on the
midplane with its four Crosstalk ports each serving one XIO slot, so cards
from Origin 2000 and Octane systems can move to an Origin 3000
(*established*, :src:`108-0240-002 pp. 2-32, 2-33`). The slot-to-port
mapping is slot 1 to port 8, slot 2 to 9, slot 3 to C and slot 4 to D, drawn
at 400 MHz (*established*, :src:`108-0240-002 p. 2-33`). The PCI ports E and
F are not used by any drawing. The brick is 4 U, +48 V at about 225 W, and
each slot takes one card of at most 50 W (*established*,
:src:`007-4311-002 p. 35`; :src:`007-4240-002 p. 154`).

Connections: rear ``XIO 10`` and ``XIO 11``, the second optionally to another
C-brick, drawn behind a single host interface card; an L1 controller. The
X-brick is also a G-brick intermediary (see above).

The supported cards are single-port serial HIPPI, dual-attach FDDI, one- and
two-port copper GSN, digital video, high-definition video, 6U and 9U VME
adapters and, in the earlier configuration guides only, a four-port ATM
board (*established*, :src:`007-4240-002 p. 154`;
:src:`007-4311-002 p. 50`). Graphics boards are not supported. Which
connector is port A (the guides conflict), the XBridge revision, and the
per-board widget identities are *unknown*.

Catalogue: the ``x-brick`` layer and the ``origin3200-xbrick`` reference
composition, described below.

.. _sgi-sn-family-xbrick:

X-brick XIO slots
-----------------

The ``x-brick`` layer holds the one XBridge, ports A and B for compute
nodes and one XIO slot on each of ports 8, 9, C and D, slots 1 to 4 in
that order (*established*, :src:`108-0240-002 pp. 2-32, 2-33`,
Figure 2-25). The figure draws nothing on the PCI ports E and F, so the
layer declares no PCI bus. The slots stay unresolved in the catalogue:
no XIO board is specified for them.

An Origin 3200 C-brick has one ``XIO (II)`` connector (*established*,
:src:`007-4240-002 p. 125`). With the I-brick on the first C-brick, an
X-brick hangs off the second, and ``origin3200-xbrick`` is that
composition. It cables the X-brick at port A as a choice
(*approximation*). Whether rear connector ``XIO 10`` or ``XIO 11`` is
port A is *unknown*, because the two guides disagree. A source that ties a
connector label to a port would settle it. The layer records the brick's
L1 type letter as ``X`` (``0x58``, Linux ``ksys/l1.h``), which IRIX 6.5.30
and the stock IP35 PROM 6.210 name an X-brick with module class ``x``,
expecting a 400 MHz link, *software expectation*. Nothing is built from
the letter. The XBridge revision and the widget numbers a C-brick sees
for the slots are *unknown*; only an X-brick widget inventory would
settle them.

The builder qtest does build this layout. It places Origin 2000 PCI
carrier cards in slots 1, 2 and 4 as stand-ins (*approximation*), leaves
slot 3 empty, and reaches each card only from the C-brick cabled at the
chip's host port.

.. _sgi-bricks-numalink-module:

NUMAlink module
===============

The NUMAlink module is the Origin 300's router enclosure. It joins Origin
300 base modules over NUMAlink 3 through eight external ports and holds an
L1 controller and an internal USB hub whose rear L1 connector goes to the
rack's L2 (*established*, :src:`007-4498-002 pp. 10, 12, 96, 130, 132`).
It is in AC and DC versions. The L2 is a separate rack-level controller and
is not a chip in the module.

Ports are lettered A to H and numbered 1 to 8 (*established*,
:src:`007-4498-002 pp. 10-11, 96`):

- Ports 1, 6, 7 and 8 take base modules only and carry no USB.
- Ports 2 to 5 take base or PCI expansion modules and carry USB to the L2.

A system holds two to eight base modules in a tall rack and two to six in a
short rack, with the lowest base module the global master (*established*,
:src:`007-4498-002 pp. 2, 16, 93, 98`). A published six-chassis Origin 300
inventory reports the router board as part ``030-1634-003`` revision ``-A``
and the router as ``Revision 1`` (*observed*, :src:`nekonomicon-16727514`).
The fitted ASIC part, its count and its silicon revision are *unknown*. The
manuals do not show eight ports on one chip. Public Linux SN1 headers define
an eight-port router family with chip identifier ``0x3017``, which makes the
mapping from family to product *inferred* (see :doc:`sn1-router`).

Catalogue: ``modules/numalink-module.yaml``, composed with two Origin 300
base modules in ``origin300-numalink-module.yaml`` (a reference model that
cannot compile or run). The layer has the enclosure, an SN1 router
component with eight ports, an L1, a USB hub, and connectors 1 to 8 on
fabric buses. It selects no ASIC revision and no cable between bases and
ports. The runnable routed topology ``origin300-numalink-pci-expansion``
does not use this layer. It builds one ``sgi-sn1-router`` directly and
cables its ports 1, 4 and 6 to two base modules and an expansion module.

.. _sgi-bricks-measurements:

Measurements
============

No brick has been measured. The *observed* statements above come from
published inventories.

.. _sgi-bricks-limits:

Model limits
============

- The Origin 3200, Origin 3400, Onyx 3000 and Onyx 3400 with
  InfiniteReality3 are built. ``origin300-numalink-module.yaml``,
  ``origin3900.yaml``, ``onyx3400.yaml`` and the Origin 3200 P-brick,
  X-brick and D-brick compositions are reference models, and the compiler
  stops with an error naming ``model_role 'reference'``. The builder
  refuses the P-brick's four chip-to-chip links.
- The IX- and PX-brick have no layer. The composition rules can name
  ``p-brick``, ``x-brick``, ``g-brick``, ``n-brick`` and ``v-brick``
  alongside the layers that exist. A rule that names a brick with no layer
  counts zero of it, so a misspelled id in a maximum, exclusion or
  requirement rule passes silently.
- The model validator accepts an L1 brick-type character only for the two
  boards a source establishes, ``C`` (IP35) and ``4`` (IP45). An I-, P-,
  R- or other brick cannot carry one.
- The layers describe topology and identity, not behaviour. They carry no
  register map, reset value, link timing or interrupt route for any brick.
  Router tables, Crosstown2 speed selection and L1 requests for bricks
  other than the node boards are not modelled.
- The C-brick layer holds no memory fit. The I-brick layer attaches only
  host port A.
- The V-brick and N-brick have no L1. A guest request to a part that does
  not exist gets no brick-specific answer.

.. _sgi-bricks-tests:

Tests
=====

``scripts/tests/test_sgi_model_validation_contract.py`` has
``Origin3000BrickValidationTests``. They check that the Origin 3200 reference
validates. They also check that the I-brick's connectors record XBridge ports
``0xa`` and ``0xb`` and that a connector port must be a crossbar XIO port, be
unique and sit on a crossbar, that the C-brick cables to I-brick port A, that
an XIO cable has two distinct XIO connectors that share a bus and carries
evidence, and that the C-brick's L1 brick type is ``C`` while any other letter,
or a letter on a non-chassis, is refused. The same class validates the P-brick
and X-brick compositions and refuses a port that two P-brick cables share or
two X-brick connectors name.

``scripts/tests/test_sgi_model_resolution_contract.py`` checks that the C-brick
has one ``XIO (II)`` connector with a Crosstown2 interface on an ``xio`` bus
mastered by its Bedrock, that the I-brick attaches to the C-brick through a bus
binding, that the R-brick's router reaches its cable through a binding and
fails without one, the composition-rule counts, exclusions and requirements
(including ``g-brick`` against ``v-brick`` and ``n-brick`` only with
``g-brick``). It also checks that the Origin 3200 resolves to two C-bricks and
an I-brick and refuses a third C-brick, no I-brick, three I/O bricks or an
R-brick, and that it still does not compile.

``scripts/tests/test_sgi_model_resolution_contract.py`` also checks the
X-brick layer (four slots on ports 8, 9, C and D, port B and the slots on
the crossbar, no L1 type), the P-brick layer (three crossbars, the four
Figure 2-24 links, two slots on each chip's buses), that each brick hangs
off the second C-brick of its Origin 3200 composition, that the X-brick
counts against the I/O-brick limit and that the X-brick composition does
not compile.

``scripts/tests/test_sgi_model_rack_numbers.py`` checks the ``rack_numbers``
schema and each rule above on the Origin 3800 with its bricks moved into
numbered racks: a rejection for an invalid rack number, a repeated rack or
rack number, a bay of 0 or 40, a repeated (rack, bay) pair, a module outside
any numbered rack, an empty rack, an I-brick in a compute rack, a C-brick in
an I/O rack, a brick with no letter and too many C-, R- or I/O bricks, and
acceptance of two racks that share a bay. It also checks that the Origin
2000 module number rules and the unnumbered SN1 rules are unchanged, that a
Cx-brick claims its router bay and none passes bay 39, that a null
``rack_numbers`` leaves the module number rules in force, that a module
record carries ``rack`` only in a numbered topology and that no shipped
topology numbers a rack.

``tests/qtest/sn-builder-test.c`` builds the C-brick fixtures from
``tests/qtest/sn-builder-fault-test.c``. ``sn/builder/cbrick/origin3200-xbrick-slots``
checks that each filled X-brick slot answers only the C-brick cabled at
the chip's host port, at its own port, and that empty slot 3 decodes
nothing. The N-brick-shaped fixture (``nbrick-own-boards``) checks four
chips named ``xbridge0`` to ``xbridge3``. The refusal cases under
``sn/builder/cbrick/refused/`` check the P-brick's four links (``pbrick``),
a reused P-brick port, a chip revision the XBridge model does not present,
and the board-cable errors for an X-brick slot.

``tests/qtest/sn1-l1-test.c`` covers the V-brick controller profile
(``sn1/l1/vbrick/reference-refusals``, ``profile-mismatch`` and
``compat-refusal``): refused identity and lifecycle queries, and a refused
migration between mismatched profiles. ``tests/qtest/board-test-fixture.c``
builds the V-brick projection as a fixture for the board qtests.
