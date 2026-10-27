.. SPDX-License-Identifier: BSD-3-Clause

.. _sgi-bricks:

==============================================
Origin 3000 bricks and the NUMAlink module
==============================================

The Origin 3000 and Onyx 3000 generation builds a system from separate
rack-mounted enclosures called bricks, joined by NUMAlink 3 and Crosstown2
cables. This page covers the C-, CX-, D-, G-, I-, IX-, N-, P-, PX-, R-,
V- and X-bricks and the Origin 300 NUMAlink module: what each is, what it
holds, how it connects, and how much of it the model catalogue builds. The
catalogue is the set of YAML layers under ``hw/mips/sgi/models/layers/``
and the compiler in ``scripts/sgi_model/``. Four bricks have a module
layer there: the C-brick (``modules/c-brick.yaml``), the I-brick
(``modules/i-brick.yaml``), the R-brick (``modules/r-brick.yaml``) and the
NUMAlink module (``modules/numalink-module.yaml``). The V-brick has a
machine-builder preset and a controller profile but no layer. The other
seven bricks have no code at all and are described here as reference only.
No QEMU machine builds any brick: the two catalogue compositions that hold
bricks are reference models, which the compiler refuses to compile. Facts
are rated as described in :ref:`sgi-ratings`.

.. list-table::
   :header-rows: 1

   * - Brick
     - Role
     - Catalogue
   * - C-brick
     - Compute node (one Bedrock)
     - Layer, reference composition
   * - CX-brick
     - Up to four compute nodes and a router
     - Reference only
   * - D-brick
     - Fibre Channel disk enclosure
     - Reference only
   * - G-brick
     - InfiniteReality graphics enclosure
     - Reference only
   * - I-brick
     - Base I/O (XBridge, PCI, IO7)
     - Layer, reference composition
   * - IX-brick
     - Base I/O (three PICs, PCI-X, IO9)
     - Reference only
   * - N-brick
     - Passive C-brick to graphics pipe connector
     - Reference only
   * - P-brick
     - PCI expansion (three XBridges)
     - Reference only
   * - PX-brick
     - PCI-X expansion (three PICs)
     - Reference only
   * - R-brick
     - Eight-port NUMAlink router
     - Layer, in no composition
   * - V-brick
     - VPro graphics enclosure
     - Builder preset and L1 profile, no layer, no topology
   * - X-brick
     - Legacy XIO expansion (one XBridge)
     - Reference only
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
  cited through :ref:`sgi-sn-family-ibrick` and
  :ref:`sgi-sn-family-bedrock`. No inventory of a CX-, D-, G-, IX-, N-, P-,
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
  :src:`007-4264-002 pp. 8, 10, 62`). - A system holds G-bricks or V-bricks,
  not both (*established*, :src:`007-4264-002 pp. 10, 21`). - A C-brick on
  an R-brick must use ports 2 to 5 (*established*,
  :src:`007-4311-002 p. 36`). - An Origin 3200 holds one or two C-bricks, at
  most two I/O bricks of which one is an I-brick, and no R-brick
  (*established*, :src:`007-4311-002 p. 87`). - The D-brick is not on the
  NUMAlink or XIO network. It hangs off a Fibre Channel adapter in an I- or
  P-brick (*established*, :src:`007-4240-002 p. 168`).

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
:src:`108-0266-001 p. 28`). Local memory is 512 MiB to 8 GiB in banks 0 to
7, two banks per DIMM pair, with the two DIMMs of a pair matching
(*established*, :src:`108-0240-002 pp. 2-5, 2-6`). The guides disagree
on the bank count, the minimum size and the cache size, and the layer
records none of them.

Connections:

- One rear ``LINK (NI)`` NUMAlink 3 connector, to an R-brick or, in an
  Origin 3200, to another C-brick. - One rear ``XIO (II)`` Crosstown2
  connector, to an I-, P- or X-brick or, for graphics, to a V-brick. - An L1
  controller on the motherboard with a front display, a DB-9 console port
  and a USB port for an L2 (*established*,
  :src:`007-4240-002 pp. 115-116, 121`).

Published inventories of an Origin 3200 and an Origin 3400 report the
Bedrock as ``Hub`` revision 2 at 200 MHz and the PROM as 6.210 on every
C-brick (*observed*; see :ref:`sgi-sn-family-bedrock`). That the C-brick
and Origin 300 Bedrocks are the same silicon is *inferred* from the
matching reports. Whether ``Hub revision 2`` is the value of the Bedrock's
revision register is *unknown*. A revision register read on a C-brick
would settle it.

Software expectations: OpenBSD reads bits 15:12 of the Hub board
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
possible CPUs. The layer has no ``LINK (NI)`` connector: the Origin 3200
composition joins the two Bedrocks directly on a ``ni-cable`` bus. The layer
holds no L1 component and no memory fit. The widget part and manufacturer
are software expectations, not reads from a C-brick, so the Bedrock identity
is an *approximation*.

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

Catalogue: reference only. The IP53 layers in ``assemblies/ip53.yaml`` and
``modules/ip53-origin350-module.yaml`` describe the Origin 350 module and
do not transfer: that module has two or four processors, a PIC at widget
15 and an IO9, and the CX node board has none of these.

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

The D-brick has no L1 controller, no NUMAlink or XIO connection and no
module number. A microcontroller on the operator-panel module monitors
power, cooling and temperature over an I2C bus and runs SCSI enclosure
services (*established*, :src:`007-4240-002 pp. 69, 72, 163, 166`; the
L1 brick-type table on p. 235 has no D). No public IRIX document says a
guest queries the enclosure services.

Catalogue: reference only. The host side of the loop is the QLogic
ISP2200 adapter (:doc:`qlogic-isp2200`).

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

Catalogue: reference only. The InfiniteReality pipe itself exists as the
``sgi-kona`` device (:doc:`graphics`) and attaches only in an Onyx2
Crossbow slot. No Ktown2, G-brick chassis, second pipe or Crosstown2
attachment exists.

.. _sgi-bricks-ibrick:

I-brick
=======

The I-brick is the base I/O brick. It holds one XBridge with eight ports:
Crosstown2 ports A (10) and B (11), Crosstalk ports 8, 9, C and D, and PCI
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
(*observed*; the table is in :ref:`sgi-sn-family-ibrick`). The bus is
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
(*established as drawn*, :src:`007-4604-001 p. 4`). Each bus runs at its
own speed and a PCI card in a bus forces it to PCI mode.

The IO9 carries an external SCSI connector, an internal SCSI port for the
two front drive bays, a Gigabit Ethernet connector, RTI and RTO connectors
and, through daughtercards, two or four RS-232 ports (*established*,
:src:`007-4604-001 pp. 3, 5, 10`). A DVD-ROM is in the front bay. Its chips
are a QLogic SCSI controller, a Broadcom BCM5701 and an IOC4. That the
QLogic part is an ISP12160 is *inferred* from a report on a sibling
platform, which also prints the IO9's PCI numbering as devices 1, 3 and 4.

Connections: rear connectors ``XIO 10`` and ``XIO 11``, each to one
compute brick, the second optionally to another; an L1 controller with a
display (*established*, :src:`007-4604-001 pp. 6, 9-10`).

An Origin 350 guest names its own compute module ``IXbrick`` in the
hardware graph. That module has a Bedrock and one PIC, and is not this
brick. Widget numbers of the PICs, PCI device numbers per slot, and the
PIC revisions are *unknown*. An inventory of a standalone IX-brick would
settle them.

Catalogue: reference only. The PIC (:doc:`pic`), IOC4 (:doc:`ioc4`) and
BCM5701 (:doc:`bcm570x`) are modelled for the Origin 350 but not placed in
a three-PIC enclosure.

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

Catalogue: reference only. A model of the N-brick would carry an L1 and no
widget, register window or board record, and a write to any address in a
supposed window would have to fault visibly.

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

Catalogue: reference only. The nearest built device is the Origin 300
expansion module in :doc:`pci-expansion`, on :doc:`xbridge`. The
composition rules name ``p-brick`` as one of the I/O bricks an Origin 3200
may hold, but no layer by that name exists.

.. _sgi-bricks-pxbrick:

PX-brick
========

The PX-brick is the PCI-X successor to the P-brick: a 4 U enclosure with
twelve slots as six buses of two, each taking PCI or PCI-X cards at 33, 66,
100 or 133 MHz (*established*, :src:`007-4605-001 pp. 1, 8-9`). Three PIC
ASICs provide the buses and two Crosstown2 ports. As drawn, the two
external ports attach to the middle PIC, which has one link to each of the
others (*established as drawn*, :src:`007-4605-001 p. 3`). The addendum
lists InfiniteReality pipe connectivity as an enhancement but describes no
connector, card or cable for it (*established*, p. 1), so that attachment
is *unknown*.

Connections: rear ``XIO 10`` and ``XIO 11`` (the second for an optional
dual-port arrangement), an L1 controller with a display, three hot-swap
fans, a slot power LED and a fault LED per slot.

How many links join the middle PIC to each other PIC, the PIC widget
numbers, head offsets and slot wiring are *unknown*. The Origin 350 PIC
contract does not transfer to this three-PIC enclosure.

Catalogue: reference only.

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
:src:`108-0301-001 pp. 3-2, 3-3, 3-7 to 3-10`; :src:`007-4311-002 pp. 36-37`; :src:`108-0240-002 p. 2-20`).
A published Origin 3400 inventory reports the router board as part
``030-1634-002`` revision ``-B`` (*observed*). How the controller makes the
ASIC refuse two ports is *unknown*.

Catalogue: ``modules/r-brick.yaml``. It describes the chassis, one SN1
router component with eight ports, connectors for ports 1 and 3 to 8, a
fabric bus for each, and one open ``c-brick-cable`` bus that a
composition binds to a cable. It does not model the disabled-port variant.
The layer has no component for port 2, so seven of the eight connectors
exist (*approximation*: nothing in the layer's sources removes port 2).
No composition holds an R-brick: the Origin 3200 forbids one, and a
resolver test instances the layer alone. The SN1 router device that the
Origin 300 routed topology builds is described in :doc:`sn1-router`.

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
  (*established as drawn*). Its function is *unknown*.
- The four slot links are *inferred* to be the XBridge's Crosstalk ports
  8, 9, C and D, one per slot.

No V-brick has been measured or inventoried. The board revision, the V12
widget number and the link rate (800 MB/s here, 1.2 GB/s for the same
chip's other cabling) are *unknown*.

Catalogue: no layer and no topology. The machine builder keeps a
``vbrick`` I/O board preset (``qapi/sgi-machines.json``,
``hw/mips/sgi/sn-machine.c``). When a topology names the preset on a node's
XBridge external port ``0xb``, the builder adds a second ``sgi-xbridge``
as the enclosure, links it to the node over a cable object, and projects
that chip's widget 8 (the V12 slot) and widget 0 (its crossbar) into the
first and second 8 MiB of the node's widget ``0xb`` window
(``sgi_xbridge_project_vbrick_pio()``). That split is a QEMU address
choice, not a hardware route (*approximation*). The V12 is the topology's
pipe slot, claimed by a pipe ``-device``. The builder also adds an
``sgi-l1`` with brick type ``vbrick-discovery-reference``, which carries no
identity and refuses identity, EEPROM, environment and lifecycle queries
(``LOG_UNIMP``). No shipped topology uses any of this, because the Origin
300 pairing it was written for was never an SGI configuration.

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

Catalogue: reference only. The composition rules name ``x-brick`` as an
I/O brick an Origin 3200 may hold, but no layer by that name exists.

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
inventory reports the router board as part ``030-1634-003`` revision
``-A`` and the router as ``Revision 1`` (*observed*,
:src:`nekonomicon-16727514`). The board uses the same part root as the
R-brick's (``030-1634``), and the R-brick's board is revision ``-B`` of
``-002``. The fitted ASIC part, its count and its silicon revision are
*unknown*. The manuals do not show eight ports on one chip. Public Linux
SN1 headers define an eight-port router family with chip identifier
``0x3017``, which makes the mapping from family to product *inferred*
(see :doc:`sn1-router`).

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

No brick has been measured on a physical machine for this project. The
*observed* statements above come from published inventories of other
owners' machines.

.. _sgi-bricks-limits:

Model limits
============

- No machine builds a brick. ``origin3200.yaml`` (two C-bricks and an
  I-brick) and ``origin300-numalink-module.yaml`` are reference models, and
  the compiler stops with an error naming ``model_role 'reference'``. The
  machine list does not offer them.
- The CX-, D-, G-, IX-, N-, P-, PX-, V- and X-bricks have no layer. The
  composition rules can name ``p-brick``, ``x-brick``, ``g-brick``,
  ``n-brick`` and ``v-brick``. A rule that names a brick with no layer
  counts zero of it, so a misspelled id in a maximum, exclusion or
  requirement rule passes silently.
- The model validator accepts an L1 brick-type character only for the two
  boards a source establishes, ``C`` (IP35) and ``4`` (IP45). An I-, P-,
  R- or other brick cannot yet carry one.
- The layers describe topology and identity, not behaviour. They carry no
  register map, reset value, link timing or interrupt route for any brick.
  Router tables, Crosstown2 speed selection and L1 requests for bricks
  other than the node boards are not modelled.
- The C-brick layer lacks the ``LINK (NI)`` connector, an L1 and a memory
  fit. The R-brick layer lacks port 2. The I-brick layer attaches only
  host port A.
- The builder's V-brick preset is unreachable from any shipped topology.
- A guest request to a part that does not exist gets no brick-specific
  answer. The V-brick controller profile logs ``LOG_UNIMP`` for each
  refused task.

.. _sgi-bricks-tests:

Tests
=====

``scripts/tests/test_sgi_model_validation_contract.py`` has
``Origin3000BrickValidationTests``. They check that the Origin 3200
reference validates, that the I-brick's connectors record XBridge ports
``0xa`` and ``0xb`` and that a connector port must be a crossbar XIO port,
be unique and sit on a crossbar, that the C-brick cables to I-brick port A,
that an XIO cable has two distinct XIO connectors that share a bus and
carries evidence, and that the C-brick's L1 brick type is ``C`` while any
other letter, or a letter on a non-chassis, is refused.

``scripts/tests/test_sgi_model_resolution_contract.py`` checks that the
C-brick has one ``XIO (II)`` connector with a Crosstown2 interface on an
``xio`` bus mastered by its Bedrock, that the I-brick attaches to the
C-brick through a bus binding, that the R-brick's router reaches its cable
through a binding and fails without one, the composition-rule counts,
exclusions and requirements (including ``g-brick`` against ``v-brick`` and
``n-brick`` only with ``g-brick``), that the Origin 3200 resolves to two
C-bricks and an I-brick and refuses a third C-brick, no I-brick, three I/O
bricks or an R-brick, and that it still does not compile.

``tests/qtest/sn1-l1-test.c`` covers the V-brick controller profile
(``sn1/l1/vbrick/reference-refusals``, ``profile-mismatch`` and
``compat-refusal``): refused identity and lifecycle queries, and a refused
migration between mismatched profiles. ``tests/qtest/board-test-fixture.c``
builds the V-brick projection as a fixture for the board qtests.
