================================
SGI Scalable Node family machine
================================

SGI's Scalable Node machines come in two generations. SN0 is built from
the Hub node controller and covers the IP27 platform: the Origin 200,
Origin 2000 and Onyx2 (:doc:`ip27`). SN1 is built from the Bedrock node
controller and covers the IP35 platform: the Origin 300, Fuel, Origin
350, Onyx 350, Tezro and Onyx 4 host (:doc:`ip35`). QEMU builds both from
one machine base, ``sgi-sn`` (``TYPE_SGI_SN_MACHINE``,
``hw/mips/sgi/sn-machine.c``). This page describes what the two
generations share: the address model and PROM format, the catalogue that
decides which boards a machine fits, how boards and graphics pipes are
composed, the firmware and storage a launch supplies, the serial-line
order and the launch metadata. Machine-specific content is on the
platform pages and on the part pages listed under :ref:`sgi-sn-devices`.

Facts are rated as described in :ref:`sgi-ratings`.

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

.. _sgi-sn-product-family:

Product family
==============

.. _sgi-sn-family-table:

Family summary
--------------

.. list-table::
   :header-rows: 1

   * - Machine
     - Platform and node board
     - Node controller
     - Source
   * - ``origin200``
     - IP27 platform, IP29 system board
     - Hub
     - ``hw/mips/sgi/sn-machine.c``, node in ``hw/mips/sgi/sn0.c``
   * - ``origin2000``, ``onyx2``
     - IP27 platform, IP27 node boards
     - Hub
     - as ``origin200``
   * - ``origin300``
     - IP35 platform, IP45 board
     - Bedrock
     - ``hw/mips/sgi/sn1.c``, node in ``hw/mips/sgi/sn1-node.c``
   * - ``fuel``
     - IP35 platform, IP34 board
     - Bedrock
     - as ``origin300``
   * - ``origin350``, ``onyx350``, ``tezro``, ``onyx4-host``
     - IP35 platform, IP53 board
     - Bedrock
     - as ``origin300``

The Onyx2 is Origin 2000 silicon with graphics: the same IP27 node
boards, memory and stock PROM, told apart only by the hardware its
topologies fit. The IP53 machines are software profiles of one capture
or manual each, not qualified models of every configuration of the
product (:ref:`sgi-ip35-ip53`). The IP35 and IP53 machine classes parent
``sgi-sn`` like the IP27 ones and take the same ``topology=`` contract
and compiled catalogue. Only the node device is product-specific.

A Hub carries two processors on one processor interface
(*established*, :src:`007-3439-002 Figure 2-1`). A Bedrock carries up to
four on two interfaces (*established*, :src:`108-0240-002 p. 2-12`).
The node controller pages are :doc:`hardware/hub` and
:doc:`hardware/bedrock`. IP27 I/O is a Bridge, behind a Crossbow where
the module has one. IP35 I/O is an XBridge, or a PIC on the IP53 board
(:doc:`hardware/bridge`, :doc:`hardware/xbow`, :doc:`hardware/xbridge`,
:doc:`hardware/pic`). The system controller is the ELSC or the module
system controller on IP27 (:doc:`hardware/elsc`) and an L1 over a UART
on IP35 (:doc:`hardware/l1`). Those pages rate and cite each part.

The subsections below hold the brick and product facts that the model
catalogue cites on this page. The platform pages and :doc:`hardware/bricks`
own the rest of each product's description.

.. _sgi-sn-family-origin2000-xpress:

Origin 2000 two-module racks and Xpress links
---------------------------------------------

A rack holds two modules. Within and between modules, Link 1 runs
vertically between the upper and lower Routers of the same letter, and
Link 2 is the Xpress link, which must cross. Xpress links are only used
in systems of 16 or 32 processors (*established*,
:src:`007-3439-002 p. 59`). No source cited here draws the full
cable-by-cable wiring, and which SGI document does is *unknown*. The
``origin2000-two-module-xpress`` layer records the link rule as a
reference configuration. No machine builds it, and its fitted memory is
a model choice (*approximation*).

.. _sgi-sn-family-origin3000-cbrick:

Origin 3000 C-brick
-------------------

The C-brick has no crossbar of its own. Its Bedrock reaches I/O through
a Crosstown2 port into an I-, P- or X-brick's XBridge, whose crossbar
section does the switching (*established*, :src:`108-0240-002 p. 2-26`).
It has one rear ``XIO (II)`` connector, which joins it to an I-, P- or
X-brick (*established*, :src:`007-4240-002 p. 124`). The layer's
C-brick buses, one SysAD per processor interface and the ``xio-ii`` bus
from the Bedrock to that connector, follow from this. Which port of the
far brick's XBridge the cable reaches is the composition's choice, not
the C-brick's. The processors, DIMM banks and cabling are on
:ref:`sgi-bricks-cbrick`.

The eight DIMM slots form four pairs. Banks 2n and 2n+1 sit on pair n
and both DIMMs of a pair match, so the two banks of a pair are fitted
alike; pairs may differ (:src:`108-0240-002 pp. 2-5, 2-6`),
*established*. The Origin 3200 catalogue entry fixes each C-brick at the fit
a posted Origin 3200 inventory (:src:`nekonomicon-16723604`, *observed*)
reports, four 400 MHz R12000s with 8 MB secondary caches on two PIMMs
and 4096 MB in eight 512 MB banks, and resolution refuses any other
processor or bank population for that product.

The entry is the ``origin3000`` machine, topology ``origin3200``. Its
C-brick values the sources do not settle are model choices:

* The board-identification word is ``0x2``. Product code 0 in bits 15:12
  is a C-brick (*established*, OpenBSD ``ip27.h``). The low nibble 2
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
~~~~~~~~~~~~~~~~~~~

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
* The IO7's IEEE 1394 link controller is left out, because its model has
  no PHY or bus and the stock PROM's probe waits on them.
* The IO7's time-of-day part is unidentified, so its IOC3 carries the
  Origin 300's M48T35-compatible Timekeeper as a compatibility choice
  (*approximation*), with blank storage on every start.
* Head E's PCI slots are empty. The I-brick's XBridge revision is the
  model's.

.. _sgi-sn-onyx3400-ir:

Onyx 3400 with InfiniteReality3 machine
~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~

``-M origin3000,topology=onyx3400-ir`` builds the Figure 1-10 brick set
of the Onyx 3000 graphics guide (:src:`007-4264-002 p. 27`) as a
catalogue entry, ``onyx3400-ir``. The ``onyx3400`` composition stays the
reference of the same brick set. The machine has four C-bricks on ports
2 to 5 of one R-brick, an I-brick on c0's ``XIO (II)`` connector, an
N-brick whose first three chips carry c1 to c3, and two G-bricks. It has
16 R12000s, four per C-brick, and 16384 MB. The stock PROM 6.210 finds 4
nodes, 16 CPUs and one router, and its diagnostics summary reads ``ALL
DIAGS PASSED``. Its ``hinv -v`` lists the four node boards, the
I-brick's two heads and the router, and an XBow for the I-brick and for
the N-brick chips of c10 and c13, but none for c07's.

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

Model limits:

* Three pipes, not four. A pipe cabled to a free port of the I-brick's
  XBridge makes the PROM of the C-brick that hosts that chip print
  ``iobrick_attached_modids failed``; on the I-brick that C-brick is the
  global master, which then finds no base I/O. What an I/O brick's port
  B presents to that query when a graphics pipe is cabled to it is not
  established, so I-brick port B is left uncabled. G-brick 0 holds two
  pipes and G-brick 1 one, on the N-brick's three pipe ports.
* The graphics contract builds one pipe at once, so the machine offers
  the four pipe slots and fits at most one ``sgi-kona``. The stock PROM
  inventory lists no pipe with one fitted.
* The C-bricks cabled to the N-brick print ``Unexpected I/O brick type``
  at start: the N-brick has no L1 model, and what an N-brick answers to
  the attached-brick request is *unknown*. They still join the PROM's
  node count.
* The router's ports 1, 6, 7 and 8 are uncabled, and the PROM prints
  ``discover_object: vector read 0x2 LRI failed`` and ``erect_fences``
  messages and an EEPROM vector read error against the router.
* IRIX has not been booted on this topology.

.. _sgi-sn-family-ibrick:

I-brick host ports and base I/O
-------------------------------

The XBridge in an I-brick has ports A (widget 10) and B (widget 11) for
compute nodes, port E for bus 2 and port F for bus 1 with the IO7
devices (*established*, :src:`108-0240-002 pp. 2-26 to 2-29`;
:ref:`sgi-bricks-ibrick`).

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
F and widget 14 port E, matching the block diagram (*inferred*).
*Unknown*: the I-brick XBridge revision, and which host port a booting
I-brick uses. A verbose Origin 3000 PROM boot would settle both, and
none is published.

The X-brick and P-brick are on :doc:`hardware/bricks`.

.. _sgi-sn-family-ixpx:

IX-brick and PX-brick PIC enclosures
------------------------------------

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
* No catalogued machine builds either brick yet, because no reviewed
  configuration attaches one to a C-brick. The builder test builds them.

.. _sgi-sn-family-vbrick:

Onyx 3000 V-brick
-----------------

A V-brick holds one or two InfinitePerformance pipes, each one VPro V12
board taking two of four board slots, behind an XBridge on its
midplane. Its two rear XIO ports each cable to a C-brick's ``XIO (II)``
connector, and each pipe needs a C-brick of its own, besides the one
that hosts the system I-brick (*established*, *SGI Onyx 3000 Series
Graphics System Hardware Owner's Guide*, :src:`007-4264-002 pp. 8, 10,
19, 52-56`). A system holds G-bricks or V-bricks, never both
(*established*, :src:`007-4264-002 pp. 10, 21`). The enclosure is on
:ref:`sgi-bricks`.

The ``onyx3000`` topology (``-M origin3000,topology=onyx3000``) puts the
I-brick on the first C-brick and a one-pipe V-brick on the second, with
the cable on the midplane XBridge's port A and the V12 on its Crosstalk
port 8. Both ports are the composition's choices (*approximation*).
Which connector is port A, which Crosstalk port serves a slot, and so
the V12's widget number, are *unknown*, and a PROM inventory or hardware
graph of an Onyx 3000 with a V-brick would settle them. The V-brick's L1
is not modelled, since its address and identity are unknown. No source
fixes an Onyx 3000 C-brick's processors, caches or memory, so each
C-brick takes the Origin 3200 C-brick's fit (four 400 MHz R12000s with 8
MB caches and 4096 MB), a model choice. The V12 is fitted by default and
``-vga none`` leaves its port empty. The stock PROM lists the V12 as a
graphics board in the second C-brick's module and diagnoses all four
processors on each node. IRIX 6.5.30 boots to a login without the V12
and stops in an early kernel assertion with it; the cause is not
established.

.. _sgi-sn-family-rbrick:

R-brick and NUMAlink module ports
---------------------------------

The R-brick router has eight ports, 1 to 8, lettered A to H. A C-brick
connects only to port 2, 3, 4 or 5, since only those cables carry the
USB signals that reach its L1. Router-to-router links normally use 1, 6,
7 and 8 but may use any port (*established*, :src:`007-4311-002 pp.
36-37`; *SGI Origin 3000 Series Owner's Guide*, :src:`007-4240-002 p.
196`). An Origin 3400 R-brick ships with two of its ports disabled
(:src:`108-0240-002 p. 2-20`). Which two, and how, no source cited here
establishes (*unknown*). No SGI document cited here names the router
ASIC itself (*unknown*; :ref:`sgi-bricks`).

The ``r-brick`` model layer is the enclosure: the router, its L1 and USB
hub, and the eight port connectors, each with a cable bus a composition
attaches the far end to. The router's ``node_ports`` names ports 2 to 5.
Resolution refuses a node cabled to any other port, and the builder
refuses such a cable in a topology it is handed, with the message "node
'<id>' is cabled to router '<id>' port <n>; it takes a node only on
ports 2, 3, 4, 5". A cable to another router may use any port. The layer
sets no L1 brick-type character, since no reviewed L1 contract gives the
R-brick's.

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
routed, the composition assigns no master or slave role to a C-brick.

Model limits of a routed Origin 3000 build:

* The L2 controller, which a rack holding an R-brick requires
  (:src:`007-4311-002 p. 42`), is not modelled, nor is the R-brick's USB
  hub traffic to the C-brick L1s. Running without them is a limit, not a
  claim that the hardware does.
* The router presents all eight ports, not the six an Origin 3400
  enables.
* Its forwarding is the routed Origin 300 experiment's (below), which no
  R-brick register contract confirms. The table shapes, the destination
  ID mapping and whether an entry names an absolute port or a turn are
  *unknown*.

The Origin 300 NUMAlink module is a different product. Published
inventories report its router board as another revision of the R-brick
board's part family; whether the two carry the same router ASIC revision
is *unknown*. Ports 1, 6, 7 and 8 take base modules only. Ports 2 to 5 take base or PCI expansion modules
and carry USB to the L2 controller (*established*,
:src:`007-4498-002 pp. 4, 10-11, 16, 93-98`). In the tall rack
(figure 4-2) the cabling runs:

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

The mapping is *established* from the manual's figure. A live
six-chassis rack whose modules at U12, U14, U18, U22 and U24 reported
active ports 1, 4, 5, 7 and 8 agrees with it (*observed*,
:src:`nekonomicon-16727514`). The layer's eight router-port buses carry
these port numbers.

The routed Origin 300 topology selects one logical eight-port SN1 router
(:doc:`hardware/sn1-router`). The public Linux v2.5.0 tree
(``linux-history:381ab1f538162eb985c9244aa7d56e54315881ab``) names router
chip identity ``0x3017`` in ``include/asm-ia64/sn/sn1/bedrock.h`` lines
19-20 and defines the router status fields in
``include/asm-ia64/sn/sn1/router.h`` (*software expectation*). A
published six-chassis Origin 300 inventory reports router revision 1
(:src:`nekonomicon-16727514 lines 390-409 and 1860-1869`). The model
selects chip revision one by inference from that software report
(*inferred*). Neither the inventory nor the manuals measure the fitted
silicon or establish how many router ASICs the enclosure holds. Chip
inputs of zero are an emulator choice (*approximation*).

The router's L1 type is ``0x52`` (``R``), a software profile and not a
measured controller reply (*approximation*). Linux v2.5.0
``include/asm-ia64/sn/ksys/l1.h`` line 266 assigns uppercase ``R`` to
that code. Its ``arch/ia64/sn/io/l1_command.c`` lines 413-436 normalise
uppercase types for module names, and lines 1031-1074 identify the
router module independently of the returned type (*software
expectation*). The NUMAlink module guide establishes a separate L1
controller (:src:`007-4498-002 pp. 14, 96, 130`) but gives no raw type
response. A binary type-query capture from a matching controller would
settle that byte. The module, its ports and the routed topology are on
:ref:`sgi-bricks-numalink-module` and :ref:`sgi-ip35-numalink`.

.. _sgi-sn-family-origin350:

Origin 350 compute module
-------------------------

The ``origin350`` machine is a one-node software profile. It is an
emulation target with explicit model choices, not a claim that its
memory, firmware seed, PIC setup or board wiring reproduce a measured
machine (:ref:`sgi-ip35-origin350`). The hardware facts the layers cite
are:

- Two or four R16000, 4 MB of secondary cache each, and eight DIMM slots
  in four pairs, each pair feeding a pair of banks, 1 to 8 GB
  (*established*, :src:`007-4566-001 pp. 69, 114, 116`).
- One Bedrock, with NUMAlink 3 on one external LINK connector that is
  empty in a one-module system (*established*, :src:`007-4566-001 p.
  69`). Revision 2 at 200 MHz is *observed* in one Origin 350 inventory
  (:src:`nekonomicon-16729141`).
- An L1 controller with a display, and an L2 only with a NUMAlink module
  (*established*, :src:`007-4566-001 pp. 68-75`).
- A PIC with two PCI-X heads on one widget, the second head's registers
  ``0x800000`` above the first (*software expectation*: OpenBSD
  ``sys/arch/sgi/xbow/xbridgereg.h`` lines 24-28 and ``xbowdevs``, part
  ``0xd102``, second head ``0xd112``, manufacturer ``0x024``, at
  ``2df91d80c104636deeef8124252ddd5951b55fbf``). Widget 15 is *observed*
  in one Origin 350 inventory (:src:`nekonomicon-16724207`).
- The IO9 card in the lowermost slot, on bus 1 (*established*,
  :src:`007-4566-001 p. 69`). The IOC4 ``10a9:100a`` is at configuration
  device 1, the ISP12160 ``1077:1216`` revision 6 at 3 and the BCM5701
  ``14e4:1645`` at 4 (*observed* in every captured IP53 inventory;
  :ref:`sgi-ip35-ip53-io9`). PIC's configuration devices are its
  internal slots plus one (OpenBSD ``xbridge.c``), so the PCI ids IRIX
  prints are OpenBSD's devices plus one (*inferred*).

The Origin 350's widget identity words for Bedrock and PIC are
*unknown*. One published PROM trace from an Onyx 350 module (an IP53
graphics blade with four processors and 8 GiB) attaches the Bedrock at
widget ``0xb`` (:src:`graphica-o350-chimera-notes lines 949-952`),
*observed*. No trace from an Origin 350 server is published, and that the
server board enters at the same widget is *inferred* from the shared IP53
board. The model enters the crossbar at widget ``0xa``
(``SN1_XBRIDGE_HOST_PORT`` in ``hw/mips/sgi/sn1-node.c``), an
*approximation* that disagrees with the observed trace.

.. _sgi-sn-family-bedrock:

Bedrock revisions
-----------------

Every published IRIX inventory of an Origin 300, Origin 3000, Origin
350, Fuel or Onyx 4 reports the Bedrock, printed as ``HUB``, at revision
2, 200 MHz (*observed*, many machines). No capture shows another
revision. The raw register value behind the revision field IRIX prints
is *unknown*.

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
:ref:`sgi-ip27-memory`, :ref:`sgi-ip35-products`,
:ref:`sgi-ip35-memory`, :ref:`sgi-ip53-products` and
:ref:`sgi-ip53-memory`.

.. _sgi-sn-origin200-processors:

Origin 200 processor options
----------------------------

``processor=`` selects the processor module every node of an
``origin200`` topology fits, by catalogue id. ``-cpu`` defaults to the
module's part and must name it when given. ``query-sgi-machines`` lists
each topology's choices as ``processor-options``. The four modules, the
inventories that establish each one's clock, cache, tap and chip
revision, the mode word the machine writes into each node's flash
configuration block, and the model limits on Hub revision and existing
flash are on :ref:`sgi-ip27-processors`. The bank layout each module
allows is on :ref:`sgi-ip27-memory`.

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

The two generations differ when no image is given:

- On IP27, absent an image, the machine still builds a working reset
  vector. A default template answers the flash's command interface with a
  spin-in-place loop, a plausible configuration block and a factory-fresh
  environment log, the shape a real chip ships pre-formatted in rather
  than merely erased. The machine patches five fields into any block
  that carries the IP27 configuration magic: the module's mode word and
  clock, and the product's two flash timings and machine-type word.
  Every other field and the block's byte sum are kept, with the checksum
  field adjusted to hold the sum. The product values come from the topology's
  ``ip27-firmware`` record in ``query-sgi-machines``
  (:ref:`sgi-prom-image-ip27-values`).
- On IP35, no template exists. Every node needs a real image or a bound
  flash, and the PROM's writes to a flash that is only filled from
  ``-bios`` do not persist. :ref:`sgi-ip35-firmware` describes the effect
  and :ref:`sgi-prom-image-ip35-views` the views the model maps.

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
block per node. Within a node's block, IOC3 port A comes first (it is
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
they are the four IOC4 serial ports (:ref:`sgi-ip35-firmware`). A
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
controller and TI TSB12LV22 IEEE 1394 link at head F devices 4, 5 and 6
(*observed*; :ref:`sgi-sn-family-ibrick`). Any other preset name is
refused. :doc:`hardware/baseio` describes the
presets. The Origin 200's IP29 onboard I/O is soldered to the System
Board rather than plugged into a slot, so the Origin 200 node builds its
``ip29`` instance directly on its own XIO port, with no board wrapper.
The Origin 300 node's ``io8`` instance is placed the same way, directly
on its XBridge head, not through a board class. The machine places the
I-brick's ``io7`` instance as ``/machine/module<M>/io7``, directly on head
F of the XBridge in the I-brick's module, also with no board class.

``sgi-xio-board`` (``hw/mips/sgi/xio-board.c``) is the abstract base for
a removable XIO board: the unit that plugs into one of a module's XIO
slots. Its concrete classes are ``sgi-xio-baseio`` (a Bridge-based card
built from a ``sgi-baseio`` preset: ``io6``, ``io6g``, ``menet`` or
``mscsi``), ``sgi-pci-expansion`` (a plain Bridge with an empty PCI
bus), ``sgi-pci-carrier`` (the Origin 2000 PCI Carrier Assembly, three
PCI slots and its own board record), ``sgi-pci-shoehorn`` (a Bridge
carrying one PCI card), ``sgi-gigachannel`` (the GIGAchannel cabinet's
own Bridge), ``sgi-kona`` (the InfiniteReality pipe), ``sgi-mgras`` (the
IMPACT graphics board, whose widget answers XIO directly with no Bridge)
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

A catalogue node record carries ``hub-slot-bits`` where its midplane
differs from the eight-processor module's, and the builder hands the Hub
the result as its ``slot-bits`` property (:ref:`sgi-onyx2-node-slot-codes`).

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

An Origin 3000 C-brick's IP35 board has no XBridge: an XIO cable joins
its Bedrock to port A or B of the XBridge in an I/O brick. The builder
creates that chip as ``/machine/module<M>/xbridge``, or ``xbridge0`` to
``xbridge3`` where a module holds several, as the N-brick's four chips do.
It gives the chip the cable's port as its host port and its module's
catalogue id as its bus prefix, and hands it to the cabled node through
the node's ``xbridge`` link. The node joins and realizes it as an
Origin 300 node does its own. The node's Bedrock decodes the chip at the
cable's port and at each of the chip's heads the node hosts, and nothing
at the chip's other Crosstown2 port.

The builder refuses a second host node on one XBridge, because the model
takes one host port and whether one chip serves hosts on A and B at once
is *unknown*. It refuses a host that is also cabled to a router, for which
it has no rule. It checks a cable between two crossbars, such as the
P-brick's links from its first chip to each of the others, like any other
cable and then refuses it while the cables are built. No source cited
here gives the downstream chips' mode (*unknown*;
:ref:`sgi-sn-family-pbrick`).

An XIO cable from one of the chip's other link ports, 8 to 0xd, reaches
one XIO board: a card in an X-brick slot, or a board on the I-brick's
free Crosstown2 port. The board's own attachment names that port and the
chip's host node, whose Bedrock decodes the board's widget there. The
builder creates the board as ``/machine/module<M>/<board id>`` in the
board's own module and joins it to the port with one XIO link once the
nodes have realized. Only the XIO cards ``pci-carrier``,
``pci-shoehorn``, ``menet`` and ``mscsi``, the V-brick's VPro V12 and
the InfiniteReality pipe of a G-brick are built at such a port; any
other graphics pipe, a BaseIO card or any other preset cabled there is
refused. A V12
record cabled to a crossbar port is a graphics slot named by the
record's catalogue id: ``-device sgi-odyssey,slot=<id>`` fits the pipe
there, it is built in the record's module, and the chip's host node
reaches it at that port. The V-brick's midplane XBridge takes one host,
so a two-pipe V-brick, whose second pipe needs a second C-brick on the
other Crosstown2 port, is refused like any second host.

A G-brick pipe record at such a port is a slot, named by the record's
label, that ``-device sgi-kona,slot=<label>`` claims after machine init,
as the Onyx2's pipe slot is. The pipe joins the chip's port, sits in its
G-brick's module as ``/machine/module<M>/<board id>`` and composes its
packets for the chip's host port, which the crossbar delivers to the
hosting Bedrock. Each pipe of an N-brick is therefore reached only from
the C-brick cabled to the same chip. A G-brick pipe on an Origin 3000
machine is a model arrangement (*approximation*): no Onyx 3000 hardware
graph or widget listing has been captured, so the widget number at which
a C-brick sees its pipe and the pipe's identity there are not
established.

An SN1 router is built as ``/machine/module<M>/router`` beside its
module's L1 and that L1's UART, whether or not the topology carries the
L1 management registry the Origin 300 PCI expansion experiment uses, so
an R-brick needs no I/O peer. Its L1 keeps the routed Origin 300
experiment's type profile described under :ref:`sgi-sn-family-rbrick`;
an R-brick's own L1 type is *unknown*. A router record's ``node-ports``
lists the only ports a node's cable may use; the builder checks every
cable against it before any device exists.

A node with ``role: memory`` is a Bedrock with memory banks and no
processor, the shape of the Cx-brick's zero-processor board. It realizes
with no CPUs, its boot flash image left as supplied, and answers other
nodes' memory traffic once they have configured and numbered it, as the
firmware reaches a peer through its vector engine. It cannot be the
console node, and a population that fits it a processor is refused.
Whether such a board carries boot flash is *unknown*, so the builder still
requires one.

A controller with ``owner: module`` is built once as
``/machine/module<M>/l1`` and linked into the module's node through the
node's ``l1`` link, in place of the node's own ``/machine/node<N>/l1``.
The node configures it exactly as it would its own, so the guest sees
the same L1. A module with several nodes may own its L1 only with
``profile: cx-brick``: the machine builds the :ref:`Cx-brick L1
<sgi-l1-cxbrick>` itself for one to four IP53 nodes in distinct slots 0
to 3, and each node adds its own UART window on the link its slot names.
A node with the memory role is a board without processors. The brick's
console is the first listed node's L1 serial line. Every other
module-owned L1 shared by several nodes is refused, because how several
Bedrocks reach one L1 is *unknown* outside that profile.

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

.. _sgi-sn-gigachannel:

GIGAchannel expansion cabinet
-----------------------------

Two Origin 200 topologies fit the cabinet: ``origin200-gigachannel``,
where the cabinet's Crossbow sits between the Hub and the module's base
I/O Bridge, and ``origin200-gigachannel-loopback``, the converted module
with a loopback cable instead of a cabinet. The cabinet's own I/O is the
``sgi-gigachannel`` board, which the machine places and which is not
``-device``-creatable. :doc:`hardware/gigachannel` describes both
topologies, the board, its BASEIO PROM flash and its status word.

Module power and reset
======================

The IP27 machines connect each module controller's power and reset
outputs to the module's parts, and :ref:`sgi-ip27-power` describes what
the model does. These implementation details are not repeated there.

The cold start of a module runs from a bottom half and not from the
controller's own timer. It stops the module's processors with work queued
on each of them and does not wait. The last completion schedules the rest
of the start (the parts' reset and the processors' release) as a bottom
half, so the start never blocks on a processor thread and runs under
record and replay, where that thread needs a lock the main loop holds. A
processor that runs no guest code, in a paused machine or under qtest, is
stopped on the spot, and a start with no running processor to wait for
goes on at once. The start is owed until that bottom half has run, so a
start owed when a migration stream was taken, including one still
waiting for its processors, is part of that stream and is carried out
once the destination runs.

The module start also resets every Crossbow the module's nodes reach and
the boards behind them. Whether a module reset reaches the Crossbows and
XIO boards on hardware is *unknown*. A midplane schematic or a register
capture across a module reset would settle it. The stock IP27 PROM sends
a warm reset down each XIO link on every cold entry (*software
expectation*), and a Crossbow treats one as a power-on reset of its core
(*established*, :src:`crossbow-asic-spec-12.0 pp. 132-133`). The firmware
therefore resets them either way. The model resets them at module start
as well, because the PROM's warm reset does not reach a Bridge's PCI
side, and the stock PROM cannot read a Bridge that kept a running
kernel's PCI setup.

While a module is off its ports read disabled to their peers, the state a
cable marked down already has. A peer Hub reads link-up zero with the
down reason clear, which Linux ``hubni.h`` defines as a link that failed
(*software expectation*). Treating a lost peer as failed and not as never
reset is an *approximation*. A peer Router reads the port not working.
Whether a Router port facing a lost peer reads reset-failed is *unknown*,
and the model leaves it clear. Power returning brings the links up
through the normal flood after the module's parts have reset and before
its processors run. An off module keeps its register state until the
reset that comes first at power-up.

Power state is derived from the controllers' power, which migrates, so
none of it does. It is applied when a machine starts running and after
the machine's own reset, and by the machine's migrated section, which
loads after every device, so a load shows the right gating even while
the destination is paused.

The Hub's I2C adapter window is the one register window that keeps
answering while its module is off, and only from the off module's own
view. A peer module's access to it fails like the rest of the Hub. The
adapter is a Hub register window, and the Hub is in the module's switched
power domain (*established* for the Origin 200,
:src:`007-3708-002 p. 97`, and for the Origin 2000 rails,
:src:`007-3456-003 p. 113`). On hardware the adapter is therefore off
with the module and no host could use it (*inferred*). Keeping it
answering is an *approximation*. It keeps the host side of the standby
controller reachable over the module's own bus, which the controller
tests and the Origin 200 controller's grants rely on. In the
model an operator reaches an off Origin 2000 module's controller through
its serial line, and an off Origin 200 slave's through its master. The
slave has no connector of its own, and a ``pwr u`` the master accepts
while it is itself off powers the master up, whose forward then starts
the slave (:ref:`sgi-elsc-master-slave`). A ``pwr u`` sent to a master
that is already on has nothing to power up and forwards nothing, so the
model has no way to start only the slave from a peer's bus. No topology
with more than one Origin 2000 module is built, so the several-module
Origin 2000 path is the same code as the Origin 200 pair's and is only
exercised there.

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

.. _sgi-sn-fuel-interrupts:

Fuel onboard interrupt routing
==============================

The ``fuel`` machine wires the four interrupt sources on its widget-15
XBridge head (head F) to head F inputs 1, 0, 4 and 5. The routing is an
*approximation* of board wiring, and the physical traces on any Fuel
board revision are *unknown*. :ref:`sgi-ip35-fuel-interrupts` has the
table, the evidence, the path to the Bedrock and the tests.

.. _sgi-sn-fuel-interrupts-evidence:

Evidence
--------

The evidence for the routing is on :ref:`sgi-ip35-fuel-interrupts`.

.. _sgi-sn-devices:

Devices
=======

The node ASICs, bridges, controllers and I/O chips this family builds
from have their own pages: :doc:`/system/mips/sgi/hardware/hub`,
:doc:`/system/mips/sgi/hardware/bedrock`,
:doc:`/system/mips/sgi/hardware/bridge`,
:doc:`/system/mips/sgi/hardware/xbridge`,
:doc:`/system/mips/sgi/hardware/elsc`,
:doc:`/system/mips/sgi/hardware/l1`,
:doc:`/system/mips/sgi/hardware/router`,
:doc:`/system/mips/sgi/hardware/sn1-router`,
:doc:`/system/mips/sgi/hardware/ioc3`,
:doc:`/system/mips/sgi/hardware/qlogic-isp` and
:doc:`/system/mips/sgi/hardware/gigachannel`.
