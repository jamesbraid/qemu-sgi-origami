.. SPDX-License-Identifier: BSD-3-Clause

.. _sgi-router:

===================================
SGI Router (Spider) CrayLink switch
===================================

The Router is the six-port switch of the Origin 2000 and Onyx2 module
midplane and cable fabric. QEMU models its register target, route
lookup and vector-routed access in ``hw/misc/sgi-router.c``; it does not
model packet timing, virtual channels or credits. This page says what
public sources establish about the chip, which statements are emulator
choices, and which register facts have no public source. Ratings follow
:ref:`sgi-ip30`.

Sources: *Origin200/2000 IP27 PROM Technical Reference Manual*
(108-0170-001; pages are PDF pages, with the printed page added where
the manual's own numbering is cited), *Origin and Onyx2 Theory of
Operations* (007-3439-002), Mike Galles, "Spider: A High-Speed Network
Interconnect", IEEE Micro 17(1), 1997 (``web:galles-spider-1997``), US
patent 5,721,819 (``web:us-patent-5721819``) and the Linux SN0 Hub
headers.

.. _sgi-router-ports:

Ports and message layer
=======================

.. list-table::
   :header-rows: 1
   :widths: 44 12 44

   * - Fact
     - Rating
     - Source
   * - The chip has six full-duplex ports and a non-blocking crossbar;
       messages use four virtual channels with credit-based flow control
       and CCITT-CRC link protection with hardware retry.
     - established
     - Galles, printed pages 34-39; 007-3439-002 Figure 3-3 and printed
       pages 80-83 (PDF 94-97).
   * - Ports 4 to 6 are internal to a module midplane; ports 1 to 3
       are the cable connectors, numbered top to bottom, and the green
       LEDs show connection state for ports 1 to 6.
     - established
     - 108-0170-001 section 6, printed page 6-1 (PDF 95) and the
       port-numbering text of section 1.1 (PDF page 15).
   * - On the Origin 2000 Router board three ports use single-ended STL
       inside the enclosure and three use differential PECL between
       modules.
     - established
     - 007-3439-002 printed pages 80-83; Galles.
   * - A message is a header micropacket plus data micropackets; the
       header carries a 9-bit destination, 4-bit direction, 8-bit age and
       2 bits of congestion control.
     - established
     - Galles, printed pages 34-39. How many header bits can also
       carry data is open: the paper's figure and prose differ.
   * - The destination is the 9-bit node number placed in physical
       address bits 40:32; its upper 5 bits are the meta address and the
       lower 4 the local address.
     - established
     - 108-0170-001 section 6.1, printed page 6-1 (PDF 95); Galles.

.. _sgi-router-routing:

Routing tables
==============

.. list-table::
   :header-rows: 1
   :widths: 44 12 44

   * - Fact
     - Rating
     - Source
   * - A chip holds a 16-entry local table indexed by the destination's
       4 local bits and a 32-entry meta table indexed by its 5 meta
       bits. Each Router entry is 24 bits, six 4-bit directions, one per
       output port (a Hub's entries hold one 4-bit direction).
     - established
     - 108-0170-001 ``rtab`` command text (PDF page 74) and the register
       names on printed page 3-15 (PDF 53); Galles; patent 5,721,819.
   * - Lookup is pipelined one hop ahead: a chip looks up the direction
       the next chip will use.
     - established
     - Galles, printed pages 34-39; patent 5,721,819.
   * - Routes are software-programmed, static between every pair of
       endpoints, and must be cycle-free. The Theory of Operations
       describes the routing as adaptive; the two accounts are not
       reconciled.
     - unknown
     - 007-3439-002 printed page 80 against Galles printed page 36.
       A Router register document or a console observation of two
       messages to one destination taking different paths would settle
       it. The model forwards by the tables the firmware programs.
   * - The bit packing of an entry, the addresses of the meta-id and
       force-local controls and the order firmware programs them.
     - unknown
     - No public source states them. The model's encoding is an
       emulator choice and a guest that reads the tables back sees only
       what it wrote.

.. _sgi-router-vector:

Vector routing
==============

Before node numbers exist, firmware reaches Router registers and a subset
of a remote Hub's network-interface registers by giving the route. A
vector is a string of output port numbers 1 to 6, one per nybble, in
reverse order: ``0x512`` leaves the Hub into a Router, leaves port 2 of
that Router, port 1 of the next and port 5 of the third. Vector ``0x0``
reaches the device attached to the local Hub; the response follows the
reverse path. Address 0 is the status and revision register of both a
Hub and a Router, so a probe can tell them apart (established,
108-0170-001 section 6.1, printed page 6-2, PDF 96). Linux gives the
Hub chip identity as 0 and the Router's as 1 (Linux
``arch/mips/include/asm/sn/sn0/hub.h`` at
``linux:db2ddb87143519e20a95aa36c60b36107b736a58``).

Vector routes inside one fully populated module are printed in Table 6-1
(printed page 6-3, PDF 97): from N1 or N2, N1 is ``5``, N2 ``4``, N3
``0x56``, N4 ``0x46``, R1 ``0`` and R2 ``6``; from N3 or N4 the node
vectors swap and R1 is ``6``, R2 ``0``. That fixes R1 port 5 to N1,
port 4 to N2 and port 6 to R2, and R2 port 5 to N3, port 4 to N4 and
port 6 to R1 (established). With a Star Router the table is ``5``, ``4``,
``6`` and ``3`` for N1 to N4 (Table 6-2, printed page 6-4, PDF 98).

.. _sgi-router-registers:

Register names and the register block
=====================================

The PROM manual lists forty-six Router registers by name (printed page
3-15, PDF 53): the status and revision, port reset, protection
configuration, global and diagnostic parameters, two scratch
registers, the one-wire (NIC) register, six built-in-self-test
registers, and for each of the six ports parameters, status and error,
error clear, reset mask and histogram registers, plus the local and meta
tables. Established.

The offsets, field layouts, access types and reset values behind those
names have no public source this page can cite. Linux's SN1 router
header (``include/asm-ia64/sn/sn1/router.h`` in Linux 2.5.0) describes a
different, eight-port part with other offsets and is not evidence for
the Origin 2000 chip. The model's offsets and fields (``RR_`` names in
``hw/misc/sgi-router.c``) were taken from firmware observation and
vendor material this page does not cite, so every register offset,
field and reset value is *unknown* for citation purposes and is an
emulator choice. What would settle them is a Router register document
or register reads from a real chip. Facts the model gives that are
specific, rather than names:

.. list-table::
   :header-rows: 1
   :widths: 44 12 44

   * - Model behaviour
     - Rating
     - Basis
   * - Status register reports chip identity 1 (Router) and revision 1.
     - approximation
     - Identity 1 follows Linux. The revision is a model choice: IRIX
       inventories print ``ROUTER Revision 2`` for the board, and a
       photographed package is marked 1.1; no source gives the register
       field's silicon value.
   * - Reply-source encoding of a vector response is not constructed by
       the ports.
     - unknown
     - Not stated in any public source.
   * - Reads of an unmapped vector address.
     - unknown
     - Not stated in any public source; logged, not answered.
   * - Packet timing, credits, age arbitration and DAMQ occupancy.
     - approximation
     - Not modelled; the model forwards a message at once by table.
       A functional model may collapse them while preserving the route
       taken, per-port error state and reset-fence effect.

.. _sgi-router-board:

Board attachment
================

An Origin 2000 module midplane carries up to two Router boards (R1 and
R2), each with one ASIC (:ref:`sgi-origin2000-module-routers`). A published
two-module Origin 200 connects its Hubs directly, so the six-port part
does not justify inserting a Router there (007-3708-002 Figures 1-4 and
1-5). A Star Router is a carrier with no Router ASIC; a Null Router is a
passive Hub-to-Hub link (108-0170-001 section 6.2, printed pages 6-2 to
6-6, PDF 96-100).
