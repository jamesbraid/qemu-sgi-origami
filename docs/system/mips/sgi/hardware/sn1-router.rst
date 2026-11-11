.. SPDX-License-Identifier: BSD-3-Clause

.. _sgi-sn1-router:

===========================
SN1 router: NUMAlink router
===========================

Used in :ref:`sgi-ip35` (the Origin 300 NUMAlink module, in the
``origin300-numalink-pci-expansion`` topology). The Origin 3000 R-brick
and CX-brick router board (:ref:`sgi-bricks`) are eight-port NUMAlink
routers too. No cited source names their router ASIC, so that they
carry this part is *unknown*. The ``origin3000`` machine's
``origin3400``, ``onyx3400-ir``, ``origin3900-ip53`` and ``origin3900-ip59`` topologies build this
model for them, an *approximation*.

.. seealso::

   Platform pages: :ref:`sgi-ip35`.

.. seealso::

   - Router (:doc:`router`): the :term:`SN0` interconnect router.

The SN1 router is the eight-port NUMAlink switch of the SN1 interconnect.
In an Origin 300 system two base modules can be cabled to each other
directly, *established* :src:`007-4499-002 pp. 12, 36, 52`. A larger system joins
two to eight base modules and up to four PCI expansion modules through
the NUMAlink module's eight ports, *established*
:src:`007-4498-002 pp. 2, 10, 93, 98`. QEMU's one routed topology uses
three of those ports. QEMU models it as the ``sgi-sn1-router`` type in
``hw/misc/sgi-sn1-router.c``, with the interface in
``include/hw/misc/sgi-sn1-router.h``. The SN1 machine builder in
``hw/mips/sgi/sn-machine.c`` creates one per router in the topology and
joins its numbered ports to :term:`Bedrock` nodes and I/O peers with
``sgi-net-cable`` objects (``port1-cable`` to ``port8-cable``). Only the
topology ``origin300-numalink-pci-expansion`` has a router. An ordinary
Origin 300 does not acquire one. This is the SN1 router, not the six-port
Origin 2000 router in :doc:`router`.

The router has no window in any processor's address space: the machine
does not map its register region, so a processor never reaches it. Guests reach
its registers through vector operations launched from a Bedrock node
(:doc:`bedrock`). The model does not carry packets, virtual channels or
credits. It decodes the register target, applies the route tables guests
program and follows cabled links. No ASIC or chip revision is established
for the part fitted to the NUMAlink module. The model reports revision 1,
the only value it accepts, which matches the ``Revision 1`` a published
Origin 300 inventory reports for the router (*observed*,
:src:`nekonomicon-16727514`). Facts are rated as described in
:ref:`sgi-ratings`.

.. _sgi-sn1-router-sources:

Sources
=======

- *Origin 300 System with NUMAlink Module User's Guide*,
  :src:`007-4498-002`. It describes an eight-port NUMAlink module with a
  router ASIC (arbitration and a crossbar) and the attachment rules for
  base and PCI expansion modules, printed pp. 129 and 132-135. It names
  neither the ASIC nor its revision and gives no map from the eight
  external connectors to register link numbers.
- The SN1 router register header of the Linux history tree, tag ``v2.5.0``
  (commit ``381ab1f538162eb985c9244aa7d56e54315881ab``),
  ``include/asm-ia64/sn/sn1/router.h``, with the Bedrock local-block vector
  registers in ``include/asm-ia64/sn/sn1/hublb.h`` and ``hublb_next.h`` and
  the 16550 register numbers in ``sn1/uart16550.h``, from the
  ``linux-history`` tree of :ref:`sgi-references`. They are software
  expectations. They give register offsets, field positions and table
  sizes. They do not give reset values, readback masks or the routing
  rule. Its vector read and write routines are stubs, so the tree never
  shows a completed router access.
- OpenBSD ``sys/arch/sgi/sgi/ip27_machdep.c`` at ``2df91d80c104``, which
  treats the Origin 300 as an IP35 subtype. This does not say which router
  the Origin 300 uses.
- Mike Galles, "Spider: A High-Speed Network Interconnect",
  :src:`galles-spider-1997`, and US patent 5,721,819,
  :src:`us-patent-5721819`, for the general routing scheme of the earlier
  router. US patent 6,085,303, :src:`us-patent-6085303`, describes a
  32-context barrier in an SN1 embodiment. Neither identifies the chip in
  the Origin 300 module.

.. _sgi-sn1-router-identity:

Identity and placement
======================

The NUMAlink module has eight external connectors, *established*
:src:`007-4498-002 pp. 129, 132-135`. That the chip behind them is an
SN1-family router follows from the module being an Origin 300 part and the
Linux IP35 build selecting this header, a *software expectation*. The ASIC
product, its revision and the correspondence between connector numbers and
the register link numbers 1 to 8 are *unknown*. Only a read of the part in
a NUMAlink module, or a manufacturer document for it, would settle them.
No measurement of the part exists, so this page has no measurements
section.

The topology ``origin300-numalink-pci-expansion`` (machine options
``origin300,topology=origin300-numalink-pci-expansion,nodes=2``) is an
explicit model choice, an *approximation*. It cables the first base
module to router port 1, the second to port 6 and the PCI expansion
module to port 4. The manual allows an expansion module only on
connectors 2 to 5, so port 4 is permitted, and base modules on any of
the eight :src:`007-4498-002 pp. 10-11, 96, 132-133`, *established*. It
gives no map from connectors to ASIC links. The router owns no memory and no
processors. Each router sits in its own module with a module L1 controller
(brick type ``router``) and an L1 UART.

The ``sgi-sn1-router`` device is also user creatable for tests. It has the
following properties.

``chip-revision``
  Defaults to 1. Realize fails for any other value, so the status word
  always reports revision 1. The inventory label is a software-reported
  one, not a silicon revision, so the value is an *approximation*.

``port1-cable`` to ``port8-cable``
  Links to ``sgi-net-cable`` objects. The machine builder fixes them after
  construction.

``l1-uart``
  Link to a realized ``sgi-l1-uart``, which backs the UART window below.

Two profiles exist, an *approximation* in both. The machine builder
selects the diagnostic profile, through the topology record's
``synthetic-diagnostics`` flag, for every router of the ``origin300``
expansion topology and of the Origin 3400, Origin 3800 and Origin 3900
topologies (the ``origin3400*``, ``origin3800`` and ``origin3900*``
entries, both routers of ``origin3400-two-routers`` and ``origin3800``
included). The Origin 2000, Onyx 2 and Onyx 3400
topologies do not set the flag. A router
created with ``-device``, and the R-brick router of the ``onyx3400-ir``
topology, get the ordinary profile, so the 4-bit route table rule below
applies to them and to no other catalogue router: the diagnostic profile
keeps whole 64-bit table words, because the experiments it supports write
and read them back. Which profile the silicon matches is *unknown*; a qtest
pins each profile's table behaviour. The profile is fixed before realize
and migration refuses a mismatch. The diagnostic profile accepts more
registers and masks some of them. The ordinary profile keeps each route
table entry to its 4-bit field, stores full words elsewhere, and refuses
everything outside the public layout. Both are
bounded model policy, not statements about the silicon.

.. _sgi-sn1-router-registers:

Register map
============

Offsets are within the router register space. The space is 0x600008 bytes
and every access is a naturally aligned 64-bit word. A narrower or
unaligned access made through the memory region fails as a decode error.
The Bedrock local block masks a vector address to whole words, so only the
test adapter can send an unaligned offset, and the router refuses it. Registers are
big-endian when read through the memory region; a vector payload is a
plain 64-bit value.

``B(L)`` is the base of register link ``L``, from 1 to 8: ``0x10000``,
``0x10800``, ``0x20000``, ``0x20800``, ``0x30000``, ``0x30800``,
``0x40000``, ``0x40800``. This is the public header's layout,
``(((L + 1) & 0xe) << 15) | (((L + 1) & 1) << 11)``, *software
expectation*. The link banks are not linear.

Reset values are the model's cold state, all zero, an *approximation*:
no source gives silicon reset values. "Diag" marks a register that exists
only in the diagnostic profile. Elsewhere the ordinary profile applies
the same rule.

.. list-table::
   :header-rows: 1

   * - Offset
     - Name
     - Access
     - Notes
   * - ``0x000``, ``0x600000``
     - Status
     - read
     - Computed, see below. A write is refused.
   * - ``0x008``
     - Port reset
     - Diag write 1, read
     - Reads zero. Diag: writing exactly 1 resets router-local state.
       Any other write is refused.
   * - ``0x010``
     - Protection configuration
     - read, write
     - Diag: bits 13-0 are kept, higher bits read zero. Ordinary: all
       64 bits are kept. No bit has an effect.
   * - ``0x018``
     - Global port definition
     - Diag read, write
     - Bits 15-0 are kept. Ordinary: reads zero, writes refused.
   * - ``0x020``, ``0x028``
     - Global parameters 0 and 1
     - read, write
     - Full 64-bit words with no effect.
   * - ``0x030``
     - Reserved
     - read
     - Reads zero. A write is refused with an address error, or with a
       command error under Diag.
   * - ``0x038``
     - Debug address
     - Diag read, write
     - Bits 9-0 are kept and reads add the fixed field
       ``0x3fffc00``. Ordinary: refused.
   * - ``0x040``, ``0x048``
     - Diagnostic words 1 and 2
     - Diag read, write
     - Keep 33 and 34 low bits (``0x1ffffffff`` and ``0x3ffffffff``).
       Ordinary: refused.
   * - ``0x050``
     - Bus control
     - read, write
     - Full word. A read ORs in ``3 << 20``.
   * - ``0x100`` to ``0x110``
     - Scratch 0-2
     - read, write
     - Full words. The stock :term:`PROM` stores the router's serial in scratch 0
       bits 47:0, the booted flag in bit 63 and a local identifier in bits
       62:56 (*observed*), and skips a router whose scratch 0 reads as booted.
       The model needs bit 63 clear after reset, which zero gives. The PROM
       reads the serial text from the router's L1 as base 36 digits, so a
       serial of more than nine letters and digits, or with another
       character, spills into bits 63:48. That reading and the
       nine-character limit are a software expectation seen in a run of
       the stock PROM on the model; no landed datasheet states them.
   * - ``0x118``, ``0x120``
     - Scratch 3 and 4
     - read, write
     - Only bit 0 is kept.
   * - ``0x240`` to ``0x278``
     - L1 UART window
     - read, write
     - Eight registers, see below.
   * - ``0x518``
     - Scratch 3 alias
     - read
     - A read returns scratch 3, then sets it to 1. A write is refused.
   * - ``0x8000`` to ``0x80f8``
     - Inactive barrier images
     - read, write
     - 32 words, see below.
   * - ``B(L)``
     - Link parameter
     - read, write
     - Bits 29-0 are kept.
   * - ``B(L)+0x008``, ``+0x088``
     - Link error status and clear
     - read
     - Read zero. Writes are refused.
   * - ``B(L)+0x010``
     - Link channel test
     - Diag read, write
     - Bit 54 and bits 27-0 (``0x004000000fffffff``) are kept.
   * - ``B(L)+0x018``
     - Link remote reset mask
     - Diag read, write
     - Bits 7-0 are kept.
   * - ``B(L)+0x100`` to ``+0x178``
     - Global route table
     - read, write
     - 16 entries, each keeping its low 4 bits (approximation: inferred from
       what the stock firmware writes). The diagnostic profile keeps full words.
   * - ``B(L)+0x200`` to ``+0x5f8``
     - Local route table
     - read, write
     - 128 entries, with the same 4-bit rule.

The register names and the offsets of the status, protection, global
parameter, port reset, link, error and table registers are the public
header's, *software expectation*. Which bits each register keeps is a
model choice for every row, an *approximation*. The header gives field
names but no readback masks. For protection configuration the stock
Origin 300 router diagnostic expects an all-ones write to read back
``0x3fff``, a *software expectation*, and that is the only reason the
diagnostic profile keeps bits 13-0. An access to any other offset,
including every unlisted link register, is refused.

Status
------

A read at ``0x000`` or ``0x600000`` returns, for ingress port ``i``:

- ``0x3017`` in bits 27-12. The header lists ``0x3017`` as the SN1 router
  family part number, *software expectation*.
- The configured revision (1) from bit 28.
- ``i`` from bit 52. Port 0 means the memory region, a direct access; a
  vector operation arrives on its cable's router port, 1 to 8.
- Link state for port ``p`` (1 to 8), two bits each. A port with a
  connected neighbour sets bit ``35 + 2p``, matching the header's link
  code 2 for up. Under the diagnostic profile a port without one sets
  bit ``34 + 2p`` instead. Under the ordinary profile it reads zero.
- Under the diagnostic profile only, the value ``0x49`` in bits 6-0.

The part number and link-up code are *software expectation*. The
revision, the ingress encoding, the lower link bit and ``0x49`` are model
choices, an *approximation*. The cable-to-link mapping is *unknown* for the
module. A write to status fails.

L1 UART window
--------------

When the ``l1-uart`` link is set, offsets ``0x240`` to ``0x278`` in steps of
eight map to the eight UART registers, 0 to 7. A write sends the low byte
of the payload. A read returns a byte in the low bits of the reply. The
router bridges to the UART with an eight-byte big-endian access. The
Origin 300 guide puts L1 control on a separate USB management path
:src:`007-4498-002 pp. 129, 132-135`. Whether the router carries a UART
window, and at which offsets and byte lane, is *unknown*. The Linux header
names a remote UART register range and 16550 register numbers, a *software
expectation*, but not the wiring. The window is an *approximation*. It is
absent, and its offsets are refused, when no UART is linked.

Inactive barrier images
-----------------------

The header names a 32-entry barrier register array at ``0x8000``,
*software expectation*. The model keeps each word and performs no barrier.
The diagnostic profile keeps all 64 bits. The ordinary profile refuses a
write with any bit above bit 25 set. Neither profile has an active
barrier, a transmit trigger, or fan-in and completion. The patent
:src:`us-patent-6085303` describes active barriers in an SN1 embodiment, so
the lack of one is a model limit, not a hardware statement.

.. _sgi-sn1-router-vector:

Vector access and the reply route
=================================

A Bedrock node reaches the router with a vector operation: a route, an
address and a 64-bit payload. The Bedrock local block launches it when its
parameter register is written, using the route and data registers. This
register layout is public Linux, ``hublb.h`` and ``hublb_next.h`` at the
tag above, *software expectation*. The route and reply behaviour below is
the model's, an *approximation*, because the public sources define no hop
encoding or reply route for this part.

The route is read from its least significant nibble. At the first router,
the nibble is the output port, 1 to 8, and the request leaves by that
port's cable. The router shifts the route right by four bits, adds one hop
and appends its ingress port to the reply route:
``reply = (reply << 4) | ingress``. A nibble of 0 or above 8, a missing
neighbour, 15 hops already taken, or a reply route that would overflow
refuses the request. A request that arrives with no route left is a
register access to that router, with the arrival port as its ingress.
With the route consumed at a cable that leads to a Bedrock, the request
is performed by that node.

Only types 0 (read) and 1 (write) reach a router. The Bedrock local block
refuses other types before sending. On success it latches read data for a
read, the final reply route in its return register, and a valid status that
echoes the identifiers of the launch with the request type in bits 2-0. A
refused request latches nothing and logs ``LOG_GUEST_ERROR``. A launch
while an earlier reply is unread sets the overrun bit and keeps the old
reply. How hardware reports a remote failure or timeout is *unknown*.

For example, a request leaving the source node into router A at port 2,
with route ``0x45`` (outputs 5, then 4), crosses A port 5 into router B
port 7, leaves by B port 4 and reaches the target Bedrock. The return route
is ``0x27``: ingress 2 appended first, 7 second.

.. _sgi-sn1-router-routing:

Memory routing
==============

Remote memory accesses between nodes of the routed topology also cross the
router. The model chooses the path as follows, an *approximation*.

1. The source Bedrock's local-route register for the destination node gives
   the first turn, a four-bit value. A value above 7 fails the route.
2. At each router with ingress ``i`` (1 to 8) and turn ``t``, the output
   port is ``1 + ((i - 1 + t) mod 8)``. The public header defines
   routing as clockwise turns from the ingress port, *software
   expectation*.
3. The next turn is the low three bits of word ``destination`` in the
   local route table of the *ingress* link's bank, where ``destination`` is
   the node number from 0 to 127. A router therefore names the turn the
   next router takes, from the link the message entered by.
4. The output port's cable leads to the next router or to the target.

Step 3 follows what the stock IP35 :term:`PROM` 6.210 programs, a
*software expectation*. On the Origin 3400 with two R-bricks (C-bricks 0 and 1
on ports 2 and 3 of router A, C-bricks 2 and 3 on ports 2 and 3 of router B,
the routers joined port 1 to port 1 and port 6 to port 6) the PROM writes, for
each destination behind the other router, the next router's turn on the two
links its C-bricks enter by and ``0xf`` on every other link. For example,
router B holds turn 1 for node 0 (into router A at port 1, out at port 2) and
turn 5 for node 1 (in at port 6, out at port 3) on links 2 and 3, and ``0xf``
on links 1 and 6. Read on the output link instead, those routes leave router A
by port 8 and port 5, and every access between the two routers fails. How the
silicon itself indexes its tables is *unknown*.

The route succeeds only if every output is cabled, the path has no repeated
router, ingress and turn combination, and it ends within 15 routers on a
Bedrock port whose node is ready and carries the requested number. A
failure leaves no partial effect and the access fails. The model reads
the local table only. It never reads the global table, the protection
configuration or any other register while routing. A write to the
protection word, global parameters, bus control, scratch words or either
route table makes the machine recompute its remote-memory mappings, and
so does the local reset. Writes to link parameter, channel test, remote
reset mask, global port definition, barrier and diagnostic registers do
not.

The public header gives the turn arithmetic. Neither it nor the cited
patents define destination-indexed local-table selection, the three-bit
use of a table word, the omission of the global table or the physical
applicability of any of these to the Origin 300 module. Those choices are
*unknown* for the hardware and are model policy.

.. _sgi-sn1-router-reset:

Reset
=====

Cold reset clears every stored register, table and image to zero and
restarts routing notification. This is an *approximation*: the part's
reset values are *unknown*.

Under the diagnostic profile, writing exactly 1 to offset ``0x008`` is a
router-local reset, an *approximation* drawing on the header's distinction
between a local bit and per-link selections. It clears the protection
word, the bus control word, both global parameter words, the global port
definition, scratch 0-4, all barrier images, the debug address
and diagnostic words 1 and 2. It leaves the per-link parameter, channel test and remote reset
words, both route tables, the cables and the L1 UART untouched. It adds no
delay. The stock router diagnostic expects a nonzero global parameter 0
value after a local reset, a *software expectation*. The model returns
zero there. What the hardware holds is *unknown*; a manufacturer register
specification or a read of the part would settle it.

.. _sgi-sn1-router-power:

Power and reset hold
====================

The router has two named level inputs, ``power`` (high when the brick is
supplied, high at creation) and ``reset-hold`` (high while the chip is held
in reset, low at creation). In a machine the brick's L1 drives them from
its ``node-power`` and ``reset-hold`` outputs, and settles them when the
machine is built, so a router whose L1 starts off or held starts offline. The router is offline while ``power`` is low or
``reset-hold`` is high, and the two combine: it comes back only when both
allow it.

While offline:

- Every register access, direct or by vector, gets no reply, and a vector
  request does not forward through it. That includes the router's L1 UART
  window (``0x240`` to ``0x278``), which is refused like any other register
  while the router is offline. *approximation*: the datasheet records that
  the L1 holds the router in reset and releases it, in order, but not what
  a held chip answers on the network, and says nothing of a UART that
  belongs to a held chip.
- Each port reads down at the neighbour (the neighbour's status link bit
  clears, and a Bedrock facing it reads its port down), and the router's
  own status reads nothing. *approximation*: the landed sheets say what a
  port reads with an unpowered or held peer is *unknown*, and reading it as
  down is the model's choice. What a port facing an unpowered or held node
  reads, in the other direction, is also *unknown*.
- Remote memory that routes through the router leaves the other nodes'
  maps, because routing re-derives from the links.

Going offline clears every stored register, table and image to zero, as a
cold reset does, so they are cold whenever the router returns. This is an
*approximation*: which registers a held or unpowered chip keeps is
*unknown*. A line change that leaves the router online or offline as it
was does nothing, and a system reset leaves both lines as they are, since
they are wires from the L1. A warm reset that arrives from a neighbour is
a different event, described in :ref:`sgi-sn1-router-warm-reset`.

.. _sgi-sn1-router-warm-reset:

Warm reset forwarding
=====================

A warm reset is the reset a chip's port reset sends to the chip across a
cable. The router passes one on, and never resets itself in doing so. In
QEMU this is the ``warm_reset`` operation of the ``sgi-net-port`` interface
(``include/hw/mips/sgi-net-port.h``). The caller creates the event with
``sgi_net_warm_reset_begin()``, which names the sending chip as already
reached, delivers it to a port with ``sgi_net_port_warm_reset()`` and frees it.
A Bedrock registers the same operation for the chip at the far end of a
cable, and its network interface port reset sends a reset through its own
port (see the Bedrock page, :ref:`sgi-bedrock-reset`); the Bedrock records
the chips the reset reaches, and the router is not one of them. The
``sgi-sn1-router-test`` adapter sends one on its own: setting its
``warm-reset`` property to a port number delivers a warm reset to that port of
the router, and ``warm-reset-taken`` reads 1 when the router took it. The
``sgi-sn1-router-test-peer`` device stands for a far chip and counts the
warm resets it takes in ``warm-resets``.

On an arrival at port *L* the router:

- Takes it only when protection configuration (``0x10``) bit *L*-1 is set.
  The name and position of the bit are *established* (the public header
  names it per port as "reset ok"); that it governs a reset arriving on
  that port is *inferred*. A reset it refuses goes nowhere: it is not
  passed on and no register changes.
- Passes a taken reset out of every other port *n* whose link remote reset
  mask (``B(n)+0x018``) has bit *L*-1 set and whose cable is up with both
  ends enabled, and applies the neighbour's own rule there. The mask
  direction, "a reset arriving on port *p* may leave by port *n* when mask
  *n* has bit *p*-1 set", is *inferred* from the firmware's partition
  fencing and is the working reading, an *approximation*; the opposite
  reading is not excluded by any source.
- Visits each chip once per reset. A router reached again by a cabled loop
  does nothing the second time, whatever its permit says, an
  *approximation* of how a reset event is bounded.
- Keeps every register. Whether an accepted warm reset clears the router's
  permit, masks, tables or scratch is *unknown*; the model keeps them all,
  the fences and the reset-space mark in scratch 0 included, an
  *approximation* that the partition-aware boot needs a surviving router
  to have.
- Does nothing while offline (unpowered or held), and does not reach a
  neighbour whose link reads down.

Not modelled: the warm-reset bit of the port reset register (bit 9, name
*established*) stays a refused write, no consumer writes it, and the
per-port error status bit a refused arrival is named to set (bit 36, name
*established*, latching *inferred*) is not set, because how the register
clears is *unknown*.

Errors and unexpected accesses
==============================

An unsupported offset, a refused write, an unsupported packet command, an
oversize ingress and an unaligned offset make the router return an error
with an address failure code (4) or command failure code (5), and log
``LOG_UNIMP`` ("refusing operation"). Through the memory region, which only
tests reach, this is a decode error. Through a vector
operation the Bedrock local block logs ``LOG_GUEST_ERROR`` and leaves its
reply registers as they were. The failure codes are internal. They are not
a hardware error reply, which is *unknown*.

Migration
=========

Warm reset forwarding adds no state. State is the registers, tables and images above, saved at version 4. A
router that is unpowered or held adds a subsection holding the two line
states; one that is online at the source sends nothing extra. The
revision and the profile must match between source and destination, and
loading checks the per-link masks, scratch 3 and 4 and the global
port definition. Migration is supported only between identical
builds.

.. _sgi-sn1-router-limits:

Model limits
============

- Packets, virtual channels, credits, retries, link training and
  arbitration are not modelled. A vector operation and a remote memory
  access complete synchronously.
- The ASIC, its revision and the connector-to-link mapping are unknown.
  Reset values, readback masks and the diagnostic profile's extra registers
  are model choices.
- Routing follows the rule above, not a known hardware rule. The global
  route table, the protection bits and active barriers have no effect.
- In a machine, a Bedrock's port reset delivers a warm reset to the router
  and the router only passes it on; see :ref:`sgi-sn1-router-warm-reset`.
  The router is reset only when the board resets the whole machine.
- Management over USB or the rack L2, and L1 access through the router,
  are not modelled beyond the UART window. Supply timing and the order of
  the reset release are not modelled; the lines are levels.
- A stock firmware or IRIX run that gets past this router shows what the
  model does, not what the module does.

Tests
=====

``tests/qtest/sgi-sn1-router-test.c`` drives the router through the
``sgi-sn1-router-test`` adapter in ``hw/misc/sgi-sn1-router-test.c`` and
through a full machine. It covers register masks and refusals in both profiles,
status and link bits as cables change, local reset, error registers, port
reset, global port definition and diagnostic words, the barrier images,
route-table words, the routing arithmetic, vector links, silent peers, power
and reset hold on both ends of a cable, cold reset. It also covers the revision
range and overflow checks, the UART window and migration, including profile
mismatch. The warm reset cases cover a reset forwarded by permit and mask, a
refusal when the permit is clear, the mask direction, three routers in a cabled
ring visited once each, each router applying its own rule, registers kept, and
an offline router or neighbour forwarding nothing.
``tests/qtest/sn1-bedrock-link-test.c`` covers a two-router vector read, the
``0x27`` return route, each cable down, a powered-off target, route-table words
and the UART window through the Bedrock local block.
``tests/qtest/sn1-cross-node-test.c`` reads memory across both routers of the
Origin 3400 with two R-bricks with the PROM's table values, which fails when
the next turn is read on the output link. ``tests/qtest/sn-builder-test.c`` and
``tests/qtest/sn-builder-fault-test.c`` cover how the machine builder
constructs routers and cables.
