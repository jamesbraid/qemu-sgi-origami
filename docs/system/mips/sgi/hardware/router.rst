.. SPDX-License-Identifier: BSD-3-Clause

.. _sgi-router:

===============================
Router: SN0 interconnect router
===============================

Used in :ref:`sgi-ip27` (the Origin 2000 and Onyx2 modules). The Origin
200 has no Router: its two towers join Hub to Hub.

The Router is the six-port switch of the SN0 interconnect. It joins the
Hubs of a module and, through cables, other modules. SGI's published
paper names the chip Spider (:src:`galles-spider-1997`). QEMU models it
as the ``sgi-router`` type in
``hw/misc/sgi-router.c``, with the interface in
``include/hw/misc/sgi-router.h``. The type is not user creatable. The
SN0 machine builder in ``hw/mips/sgi/sn-machine.c`` creates one per
Router in the machine's topology and joins its numbered ports to Hubs
and other Routers with ``sgi-net-cable`` objects (``port1-cable`` to
``port6-cable``).

The Router has no window in any processor's address space. Guests reach
its registers only through vector operations launched from a Hub
(:doc:`hub`). The model does not carry micropackets, virtual channels or
credits. It decodes the register target, looks up routes in the tables
guests program and follows cabled links. No chip revision is
established for the part. The model reports revision 1. Facts are rated
as described in :ref:`sgi-ratings`.

.. _sgi-router-sources:

Sources
=======

- *Origin200/2000 IP27 PROM Technical Reference Manual*,
  :src:`108-0170-001`. Section 6 covers vector routing and the module
  cabling, and page 3-15 lists the Router register names. Page numbers
  below are printed pages.
- *Origin and Onyx2 Theory of Operations*, :src:`007-3439-002`.
- Mike Galles, "Spider: A High-Speed Network Interconnect", IEEE
  Micro 17(1), 1997, :src:`galles-spider-1997`, and US patent 5,721,819,
  :src:`us-patent-5721819`. They describe the chip's message format and
  routing.
- Uros Prestor, *Evaluating the Memory Performance of a ccNUMA System*,
  2001, :src:`prestor-ccnuma-thesis-2001`, for the histogram bucket
  definitions.
- The *Origin 2000 Illustrated Parts Catalog*, :src:`hmm-408-b`, and a
  published Origin 2000 inventory, :src:`usenet-9uhh6o`, for the board
  record.
- Linux ``arch/mips/include/asm/sn/sn0/hub.h`` at ``db2ddb871435``, as a
  software expectation for the chip identity values.
- The IP27 PROM 6.156 image from IRIX 6.5.30, :src:`ip27-prom-6.156`,
  for what its Router code does and does not consume.

.. _sgi-router-board:

Identity and placement
======================

An Origin 2000 module midplane carries up to two Router boards, R1 and
R2, with one Router ASIC on each (:ref:`sgi-origin2000-module-routers`),
*established* :src:`007-3439-002 pp. 80-83`. A Star Router is a carrier
with no Router ASIC, and a Null Router is a passive Hub-to-Hub link
:src:`108-0170-001 section 6.2, pp. 6-2 to 6-6`, *established*. A
published two-module Origin 200 connects its Hubs directly, so the
six-port part does not justify a Router there
:src:`007-3708-002 Figures 1-4 and 1-5`, *established*. The model
creates one ``sgi-router`` for each Router in a machine's topology. A
Null Router is a cable between two Hubs and creates none.

Linux gives the chip identity as 0 for a Hub and 1 for a Router. The
status register at address 0 serves both chips, so a probe can tell
them apart (Linux ``hub.h`` at ``db2ddb871435``;
:src:`108-0170-001 section 6.1, p. 6-2`), *software expectation*. The
model reports identity 1. Revision field 1 is an *approximation*: no
cited source gives the silicon revision the field holds.

Two properties set the identity a guest sees. ``chip-inputs`` (four
bits, default 1) is the value of the four chip input pins in status
bits 11:8. The machine builder sets 15 for R1 and 14 for R2. No cited
source states what the real boards present, so the values are an
*approximation*. ``nic-clock-hz`` (default 100 MHz, 90 MHz in the SN0
machines) sets the one-wire master calibration described under
:ref:`sgi-router-nic`.

.. _sgi-router-ports:

Ports and message layer
=======================

.. list-table::
   :header-rows: 1
   :widths: 56 14 30

   * - Fact
     - Rating
     - Source
   * - The chip has six full-duplex ports and a non-blocking crossbar.
       Messages use four virtual channels with credit-based flow
       control. Links are protected by a CCITT CRC with hardware
       retry.
     - *established*
     - :src:`galles-spider-1997 pp. 34-39`;
       :src:`007-3439-002 Figure 3-3, pp. 80-83`
   * - Ports 4 to 6 are internal to a module midplane. Ports 1 to 3 are
       the cable connectors, numbered top to bottom. Green LEDs show
       connection state for ports 1 to 6.
     - *established*
     - :src:`108-0170-001 p. 6-1 and section 1.1`
   * - On the Origin 2000 Router board three ports use single-ended STL
       inside the enclosure and three use differential PECL between
       modules.
     - *established*
     - :src:`007-3439-002 pp. 80-83`; :src:`galles-spider-1997`
   * - A message is a header micropacket plus data micropackets. The
       header carries a 9-bit destination, a 4-bit direction, an 8-bit
       age and 2 bits of congestion control.
     - *established*
     - :src:`galles-spider-1997 pp. 34-39`. How many header bits can
       also carry data is open: the paper's figure and prose differ.
   * - The destination is the 9-bit node number placed in physical
       address bits 40:32. Its upper 5 bits are the meta address and its
       lower 4 bits the local address.
     - *established*
     - :src:`108-0170-001 section 6.1, p. 6-1`;
       :src:`galles-spider-1997`

The model carries no micropackets. A vector operation or a routed Hub
access is a function call along the cabled path. Age, arbitration,
buffering, credits, tail timeouts and deadlock timeouts have no
equivalent. That omission is an *approximation*: the model forwards at
once and keeps the route taken, per-port error state and link state.

.. _sgi-router-routing:

Routing tables
==============

.. list-table::
   :header-rows: 1
   :widths: 56 14 30

   * - Fact
     - Rating
     - Source
   * - A chip holds a 16-entry local table indexed by the destination's
       4 local bits and a 32-entry meta table indexed by its 5 meta
       bits. A Router entry is 24 bits, six 4-bit directions, one per
       output port. A Hub entry holds one 4-bit direction.
     - *established*
     - :src:`108-0170-001 rtab command text and p. 3-15`;
       :src:`galles-spider-1997`; :src:`us-patent-5721819`
   * - Lookup is pipelined one hop ahead: a chip looks up the direction
       the next chip will use.
     - *established*
     - :src:`galles-spider-1997 pp. 34-39`; :src:`us-patent-5721819`
   * - Routes are software-programmed, static between every pair of
       endpoints, and cycle-free.
     - *unknown*
     - :src:`007-3439-002 p. 80` calls the routing adaptive, against
       :src:`galles-spider-1997 p. 36`. A Router register document, or
       a console observation of two messages to one destination taking
       different paths, would settle it.

The model forwards by the tables the guest programs. The bit-level
encoding below is an *approximation*, because no cited source states it:

- A direction is a turn count relative to the port the message enters
  on. A message entering on port *e* with direction *d* in 0 to 5 leaves
  on port ((*e* - 1 + *d*) mod 6) + 1. Directions 6 to 15 are refused.
- The chip then reads the direction for the next chip from the entry
  the destination selects. The 4-bit field numbered by the exit port is
  the one used: port 1 is entry bits 3:0 and port 6 is bits 23:20.
- Protection-configuration bit 12 (force local) makes every lookup use
  the local table. Otherwise, with bit 11 (meta identifier valid) set,
  a destination whose meta bits equal the meta identifier in bits 10:6
  uses the local table, indexed by its low 4 bits. Any other
  destination uses the meta table, indexed by its 5 meta bits. With bits
  12 and 11 both clear the model refuses every lookup. What the chip
  does with meta-valid clear is *unknown*.
- An entry not written since reset is invalid. A lookup that selects it
  is refused. A read of it returns zero. The chip's reset contents are
  *unknown*.
- A terminal Hub does not use the next-direction field. A route through
  one Router to a matching Hub works with that entry still holding all
  ones. If the link leads to another Router, the field it carries must
  be in 0 to 5.

A table write stores the low 24 bits. The model does not use the
partition identifier in protection-configuration bits 20:13.

Ordinary node-addressed accesses do not travel as packets. The Hub
walks the same tables hop by hop. It checks that every link on the walk
is up and that the end Hub's programmed node number matches the
destination, and refuses the access if any step fails. A walk through
more than 15 Routers fails, the same limit a vector obeys.

.. _sgi-router-vector:

Vector routing
==============

Before node numbers exist, firmware reaches Router registers and a
subset of a remote Hub's network-interface registers by giving the
route instead of a node number, *established*
:src:`108-0170-001 section 6.1, pp. 6-1 to 6-2`. A vector is a string of
output port numbers 1 to 6, one per nybble, in reverse order:
``0x512`` leaves the Hub into a Router, leaves port 2 of that Router,
port 1 of the next and port 5 of the third. Vector ``0x0`` reaches the
device attached to the local Hub. The response follows the reverse
path.

Vector routes inside one fully populated module are printed in Table
6-1 (:src:`108-0170-001 Table 6-1, p. 6-3`). From N1 or N2, N1 is ``5``,
N2 ``4``, N3 ``0x56``, N4 ``0x46``, R1 ``0`` and R2 ``6``. From N3 or N4
the node vectors swap and R1 is ``6``, R2 ``0``. That fixes R1 port 5
to N1, port 4 to N2 and port 6 to R2, and R2 port 5 to N3, port 4 to N4
and port 6 to R1, *established*. With a Star Router the table is ``5``,
``4``, ``6`` and ``3`` for N1 to N4
(:src:`108-0170-001 Table 6-2, p. 6-4`).

Each Router's port receive handler implements the path:

- A packet whose remaining vector is zero ends at this Router. The
  handler appends the arrival port to the reply route and decodes the
  access as a register operation (:ref:`sgi-router-registers`).
- Otherwise the low nybble names the exit port. A zero low nybble in a
  non-zero vector, a value above 6, or a 16th hop is refused with
  ``LOG_UNIMP`` and no answer. The 15-hop limit is the model's, an
  *approximation*.
- The exit port's link must be up: a cable not marked down, with both
  ends enabled. Otherwise the packet gets no answer and nothing is
  logged. If the far end is a Hub and nybbles remain, the packet is
  refused with ``LOG_UNIMP``.
- An unpowered Router answers nothing.
- Every Router that forwards a packet appends its arrival port to the
  reply route, so the reply carries its own return path.

The reply-source encoding of a Router reply is *unknown*. The model
leaves the field zero. The PROM 6.156 payload does not consume that
field when a mapped read, write or exchange completes
(:src:`ip27-prom-6.156 Router vector completion`), a *software
expectation*. A vector read captured from a real Router would settle
the encoding.

.. _sgi-router-registers:

Register map
============

The PROM manual names forty-six Router registers
(:src:`108-0170-001 p. 3-15`), *established*: status and revision, port reset, protection
configuration, global and diagnostic parameters, two scratch
registers, the one-wire (NIC) register, six built-in-self-test
registers, and for each of the six ports parameters, status and error,
error clear, reset mask and histogram registers, plus the local and
meta tables. The stock IP27 PROM drives the chip through those
registers, a *software expectation*.

No cited public source gives the offsets, field layouts, access types or
reset values behind those names. Linux's SN1 router header describes a
different, eight-port part and is not evidence for this chip. Every
offset, field and reset value below is the model's choice, an
*approximation*. A register document or register reads from a real chip
would settle them.

Every access is a whole-register 64-bit vector operation on an 8-byte
boundary. Addresses are 20 bits. Bits 19:16 select a block. Block 0
holds the chip-wide registers, blocks 1 to 6 hold that port's
registers at the same low offsets, and block 7 holds the routing
tables. A narrower register stores its defined bits and reads zero
elsewhere. The operations are read, write and exchange. An exchange is
accepted at scratch register 1 only. It returns the old value and
stores the new one only if the old value was zero. That the exchange is
conditional is an *approximation*: no cited source says.

Chip-wide registers (block 0)
-----------------------------

.. list-table::
   :header-rows: 1
   :widths: 9 22 12 16 41

   * - Offset
     - Name
     - Bits stored
     - Reset
     - Notes
   * - ``0x00000``
     - ``RR_STATUS_REV_ID``
     - mixed
     - see below
     - Identity, revision, inputs, outputs, LEDs, local errors,
       per-port link bits and arrival port.
   * - ``0x00008``
     - ``RR_PORT_RESET``
     - none
     - reads 0
     - Write side effects only.
   * - ``0x00010``
     - ``RR_PROTECTION_CONFIG``
     - 20:0
     - ``0x3f``
     - Partition identifier (20:13), force local (12), meta identifier
       valid (11), meta identifier (10:6), reset permits (5:0).
   * - ``0x00018``
     - ``RR_GLOBAL_PARMS``
     - all but 11:10
     - ``0x3fffffe0a01ffff1``
     - Timeouts, aging, retry limit, clock select and bypass enable.
       Bits 11:10 always read 1. No field changes model behaviour.
   * - ``0x00020``
     - ``RR_SCRATCH_REG0``
     - 63:0
     - 0
     - Scratch.
   * - ``0x00028``
     - ``RR_SCRATCH_REG1``
     - 15:0
     - 0
     - Scratch. Target of the conditional exchange.
   * - ``0x00030``
     - ``RR_DIAG_PARMS``
     - 19:0
     - 0
     - Per-port send-data-error (5:0), disable (11:6), force
       eight-bit (17:12), reset-on-deadlock (18, no effect), absolute
       histogram (19).
   * - ``0x00038``
     - ``RR_NIC_ULAN``
     - see below
     - ``0x18b100002``
     - One-wire bus master (:ref:`sgi-router-nic`).
   * - ``0x00050`` to ``0x00078``
     - ``RR_BIST_*``
     - see below
     - 0
     - Scan engine (:ref:`sgi-router-bist`).

**Status register.** The model assembles the value on every read:

.. list-table::
   :header-rows: 1
   :widths: 14 16 70

   * - Bits
     - Access
     - Meaning
   * - 3:0
     - read-only
     - Chip identity, 1.
   * - 7:4
     - read-only
     - Chip revision, 1.
   * - 11:8
     - read-only
     - The ``chip-inputs`` property.
   * - 15:12
     - read-write
     - Chip outputs. Reset 1.
   * - 21:16
     - read-write
     - Software LEDs, one per port at bit 15 + port.
   * - 23:22
     - read-write
     - Unused, stored.
   * - 27:24
     - write one clears
     - Local administration errors (single-bit, stuck, bad direction,
       tail). Nothing in the model latches them.
   * - 45:28
     - read-only
     - Per port *p* (1 to 6): reset-failed at bit 25 + 3\ *p*,
       link-working at 26 + 3\ *p*, eight-bit mode at 27 + 3\ *p*.
   * - 48:46
     - read-only
     - The port on which the reading request arrived, 1 to 6.
   * - 63:49
     - read-only
     - Zero.

A write replaces bits 23:12 and clears the local-error bits written as
one. With ``chip-inputs`` at 1 the register reads ``0x1111`` after reset
plus the arrival port and any link bits.

**Port reset register.** A read returns zero. A write acts on the
highest-priority bit set:

- Bit 7 is a warm reset. It runs the reset described under
  :ref:`sgi-router-reset`, returns every port to its power-on link
  state, then resets the link of every working neighbour so both ends
  settle the same width again.
- Bit 0 resets the administration block. It runs the same register reset
  but keeps the link state and does not touch the peers.
- Bits 6:1 reset the link of port *p* at bit *p*. A link reset with a
  cabled, enabled peer completes at once: link-working sets,
  reset-failed clears, and the width is negotiated again. The peer is
  told its link was reset so it settles the same width. With no peer,
  link-working stays clear and reset-failed reads set. The Router
  latches no error for a link reset, and the reset-mask and permit
  registers are not consulted.

The bit assignments and the behaviour of each reset are an
*approximation*. Galles describes reset negotiating a width common to
both ends of a link, *established* :src:`galles-spider-1997`.

Per-port registers (blocks 1 to 6)
----------------------------------

Port *p* registers sit at ``(p << 16) + offset``.

.. list-table::
   :header-rows: 1
   :widths: 9 22 10 11 48

   * - Offset
     - Name
     - Bits stored
     - Reset
     - Notes
   * - ``0x0000``
     - ``RR_PORT_PARMS``
     - 19:0
     - ``0x1bf0``
     - Bits 19:18 select the histogram mode. Other fields store only.
   * - ``0x0008``
     - ``RR_STATUS_ERROR``
     - 33:0
     - 0
     - Write one clears. Counters: sequence (7:0), check-bit (15:8),
       retry (23:16).
   * - ``0x0010``
     - ``RR_HISTOGRAM``
     - 63:0
     - 0
     - Four 16-bit buckets.
   * - ``0x0018``
     - ``RR_RESET_MASK``
     - 5:0
     - ``0x3f``
     - Stored only.
   * - ``0x0088``
     - ``RR_ERROR_CLEAR``
     - 33:0
     - 0
     - Reads and clears the status/error register. A write of zero
       clears it.

Routing tables (block 7)
------------------------

Local table entries are at ``0x70100 + 8 * n`` for *n* from 0 to 15 and
meta table entries at ``0x70000 + 8 * n`` for *n* from 0 to 31. Each
stores 24 bits. Both tables reset to zero with every entry unwritten.
The lookup is under :ref:`sgi-router-routing`.

Reset permits and masks
-----------------------

Galles describes reset fences that stop a reset crossing between ports,
*established* :src:`galles-spider-1997`. The model stores the 6-bit
permits in protection configuration and the per-port reset masks and
applies neither: no reset is blocked by them. The direction in which a
port mask applies is *unknown*. A real-machine partition reset would
settle it.

Errors
------

The status/error register and its clearing alias are one value at two
addresses. The plain address is write-one-to-clear over the 34 defined
bits. A read of the alias returns the value and clears it. A write of
zero to the alias clears it. A write of nonzero data to the alias logs
``LOG_UNIMP`` and is refused. That a nonzero write stores or clears
anything is not established.

In a running machine only the check-bit count moves. When the Router's
send-data-error bit for a port is set, a successful vector read that
arrived on that port adds one to the check-bit count of the port that
sent it, an *approximation*. Counters are 8 bits wide and saturate
at 255. The sequence-number and retry counts, the flag bits and the
local error bits have no source of events in the model. Test fixtures
can latch them.

.. _sgi-router-link:

Link state
----------

The model derives each port's link-working bit from its cable. A port
is working when its cable exists and is not marked down, the Router is
powered, both ends are enabled, and the Router's own disable bit for the
port (diagnostic bits 11:6) is clear. A port with no cable keeps the
state reset or a test fixture gave it, which is not working.

Width is a property of the link, settled when it comes up. A link that
is already working keeps its width. A link that comes up, or is reset,
runs eight bits wide if either end forces eight bits (diagnostic bits
17:12). Writing a force bit alone changes nothing until the link next
resets. The timing of a force bit against a real link is *unknown*: no
cited source states it.

Any change that affects a link recomputes every port and tells every
neighbour. Changes include a table or protection write, a diagnostic
write, reset exit and module power. A change in one Router therefore
reaches the connected fabric once.

.. _sgi-router-histograms:

Histograms
----------

Port parameters bits 19:18 select what the four buckets count.
:src:`prestor-ccnuma-thesis-2001 p. 81` defines them, *established*.

.. list-table::
   :header-rows: 1
   :widths: 24 19 19 19 19

   * - Selector
     - Bucket 0
     - Bucket 1
     - Bucket 2
     - Bucket 3
   * - 0, message age
     - age 0 to 63
     - 64 to 127
     - 128 to 191
     - 192 to 255
   * - 1, link utilisation
     - received micropacket bypassed the queue
     - micropacket received
     - micropacket sent
     - tick of a 50 MHz timebase
   * - 2, DAMQ occupancy
     - no entries
     - 1 to 7
     - 8 to 14
     - 15

The model implements selector 1 only. While it is selected and
diagnostic bit 19 (absolute mode) is clear, bucket 3 advances once per
20 ns of virtual time. The first tick comes 20 ns after reset ends.
Buckets 0 to 2 advance only when another part of the model reports a
micropacket event, and nothing in a machine does, so they stay zero.
When any bucket passes 65535 all four halve. A long idle gap is applied
as a batch of ticks and reads the same as ticking one at a time.

A read returns the buckets. A write of zero clears all four. A nonzero
write clears the bits set in the written value. Selecting selector 0, 2
or 3, or setting absolute mode, logs ``LOG_UNIMP`` when the value
changes, and the buckets then stop advancing. The timebase, rescale
and clearing rules are an *approximation*. What the hardware does at
overflow, and which events sample age and occupancy, is *unknown*.

.. _sgi-router-nic:

One-wire identity register
--------------------------

``RR_NIC_ULAN`` is the Router's one-wire bus master. Its layout is the
Hub's one-wire master, field for field. One write starts one bit slot.
Software polls the done bit and reads the sampled wire level from
bit 0.

.. list-table::
   :header-rows: 1
   :widths: 14 16 70

   * - Bits
     - Access
     - Meaning
   * - 33:27
     - read-write
     - Second divider phase. Reset ``0x31``.
   * - 26:20
     - read-write
     - First divider phase. Reset ``0x31``.
   * - 19:10
     - write-only
     - Pulse width in microseconds.
   * - 9:2
     - write-only
     - Sample point in microseconds after the pulse.
   * - 1
     - read-only
     - Done. Set at reset, clear while a slot runs.
   * - 0
     - read-only
     - Received data.

The model accepts a write when the two divider phases are equal and
match the configured clock (44 at 90 MHz, 47 at 97.5 MHz, 49 at
100 MHz), and the pulse and sample pair is one of four standard
one-wire slots: reset (pulse 520, sample 65), write one (6, 110), write
zero (80, 30) or read (6, 13). Any other combination logs ``LOG_UNIMP``
and is dropped. A write while a slot runs logs ``LOG_GUEST_ERROR`` and
is dropped. The calibration cases and the accepted slots are an
*approximation*, not a model of the divider.

The model times each slot on the virtual clock. It runs the bus
operation at the pulse deadline and sets done and the data bit at the
sample deadline. A read of the register applies any elapsed deadline
first, so polling does not wait on a timer callback. A reset slot reads
data 0 if a part answers and 1 if none does. A read slot holds its bit
through the sample point, which stretches the DS2505 read window and is
an *approximation*.

The bus carries a DS2505-class part (family ``0x0b``) unless
``nic-present`` is false. ``nic-serial`` sets the part's 48-bit serial
number. ``nic-image`` supplies up to 2048 bytes in place of the default
board record.

.. _sgi-router-bist:

Built-in self-test registers
----------------------------

The model implements a scan protocol and no real scan chain.

.. list-table::
   :header-rows: 1
   :widths: 14 24 62

   * - Offset
     - Name
     - Behaviour
   * - ``0x50``
     - ``RR_BIST_DATA``
     - Read-write 64-bit scan data.
   * - ``0x58``
     - ``RR_BIST_READY``
     - Reads 1 once reset has ended. Writes are refused.
   * - ``0x60``
     - ``RR_BIST_COUNT_TARGET``
     - Bits 7:6 select a chain (1 logic, 2 array, 3 clock control).
       Bits 5:0 give the shift length, 0 meaning 64. Changing the
       chain restarts the unload position.
   * - ``0x68``
     - ``RR_BIST_SHIFT_LOAD``
     - Counts the length against the chain's input size: 191 bits for
       logic, 110 for array, 1 for clock control. A load that would
       exceed the size is refused.
   * - ``0x70``
     - ``RR_BIST_SHIFT_UNLOAD``
     - Puts the next chunk of the completed chain's result in the data
       register. A chain that did not complete, or a position past the
       end of the result, gives zero.
   * - ``0x78``
     - ``RR_BIST_ENTER_RUN``
     - Starts the test: the Router resets its registers and link state
       as for a warm reset, but does not reset its neighbours' links.
       If the clock chain and one of the logic or array chains are
       fully loaded, that chain becomes the completed chain.

The result of a completed chain is zero except result bit 62, which the
model sets so a guest that assembles the unloaded chunks reads a
completed status. The register offsets, chain sizes and result are an
*approximation*. The real result signature and the value a good chip
produces are *unknown*. The model reports completion by construction
and tests nothing.

.. _sgi-router-reset:

Reset
=====

Machine reset and the two programmed resets (bits 7 and 0 of the port
reset register) clear:

- the status controls (outputs to 1, LEDs off) and local errors,
- protection configuration, global parameters, both scratch registers
  and diagnostic parameters, to their reset values,
- the NIC register and any one-wire slot under way,
- all BIST state,
- every port's parameters, errors, histogram and reset mask, and
- both routing tables, to unwritten.

Machine reset, warm reset and a BIST run also clear the link state. After any reset
the BIST ready bit reads 1 and the model derives the state of every
cabled port again. Reset completes at once. How long a real Router takes
and what it keeps are *unknown*. A programmed reset or a BIST run
applies any one-wire release or sample deadline that has already
passed before it clears the slot, because the one-wire part is not
reset with this register, an *approximation*.

.. _sgi-router-board-record:

Board record
============

The Router board's one-wire part, a DS2505-class family-0x0b part,
holds the board record SGI boards share: two 32-byte pages with format
byte 0x01, a ten-byte barcode, the part number split around page 0's
check value, a four-byte revision, group, capability and variety bytes,
a fourteen-byte name, and each page's complemented CRC16 in its last
two bytes. OpenBSD ``owserial`` reads the same layout, a *software
expectation*. The parts catalogue names 030-0841-002 as the Router
fitted with IP27 nodes (:src:`hmm-408-b p. 29`), *established*. A published
Origin 2000 inventory prints that board as ``ROUTER-IR1`` revision G
(:src:`usenet-9uhh6o`), *observed*.

The model stores that record: name ``ROUTER-IR1``, part 030-0841-002,
revision G, and ``0xff`` in group, capability and variety. The ``0xff``
bytes are *inferred* from Router records on the later 030-0841-003 board
in published inventories, which carry them. No registered capture shows
them for 030-0841-002. The barcode
is synthetic and marked: ``EMUR`` followed by the instance, taken from
the marked ``nic-serial``'s instance field. An unmarked ``nic-serial`` gives
instance 0, so two Routers given unmarked serials would both read
``EMUR00``. The machine builder always supplies marked serials. A
``nic-image`` file replaces the whole record.

Migration
=========

Migration works only between identical builds. The saved state is the
registers, the tables and their written flags, the BIST and one-wire
state, the histograms with their phase to the next tick, and the link
state. The one-wire clock, serial and population must match on the
destination. A restore is refused if register bits fall outside their
masks, the one-wire slot or BIST state is inconsistent, or a port
claims eight-bit width without a working link. Power state, cable
references and the change observer are not saved. The machine derives
them again from its topology and controller.

Model limits
============

- Register offsets, field layouts and reset values are the model's
  choice (:ref:`sgi-router-registers`).
- The model has no micropackets, credits, virtual channels, age, arbitration
  or buffers. Node-addressed accesses are table walks.
- Per-port reset masks and reset permits are stored only.
- A BIST run resets this Router's links but does not retrain its
  neighbours' links, which a warm reset does. Whether a real self-test
  disturbs the neighbours is *unknown*; a capture across a Router
  self-test would settle it.
- Histogram selectors other than utilisation, and absolute mode, log
  ``LOG_UNIMP``. In a machine, utilisation buckets 0 to 2 never count.
- The local error bits, the sequence-number and retry counters and the
  high flag bits of the status/error register have no event source.
- A vector to an unmapped or unsupported address logs ``LOG_UNIMP`` and
  gets no answer. An access outside the 20-bit window or not on an
  8-byte boundary logs ``LOG_GUEST_ERROR``. The launching Hub sees no
  reply. What a real Router answers is *unknown*.
- The reply-source field is always zero.
- A one-wire write with an unsupported clock or waveform logs
  ``LOG_UNIMP``. A write during a slot logs ``LOG_GUEST_ERROR``.

Tests
=====

``tests/qtest/sn0-router-test.c`` drives a standalone Router through
its register target: registers, the BIST sequence and programmed resets,
link resets, a port reset with no peer, histograms (rescaling and
unsupported modes), the one-wire master (search, timing, absent part,
memory contents, reset, oversized image) and migration.

``tests/unit/test-sgi-router.c`` covers the same target below the QOM
layer: reset values, storage masks, the conditional exchange, the
error-clear addresses and counter saturation, histogram events and
timebase, table decode and route lookup, next-turn consumption, port
disable and forced width, one-wire slot deadlines and resets, change
notifications, an unpowered Router and migration.
``tests/unit/test-sn0-fabric-routed.c`` builds synthetic Router graphs
for the vector reply path, the diagnostic error paths, link-reset width,
reset masks, a missing peer and the 15-hop limit.

``tests/qtest/sn0-origin2000-test.c`` checks the Origin 2000 module
with real Hubs: vectors through both Routers, programmed routes and
their refusal for bad table entries, every route in the module,
isolation of a downed link, Router link resets and the Router inventory
read through the one-wire register.
``tests/functional/mips64/test_prom_mdk_router.py`` netboots the field
diagnostic for Routers on the four-node module and requires every node
and both Routers to be found and all its passes to finish clean.
