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
record ``nvram<N>-clock``, an IO9 board's DS1742 ``ds1742-<board>-data``
and its clock record ``ds1742-<board>-clock``, module system controller
configuration NVRAM ``module<N>-nvram``, the flash EEPROM of the
InfiniteReality pipe in a graphics slot ``<slot>-kona-flash``, each with
its exact size and
what a new machine's copy starts from) and the firmware images that
initial content is built from (``init-inputs``).
Nine products stand on this base:
``origin200``, ``origin2000`` and ``onyx2`` (SN0,
``hw/mips/sgi/sn-machine.c``), and ``origin300``, ``fuel``,
``origin350``, ``onyx350``, ``tezro`` and ``onyx4-host`` (SN1,
``hw/mips/sgi/sn1.c``). ``onyx2`` is Origin 2000 silicon with graphics:
the same IP27 node boards, memory and stock PROM, told apart only by the
hardware its topologies fit. The SN1 product classes parent ``sgi-sn``
like the others and name the same ``topology=`` contract and compiled
descriptor this page describes; only their node device
(``sgi-sn1-node``, ``hw/mips/sgi/sn1-node.c``) is product-specific, the
way each SN0 node's own device is.

.. _sgi-sn-product-family:

Product family
==============

This section is the hardware contract the SN model layers cite for
the products, bricks and cabling they describe. Each fact is rated
*established* (a cited specification or manual, or an identified real
machine), *inferred* (follows from established facts by the stated
step), *approximation* (an emulator value standing in for unknown
hardware) or *unknown*. SGI manuals are cited by document number and
printed page; forum captures by site and thread number. Origin 350 hardware
facts below describe the product; the ``origin350`` catalogue entry is an
implemented one-node software profile with explicit approximations and
unresolved hardware behavior. It does not qualify every Origin 350
configuration or establish stock PROM behavior. The ``onyx350``,
``tezro`` and ``onyx4-host`` catalogue entries are software profiles of the
same kind. The Onyx 4 host/G2 and host/G2N entries remain
reference models, as does the optional second Tezro rack enclosure. Origin
3000 brick arrangements and the NUMAlink module also remain reference
configurations.

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
switching (established, 108-0240-002 printed page 2-26). The C-brick
has one rear ``XIO (II)`` connector, which joins it to an I-, P- or
X-brick (established, *SGI Origin 3000 Series Owner's Guide*,
007-4240-002, printed page 124). The layer's C-brick buses -- one SysAD
per processor interface and the ``xio-ii`` bus from the Bedrock to that
connector -- follow from this. Which port of the far brick's XBridge
the cable reaches is the composition's choice, not the C-brick's.

The eight DIMM slots form four pairs. Banks 2n and 2n+1 sit on pair n
and both DIMMs of a pair match, so the two banks of a pair are fitted
alike; pairs may differ (established, 108-0240-002 printed pages 2-5
and 2-6). The Origin 3200 catalogue entry fixes each C-brick at the fit
a :ref:`posted Origin 3200 inventory <sgi-ref-web-nekonomicon-16723604>`
reports, four 400 MHz R12000s with 8 MB secondary caches on two PIMMs
and 4096 MB in eight 512 MB banks, and resolution refuses any other
processor or bank population for that product.

The entry is the ``origin3000`` machine, topology ``origin3200``. Its
C-brick values the sources do not settle are model choices:

* The board-identification word is ``0x2``. Product code 0 in bits
  15:12 is a C-brick (established, OpenBSD ``ip27.h``); the low nibble
  2 selects the stock IP35 PROM 6.210's configuration row for a
  400 MHz, 8 MB fit, and the other bits read zero (approximated: no
  C-brick has been read).
* The flash configuration block carries machine type 0 and that row's
  word ``0x5414a785``, and each processor's reset mode is the word's
  low 25 bits (approximated, as the Origin 300's R14000 values are
  taken from its own row).
* The FPU identification is ``0x0935``, from the R12010 revision 3.5
  the inventory prints (inferred).
* The L1 is the ``cbrick`` profile, named by its slot: 001c04 and
  001c07. The two C-brick L1s are peers over the NUMAlink cable.
* With no XIO cable, a C-brick's II link reads down (inferred, as the
  Bedrock II model already takes it).

.. _sgi-sn-origin3200:

Origin 3200 machine
-------------------

``-M origin3000`` builds two C-bricks joined LINK (NI) to LINK (NI)
and an I-brick on c0's ``XIO (II)`` connector at XBridge port A, with
8 R12000s (``-cpu R12000 -smp 8``) and 8192 MB. The I-brick's XBridge
is ``/machine/module2/xbridge``; its heads E and F are widgets 14 and
15 of c0, and its IO7 is ``/machine/module2/io7`` on head F. The
I-brick's L1 is ``/machine/module2/l1``, the ``ibrick`` profile named
001i14, reached from c0's L1 over the XIO cable as an in-process call;
it holds the IO7's Ethernet address, the machine's ``mac``. The
I-brick's Fibre Channel card is ``/machine/module2/io7/fibre-channel``,
a ``qlogic-isp2200`` at head F device 1 with its pin A on head pin 1.
Its private loop is the SCSI bus ``scsi.0``, the boot disk bus; each
disk there is a loop port at the loop ID its ``scsi-id`` gives and
needs the ``port_wwn`` and ``wwn`` names that port has, which the
launcher chooses.

Stated limitations: no internal FC disk is built with the machine;
disks join the loop only as the launcher attaches them, and which of
the brick's two drive bays a disk stands for is unknown. The IO7's
IEEE 1394 link controller
is left out, because its model has no PHY or bus and the stock PROM's
probe waits on them; the IO7's time-of-day part is unidentified, so its
IOC3 carries the Origin 300's M48T35-compatible Timekeeper as a
compatibility choice, with blank storage on every start; head E's PCI
slots are empty; the I-brick's XBridge revision is the model's.

.. _sgi-sn-onyx3400-ir:

Onyx 3400 with InfiniteReality3 machine
---------------------------------------

``-M origin3000,topology=onyx3400-ir`` builds the Figure 1-10 brick set of
the Onyx 3000 graphics guide (007-4264-002 printed page 27) as a
catalogue entry, ``onyx3400-ir``; the ``onyx3400`` composition stays the
reference of the same brick set. It has four C-bricks on ports 2 to 5 of
one R-brick, an I-brick on c0's ``XIO (II)`` connector, an N-brick whose
first three chips carry c1 to c3, and two G-bricks. It has 16 R12000s, four per
C-brick, and 16384 MB. The stock PROM 6.210 finds 4 nodes, 16 CPUs and
one router, and its diagnostics summary reads ``ALL DIAGS PASSED``.
Its ``hinv -v`` lists the four node boards, the I-brick's two heads, the
router, an XBow for each C-brick and, for the pipe that is fitted, a
``KONA Graphics Board`` under the module of the C-brick that reaches it.

All four pipes are cabled: the I-brick's second port (port B) carries pipe 0
of G-brick 0, and the N-brick's first three chips carry pipe 1 of G-brick 0
and both pipes of G-brick 1. The N-brick's fourth connector pair is empty.

Choices the sources do not settle, each a model choice:

* One R-brick, not Figure 1-10's two. The routed Origin 3400 drawing of
  the technical configuration guide (007-4311-002 printed page 55) has
  one, with the C-bricks on ports 2 to 5. The R-brick presents all
  eight ports, and its L2 controller and USB hub traffic to the C-brick
  L1s are not modelled. The C-brick L1s are not peers of each other.
* The C-bricks are fitted like the Origin 3200's: four 400 MHz R12000s
  with 8 MB caches and 4096 MB. The C-brick layer carries both PIMMs
  unconditionally, so Figure 1-10's two-processor C-bricks are not
  expressible.
* The N-brick's XBridge presents the revision the model's XBridge
  presents, as the I-brick's does.
* Module numbers of the bricks.
* The I-brick and N-brick controllers' attached-brick replies, and the
  N-brick's type letter; see the L1 notes (:ref:`sgi-l1-model`).

Stated limitations:

* The machine fits up to four ``sgi-kona`` pipes, one per pipe slot;
  the first, I-brick port B's, is the default. Each G-brick's pipe 1
  takes one or two raster managers, its pipe 0 one, two or four. Each
  C-brick's L1 knows its chip's G-brick whether or not a pipe is
  fitted.
* Each N-brick chip presents its two PCI heads, widgets 14 and 15, with
  nothing behind either, to the C-brick cabled to it. The N-brick Overview
  (revision 10/24/01, printed page 3) calls the chip's ports E and F
  logically disconnected, so this is an approximation grounded in what the
  guest needs. On this model, the stock PROM 6.210 recorded no I/O brick
  board for those C-bricks' crossbars until the heads decoded, and then
  recorded an N-brick board (type 0x74, Linux ``KLTYPE_NBRICK``) at each
  head; clearing the heads' link-alive bits alone recorded none. IRIX
  6.5.30 looks for an I/O brick board on the node, then on its crossbar
  peer, when it brings up a widget on the crossbar, as Linux's
  ``io_xswitch_widget_init`` does, and panics on a failed assertion when
  there is none, which a pipe behind the N-brick triggers. Without the
  heads, the PROM's start on those C-bricks also failed with
  ``xtalk_init resulted in an exception``. A widget scan of a real N-brick
  would settle which heads answer.
* The router's ports 1, 6, 7 and 8 are uncabled, and the PROM prints
  ``discover_object: vector read 0x2 LRI failed`` and ``erect_fences``
  messages and an EEPROM vector read error against the router.

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

.. _sgi-sn-family-xbrick:

X-brick XIO slots
-----------------

The X-brick holds one XBridge. Its Crosstown2 ports A and B go to
compute nodes, and its Crosstalk ports 8, 9, C and D each serve one XIO
slot, slots 1 to 4 in that order (established, 108-0240-002 printed
pages 2-32 and 2-33, Figure 2-25). The figure draws nothing on the PCI
ports E and F, so the ``x-brick`` layer declares no PCI bus. In an
Origin 3200 each C-brick has one ``XIO (II)`` connector (established,
007-4240-002 printed page 125), so with the I-brick on the first C-brick
an X-brick hangs off the second; ``origin3200-xbrick`` is that reference
composition. It cables the X-brick at port A by choice: whether the rear
connector ``XIO 10`` or ``XIO 11`` is port A is unknown, because the
sources disagree. The slots stay unresolved in the catalogue, since no
X-brick board has a reviewed contract. The builder test places Origin
2000 PCI carrier cards in three slots as stand-ins, each reached only
from the cabled C-brick at its slot's port. The layer records the
brick's L1 type letter as ``X`` (``0x58``, Linux ``ksys/l1.h``), which
IRIX 6.5.30 and the stock IP35 PROM 6.210 name an ``Xbrick`` with module
class ``x``, expecting a 400 MHz link; this is the software's
expectation, not a capture of a fitted brick, and nothing is built from
it yet. Unknown: the XBridge revision, and the widget numbers a C-brick
sees for the slots, which only an X-brick widget inventory would settle.

.. _sgi-sn-family-pbrick:

P-brick chips and links
-----------------------

The P-brick has twelve PCI slots on six buses and three XBridges. U0
serves buses 3 and 4 and takes the C-brick on Crosstown2 port A or B.
U1 serves buses 1 and 2, U2 buses 5 and 6, and U0 joins each by two
links: U0 D to U2 D, U0 C to U2 8, U0 9 to U1 8 and U0 8 to U1 D
(established, 108-0240-002 printed pages 2-30 and 2-31, Figure 2-24).
The ``p-brick`` layer records those chips, ports, buses and slots, and
lists one XIO cable per link. ``origin3200-pbrick`` hangs it off the
second C-brick at U0 port A, the same way ``origin3200-xbrick`` places
the X-brick. The builder still refuses the four links, so no P-brick is
built: whether U1 and U2 present each head on its own link, which head
rides which link, the PCI device number of each slot and the slots'
interrupt wiring all lack a reviewed contract. The layer records the
brick's L1 type letter as ``P`` (``0x50``, Linux ``ksys/l1.h``), which
IRIX 6.5.30 and the stock IP35 PROM 6.210 name a ``Pbrick`` with module
class ``p``, expecting a 600 MHz link; as for the X-brick, it is the
software's expectation, and nothing is built from it yet.

.. _sgi-sn-family-ixpx:

IX-brick and PX-brick PIC enclosures
------------------------------------

Both bricks hold three PICs. The middle one serves buses 3 and 4, meets
the two Crosstown2 connectors on its ports A and B, and joins the lower
PIC (buses 1 and 2) and the upper PIC (buses 5 and 6) by one link each
(established, 007-4604-001 Figure 2 and 007-4605-001 Figure 2). The
model builds the middle PIC's integrated PXBOW as the enclosure
crossbar, with the host on port 0xa, the lower PIC on 0xc, the upper on
0xd and the middle PIC's own heads on 0xf; links present, alive at
startup and restored by a link reset are those four. That port reading
follows the XBridge port letters as hex digits and the public Linux
Altix brick table; it is a reviewed approximation, not a measured
board. Every PIO, DMA and interrupt packet crosses the crossbar: no PIC
has a direct Bedrock peer. Each packet enters at its PIC's port and
reaches the Bedrock as a packet from that widget, so the Bedrock's
inbound access register governs it per PIC. Interrupt packets go to the
host port the cable meets, with the level in destination bits 63:48.

All six heads run ``pic-rev3-enclosure-experimental``: the PIC register
layout at revision 3, configuration device 0 reserved, the direct
32-bit DMA window and the ``DIR_MAP`` field rule, and none of the IP53
compatibility behaviours (no held-input reassertion, no IP35 PROM reset
values or masks, no 32-bit control writes, no direct64 decode). Head 0
drives the lower-numbered bus. INTA of configuration device *d* drives
interrupt input *d* - 1; INTB to INTD reach no input and are logged. A
failed DMA send is logged by the PIC from its own result.

Only single attachment is built: a cable to port 0xb, a cable from an
Origin 350 compute module and a board on any other crossbar port are
refused. Neither brick has an L1, and no on-board device is fitted: the
IX-brick's IO9 waits on contracts for its IOC4 port presence, its
QLogic part and its BCM5701 identity, so its bus 1 slot 1 is empty and
logged as such. No catalogued machine builds either brick yet, because
no reviewed configuration attaches one to a C-brick; the builder test
builds them.

.. _sgi-sn-family-vbrick:

Onyx 3000 V-brick
-----------------

A V-brick holds one or two InfinitePerformance pipes, each one VPro V12
board taking two of four board slots, behind an XBridge on its
midplane; its two rear XIO ports each cable to a C-brick's ``XIO (II)``
connector, and each pipe needs a C-brick of its own, besides the one
that hosts the system I-brick (established, *SGI Onyx 3000 Series
Graphics System Hardware Owner's Guide*, 007-4264-002, printed pages 8,
10, 19, 52 to 56). A system holds G-bricks or V-bricks, never both
(established, printed pages 10 and 21). The ``onyx3000``
topology (``-M origin3000,topology=onyx3000``) puts the I-brick on the
first C-brick and a one-pipe V-brick on the second, with the cable on the
midplane XBridge's port A and the V12 on its Crosstalk port 8. Both ports
are the composition's choices: which connector is port A, which Crosstalk
port serves a slot, and so the V12's widget number, are unknown, and a
PROM inventory or hardware graph of an Onyx 3000 with a V-brick would
settle them. The V-brick has an L1 (``brick-type=vbrick``, see
:ref:`sgi-l1-origin3000`), reached from the C-brick its cable joins; its
type letter and records are approximations, and its XBridge answers at both
PCI heads (widgets 14 and 15) with nothing on them, as every XBridge does.
No source fixes an Onyx 3000 C-brick's processors, caches or memory, so each C-brick takes the Origin 3200 C-brick's fit
(four 400 MHz R12000s with 8 MB caches and 4096 MB); that fit is a model
choice. The V12 is fitted by default and ``-vga none`` leaves its port
empty. The stock PROM lists the V12 as a graphics board in the second
C-brick's module, builds the V-brick's I/O records from its heads and
diagnoses all four processors on each node. It prints a warning that the
link to the V-brick runs at 400 MHz where it expects 600 MHz; the model keeps
the guide's 800 MB/s port rate. IRIX 6.5.30 boots to a login with the V12
fitted.

.. _sgi-sn-family-rbrick:

R-brick and NUMAlink module ports
---------------------------------

The R-brick router has eight ports, 1 to 8, lettered A to H. A C-brick
connects only to port 2, 3, 4 or 5, since only those cables carry the
USB signals that reach its L1; router-to-router links normally use 1,
6, 7 and 8 but may use any port (established, *SGI Origin 3000 Series
Technical Configuration Owner's Guide*, 007-4311-002, printed pages
36-37; *SGI Origin 3000 Series Owner's Guide*, 007-4240-002, printed
page 196). An Origin 3400 R-brick ships with two of its ports disabled
(*Origin 3000 System Architecture*, 108-0240-002, printed page 2-20);
which two, and how, no source cited here establishes (unknown). The
router ASIC itself is not named in any SGI document found (unknown).

The ``r-brick`` model layer is the enclosure: the router, its L1 and
USB hub, and the eight port connectors, each with a cable bus a
composition attaches the far end to. The router's ``node_ports``
names ports 2 to 5. Resolution refuses a node cabled to any other
port, and the builder refuses such a cable in a topology it is handed,
with the message "node '<id>' is cabled to router '<id>' port <n>; it
takes a node only on ports 2, 3, 4, 5". A cable to another router may
use any port. The layer sets no L1 brick-type character: the router L1
profile answers ``R`` (``0x52``) for every router's L1, which is what the
stock IP35 PROM 6.210 and IRIX 6.5.30 expect from a router's controller
(their expectation, not a capture of a fitted R-brick).

The ``origin3400`` composition is the guide's 8- to 16-processor
interconnect drawn with one R-brick (007-4311-002 printed page 55): four
C-bricks on ports 2 to 5 and an I-brick on the first C-brick's
``XIO (II)`` connector at XBridge port A. Its composition rules follow
Appendix B (printed page 89): one to eight C-bricks, one or two R-bricks
and at least one I-brick among at most eight I/O bricks. Every Origin 3400
bundle ships two R-bricks (printed pages 76-78), which this composition
does not show. It is an emulation target. Each C-brick takes the Origin
3200 fit: four 400 MHz R12000 processors with 8 MB secondary caches and
4096 MB in eight 512 MB banks. The bundles ship C-bricks without memory
and the customer chooses the DIMMs, so the memory is a model choice, as
are the brick slot numbers (the guide places no brick of this system in a
slot) and the synthetic identities. Being routed, the composition assigns
no master or slave role to a C-brick.

Stated limitations of a routed Origin 3000 build. The L2 controller,
which a rack holding an R-brick requires (007-4311-002 printed page
42), is not modelled, nor is the R-brick's USB hub traffic to the
C-brick L1s; running without them is a limitation, not a claim that
the hardware does. The router presents all eight ports, not the six an
Origin 3400 enables. Its forwarding is the routed Origin 300
experiment's (below), which no R-brick register contract confirms:
the table shapes, the destination-ID mapping and whether an entry
names an absolute port or a turn are unknown.

The Origin 300 NUMAlink module is a different product. Published
inventories report its router board as another revision of the
R-brick board's part family; whether the two carry the same router ASIC
revision is unknown. Ports 1, 6, 7 and 8
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

The routed Origin 300 experiment selects one logical eight-port SN1
router. Public Linux v2.5.0
(``linux-history:381ab1f538162eb985c9244aa7d56e54315881ab``)
names router chip identity ``0x3017`` in
``include/asm-ia64/sn/sn1/bedrock.h`` lines 19-20 and defines the
router status fields in ``include/asm-ia64/sn/sn1/router.h``. A
published six-chassis Origin 300 inventory (Nekochan thread 16727514,
lines 390-409 and 1860-1869) reports router Revision 1. The model
selects chip revision one by inference from that software report.
Neither the inventory nor the manuals measure the fitted silicon or
establish how many router ASICs the enclosure contains. Chip inputs
zero is an emulator configuration choice.

The experiment also selects router L1 type ``0x52`` as a software
profile, not a measured controller reply. Public Linux v2.5.0
``include/asm-ia64/sn/ksys/l1.h`` line 266 assigns uppercase ``R``
that code. Its ``arch/ia64/sn/io/l1_command.c`` lines 413-436
normalize uppercase types for module names and lines 1031-1074
identify the router module independently of the returned type.
The Origin 300 NUMAlink module guide (007-4498-002, printed pages
14, 96 and 130) establishes a separate L1 controller, but gives no
raw type response. A binary type-query capture from a matching
controller would settle that byte.

.. _sgi-sn-family-cxbrick:

Origin 3900 Cx-brick
--------------------

The Cx-brick holds one to four IP53 node boards, one router board and a
power entry module. Each node board carries one Bedrock, zero or four
processors and up to eight DIMMs; a board with no processors serves as
a memory node (established, *Cx-brick Addendum*, 007-4572-001, printed
pages 1 and 3-4). The router board's router ASIC is the R-brick's, with
four internal NUMAlink 3 links, one per node board, and four external
connectors labelled NUMAlink 1 Port A, NUMAlink 8 Port H, NUMAlink 7
Port G and NUMAlink 6 Port F (printed pages 4-5 and 11). Each external
XIO connector serves one node board: XIO 0 to node 0, and so on
(printed page 10). The *Partitioning Guide for Systems That Contain
Cx-bricks* (007-4426-001, printed pages xi, 3, 6 and 18) covers the
Origin 3900 and gives a partition of one Cx-brick with an I- or
IX-brick.

The ``cx-brick`` layer and the ``origin3900`` composition record these
facts: one module with four Bedrock nodes, an eight-port router linked to
each node over a midplane link, and each node's XIO connector on its own
Bedrock's ``xio-ii`` bus. The composition cables an I-brick to node board
0's XIO connector at its port A, a choice of the composition.

``-M origin3000,topology=origin3900 -cpu R16000`` runs it. Nothing a source
read gives fixes the Cx-brick's processors, node-board part, router ports,
NASIDs, widget numbers or type character, so each node board is fitted as
the Origin 350's IP53 is (four 700 MHz R16000 processors with 8 MB caches
and 2048 MB), node board *n* takes router port *n*\ +2, the L1 reports the
C-brick's type character, and board *n*'s link reports the brick's slot
plus *n* with a serial of its own, because the stock PROM tells nodes apart
by those. Each is a stated approximation in the Cx-brick datasheet. The
node board has its own board kind, ``ip53-cx``: like the C-brick's IP35 board
it has no XBridge and takes an XIO cable, and like the Origin 350's IP53 it
carries R16000 processors, but it has none of the Origin 350's PIC, IO9 or
runtime profile. With the stock IP35 PROM 6.210 payload the model reports 4
nodes, 16 CPUs, 8192 MB and 1 router and passes the PROM diagnostics.

.. _sgi-sn-family-origin350:

Origin 350 compute module
-------------------------

The catalogue's ``origin350`` entry selects a four-processor, one-node
software profile. This is an emulation target with explicit model choices,
not a claim that its synthetic memory, firmware seed, PIC setup, or board
wiring reproduces a measured machine. See :ref:`sgi-ip53-pic` for the
component boundary and the catalogue profile for the selected runtime
inputs. The hardware facts in this table remain independently rated below.

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
compute module. No public trace is attributed to an Origin 350: the
graphica page's PROM trace that attaches a Bedrock at widget ``0xb``
(lines 949-952) lists an ODYSSEY graphics assembly and the same module id
as the page's Fuel, so it reads as a Fuel and is not Origin 350 evidence.
The widget stays unknown.

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
Onyx2 deskside. ``-M <product>,topology=help`` lists the rest:

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
     - 64 to 2048 MiB, as banks (see below)
   * - ``origin200-dual``
     - ``origin200``
     - 2, direct-linked
     - 2+2 (fixed)
     - as ``origin200``, same on both nodes
   * - ``origin2000-rack`` (default)
     - ``origin2000``
     - 1 to 4; two Crossbows, and two Routers from two nodes on
     - 1 or 2 per node
     - 64 to 4096 MiB, as banks (see below)
   * - ``origin2000-deskside``
     - ``origin2000``
     - 1 to 4; two Crossbows, a Null Router for two nodes, two Routers
       for three or four
     - 1 or 2 per node
     - as ``origin2000-rack``
   * - ``onyx2-rack`` (default)
     - ``onyx2``
     - 1 to 4, wired as ``origin2000-rack``
     - 1 or 2 per node
     - as ``origin2000-rack``
   * - ``onyx2-deskside``
     - ``onyx2``
     - 1 or 2, a Null Router for two, one Crossbow
     - 1 or 2 per node
     - as ``origin2000-rack``
   * - ``origin300`` (default)
     - ``origin300``
     - 1
     - 2 or 4
     - 512 MiB (fixed)
   * - ``fuel`` (default)
     - ``fuel``
     - 1
     - 1 (fixed)
     - 512 MiB (fixed)
   * - ``origin350`` (default)
     - ``origin350``
     - 1
     - 4 (fixed)
     - 2048 MiB (fixed)
   * - ``onyx350-compat`` (default)
     - ``onyx350``
     - 1
     - 4 (fixed)
     - 8192 MiB (fixed)
   * - ``tezro-tower-multi`` (default)
     - ``tezro``
     - 1
     - 2 or 4
     - 4096 MiB (fixed)
   * - ``onyx4-host-serial-compat`` (default)
     - ``onyx4-host``
     - 1
     - 4 (fixed)
     - 8192 MiB (fixed)

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
does not settle its wiring is refused rather than built. ``-smp``
keeps the product's default, which fits its most node boards: the machine
cannot tell a default from an explicit value, so a launch with fewer node
boards names it, as ``-M origin2000,nodes=1 -smp 2`` does. Without ``-m``
the machine fits each node board of the selected topology and ``nodes=``
with that topology's per-node default memory, 256 MiB for the Origin 2000
and Onyx2, so ``-m`` is needed only to choose a different size. Naming
a topology that is not one of a product's own (or any other unknown
name, including ``topology=help``) makes the machine report every
topology that product accepts and exit, since QEMU has no generic help
path for a property value. A topology with more than one node splits
``-smp`` evenly across its nodes when no explicit core/socket count is
given, the way a bare ``-smp 8`` on ``origin2000`` becomes four sockets
of two cores each; naming sockets and cores explicitly must still match
one of the topology's own accepted populations. ``-m`` splits evenly
across nodes and each node's share must be one of its own accepted
sizes -- ``origin200-dual`` accepts the same per-node sizes
``origin200`` does, so its total ranges from 128 MiB to 4096 MiB, while
a four-node ``origin2000-rack`` takes from 256 MiB to 16 GiB. The fixed,
fingerprinted IP53 Bedrock assembly also accepts descriptor-sized unequal
node spans when each absolute bank total is
accepted by the selected processor option, the descriptor processor
population is selected, and their sum matches ``-m``; its bank positions
stay unchanged. Other heterogeneous layouts are refused.
QOM bank values and address-map checks do not establish the stock PROM's
KLCONFIG memory entries; that remains full-composition boot acceptance for
any later unequal-memory IP53 profile.
A refused size is reported with the per-node sizes that are accepted.
``nodes`` is part of the machine configuration: a snapshot
restores only into a machine with the same value. ``origin300`` and
``fuel`` accept only R14000 for ``-cpu``; ``origin350``, ``onyx350``,
``tezro`` and ``onyx4-host`` accept only R16000; ``origin2000`` and ``onyx2`` accept only
R10000, the default and the only member of their own
``valid_cpu_types``; ``origin200`` accepts R10000 and R12000, as its
``processor=`` option below selects.

.. _sgi-sn-origin200-processors:

Origin 200 processor options
----------------------------

An Origin 200 node fits one processor module (PIMM) of one or two
processors (SGI 007-3708-002, Table 1-1, printed page 5: "MIPS R10000 or
MIPS R12000", one or two processors per module). ``processor=`` selects
the module every node of an ``origin200`` topology fits, by catalogue
id; ``query-sgi-machines`` lists each topology's choices as its
``processor-options``. ``-cpu`` defaults to the module's part and must
name it when given.

.. list-table::
   :header-rows: 1

   * - ``processor=``
     - Part, PRId
     - Secondary cache
     - Processors per module
     - Mode word
   * - ``r10000-180mhz-1mb`` (default)
     - R10000 rev 2.6, ``0x0926``
     - 1 MiB at 120 MHz, tap 9
     - 1 or 2
     - ``0x1211a785``
   * - ``r10000-180mhz-2mb``
     - R10000 rev 2.6, ``0x0926``
     - 2 MiB at 180 MHz, tap 8
     - 1 or 2
     - ``0x100aa785``
   * - ``r10000-225mhz-2mb``
     - R10000 rev 3.4, ``0x0934``
     - 2 MiB at 225 MHz, tap 9
     - 2
     - ``0x120aa985``
   * - ``r12000-270mhz-4mb``
     - R12000 rev 2.3, ``0x0e23``
     - 4 MiB at 180 MHz, tap 9
     - 2
     - ``0x1213ab85``

Four modules are evidenced. Every Origin 200 inventory shows a 90 MHz
Hub. Real machines' inventories establish the clock, cache size, cache
clock, tap and chip revision of each module:

* 180 MHz with 1 MiB: the stock PROM image's own configuration block
  (mode word ``0x1211a785``, 180 MHz) and published inventories of one-
  and two-processor machines (PIMM_1XT5_1MB 013-1895-001,
  PIMM_2XT5_1MB 013-1896-001).
* 180 MHz with 2 MiB: a two-processor machine's PROM inventory,
  "Processor A: 180 MHz R10000 Rev 2.6" and "Secondary Cache 2MB 180MHz
  Tap 0x8" (Nekochan forum archive thread 16729597); the single module
  is PIMM_1XT5_2MB, 013-2285-002 (comp.sys.sgi.hardware, 17 January
  2003).
* 225 MHz: three machines' IRIX inventories print revision 3.4 and
  "Scache: Size 2 MB Speed 225 Mhz Tap 0x9", the module
  PIMM_225_2M_2P, 013-2382-002 revision B (Nekochan threads 16727211,
  17238 and 16731033).
* 270 MHz: two four-processor machines print "270 MHz R12000 Rev 2.3"
  and "Secondary Cache 4MB 180MHz Tap 0x9" (Nekochan threads 16726704
  and 16724631).

The remaining facts are *inferred*. The system clock divisor is the
processor clock over the Hub's 90 MHz, and the cache clock divisor the
processor clock over the printed cache speed. The PRId's low byte is
the printed revision. The FPU identification is ``0x0900`` for every
module; on this PROM IRIX then prints the FPU at the processor's own
revision, as real machines with PROM 6.156 do (one 270 MHz machine with
PROM 6.27 prints 0.0). Every other mode field
(kseg0 algorithm, device number, request limits, 32-word cache blocks,
error correction, endianness) is the 180 MHz 1 MiB word's, and the
R12000's delayed speculative dirty bit is clear. The R12000 divisor
codes are NEC's (U10278EJ4V0UM, Table 8-1, printed pages 180-182). No
held source shows a one-processor 225 or 270 MHz module, so those offer two
processors per module only; no inventory gives the 180 MHz 2 MiB dual
module's or the R12000 module's board record, so their Hub one-wire
part is left unprogrammed. No held source shows an Origin 200 with a
195 or 250 MHz R10000 or a 300 MHz R12000.

The machine writes each module's clock and mode word into every node's
flash configuration block (processor clock at 0x10, mode word at 0x04;
:ref:`sgi-prom-image-ip27-values`). The mode word's bits 24:0 become
each processor's Config at cold reset, the clock drives CP0 Count, and
the PRId and secondary cache size are the CPU model's. The stock PROM
reads the block to report the processors and build their KLCONFIG
entries. That real hardware delivers the module's values this way is
*inferred*, not established. The PROM's reconfiguration dialogue
writes clock and mode bits into the block, which supports it. But no
public source says the firmware compares the block with Config
(:ref:`sgi-origin2000-module` grades that comparison *unknown*). The
stock image also prints "Config values loaded from PIMM", from a path
that reads another node's values rather than the local block, so which
source a real Origin 200 node takes its configuration from on every
boot is open. A trace of that path, or a dump of a real node's flash
block beside its PIMM, would settle it.

Limitations, *approximations* until a review clears a fix:

* The node keeps Hub revision 3 and the IP29 board record 030-1244-001
  revision H for every module. Every 225 and 270 MHz inventory shows
  Hub revision 5, and those that print the board show IP29
  030-1389-001; every 180 MHz inventory shows Hub revision 3
  (*established* from the captures above).
* A node flash that already exists, such as a reused machine-state
  directory or a ``node0-flash`` drive, keeps the block it was written
  with: ``processor=`` does not rewrite it, as a module swap on real
  hardware needs the PROM's reconfiguration. A mismatched block is not
  refused by QEMU.

An Origin 200 node's ``-m`` share is laid out in its four DIMM banks,
starting at bank 0 with no empty bank between two populated ones (SGI
007-3709-002, printed page 48). Each bank takes the largest size that
still lets the remaining banks hold the rest. The board takes 64, 128,
256 and 512 MB banks, but the stock IP27 PROM 6.156 disables a 256 MB
bank whenever the configuration block gives a processor clock of 180
or 195 MHz (*established*, from disassembly of the PROM's memory test).
So with a 180 MHz module QEMU builds no 256 MB bank: banks hold 64, 128
or 512 MiB, which gives 23 accepted sizes from 64 to 2048 MiB. With a
225 or 270 MHz module banks may hold 256 MiB too, which gives 27 sizes;
512 MiB is then two 256 MiB banks, as on a real 270 MHz machine whose
PROM inventory shows 256 MB banks 0 and 1 (Nekochan thread 16726704).

Bank 0 stays at 128 MiB (256 MiB with a faster module) whenever the
other banks can hold the rest, so 1664 MiB is 128, 512, 512 and 512 MiB.
Only the totals that cannot be built that way -- 1024, 1536 and
2048 MiB at 180 MHz, 2048 MiB at the faster clocks -- get a 512 MiB
bank 0, as on a held 225 MHz Origin 200 whose inventory reports four
512 MB banks. This preference is the model's choice; a real machine's
banks follow the DIMMs fitted.

A 512 MiB bank 0 covers the boot flash's reset address, node offset
``0x1fc0_0000``. There a cached processor reference reaches the bank
and an uncached processor reference reaches the flash: the reset
vector's fetch through ``0xffff_ffff_bfc0_0000`` is an uncached memory
reference (a compatibility-segment address carries uncached attribute
3), and the flash is mapped into each processor's uncached space alone,
at the node-less address and at the node's own. That split is
*inferred*: the stock PROM's memory test covers the offset through the
cached space, and on this model's old overlay it reported
``MEMORY FAILURE (miscompare)`` at ``0xa800_0000_1fc0_0000`` reading
flash, while real machines with a 512 MB bank 0 pass the same test. No
register selects it. A capture on a machine with a 512 MB bank 0 would
settle the processor split: write a pattern through a cached address at
the offset, read it back cached and then uncached.

Three further choices are the model's own, with the hardware behaviour
*unknown* and not settled by that capture. Devices (inbound DMA, the
block transfer engines) reach the bank. An uncached reference from
another node to this node's offset reaches the bank. And the node-less
address ``0x1fc0_0000`` names the reader's own flash on every node,
whereas with a smaller bank 0 an uncached reference there goes through
the claim layer to whichever node holds number 0. Smaller banks leave
the reset address to the flash in every view.

An Origin 2000 or Onyx2 node board (IP27) wires eight banks of DIMM
pairs. SGI's Theory of Operations (007-3439-002, printed page 35) gives
64 or 128 MB banks from 16-Mb parts and 512 MB banks from 64-Mb parts,
up to 4 GB a node, with sizes intermixed as long as each bank is a
pair, so bank 0 may be 512 MB too. That manual is the whole basis for
the IP27 sizes here. Held inventories of 250 MHz and faster machines
show eight 512 MB banks, a 512 MB bank 0 and 256 MB banks, but those
nodes are the later IP31 board on the same Hub, so they corroborate
the Hub's bank decode, not the IP27 board's population. QEMU builds no
256 MB IP27 bank: the stock PROM disables one at 180 MHz, the only
processor these products offer. Every Origin 2000
and Onyx2 chassis accepts the same 55 per-node sizes from 64 to
4096 MiB, laid out as on the Origin 200: bank 0 stays at 128 MiB when
the other seven banks hold the rest (1024 MiB is 128, 512, 128, 128 and
128 MiB), and only 3072, 3584 and 4096 MiB get a 512 MiB bank 0, which
splits the reset address the same way.

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

.. _sgi-sn-graphics-pipes:

Graphics pipes
==============

A graphics pipe is one ``-device`` of its family's device type, on the
Scalable Node products and on ``octane`` and ``octane2``:
``sgi-mgras`` (IMPACT), ``sgi-odyssey`` (VPro), ``sgi-kona``
(InfiniteReality) and ``psitech-rad4`` (PsiTech RAD4). It is placed with
the properties QEMU already has for it: ``slot=`` names an XIO slot by
its label (``io6``) or an IP30 or IP34 Crossbow port (``widget-11``), and
a PsiTech card is a PCI card placed with ``bus=`` and ``addr=``.
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
machine fits by default, whether the machine may run without a pipe, and
the reason for each combination it refuses. It lists only what QEMU
builds: a product or value it does not model appears nowhere, not even
as a refusal. The products, their names and every setting's values live
once in ``hw/mips/sgi/models/graphics-products.yaml``; each product layer
lists its slots and what each offers, and ``scripts/sgi-model.py``
checks the rules below before it emits the records:

* a default names a family, product and values its slot offers, and no
  refusal matches it; at most one slot of a topology has one;
* where one slot offers a product or a setting value and another slot of
  the same family does not, that slot carries a refusal with the reason;
* no refusal names a product or value no slot offers;
* a reason is a phrase with no final full stop;
* a topology with no default pipe may run without one;
* a slot exists from the node count its Crossbow port's node board needs,
  given as ``requires``.

A topology whose own boards do not suit a pipe says so in
``machine-properties``: the ``-M`` properties a launch that fits any pipe
adds, and the reason. The Origin 2000 lists ``baseio-preset=io6g``. Its
plain BaseIO has no keyboard or mouse port, so the X server finds no
input device; the BaseIO-G, the Onyx2's board, has them on its MediaIO
daughtercard. QEMU does not add these properties itself, because it
builds the BaseIO card while the machine initializes, before any
``-device`` exists, and a ``-device scsi-hd`` needs the card's SCSI bus
by then. A launch with no pipe keeps the plain BaseIO.

A refusal matches a requested pipe when its family matches, its product
matches if it names one, and every setting it lists has the requested
value. Every slot and setting is ``changeable: false`` until a machine's
graphics can change between boots.

Default pipes follow QEMU's display-adapter convention. A machine that
ships with graphics fits its catalogue default unless a graphics
``-device`` or ``-vga none`` is given: ``octane`` an SI at ``widget-11``,
``octane2`` a V12 at ``widget-11``, the Onyx2 desksides the
InfiniteReality2 of a real Onyx2 deskside and the racks the
InfiniteReality3 of a real Onyx2 rack with a plain DG5-2, and ``fuel``
its V12. The Origin 200 and Origin 2000 fit
none.
Any graphics ``-device`` replaces the default, whichever slot it names,
so an Octane with ``-device sgi-mgras,slot=widget-9`` leaves
``widget-11`` empty. A ``-device`` that leaves a setting unset takes the
slot's default for it: a bare ``-device sgi-kona,slot=io4`` on the
deskside is its InfiniteReality2.

``-vga none``, and ``-nodefaults``, skip the default pipe. With no pipe
``-device`` every topology then starts with no graphics: the machine
cannot tell the two options apart, so it refuses neither. A machine is
one topology whether or not a pipe is fitted. The catalogue's
``headless`` flag only tells a launcher which topologies to offer
without graphics. It is false, with the reason in ``headless-refused``,
for an Onyx2, which SGI never sold without a pipe, and a Fuel. No SGI
machine takes a PC display adapter, so a ``-vga`` value the machine can
tell from the default, such as ``std``, is refused. A machine sees only
the adapter type ``-vga`` leaves behind, and with no ``-vga`` QEMU picks
Cirrus where the binary has it, so ``-vga cirrus`` cannot be told from
no ``-vga`` and is taken as the default.

The machine checks each pipe before it realizes, as its pre-plug
handler, so the check sees the device's final property values,
``-global`` included. It fits the default once every ``-device`` exists,
from its machine-init-done notifier: the device is created while the
machine initializes, so it is not taken for a hot-plugged one, and
realized only when no ``-device`` turned out to be a pipe. The Onyx2's
InfiniteReality pipe and the Fuel's VPro are slots of
the topology's own I/O board records: the builder fits nothing there,
and the pipe that claims the slot takes the record's port, identity
records and the Hub its packets go to, keeping the QOM name the builder
gave it (``io4``, ``io3``, ``graphics-slot``). A
Crossbow port such a record names starts empty, as an empty XIO slot's
does, until its pipe attaches.

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
   * - No pipe where the topology needs one
     - ``<machine>: topology '<t>' does not run without a graphics pipe,
       which -vga none or -nodefaults asks for: <reason>``
   * - No slot given
     - ``<machine>: <device> needs slot= naming one of the graphics slots
       of topology '<t>': <slot> (<placement>), ...``, or ``bus= and
       addr=`` for a PCI card
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
as the InfiniteReality pipe the KONA qtests fit to an Origin 2000.

What each topology offers today:

.. list-table::
   :header-rows: 1

   * - Topologies
     - Slots
     - Default
     - Offers
   * - ``origin200``, ``origin200-gigachannel-loopback``
     - ``pci-5`` to ``pci-7``: ``bus=pci``, ``addr=5`` to ``7``
     - none
     - ``rad4`` (validated at ``pci-5``), ``rad4-km``
   * - ``origin200-dual``
     - ``m0-pci-5`` to ``m0-pci-7``, the first module's: ``bus=pci``
     - none
     - as ``origin200``
   * - ``origin200-gigachannel``
     - ``io6``, and ``pci-5`` to ``pci-7``
     - none
     - every IMPACT product at ``io6``; RAD4 as above
   * - ``origin2000-deskside``, ``origin2000-rack``
     - every empty XIO slot, ``io2`` to ``io12``; ``io7`` up from two
       nodes
     - none
     - every IMPACT product
   * - ``onyx2-deskside``
     - ``io4``
     - ``ir2``: GE14-4, two RM7, 64 MB, ``dg5-8``
     - ``ir2``, ``ir2e``; one or two RMs; ``dg5-8``, ``dg5-2``
   * - ``onyx2-rack``
     - ``io3``
     - ``ir3``: GE16-4, one RM, 256 MB, ``dg5-2``
     - ``ir2e``, ``ir3``; one, two or four RMs; ``dg5-2``, ``dg5-8``
   * - ``octane``
     - ``widget-9`` to ``widget-12``
     - ``si`` at ``widget-11``
     - every IMPACT product
   * - ``octane2``
     - ``widget-9`` to ``widget-12``
     - ``v12`` at ``widget-11``
     - every IMPACT product; ``v12`` at ``widget-11``
   * - ``fuel``
     - ``widget-13``
     - ``v12``
     - ``v12``
   * - ``tezro-tower-multi``, ``onyx350-compat``
     - ``widget-12`` on the PIC fabric
     - none
     - ``v12``

The IMPACT products are ``si``, ``si-t``, ``ssi``, ``mxi``, ``esi``,
``esi-t``, ``essi`` and ``emxi``.
A display board reports its real channel count, two on ``dg5-2`` and
eight on ``dg5-8``, and every channel scans out.
Each topology's ``max-pipes`` says how many pipes it builds at once:
four on ``onyx3400-ir``, two on ``onyx2-rack`` (``io3`` and, from two
node boards, ``io7``, the graphics module's second pipe), one everywhere
else. ``io7`` takes one or two raster managers because the model places it
on the module's left-hand Ktown connector, which is approximated. A second pipe takes
another slot with a second ``-device``, and a node board controls at
most one pipe (the Origin and Onyx2 Hardware Quick-reference Booklet,
HMQ-380-C).

Each pipe presents serials of its own. A pipe slot's records take
graphics serial instances sixteen per slot, numbered by the slot's place
in the catalogue's slot list rather than by the order pipes are given in:
the geometry board the pipe record's own serial (G0, G16, G32 and G48 on
``onyx3400-ir``), the raster, texture and display boards the slot's later
instances. A board two pipes share keeps one serial: the Onyx2 rack's
``io7`` lists the graphics module's Ktown with ``io3``'s serial. Whether a
real second pipe's chain lists the Ktown at all is unknown, so listing it
is approximated. A
topology with several pipes lists at most four slots, so pipe serials stay
below instance 64, where boards fitted by slot name start theirs.

A pipe fitted by slot name takes the slot's name as its device id, and
each of its display channels is a console with the channel as its head.
``screendump device=gbrick1-pipe0-pipe head=1`` captures the third pipe's
second channel, and ``-vnc :1,display=io7,head=0`` shows the Onyx2 rack's
second pipe. Without a device, ``screendump`` takes console 0, the pipe
that realized first, which is the first pipe given on the command line.

.. _sgi-sn-firmware-supply:

Firmware supply
===============

``-bios`` accepts a raw PROM image or SGI's own distribution container,
decoded as :doc:`/system/mips/sgi/prom-image` describes; the machine uses the decoded
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
  added.
- ``ds1742-<board>-data`` and ``ds1742-<board>-clock`` on the IP53 products
  (``origin350``, ``onyx350``, ``tezro`` and ``onyx4-host``): the 2 KiB
  image of the DS1742 linked to IO9 board ``<board>`` and the clock
  record its model keeps beside it. Both are needed; a zeroed image
  starts at the time of day.
- ``<module>-flash`` on an ``origin300`` PCI expansion experiment: the
  16 MiB configuration storage of expansion module ``<module>``, which
  the experiment selects. A new machine's starts blank.
- ``module<N>-nvram`` on ``origin200``, ``origin2000`` and ``onyx2``, the
  2048-byte configuration NVRAM of a module's system controller (see
  :ref:`sgi-elsc-nvram`), N counting the modules that have one: each
  Origin 200 module's embedded ELSC, and each Origin 2000 or Onyx2
  module's MSC. A bound file's contents are used as they are; only a
  store with no file starts from the controller's first power-up state.
- ``<slot>-kona-flash`` for each graphics slot that takes an ``sgi-kona``
  pipe (``io3``, ``io4`` or ``io7`` on ``onyx2``, ``gbrick<G>-pipe<P>-pipe``
  on ``onyx3400-ir``): the pipe's 512 KiB flash EEPROM as a raw image,
  flash offset n at file offset n. The machine gives it to whichever
  ``sgi-kona`` sits in the slot, the default pipe or a ``-device``, unless
  that device names its own ``drive``. Every byte the pipe programs is
  written through, so a video combination ``ircombine`` stores there runs
  at the next graphics initialization in a later process too. A new
  machine's is zero apart from the header state the PROM requires.

The older forms still work but are deprecated, and giving both forms for
one store is refused: ``-drive if=pflash,unit=N`` for node N's flash;
``-drive if=none,id=sgi-baseio-flash<B>`` (B the BaseIO card's I/O board
index) or ``id=sgi-gigachannel-flash<C>`` (C the cabinet) for a BASEIO
PROM flash; and ``-drive if=mtd,index=I`` or ``-drive
if=none,id=sgi-nvram<I>`` with its ``sgi-nvram<I>-clock`` partner for a
Timekeeper, I the I/O board index on SN0 and the node on SN1; and
``-drive if=none,id=sgi-ds1742-data-<board>`` with
``id=sgi-ds1742-clock-<board>`` for an IO9 board's DS1742; and
``expansion-storage=<module>=<drive>`` for an expansion module's
configuration storage.

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

  -M origin300,topology=origin300-pci-expansion,controller-brick-serial.0=1=EXP-A,expansion-storage.0=expansion-enclosure=cfg1

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

The ``origin300-pci-expansion`` topology selects expansion main power and an
independent Bedrock flash attachment. Every launch binds one caller-owned
image, by exact module ID::

  -M origin300,topology=origin300-pci-expansion,expansion-storage.0=expansion-enclosure=cfg1
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
I/O, on the node's own Bridge), ``io6`` (the Origin 2000 standard XIO
board, behind a Crossbow), ``io6g`` (the BaseIO-G, the Onyx2's board),
``menet`` and ``mscsi`` (optional XIO boards), ``io8`` (the Origin 300
IO-8 assembly's PCI population -- a QLogic ISP12160, an IOC3 and a Lucent
OHCI controller -- placed on the SN1 node's own XBridge head F), ``io7``
(the Origin 3000 I-brick's IOC3, Lucent OHCI and TI TSB12LV22 at head F
devices 4, 5 and 6, placed as ``/machine/module<M>/io7`` on the I-brick's
XBridge) and ``fuel`` (the Fuel's onboard I/O). Any other preset name is refused. IP29's onboard I/O is soldered to the
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
``sgi-kona`` (the InfiniteReality pipe), ``sgi-mgras`` (the IMPACT
graphics board, whose widget answers XIO directly with no Bridge) and
``sgi-odyssey`` (the VPro board). Realizing a
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

Migration is supported only between identical builds.

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

An Origin 3000 C-brick's IP35 board has no XBridge: an XIO cable joins
its Bedrock to port A or B of the XBridge in an I/O brick. The builder
creates that chip as ``/machine/module<M>/xbridge``, or ``xbridge0`` to
``xbridge3`` for the N-brick's four chips in one module, gives it the cable's
port as its host port and its module's catalogue id as its bus prefix,
and hands it to the cabled node through the node's ``xbridge`` link; the
node joins and realizes it as an Origin 300 node does its own. The
node's Bedrock decodes the chip at the cable's port and at each of the
chip's heads the node hosts, and nothing at the chip's other Crosstown2
port. A second node cabled to that port is refused, since whether one
XBridge serves two hosts at once is unknown, and so is a host also
cabled to a router. Cables between two crossbars, such as the P-brick's
two links from its first chip to each of the others, are checked like
any other cable and then refused while the machine is built, since no
contract gives the downstream chips' mode.

An XIO cable from one of the chip's other link ports, 8 to 0xd, reaches
one XIO board: a card in an X-brick slot, or a board on the I-brick's
free Crosstown2 port. The board's own attachment names that port and the
chip's host node, whose Bedrock decodes the board's widget there. The
builder creates the board as ``/machine/module<M>/<board id>`` in the
board's own module and joins it to the port with one XIO link once the
nodes have realized. Only the XIO cards ``pci-carrier``,
``pci-shoehorn``, ``menet`` and ``mscsi``, the V-brick's VPro V12 and the
InfiniteReality pipe of a G-brick are built at such a port; any other
graphics pipe or a BaseIO card cabled there is refused. A V12 record cabled to a crossbar port is a
graphics slot named by the record's catalogue id: ``-device
sgi-odyssey,slot=<id>`` fits the pipe there, it is built in the
record's module, and the chip's host node reaches it at that port. The
V-brick's midplane XBridge takes one host, so a two-pipe V-brick, whose
second pipe needs a second C-brick on the other Crosstown2 port, is
refused like any second host.

A G-brick pipe record at such a port is a slot, named by the record's
label, that ``-device sgi-kona,slot=<label>`` claims after machine init,
as the Onyx2's pipe slot is. The pipe joins the chip's port, sits in its
G-brick's module as ``/machine/module<M>/<board id>`` and composes its
packets for the chip's host port, which the crossbar delivers to the
hosting Bedrock. Each pipe of an N-brick is therefore reached only from
the C-brick cabled to the same chip. A G-brick pipe on an Origin 3000
machine is a model arrangement: no Onyx 3000 hardware graph or widget
listing has been captured, so the widget number at which a C-brick sees
its pipe and the pipe's identity there are not established.

An SN1 router is built as ``/machine/module<M>/router`` beside its
module's L1 and that L1's UART, whether or not the topology carries the
L1 management registry the Origin 300 PCI expansion experiment uses, so
an R-brick needs no I/O peer. Its L1 keeps the routed Origin 300
experiment's type profile described under :ref:`sgi-sn-family-rbrick`;
an R-brick's own L1 type is unknown. A router record's ``node-ports`` lists the
only ports a node's cable may use; the builder checks every cable
against it before any device exists.

A node with ``role: memory`` is a Bedrock with memory banks and no
processor, the shape of the Cx-brick's zero-processor board. It realizes
with no CPUs, its boot flash image left as supplied, and answers other
nodes' memory traffic once they have configured and numbered it, as the
firmware reaches a peer through its vector engine. It cannot be the
console node, and a population that fits it a processor is refused.
Whether such a board carries boot flash is unknown, so the builder still
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
Bedrocks reach one L1 is unknown outside that profile.

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
connects; an Origin 200 controller drives its own power and reset outputs
for its module the same way. Each event acts on the one module whose
controller raised it, and the machine derives which parts belong to that
module from the topology it built: the module's node boards with their Hubs
and everything below them (the Hub's adapter and identity parts, the Bridge
and the PCI devices behind it) and their boot flash, the Routers, Crossbows and I/O
boards the topology puts in it, and a board plugged into one of its XIO
slots. The controller is in none of it.

When a controller removes power, the machine resets and halts the
processors of every node in that module with their Hub's processor enables
clear, a state no interrupt ends. The module's other parts keep what they
hold while it is off. When power returns, and when ``rst`` resets the
module, the machine cold-starts that module: its processors are reset and
halted, every part of the module is cold-reset, and the processors run
again from the reset vector if the module has power. Memory keeps its
bytes, which the datasheet allows as an approximation: the PROM tests and
initialises it on a cold start. Other modules run through all of it. The
start runs from a bottom half and not from the controller's own timer. It
stops the module's processors with work queued on each of them and does not
wait: the last completion schedules the rest of the start (the parts' reset
and the processors' release) as a bottom half, so the start never blocks on
a processor thread and runs under record and replay, where that thread
needs a lock the main loop holds. A processor that runs no guest code, in a
paused machine or under qtest, is stopped on the spot, and a start with no
running processor to wait for goes on at once. The start is owed until that bottom half
has run, so a start owed when a migration stream was taken, including one
still waiting for its processors, is part of that stream and is carried out
once the destination runs.

A Hub port reset with bit 0 set restarts the whole machine, not a node
(:doc:`/system/mips/sgi/hardware/hub`): every processor stops at the write, and the machine then
cold-starts every module that has power together, the same way a single
module is started: each module's parts are reset, the I/O its nodes host
included, and the processors run again. The standby controllers are not
reset, so a reboot leaves ``pwr q`` at zero and the PROM sees a warm start.
This is an approximation. Per the Hub datasheet, hardware is described as
resetting the node, with the warm reset reaching the nodes the Routers and
the Hubs' protection registers admit. The model resets the machine, and no
supported guest depends on the difference: every software reset path writes
bits 7 and 0 together and so asks for the whole machine, and the Routers'
masks and the Hubs' protection bit, still readable and writable, do not
steer a reset. A module that is off is left off.

The module start also resets every Crossbow the module's nodes reach and the
boards behind them. On hardware a node's reset leaves a Crossbow and its
boards to the firmware's own warm reset down the XIO link, which the PROM
sends on every cold entry. That reset does not reach the Bridge's PCI side in
the model, and a PROM started over a Bridge that kept IRIX's configuration
took a bus error reading it.

The machine's own reset, ``system_reset``, is an AC cycle: every module and
every controller starts over, and a controller's start powers its module up.
Once every module's power is off with no power-up to come the machine powers
down (:doc:`/system/mips/sgi/hardware/elsc` says when it decides this); IRIX 6.5.30 sends ``pwr d
1`` to every module at ``shutdown -p``.

Unpowered parts are inert. While a module is off:

- Its Hubs refuse every register access with the bus's unassigned-access
  result, as a gated device's blocked access does in
  ``tz_mpc_handle_block()``, and refuse the inbound XIO packets that carry
  its devices' DMA and interrupts the same way, so a device behind an
  unpowered Hub cannot reach memory. The node's I/O windows, and so the
  Crossbows and boards behind them, are switched off the way a PCI device's
  BARs are with the command register (``pci_update_mappings()``). Its
  Routers answer no vector access.
- Their ports read disabled to their peers, which is the state a cable marked
  down already has: a peer Hub reads link-up zero with the down reason left
  clear (the link was up and failed, as the Hub datasheet's rule gives), a
  peer Router reads the port not working, and the claim aliases that lead to
  the module fall to the error floor. A running module's processors and a block
  transfer therefore cannot reach the module's memory, its
  registers or its Routers. The datasheets do not say whether a Router port
  facing a lost peer reads reset-failed; the model leaves it clear.
- Power returning brings the links up through the normal flood after the
  module's parts have reset and before its processors run.

Nothing else about an off module is inert: its memory keeps the bytes it had
(the datasheet allows that approximation, as the PROM tests and initialises
memory on a cold start), and its register state is kept until the reset that
always comes first at power-up.

The state is derived from the controllers' power, which migrates, so none of
it does. It is applied when a machine starts running and after the machine's
own reset, and by the machine's migrated section, which loads after every
device, so a load shows the right gating even while the destination is
paused.

The Hub's I2C adapter window is the one register window that keeps answering
while its module is off, and only from the off module's own view: a peer
module's access to it fails like the rest of the Hub. On hardware the adapter
shares the node's supply, so no host could use it; the model keeps it so the
host side of the standby controller stays reachable over the module's own
bus, which the controller tests and the Origin 200 controller's grants rely
on. The contract (rule 7) has an operator's ``pwr u`` reach an off module's
controller from another module's Hub over I2C, serial or a rack controller.
In the model an operator reaches an off Origin 2000 module's controller
through its serial line, and an off Origin 200 slave's through its master:
the slave has no connector of its own, and a ``pwr u`` the master accepts
while it is itself off powers the master up, whose forward then starts the
slave (:ref:`sgi-elsc-master-slave`). A ``pwr u`` sent to a master
that is already on has nothing to power up and forwards nothing, so the
model has no way to start only the slave from a peer's bus.

A cabinet with no node or controller of its own (the GIGAchannel cabinet)
follows the module its Crossbow's XIO link is attached to: its Crossbow and
board are in that module's parts, are reset with it and are switched off
with its Hub's windows, as the contract has the cabinet's switched domain
follow the attached module.

These limits remain. The model does not know whether ``rst`` reaches the
Routers, Crossbows and XIO boards, whether it gives the processors a cold
or a soft entry, or which Hub error registers survive it, so it resets
every part of the module and gives the processors the cold entry. A module
that is off keeps the register state it had, which returns on its next
reset; the datasheet's loss of all state is only visible after power
returns, when everything has reset. No
topology with more than one Origin 2000 module is built, so the
several-module Origin 2000 path is the same code as the Origin 200 pair's
and is only exercised there.

The module controller's NMI output, which it pulses for ``nmi`` and for its
heartbeat watchdog, and the Hub's per-processor NMI trigger both reach a
processor's NMI input through an OR gate per processor (``TYPE_OR_IRQ``, as
boards that share an interrupt line use it), so neither can shorten or hide
the other. Two overlapping NMIs are one edge at the pin, as on a wired-OR
line. On an Origin 200 each node's own controller drives an NMI output that
the node wires the same way to its processors.

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
product. On the Origin 3200 it is the I-brick's Fibre Channel loop,
which ``scsi-adapters`` lists as type ``fc-loop``. The netboot adapter names the device the stock PROM's netboot
has been confirmed to drive on the model; ``unsupported`` means the
offering names none. The Origin 200 offering also
asks launchers without a display to pass ``debug-leds=off``. Origin 300
launchers support booting an installed IRIX disk and netboot but not an
IRIX installation run; that is a harness limit, not a machine one. The
Onyx2 offerings reuse the Origin 2000 module's dialogue, console and
disk bus, since they are built on the same IP27 node boards and stock
PROM.

The install media list ``netboot`` where the harness installs IRIX by
booting the miniroot over the netboot adapter from an install server, the
PROM's "Install System Software" from a remote directory. The IRIX
6.5.30 ``prom(1M)`` page documents that menu choice and its network
source for SGI PROMs generally. Complete IRIX 6.5.30 installs this way reach the
installed system's login prompt on the Origin 2000 with an SI board, the
Onyx2 rack, the Octane with SI and the Octane2 with V12. The Onyx2
deskside has reached the miniroot's first question, not a complete
install. The Origin 2000 deskside and two-module offerings inherit the
entry from the module layer. Fuel lists ``netboot`` for its serial
installation.
``disk-image`` means a launcher can boot an installed disk image.

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
interrupt delivery. The routing is an inference from those two sources.

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

Router links
============

Each Router port reports in its status register whether its link is
working, whether its last link reset failed, and whether the link runs
eight bits wide. The width is a property of the link, settled from
both ends' forced-width bits when the link comes up or is reset: a
Router's per-port force bit in its diagnostic parameters, a Hub's
diagnostic bit 12, and the link runs eight bits wide if either end
forces it. Writing a force bit alone does not change a running link.
A port link reset (port reset bits 6:1, several at once if several are
set) completes at the write: the link renegotiates and both ends read
the new width, a Hub at the far end latches its port error's
link-reset bit, and the Router itself latches no error and keeps its
diagnostic parameters, tables, port parameters, histograms and error
counters. A port with no peer to answer, uncabled or with its cable
down, does not come out of the reset: link-working stays clear and
reset-failed reads set until the link next comes up. A warm reset
of the Router itself (port reset bit 7) clears the force bits and
retrains every working link the same way, so its peers settle the width
again and a Hub at the far end latches its link-reset bit. The administrative reset
(bit 0) clears the force bits but keeps the link engines connected,
so each link keeps its width until it is next reset. BIST completion
applies the warm-reset extent to the Router's own state, and the Router
datasheet's warm-reset extent retrains fitted links, but the model does
not retrain the peers at BIST completion. This is a stated departure:
retraining would latch bit 37 at every neighbouring Hub on each
self-test run, new guest-visible behaviour that no evidence supports.
A capture of a neighbouring Hub's port error register and the Router's
port status before and after a Router self-test on real hardware, or
the Router specification's description of BIST completion, would
settle it. The reset fences
are not consulted for a link reset, since whether they apply is not
known and software only resets open ports. The duration, the
link-down interval and the fate of traffic in flight are not modelled;
these are approximations of the Router datasheet's inferred contract,
not measured behaviour.

Devices
=======

The node ASICs, bridges, controllers and I/O chips this family builds
from have their own pages: :doc:`/system/mips/sgi/hardware/hub`, :doc:`/system/mips/sgi/hardware/bridge`,
:doc:`/system/mips/sgi/hardware/elsc`, :doc:`/system/mips/sgi/hardware/ioc3`, :doc:`/system/mips/sgi/hardware/qlogic-isp`,
:doc:`/system/mips/sgi/hardware/xbridge`.
