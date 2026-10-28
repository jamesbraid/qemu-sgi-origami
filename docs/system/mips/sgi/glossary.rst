.. SPDX-License-Identifier: BSD-3-Clause

.. _sgi-glossary:

========
Glossary
========

Terms the SGI pages use without explaining each time. The definitions
are vocabulary only. The part and platform pages carry the evidence and
the ratings.

.. glossary::
   :sorted:

   XIO
     SGI's I/O link and connector family. A node controller reaches its
     I/O boards, bridges and graphics over XIO ports, usually through a
     Crossbow. See :doc:`hardware/xbow`.

   Crosstalk
     The packet interconnect behind the XIO ports. It joins a node
     controller to numbered widgets, with a crossbar in between where a
     board has one. See :doc:`hardware/xbow` and :doc:`hardware/bedrock`.

   widget
     A device on Crosstalk, addressed by a widget number. Every widget
     has a common register block at the bottom of 16 MB of widget space.
     The Crossbow itself answers as widget 0. See :doc:`hardware/xbow`
     and :doc:`hardware/bridge`.

   Crossbow
     The eight-link XIO crossbar. It routes packets between the host links
     and the I/O widgets by destination widget number. See
     :doc:`hardware/xbow`.

   NUMAlink
     The network of the second Scalable Node generation (SN1), which joins
     Bedrock nodes and routers. The pages describe NUMAlink 3. See
     :doc:`hardware/sn1-router` and :doc:`sn`.

   CrayLink
     The network of the first Scalable Node generation (SN0), which joins
     Hubs through Routers. See :doc:`hardware/router` and :doc:`sn`.

   node
     The unit a node controller manages: one controller, its processors
     and its local memory, with one XIO port and one network port. See
     :doc:`sn`.

   node board
     The board that carries a node. The Origin 2000 and Onyx2 use IP27
     node boards, one to four per module. See :doc:`ip27` and
     :doc:`ip35`.

   module
     The enclosure that holds node boards and their shared power, system
     controller and I/O slots. An Origin 2000 or Onyx2 module holds up to
     four node boards and has one ELSC. See :doc:`ip27`.

   brick
     A separate rack-mounted enclosure of the Origin 3000 generation,
     such as the C-brick or I-brick. Bricks are joined by NUMAlink and
     XIO cables. See :doc:`hardware/bricks`.

   Hub
     The node controller of the SN0 machines. It joins a node's two
     processors, memory and directory to one XIO port and one CrayLink
     port. See :doc:`hardware/hub`.

   Bedrock
     The node controller of the SN1 machines, the successor to the Hub. It
     joins up to four processors, memory and directory to one Crosstalk
     port and one NUMAlink port. See :doc:`hardware/bedrock`.

   Bridge
     The XIO-to-PCI bridge of the SN0 machines and the Octane. One XIO
     widget in front of a PCI host bridge with eight device slots. See
     :doc:`hardware/bridge`.

   XBridge
     The SN1 XIO-to-PCI chip: a Crossbow-style crossbar fused with two
     Bridge-style PCI host bridges. See :doc:`hardware/xbridge`.

   PIC
     SGI's XIO-to-PCI-X bridge, used on the IP53 boards. One PIC is one
     XIO widget with two bridge heads, each with its own PCI bus. See
     :doc:`hardware/pic`.

   IOC3
     A PCI base I/O controller that carries serial ports, Ethernet,
     PS/2 keyboard and mouse, a parallel port and a byte bus for a clock
     part. See :doc:`hardware/ioc3`.

   IOC4
     The IO9 board's PCI I/O controller: four serial ports, PS/2, one
     ATA/ATAPI channel and a byte bus for a timekeeper. See
     :doc:`hardware/ioc4`.

   L1
     The per-chassis system controller of the SN1 machines. It runs from
     standby power and handles identity, power, reset and the operator
     console. See :doc:`hardware/l1`.

   ELSC
     The entry-level system controller, also called the module system
     controller, of the SN0 machines. It manages power, reset, the front
     panel and an alternate console for one module. See
     :doc:`hardware/elsc`.

   PROM
     The boot firmware of an SGI machine. QEMU runs the stock PROM image
     the user supplies and never ships one. See :doc:`prom-image`.

   Command Monitor
     The PROM's interactive command prompt, entered from the System
     Maintenance Menu. It can dump registers and print the hardware
     inventory. See :doc:`prom-image` and :doc:`measured-machines`.

   NASID
     The node number carried in a physical address. It selects the node
     that owns the address and is held in the Hub's status register.
     See :doc:`ip35` and :doc:`hardware/hub`.

   hardware graph
     The tree of devices that the IRIX kernel builds at run time and
     exposes under ``/hw``. Reading it shows how the kernel named a
     device. See :doc:`measured-machines`.

   IP number
     The board-family name the platform pages use, such as IP27 for the
     Origin 2000 platform, IP30 for the Octane and IP35 for the Origin
     3000 platform. A platform page covers the machines that share one.
     See :doc:`index`.

   SN0
     The first Scalable Node generation, built from the Hub and CrayLink
     on the IP27 platform. See :doc:`sn` and :doc:`ip27`.

   SN1
     The second Scalable Node generation, built from the Bedrock and
     NUMAlink 3 on the IP35 platform. See :doc:`sn` and :doc:`ip35`.
