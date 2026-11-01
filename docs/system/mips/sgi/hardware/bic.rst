.. SPDX-License-Identifier: BSD-3-Clause

.. _sgi-bic:

=====================================
BIC: expansion module board interface
=====================================

Used in :ref:`sgi-ip35`: the Origin 300 PCI expansion module
(:doc:`pci-expansion`).

.. seealso::

   Platform pages: :ref:`sgi-ip35`.

The PCI expansion module of the Origin 300 carries one :term:`Bedrock` interface
card, the BIC. The card holds a Bedrock ASIC. Its network interface
drives the module's :term:`NUMAlink` connector and its I/O interface reaches the
PCI motherboard (*established*, :src:`007-4499-002 pp. 38-40`;
:ref:`sgi-pci-expansion-origin300`). The module has no processors and no
memory. A separate L1 controller monitors it (:doc:`l1`).

QEMU models the BIC's Bedrock as the ``sgi-bedrock`` device inside the
expansion module (``hw/mips/sgi/origin300-pci-expansion.c``). This page
covers three things that sit beside that Bedrock and are specific to the
module:

- the board interface, type ``sgi-bic-board-interface`` in
  ``hw/misc/sgi-bic-board-interface.c``, which holds a board identity word
  and a rack and bay latch;
- the controller start-up that writes the latch from the L1, in
  ``hw/misc/sgi-l1.c``;
- two synthetic additions to the register space, a 16550 UART and the
  processor-interface pending registers.

The Bedrock itself is described in :doc:`bedrock`. Facts are rated as
described in :ref:`sgi-ratings`. No part of this page is a published
register specification of the BIC. The board interface, the UART and the
pending-register exposure are all approximations, and each section says
what they stand in for.

.. _sgi-bic-sources:

Sources
=======

- The PCI expansion module user's guide, :src:`007-4499-002`. It describes
  the module, its BIC, and the unused processor interfaces and memory.
  Printed page 50 says the processor interfaces and memory are unused.
- The IP35 :term:`PROM` 6.210, :src:`ip35-prom-6.210`, as a software expectation:
  how it reads the board windows. Its observable behaviour fixes the
  access widths and field packing below.
- The shipped L1 firmware 1.44.0 for the Origin 300 product group,
  :src:`l1-firmware-1.44.0`, as a software expectation: it writes the rack
  and bay at start-up. The destinations and commands in
  :ref:`sgi-bic-startup` come from observing that firmware's start-up
  transactions.
- OpenBSD ``sys/arch/sgi/xbow/hub.h`` at ``2df91d80c104`` for the address
  geometry of the :term:`Hub` special space's remote register window. Using that
  geometry for the BIC's board windows is *inferred*, see below.

Identity and placement
======================

The BIC is the only Bedrock in the module, reached by NUMAlink from one
compute module or through a NUMAlink router
(:ref:`sgi-pci-expansion-origin300`). Its processor interfaces are empty
and it has no memory, *established* :src:`007-4499-002 p. 50`. The module
has one BIC. A second board-interface position exists in the model's
controller start-up code and is always absent.

The PROM finds an expansion module by reading a 64-bit identity word from
the first board window and testing ``(word & 0xf000) == 0xc000``, a
*software expectation* of :src:`ip35-prom-6.210`. Its rack and bay reader
takes an unsigned halfword from the second window and splits it into ten
rack bits above six bay bits, also a *software expectation*. What
component answers these reads on the board, and what the word's other bits
contain, are *unknown*. A matching schematic, a component specification
or a capture of a real module's board windows would settle them.

Board windows
=============

The BIC board interface answers two read-only windows in the remote
register part of the Hub special space of the module's node number. The
offsets below are node-relative, within the Hub special space of that
node, in its remote register region that starts at ``0x30000000``.

.. list-table::
   :header-rows: 1
   :widths: 14 10 12 30 34

   * - Offset
     - Width
     - Access
     - Value
     - Rating
   * - ``0x34000000``
     - 64
     - read
     - Identity word
     - *inferred* geometry, *approximation* contents
   * - ``0x35000000``
     - 16
     - read
     - Rack and bay latch, ``(rack << 6) | bay``
     - *inferred* geometry, *approximation* contents

The window addresses come from the public geometry of the remote register
window :src:`openbsd-sgi sys/arch/sgi/xbow/hub.h`. Applying them to the BIC follows from the
PROM using exactly these offsets on an expansion module, an *inferred*
step. No local alias for them is established, and the model's local forms
are an approximation: the module decodes the two windows at their
node-relative offset and at that offset plus ``0x10000000000`` and
``0x30000000000``. Which address spaces those two prefixes stand for is not
established here.

Both reads return big-endian data. Every other read, every write and every
other width up to eight bytes at these offsets is rejected with
``MEMTX_ERROR`` and logged with ``LOG_GUEST_ERROR``. An access wider than
eight bytes returns a decode error without a log. A read of a window that is not fitted at all
(the module has no board interface attached) returns a decode error.

Identity word
-------------

The word is a property of the device, ``identity``. It has no default:
realize fails unless ``(identity & 0xf000) == 0xc000``, so a zero word can
never be taken for an expansion identity. The shipped Origin 300 expansion
products set it to ``0xc000``. Bits outside ``0xf000`` are a model choice,
*approximation*. The model's value says nothing about what a real BIC
returns there. The word is not cleared by reset.

Rack and bay latch
------------------

The latch holds two independent bytes, a high byte and a low byte, each
with a valid flag. A halfword read succeeds only when both flags are set.
Otherwise it fails with ``MEMTX_ERROR``, so software sees a transaction
error and never a zero that looks like rack 0, bay 0. Hardware behaviour
before the controller has written the latch is *unknown*. The failure is a
model choice, *approximation*.

Two device properties set an initial value: ``initial-latch-set`` and
``initial-latch``. With ``initial-latch-set`` off, reset leaves both bytes
unknown. With it on, reset loads ``initial-latch`` into both bytes and
marks them valid. Both shipped topologies leave it off, so the latch is unknown
until the L1 writes it. The initial value is a model input, not a
hardware reset value.

The only writer is the controller side, through the internal byte
interface used by start-up (next section). A successful write replaces one
byte and sets that byte's valid flag. A failed write changes nothing. A
read between a high-byte write and a low-byte write sees the new high byte
with the old low byte. Processors cannot write the latch.

.. _sgi-bic-startup:

Controller start-up
===================

The L1 of an expansion module writes its rack and bay settings into the BIC
latch whenever it starts. Two firmware behaviours are modelled, each
a *software expectation* of the shipped L1 firmware 1.44.0
:src:`l1-firmware-1.44.0`: it writes the high byte before the low byte,
and it carries on with the next write after one fails. The wiring between
controller and latch is *inferred* from the match between the controller's
packed value and the PROM's unpacking above. No bus capture exists.

The model reproduces this as a synchronous call. For position zero, the L1
issues two operations on bus 0 to device address ``0x8c``, one payload byte
each:

.. list-table::
   :header-rows: 1
   :widths: 20 20 60

   * - Order
     - Command
     - Effect
   * - first
     - ``0x13``
     - Write the high byte, ``(rack << 6 | bay) >> 8``.
   * - second
     - ``0x12``
     - Write the low byte, the low eight bits of the same value.

Position one uses address ``0x8e`` in the L1 code but is always absent: the
board interface accepts only position zero, address ``0x8c`` and these two
commands, and refuses everything else without changing the latch. The two
command values and addresses are each a *software expectation*, not electrical
conventions and not a register map of the BIC. No guest-visible bus
registers, acknowledgement or interrupt exist.

The L1 runs the sequence at each of its starts: when power is applied, after
the controller's reboot command, and at the hold phase of a whole-machine
reset. It uses the settings active at that moment. Changing the pending
rack and bay writes nothing. A failed operation is logged
(``LOG_GUEST_ERROR``) and traced as ``sgi_l1_bic_startup_operation``. It
does not stop the other byte or the other position.

Start-up is a selectable behaviour. The machine description selects it per
expansion module, it is off unless selected, and only the ``pci-expansion``
controller profile accepts it. It needs a distinct BIC board interface
bound to the L1 before the L1 is realized. Selecting start-up also turns on
the processor-absence words described under
:ref:`sgi-bic-pending`.

Reset
-----

When controller start-up or main power is selected, the board interface
and the L1 are both registered as resettable with the machine. Both
shipped topologies select both. The BIC enters reset first and restores the initial latch. The
L1 enters reset next and marks start-up pending. The write itself happens
in the L1's hold phase, after all devices have entered reset, so the
latch is written after it is cleared. A reset of the BIC alone restores the
initial latch with no write. A reset of the L1 alone repeats the writes.

Synthetic UART
==============

Whether the BIC provides a serial port in the Bedrock's Hub special space,
and how it would be wired, is *unknown*. The model offers an opt-in 16550
so that firmware that programs one can proceed. Everything in this section is an *approximation*.

Selecting ``unconnected-uart-experiment`` on the expansion module attaches
a QEMU ``serial-mm`` device with no character backend, ``regshift`` 3,
big-endian registers and ``baudbase`` 115200. It needs the NI route,
scoped main power and a compute origin count of two or four. A processor
reaches it at Hub special offsets ``0x80`` through ``0xbf`` of the BIC's
Bedrock register window. Eight 16550 registers sit at an eight-byte
stride.

The access rules follow from how the model's route is built:

- Only a processor request. Packet-originated requests do not reach the
  UART.
- Only aligned 64-bit reads and writes. Any other width or offset logs
  ``LOG_GUEST_ERROR`` and returns ``MEMTX_ERROR`` before touching serial
  state.
- Each access reaches one register. The low byte of a write is passed on;
  a read returns the register zero-extended.
- A request from a CPU ordinal at or above the configured origin count is
  refused with ``MEMTX_ACCESS_ERROR``. The count is the largest CPU count
  of any connected compute node, so it is two or four. This is a model
  routing rule, not a hardware identity.
- With the module's main power off, every access is refused.
- Without the selection the same addresses return a decode error.

The device behaves as QEMU's 16550 does: loopback, divisor, FIFO and modem
status work as in the generic device. Reset modem-status lines read as
asserted. They are not measurements of a BIC. With no backend, transmit
completes at once and receive stays empty unless loopback is enabled.
Nothing connects the UART to an interrupt, to the L1 protocol or to a
console.

The UART is reset by a module reset and by a completed main-power change.
Migration includes its full state when selected.

.. _sgi-bic-pending:

Pending registers and processor absence
=======================================

The module's processor interfaces are unused (*established*
:src:`007-4499-002 p. 50`). Software can still address them. The model has
two selectable ways of answering.

Processor absence. With controller start-up selected, PI0 and PI1 at
``0x40`` and ``0x48`` of their register blocks read as zero (no processor
present), reached through the Bedrock I/O interface window
``0x20001800000``. This reuses the Hub processor-interface presence policy
(:ref:`sgi-bedrock-pi-presence`). The value on hardware is *unknown*. Each
access must be an aligned 64-bit transfer. The same offsets in the Hub
special space are not decoded.

Pending registers. With ``pending-register-experiment`` also selected, the
following additional registers of PI0 and PI1 are reachable through that
window. PI1's offsets add ``0x200000``.

.. list-table::
   :header-rows: 1
   :widths: 18 24 58

   * - Offset
     - Access
     - Register
   * - ``0x10``
     - read, write
     - I/O protection
   * - ``0x90``, ``0x180``
     - write
     - Pending-modify and its alias
   * - ``0x98``, ``0xa0``
     - read
     - Interrupt pending bank 0 and bank 1

The registers are the processor interface's own, with the semantics of
:ref:`sgi-bedrock-pi`. Pending-modify takes a bit index, a bank and a
set or clear request. Bank 1 bits 54, 55, 62 and 63 reflect unit state and
pending-modify cannot change them. Showing these registers on a BIC is an
*approximation*: whether they work on the fitted board is *unknown*, and a
register capture of a real module would settle it. The selection needs the
UART selection and the same compute origin count.

Access rules match the UART: aligned 64-bit transfers only, a CPU ordinal
at or above the origin count is refused, and main power must be on. Every
other offset in the interface stays a decode error, and so do the same
offsets in the Hub special space. Packets addressed to the BIC keep their
refusal. The BIC's crossbar can still set a bit through the pending-modify
register at ``0x90`` or ``0x200090`` of the I/O window when it raises an
error interrupt; nothing delivers that bit to a processor, because there is
none. Migration includes the selection
and origin count and refuses a mismatch.

Reset and migration
===================

The board interface reset restores the latch as described above and leaves
the identity word. Its migration state is the identity, the initial latch,
both latch bytes and valid flags, and a flag recording whether an initial
latch was supplied. A destination whose identity, initial latch or flag
differs refuses the load. Migration works only between identical builds.

Measurements
============

No read of a physical BIC exists for this page. The only physical
evidence is the BIC ``030-1740-001`` revision F seen in a published
inventory (:ref:`sgi-pci-expansion-origin300`), which names the card and
says nothing about registers. Every register value and window on this page
is therefore a model input or a software expectation.

.. _sgi-bic-limits:

Model limits
============

- The identity word's bits outside ``0xf000`` and the reset state of the
  latch are model choices. A production default has not been fixed.
- The failing read of an unwritten latch is not hardware behaviour.
- The controller bus is a function call. There is no bus timing, no
  acknowledgement, no fault injection by guests and no second BIC.
- No other register of the board interface exists. Writes to the windows
  fail with ``LOG_GUEST_ERROR``.
- The UART and the PI pending registers are synthetic. No interrupt from
  either reaches a processor.
- A routed expansion (through a NUMAlink module) shares one UART and one
  set of pending registers across the compute nodes that reach it. Reset
  and migration of that arrangement are not tested.
- The latch is not reset by a :term:`NASID` change, a network event or a power
  event. Only machine reset and the BIC's own reset restore it.

Tests
=====

``tests/qtest/sgi-bic-board-interface-test.c`` drives the board interface
through a test fixture. It covers the controller byte interface (accepted
and refused positions, addresses and commands, failed writes), the
identity word, the unknown-latch error, supplied initial values,
unsupported widths and offsets, reset, and migration with and without an
initial latch and with a mismatched identity.

``tests/qtest/sn1-l1-test.c`` covers the start-up sequence with a fake
receiver: operation order, high-byte failure continuing to the low byte,
absent positions, resets of the L1 alone and the BIC alone, migration and
migration mismatch (the ``sn1/l1/bic-startup`` cases).

``tests/qtest/sgi-pci-expansion-ni-test.c`` covers the UART (registers,
origin ordinals, refusals, power and reset, migration), the pending
registers (two and four CPU views, refusals, permissions, the crossbar
error path, migration, mismatches) and processor absence.
``tests/qtest/sn-builder-test.c`` runs a composed expansion lifecycle with
start-up and migration.
