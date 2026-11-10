.. SPDX-License-Identifier: BSD-3-Clause

.. _sgi-bedrock:

========================
Bedrock: node controller
========================

Used in :ref:`sgi-ip35` (``origin300``, ``fuel``), :ref:`sgi-ip53`
(``origin350``, ``onyx350``, ``tezro``, ``onyx4``), one per node,
and in the Origin 3000 C-brick and CX-brick node boards
(:ref:`sgi-bricks`), which the ``origin3000`` machine builds.

.. seealso::

   Platform pages: :ref:`sgi-ip35`, :ref:`sgi-ip53`.

.. seealso::

   - :term:`Hub` (:doc:`hub`): the node controller of the :term:`SN0` machines, which the
     Bedrock succeeds.

The Bedrock is the node controller of SGI's second Scalable Node
generation, the successor to the Hub (:doc:`hub`). It joins up to four
processors, node memory and its directory, one crosstalk port and one
:term:`NUMAlink` 3 port (*established*, :src:`108-0240-002 pp. 2-11 to 2-17`). QEMU models it as
``sgi-bedrock`` in ``hw/misc/sgi-bedrock.c``, a parent that composes
seven unit devices: two processor interfaces (``sgi-bedrock-pi.c``),
the memory and directory interface (``sgi-bedrock-md.c``), the I/O
interface (``sgi-bedrock-io.c``), the network interface
(``sgi-bedrock-ni.c`` with ``sgi-bedrock-ni-regs.c``), the local block
(``sgi-bedrock-lb.c``) and the internal crossbar (``sgi-bedrock-xb.c``),
all under ``hw/misc/``. The node's boot flash is a separate device,
``sgi-bedrock-flash`` in ``hw/misc/sgi-bedrock-flash.c``.
``hw/mips/sgi/sn1-node.c`` builds all of them. Every machine fits
revision field 2, which public inventories print as Bedrock revision 2;
the silicon revision behind that label is *unknown*.

Facts are rated as described in :ref:`sgi-ratings`. SGI's register
headers give the Bedrock's register offsets and fields but almost no
reset values, so most reset values here are model choices
(*approximation*). What the stock IP35 :term:`PROM` and IRIX do is a *software
expectation*; where it was observed by running that software on the
model, it shows what the software asks of the Bedrock, not what the chip
does.

.. _sgi-bedrock-sources:

Sources
=======

- SGI's generated :term:`SN1` register headers in the Linux history tree
  (``linux-history``) at ``7570df54ef8c``, tag v2.5.40, under
  ``include/asm-ia64/sn/sn1/``: ``hubpi.h``, ``hubpi_next.h``,
  ``hublb.h``, ``hublb_next.h``, ``hubni.h``, ``hubni_next.h``,
  ``hubio.h``, ``hubio_next.h``, ``hubmd.h``, ``hubmd_next.h``,
  ``hubxb.h``, ``hubxb_next.h``, ``bedrock.h``, ``arch.h``, ``intr.h``
  and ``ip27config.h``; and ``include/asm-ia64/sn/addrs.h`` at tag v2.5.0
  (``381ab1f538162``) for the directory, ECC and boot-space addresses.
  SGI generated these from its register database. Offsets, field
  positions and the register narratives they carry are rated
  *software expectation*. They state reset values for only a handful of
  registers.
- The Itanium SN1 code in the same tree (``arch/ia64/sn/io/sn1/``,
  ``arch/ia64/sn/kernel/``), as a software expectation.
- OpenBSD ``sys/arch/sgi/xbow/hub.h``, ``xbowdevs``,
  ``sys/arch/sgi/sgi/ip27_machdep.c``, ``sginode.c``, ``ip27.h`` and
  ``sys/arch/sgi/include/mnode.h`` at ``2df91d80c104``, as software
  expectations.
- The out-of-tree MIPS IP35 Linux port (``linux-ip35``) at
  ``9f613a321ea0``: ``arch/mips/sgi-ip35/`` and
  ``arch/mips/pci/pci-sgipic.c``, as software expectations. Nothing
  records it booting.
- *Origin 3000 System Architecture*, :src:`108-0240-002 p. 2-11-2-17`:
  the roles of the crossbar, processor, memory, network, I/O and local
  units.
- *SGI Origin 300 User's Guide*, :src:`007-4365-002 p. 1, 8, 38`:
  two-chassis systems joined by a NUMAlink 3 cable.
- *sMDK-based Field Diagnostics*, :src:`108-0264-004 p. 2-31, 2-35`:
  the vendor's own Bedrock tests, as vocabulary.
- The *XBridge ASIC Specification* revision 0.5,
  :src:`xbridge-asic-spec-0.5 p. 79-80`, for a MicroLAN controller whose
  low fields match the local block's.
- US patent 6,751,698, :src:`us-patent-6751698`: a 16 MiB logical flash
  behind the Bedrock, reached through local and remote boot spaces.
- The *MIPS R10000 Microprocessor User's Manual* version 2.0,
  :src:`r10000-users-manual-2.0 p. 54, 57`, for the coherent requests
  each cache algorithm issues; and Laudon and Lenoski's Origin paper,
  :src:`laudon-lenoski-origin-1997 sec. 3.2`, for the Origin 2000
  directory protocol.
- The stock IP35 PROM, :src:`ip35-prom-6.210`, read as a binary and run
  on the model; the IRIX 6.5.30 IP35 kernel and graphics drivers, read as
  binaries. Both are software expectations.
- Published captures: a two-processor Origin 300 inventory,
  :src:`nekonomicon-16726611`; two Fuel inventories,
  :src:`sgi-sh-fuel-repair-attempt` and
  :src:`tingo-fuel-irix-console lines 132-145`; an Origin 350 console,
  :src:`graphica-o350-notes`; and an OpenBSD boot of two cabled
  Origin 300s, :src:`marc-openbsd-sgi-139223952727384`.

.. _sgi-bedrock-identity:

Identity and placement
======================

The chip carries two identity words, both a *software expectation* from SGI's
headers. The local block's revision register holds part ``0x3012`` with
revision 1 for Bedrock 1.0 and 2 for 1.1 (``bedrock.h``,
``hublb_next.h``). The I/O interface's widget identification
register holds crosstalk part ``0xc110`` (``hubio_next.h``). No
header states the manufacturer field. OpenBSD ``xbowdevs`` names
manufacturer ``0x036`` part ``0xc110`` "Bedrock", a *software
expectation*, and the model uses ``0x036`` in both words, an
*approximation* for the local block's word. Two Fuel inventories print
Bedrock revision 2 and HUB revision 2
(:src:`sgi-sh-fuel-repair-attempt`,
:src:`tingo-fuel-irix-console lines 132-145`), *observed* as software
labels rather than register reads. The model therefore answers revision
field 2, an *approximation*.

.. list-table::
   :header-rows: 1

   * - Word
     - Model value at node 0
     - Fields
   * - local block revision, ``0x600000``
     - ``0x000000002301206c``
     - revision 2 (31:28), part ``0x3012`` (27:12), manufacturer
       ``0x036`` (11:1), node 0 (39:32), region size 0 (47:46)
   * - I/O interface widget identification, ``0x400000``
     - ``0x2c11006c``
     - revision 2, part ``0xc110``, manufacturer ``0x036``; upper half
       zero

The registers are crosstalk widget 1 of the node's own I/O space: an
8 MB window reached in a local form and a node-named remote form
(:ref:`sgi-ip35-address-map-hub`). The unit bases inside it are
a *software expectation* from OpenBSD ``hub.h`` and the SN1 headers. The model maps
each unit at its base, with these sizes:

.. list-table::
   :header-rows: 1

   * - Base
     - Unit
     - Model size
     - Device
   * - ``0x000000``
     - processor interface 0
     - ``0x30108``
     - ``sgi-bedrock-pi``
   * - ``0x200000``
     - processor interface 1
     - ``0x30108``
     - ``sgi-bedrock-pi``
   * - ``0x400000``
     - I/O interface (II)
     - ``0x30010``
     - ``sgi-bedrock-io``
   * - ``0x600000``
     - local block (LB)
     - ``0xc040``
     - ``sgi-bedrock-lb``
   * - ``0x680000``
     - network interface (NI)
     - ``0x2008``
     - ``sgi-bedrock-ni``
   * - ``0x700000``
     - crossbar (XB)
     - ``0x198``
     - ``sgi-bedrock-xb``
   * - ``0x780000``
     - memory and directory (MD)
     - ``0x60048``
     - ``sgi-bedrock-md``

An access to a hole between units fails, so the processor takes a bus
error. All registers are big-endian 64-bit words. Every unit accepts
only aligned 64-bit accesses, except that the processor interface also
takes 32-bit reads of each half of its real-time counter. Any other
width or alignment fails the same way. That restriction is a model
policy: the headers declare 64-bit registers but do not say which bus
sizes the chip accepts.

OpenBSD expects the local form of a processor-interface register to
reach the requesting processor's own interface. The model gives every
processor on interface 1 an overlay that makes its local form reach
interface 1, an *approximation*; no source states the decode, and a
read of the CPU number through both forms from a processor on
interface 1 would settle it. The node-named form always reaches the
interface the offset names.

.. _sgi-sn-family-bedrock:

Revisions in published inventories
-----------------------------------

Every published IRIX inventory of an Origin 300, Origin 3000, Origin
350, Fuel or Onyx 4 reports the Bedrock, printed as ``HUB``, at revision
2, 200 MHz (*observed*, many machines, among them
:src:`nekonomicon-16723604`, :src:`nekonomicon-16720581` and
:src:`nekonomicon-16729141`). No capture shows another revision. The raw
register value behind the revision field IRIX prints is *unknown*.

.. _sgi-bedrock-pi:

Processor interface
===================

Two interfaces, PI0 and PI1, each serve two processors, slot A and slot
B, over the system bus: four processors per node, two per interface
(``sn1/arch.h``; :src:`108-0240-002 p. 2-12`), *established*. Each
interface holds the interrupt pending banks and the per-processor masks,
the real-time counter and its comparators, the region and cached-alias
controls, the protection registers, graphics flow control and error
capture. PI1's registers are PI0's at hub offset ``+0x200000``.

The header states no reset value for this unit. It states only which
registers survive a soft reset and which are not cleared at reset. The
model's reset values below are each an *approximation* unless marked. Rows
marked "kept" are never changed by any reset in the model, so they keep
their value across a local reset and a machine reset alike.

.. sgi-registers: base=0x10000000 access=64
   skip=0x40:population,0x48:population,0x50:kept,0x58:kept,0x3f8:kept,0x400:kept,0x418:kept,0x420:kept,0x428:kept
   qemu="-M none -cpu R10000 -m 256M -object memory-backend-ram,id=bedrock-dma,size=2M -device sgi-bedrock,id=bedrock,processor-count=4,dma-memory=/objects/bedrock-dma/bedrock-dma[0] -S"
   map=/machine/peripheral/bedrock:0:0x10000000

.. list-table::
   :header-rows: 1

   * - Offset
     - Name
     - Width
     - Reset
     - Access
     - Notes
   * - ``0x00``, ``0x10``
     - CPU protect, I/O protect
     - 64
     - all ones, kept
     - read/write
     - One bit per region. Both log ``LOG_UNIMP`` on a value other than
       all ones. CPU protect is stored only; I/O protect is enforced on
       crosstalk interrupt writes (:ref:`sgi-bedrock-pi-interrupts`).
   * - ``0x08``
     - protection override
     - 64
     - none
     - write-only
     - The value ``0x53474972756c6573``, the local block's key
       (``bedrock.h``), sets the writer's region bit in CPU
       protect; the model takes region 0. The header does not name the
       key for this unit, so its use here is an *approximation*.
   * - ``0x18``
     - region present
     - 64
     - all ones, kept
     - read/write
     - Stored with no effect. OpenBSD writes all ones to both interfaces.
   * - ``0x20``
     - CPU number
     - 2
     - n/a
     - read-only
     - Bit 0 is the reading processor's slot, bit 1 the interface.
   * - ``0x28``
     - cached-alias size
     - 4
     - ``0``, kept
     - read/write
     - Code 0 off, 1-15 for 4 KB doubling to 64 MB (``hubpi_next.h``). Sizes the node's cached-alias window for the
       processors on this interface.
   * - ``0x30``, ``0x38``
     - request-buffer timeout and scale
     - 8, 24
     - ``0``
     - read/write
     - Stored; no timeout runs.
   * - ``0x40``, ``0x48``
     - CPU present A, B
     - 1
     - from population
     - read-only
     - :ref:`sgi-bedrock-pi-presence`.
   * - ``0x50``, ``0x58``
     - CPU enable A, B
     - 1
     - fitted slots 1, kept
     - read/write
     - :ref:`sgi-bedrock-pi-presence`.
   * - ``0x70``, ``0x78``
     - NMI A, B
     - 64
     - none
     - write-only
     - :ref:`sgi-bedrock-pi-nmi`.
   * - ``0x90``, ``0x180``
     - interrupt pending modify and its alias
     - 64
     - none
     - write-only
     - :ref:`sgi-bedrock-pi-interrupts`.
   * - ``0x98``, ``0xa0``
     - interrupt pending 0, 1
     - 64
     - ``0``
     - read-only
     - Direct writes log ``LOG_GUEST_ERROR`` and change nothing.
   * - ``0xa8``-``0xc0``
     - masks 0 A, 1 A, 0 B, 1 B
     - 64
     - ``0``
     - read/write
     - A-to-B stride ``0x10`` (``hubpi_next.h``).
   * - ``0xc8``-``0xe8``
     - coherency-pending set A, B; clear A, B; mask
     - 64
     - ``0``
     - see notes
     - A store to set raises the requester's region bit, which the model
       takes as region 0. Clear is write-one-to-clear. The masked result
       drives pending 0 bit 5 (A) or 6 (B).
   * - ``0xf0``
     - pending 1 remap
     - 5
     - ``0``
     - read/write
     - Stored. A nonzero remap (3:0) or error forward (bit 4) logs
       ``LOG_UNIMP`` and routes nothing.
   * - ``0x108``, ``0x110``
     - real-time compare A, B
     - 55
     - ``0``
     - read/write
     - :ref:`sgi-bedrock-pi-timer`.
   * - ``0x118``
     - profile compare
     - 32
     - ``0``
     - read/write
     - :ref:`sgi-bedrock-pi-timer`.
   * - ``0x120``-``0x138``
     - real-time and profile pending A, B
     - 1
     - ``0``
     - read; any store clears
     - :ref:`sgi-bedrock-pi-timer`.
   * - ``0x140``-``0x158``
     - real-time and profile enable A, B
     - 1
     - ``0``
     - read/write
     -
   * - ``0x300``-``0x348``
     - graphics flow control, slots A and B
     - see notes
     - ``0``
     - read/write
     - :ref:`sgi-bedrock-pi-gfx`.
   * - ``0x3f8``
     - error pending, writable view
     - 35
     - kept
     - read/write
     - A write replaces the error bits, except bit 24, which it neither
       sets nor clears.
   * - ``0x400``
     - error pending
     - 35
     - kept
     - write-one-to-clear
     - Bit 24, memory uncorrectable, is set by the memory unit
       (:ref:`sgi-bedrock-md-ecc`).
   * - ``0x408``, ``0x410``
     - error mask A, B
     - 35
     - ``0``
     - read/write
     -
   * - ``0x418``, ``0x420``
     - error stack address A, B
     - bits 32:3
     - kept
     - read/write
     - Stored.
   * - ``0x428``
     - error stack size
     - 4
     - kept
     - read/write
     - A nonzero size logs ``LOG_UNIMP``; nothing is spooled.
   * - ``0x430``-``0x468``
     - error status 0 and 1, A and B, each with a clear alias at ``+8``
     - 64
     - ``0``, kept
     - read; alias write clears
     - A write to either clear alias of a slot zeroes both of that
       slot's status words. Nothing records an error here.
   * - ``0x490``
     - system-bus error-check enable
     - 6
     - ``0``
     - read/write
     - Stored.
   * - ``0x4a8``-``0x4b8``
     - nack count A, B; nack compare
     - 21, 20
     - ``0``
     - read/write
     - Stored. The model's bus never negatively acknowledges, so the
       count never moves.
   * - ``0x4c0``
     - spool mask
     - 6
     - ``0``
     - read/write
     -
   * - ``0x4c8``, ``0x4d0``
     - spurious header 0, 1
     - 64
     - ``0``, kept
     - read/write
     -
   * - ``0x20068``
     - graphics credit mode
     - 1
     - ``0``
     - read/write
     - Stored; the model counts one credit per store either way.
   * - ``0x30100``
     - real-time counter
     - 55
     - ``0``
     - read/write; 32-bit reads of each half
     - :ref:`sgi-bedrock-pi-timer`.

The reset column for error pending, error stack and error status follows
the header's "not cleared at reset" and "survives soft reset" wording,
a *software expectation*. Protection, region present and cached-alias size also
survive soft reset in the header; the model keeps them across every
reset, its choice, since their power-on values are *unknown*.

Reads of write-only registers log ``LOG_GUEST_ERROR`` and return zero.
Writes to CPU number and CPU present log ``LOG_UNIMP``. Other offsets
the header defines but the model leaves out (debug select, the
force-bad-check-bit pair, reply level, performance counters, spool
compare, error injection) and the undefined offsets ``0x80`` and
``0x88``, which only the Itanium port writes, read zero and drop writes
with ``LOG_UNIMP``.

.. _sgi-bedrock-pi-presence:

CPU number, presence and enable
-------------------------------

CPU number reports bit 0 as processor A or B and bit 1 as PI0 or PI1
(``hubpi.h``), a *software expectation*; it is OpenBSD's first hub
access. The model answers from the issuing processor's slot. An access
with no issuing processor, from a qtest or from another node, reads
slot A.

On a published two-processor Origin 300 inventory, PROM 6.210 and IRIX
6.5.29m report processors in slices A and C
(:src:`nekonomicon-16726611`): PI0/A and PI1/A are fitted, PI0/B and
PI1/B empty, *observed* for that board. It does not fix the placement on
every board revision. Four-processor products fill both slots of both
interfaces (:src:`108-0240-002 p. 2-12`), *established*. The machine
fixes which processor sits in which slot. CPU present reads 1 for a
fitted slot and 0 for an empty one; the header says the interface sets
it on the processor's first transaction (``hubpi.h``), and
the model derives it from the population, an *approximation*.

CPU enable withholds the bus from a processor while clear (``hubpi.h``), and survives soft reset, a *software expectation*. Its power-on
value is *unknown*. The model sets it for every fitted slot at power-on
and never for an empty one, an *approximation*: the stock IP35 PROM's
boot path never sets the bit, so a processor whose bit starts clear is
never counted, a *software expectation*
(:src:`ip35-prom-6.210 processor population`). The PROM writes zero to a
partner's bit when that partner fails its early liveness check. The
register drives a ``cpu-enable`` output, which the machines leave
unconnected: a processor runs whatever the bit says.

The stock PROM also records, in flash, which empty positions failed to
start, and skips them on later boots. The model does not seed those
records (:ref:`sgi-bedrock-flash`); the processor interface never
reports an empty slot as started.

.. _sgi-bedrock-pi-interrupts:

Interrupts
----------

A write to the pending-modify register, or its alias at ``0x180``, sets
or clears one pending bit: bits 5:0 name the bit, bit 6 the bank, and
bit 8 set means set, clear means clear (``hubpi.h``;
OpenBSD ``hub.h``), a *software expectation*. Bank 1 bits 54, 55, 62
and 63 only reflect other units and cannot be changed this way
(``hubpi.h``), a *software expectation*. The model computes them
from those units and leaves bit 63 clear, since it has no system
controller source.

A crosstalk write to hub offset ``0x1800090``, or ``0x1a00090`` for PI1,
carries the same value word (:ref:`sgi-ip35-address-map-interrupt`). The
model forwards it from the I/O interface to the interface's modify
register when I/O protect allows the node's region; otherwise it logs
``LOG_GUEST_ERROR`` and drops it.

Fixed sources the model drives:

.. list-table::
   :header-rows: 1

   * - Bank and bit
     - Source
     - Basis
   * - pending 0 bits 1, 2
     - graphics flow control, slots A and B
     - header (``hubpi.h``); :ref:`sgi-bedrock-pi-gfx`
   * - pending 0 bit 4
     - the L1 controller's UART (:doc:`l1`), on both interfaces
     - bit named by the header; the broadcast is an *approximation*
   * - pending 0 bits 5, 6
     - coherency pending A, B
     - header (``hubpi.h``)
   * - pending 1 bit 54
     - crossbar error
     - header; :ref:`sgi-bedrock-xb`
   * - pending 1 bit 55
     - local block error
     - header; :ref:`sgi-bedrock-lb`
   * - pending 1 bit 61
     - memory or directory correctable error
     - header; :ref:`sgi-bedrock-md-ecc`
   * - pending 1 bit 62
     - network error
     - header; :ref:`sgi-bedrock-ni`

Sources that belong to the whole node reach both interfaces, an
*approximation*: no source says whether the chip signals one or both.
Linux SN1 and OpenBSD assign bank-1 bits 36 to 53 in software, the PROM
range among them; the chip treats them as general bits.

Each processor has five lines. The model connects them to the R10000
family's hardware interrupts 0-4 (Cause IP2-IP6):

- 0: pending 0 masked by that processor's mask 0;
- 1: pending 1 masked by its mask 1;
- 2: its real-time pending bit and enable;
- 3: its profile pending bit and enable;
- 4: error pending masked by its error mask.

Lines 0, 1, 3 and 4 follow the header and both consumer headers
(``sn1/intr.h``), a *software expectation*. The header names level 4
for the real-time compare in one place and level 5 in another; both
consumers dispatch the timer on hardware interrupt 2, which the model
uses, an *approximation*. All lines are level-triggered and recomputed on
every change.

The IP53 boards add one rule (:ref:`sgi-ip53-pic`). When a processor
clears a pending bit that a :term:`PIC` input still holds asserted, the model
sets it again 1 ms of virtual time later, an *approximation*.

.. _sgi-bedrock-pi-timer:

Real-time counter and comparators
---------------------------------

The counter is 55 bits, wraps to zero and counts the clock the local
block supplies (``hubpi.h``), a *software expectation*. Linux SN1
gives that clock as 1,250 kHz (``sn1/ip27config.h``), and every
MIPS consumer assumes 1,250,000 Hz, a *software expectation*. The model
counts at that fixed rate on QEMU's virtual clock. Writes to the local
block's clock controls do not change it. A write to the counter sets the
count. Each interface keeps its own counter.

When the counter equals a slot's real-time compare, that slot's
real-time pending bit sets; when the counter's low 32 bits equal the
profile compare, both profile pending bits set (``hubpi.h``), a *software expectation*. Any store to a pending register
clears that register's bit, a *software expectation*. SGI Performer reads the
counter as two 32-bit halves, so the model accepts 32-bit reads at
``0x30100`` (high half) and ``0x30104`` (low half).

.. _sgi-bedrock-pi-nmi:

NMI
---

A store to NMI A or B sends that processor a non-maskable interrupt
(``hubpi.h``), a *software expectation*. No public source gives the
trigger predicate. The public Linux IP35 series stores 1. The model
pulses the slot's NMI line once on a store with bit 0 set and logs any
other store as ``LOG_UNIMP``, an *approximation*. Whether a store
latches, whether zero retracts it and whether a disabled processor takes
it are *unknown*. The L1 controller's ``nmi`` command reaches every
fitted processor through the same per-slot lines, also an
*approximation*.

.. _sgi-bedrock-pi-gfx:

Graphics flow control
---------------------

Ten registers per interface pace one processor's uncached stores to a
directly attached graphics pipe (``hubpi.h``),
a *software expectation*. Slot A's set starts at ``0x300``, slot B's at ``0x328``:

.. list-table::
   :header-rows: 1

   * - Offset (A, B)
     - Name
     - Fields
   * - ``0x300``, ``0x328``
     - graphics page
     - 39:17 page address, 40 enable
   * - ``0x308``, ``0x330``
     - credit counter
     - 11:0
   * - ``0x310``, ``0x338``
     - bias
     - 11:0
   * - ``0x318``, ``0x340``
     - interrupt counter
     - 25:0
   * - ``0x320``, ``0x348``
     - interrupt compare
     - 25:0

The header says uncached stores into an enabled page go out as graphics
writes the counter tracks: it falls by one per doubleword sent and rises
by one per doubleword the pipe acknowledges. Acknowledgements return
through the I/O interface's node-widget map (``hubio.h``), a *software expectation*. The IRIX 6.5.30 Odyssey and InfiniteReality
drivers program these registers and tear a pipe down when the count
reads above their total, a *software expectation*.

The model counts one credit for each uncached I/O-space store of up to
eight bytes that a processor makes into its slot's enabled page. The
count is taken on that processor's own I/O path, so qtest and DMA
accesses are never counted. A graphics head returns credits by widget
number; the node-widget map names the node, interface and slot that get
them, and the first matching entry wins. A return to a node that is
neither this one nor the one at the far end of its cable, or a return no
entry matches, is dropped with ``LOG_GUEST_ERROR``. The counter wraps
modulo 4096 with ``LOG_GUEST_ERROR``; real wrap, saturation or sign is
*unknown*. When a counted store leaves the count below the bias, the
model sets the slot's graphics pending bit at once and logs
``LOG_UNIMP`` once per slot; the write stall is not modelled. The
interrupt counter and compare are stored only. These are
each an *approximation*.

.. _sgi-bedrock-md:

Memory and directory interface
==============================

The memory and directory interface is the Bedrock's memory controller and
coherence directory: issue, memory and directory blocks, page migration,
DDR SDRAM refresh about every 8 µs and a built-in self test over data,
ECC and directory (:src:`108-0240-002 p. 2-13`), *established*. No
public MIPS consumer touches a register of this unit on IP35. OpenBSD
and the IP35 Linux port take memory size from the configuration records
the PROM leaves in memory. The unit's boot-path contract is therefore
what the stock PROM programs, a *software expectation*.

.. sgi-registers: base=0x10000000 access=64
   skip=0x7c0018:read-to-clear,0x7e0018:read-to-clear
   map=/machine/peripheral/bedrock:0:0x10000000
   qemu="-M none -cpu R10000 -m 256M -object memory-backend-ram,id=bedrock-dma,size=2M -device sgi-bedrock,id=bedrock,processor-count=4,dma-memory=/objects/bedrock-dma/bedrock-dma[0] -S"

.. list-table::
   :header-rows: 1

   * - Offset
     - Name
     - Width
     - Reset
     - Access
     - Notes
   * - ``0x780000``
     - current cell
     - 64
     - all ones
     - read/write
     - :ref:`sgi-bedrock-md-cell`.
   * - ``0x780008``
     - memory configuration
     - 64
     - ``0``
     - read/write
     - :ref:`sgi-bedrock-md-banks`. Survives the unit's soft reset.
   * - ``0x780010``, ``0x780018``
     - arbitration control, migration configuration
     - 8, 33
     - ``0``
     - read/write
     - Stored.
   * - ``0x780020``-``0x780058``
     - fetch-and-op cache status 0, 1; miscellaneous error 0, 1 and clear
     - 64
     - ``0``
     - read
     - Read zero; writes log ``LOG_UNIMP``.
   * - ``0x780060``
     - outgoing reply queue size
     - 64
     - ``0x0018_1818_0c0c_0c00``
     - read
     - 12 header and 24 data entries per queue (``hubmd.h``), a *software expectation*. A write, which resets credits on the
       chip, logs ``LOG_UNIMP``.
   * - ``0x790000``-``0x790018``
     - performance select 0, 1; counter 0, 1
     - 64, 41
     - ``0``
     - read/write
     - Stored; nothing counts. The stock PROM and the Itanium port use
       counter 0 as a software lock word.
   * - ``0x7a0000``
     - refresh control
     - 12, bit 63
     - ``0``
     - read/write
     - Threshold and enable; the header says enable resets to 0
       (``hubmd.h``), a *software expectation*. Survives soft
       reset. The live counter reads 0. No refresh runs.
   * - ``0x7a0008``
     - junk bus timing
     - 48
     - ``0``
     - read/write
     - Flash, UART and Synergy timing fields; stored.
   * - ``0x7a0010``-``0x7a0028``
     - LED 0-3
     - 8
     - ``0``
     - read
     - Read zero; writes log ``LOG_UNIMP``.
   * - ``0x7a0030``-``0x7a0048``
     - self-test control, data, error address, status
     - 64
     - ``0``
     - see :ref:`sgi-bedrock-md-selftest`
     -
   * - ``0x7a0060``, ``0x7c0060``, ``0x7e0040``
     - issue, directory and memory debug select
     - 2, 2, 1
     - ``0``
     - read/write
     - Stored.
   * - ``0x7c0000``
     - directory configuration
     - 2
     - ``0``
     - read/write
     - Bit 0 premium directory, bit 1 ignore directory ECC. Survives soft
       reset.
   * - ``0x7c0010``, ``0x7c0018``
     - directory error and clear
     - 64
     - ``0``
     - read; a read of the clear alias clears
     - :ref:`sgi-bedrock-md-ecc`.
   * - ``0x7c0020``-``0x7c0038``, ``0x7c0058``
     - protocol error and clear, migration candidate and clear, self-test
       directory error data
     - 64
     - ``0``
     - read
     - Read zero; writes log ``LOG_UNIMP``.
   * - ``0x7c0040``, ``0x7c0048``
     - migration difference and value thresholds
     - 64
     - ``0``
     - read/write
     - Stored.
   * - ``0x7c0050``
     - outgoing request queue size
     - 64
     - ``0x0000_0000_0404_0400``
     - read
     - Four entries per queue (``hubmd.h``),
       a *software expectation*. Writes log ``LOG_UNIMP``.
   * - ``0x7e0000``
     - memory ECC configuration
     - 1
     - ``0``
     - read/write
     - Bit 0 ignores memory ECC.
   * - ``0x7e0010``, ``0x7e0018``
     - memory error and clear
     - 64
     - ``0``
     - read; a read of the clear alias clears
     - :ref:`sgi-bedrock-md-ecc`.
   * - ``0x7e0020``-``0x7e0038``
     - self-test memory error data 0-3
     - 64
     - ``0``
     - read
     - Read zero; writes log ``LOG_UNIMP``.

Writes to the error registers log ``LOG_GUEST_ERROR`` and are ignored.
Read-to-clear on the clear aliases is the model's choice; the header
gives no mechanism. Reset values other than the two queue sizes and
refresh enable are each an *approximation*. The unit's soft reset keeps memory
configuration, refresh threshold and enable, and directory
configuration, as the header says (``hubmd.h``),
a *software expectation*; it clears the rest. Other offsets read zero and drop
writes with ``LOG_UNIMP``.

.. _sgi-bedrock-md-cell:

Current cell and the Hub special windows
----------------------------------------

The current-cell vector is also the access-rights vector for the Hub
special spaces: with a region's bit clear, the local and remote register
windows become inaccessible (``hubmd.h``), a *software expectation*.
Its reset value is *unknown*. The model resets it to all ones, which
permits every access the consumers make, an *approximation*. The L1
UART at offset ``0x80`` of both register windows
(:ref:`sgi-ip35-address-map-special`) checks the node's own region bit
on every access. A denied access logs ``LOG_GUEST_ERROR`` and fails with
a bus error. The model checks the region of the Bedrock that owns the
window, not the requester's region, so a processor on the other node of
a cabled pair is checked against the wrong bit; with the reset value
all ones the two agree.

.. _sgi-bedrock-md-banks:

Memory banks
------------

Memory configuration holds, for each DIMM bank ``d`` at bits ``8d`` up,
two bank enables, a DRAM width bit and a size code; bits 33:32 select
banks by exclusive-or; the upper bits hold protection and timing fields
(``hubmd.h``), a *software expectation*. Size code ``c`` is
``32 MB << c``: 1 is 64 MB, 5 is 1 GB (``hubmd_next.h``).
Software bank ``n`` sits at node offset ``n`` GB, eight banks of 1 GB
per node (``hubmd_next.h``), a *software expectation*. The Origin
300's slots and capacities are on :doc:`/system/mips/sgi/ip35`.

The model decodes node memory through this register. A software bank
answers only if its locational bank is fitted in the machine's installed
population, enabled and has size code 1-5. It then answers up to the
smaller of the coded size and the fitted size. Any other access fails
with ``LOG_GUEST_ERROR``. The register resets to zero, so after a
machine reset no bank answers until firmware writes it. The fitted
population is fixed by the machine, not read from SPD records, an
*approximation*.

.. _sgi-bedrock-md-selftest:

Built-in self test
------------------

The header names the self-test control, data, error-address and status
registers and their fields (``hubmd.h``), a *software expectation*.
The stock PROM resets the engine with ``0x4``, writes a pattern with
bit 7 set to data, starts each DIMM and bank selection with
``(dimm << 5) | (bank << 4) | 1``, and waits up to about 625 ms for
status bit 1, done. It accepts a bank only with done and passed both
set, and reads error address bit 48 or 49 to tell an absent bank from a
premium-memory one, a *software expectation*
(:src:`ip35-prom-6.210 memory self test`).

The model runs a functional state machine, an *approximation* that
tests no DRAM. It takes only the exact commands reset ``0x4``, stop
``0x2`` and start (command bits ``0x1`` with a selector in bits 6:4);
anything else logs ``LOG_UNIMP`` and changes nothing. A start completes
1 µs of virtual time later, measured from the start, and a read at or
after that time sees the result whether or not the timer has run. A
selection passes when its locational bank is fitted, enabled, has size
code 1-5 and lies wholly in node RAM. A pass reads ``0x3`` and all
detail registers zero. A failure reads ``0x2`` with error address bit
48 set. A start with data exactly ``0x80`` over a passing bank writes
zero through the bank, *inferred* from the field names (pattern zero,
write control). Other patterns leave memory unchanged. Control reads
zero. Reset and stop cancel a pending completion.

.. _sgi-bedrock-md-directory:

Directory, page counters and protection entries
-----------------------------------------------

The directory backdoor sits at seven eighths of the node's Hub special
space. Each 128-byte memory line has one 64-bit directory word, at a
16-byte stride: the M-mode address is the special base plus
``(pa & node bits) | ((pa >> 3) & 0x3ffffff0)``, from
``0x9000_0001_c000_0000`` for node 0 (``sn/addrs.h`` at v2.5.0),
a *software expectation*. A standard directory uses the low 32 bits of a word and a
premium one all 64; the directory configuration register selects which.
Offset ``+8`` in each slot is the page-counter word and ``+0x408`` a
protection entry (``sn/addrs.h``), a *software expectation*. The
software initialisation value for words and counters is zero
(``hubmd_next.h``), a *software expectation*; hardware reset values
are *unknown*.

The model answers aligned 64-bit accesses only, and only for fitted,
enabled memory; it zeroes the words at machine reset. Page-counter words
are opaque 64-bit storage, and nothing counts. Protection entries are
opaque storage across all eight software-bank positions of region 0,
because the stock PROM sizes banks by probing them. Entries above a
fitted bank's size alias lower ones, as missing address bits would; an
absent bank takes a store but does not echo it. The PROM's directory
test continues past fitted memory after it records 1 GB banks, so the
model keeps up to 74 unfitted directory words in a first-seen store, a
compatibility *approximation* sized to that test.

A directory word changes as the unit serves each processor request. The
Origin 2000 protocol makes a read of an unowned line exclusive, adds
sharers on reads of shared lines and makes a writer the exclusive owner
(:src:`laudon-lenoski-origin-1997 sec. 3.2`), and an R10000-family load
miss under cache algorithm 5 issues a coherent read-shared while a store
miss reads exclusive (:src:`r10000-users-manual-2.0 p. 57`),
*established* for those products. The word encodings are a *software expectation*
from ``hubmd_next.h``: format in bits 1:0, pointer state in 4:2, owner
pointer in 15:8 as node above three device bits. That the same protocol
applies on the Bedrock is *inferred* from the Origin 300 field
diagnostics' directory test, which expects exactly its transitions.

The model follows directory words only on watched pages, an
*approximation* that serves that test's flush-then-read pattern. A line
becomes named when a processor names it in a secondary-cache
instruction; a backdoor read of a named line's word then watches its
4 KiB page, in one of sixteen slots reused least recently used. On a
watched page every coherent cached load, store and prefetch from the
processors updates the word; device and block-transfer accesses change
nothing. Every other line's word changes only through the backdoor.
Busy, wait and poisoned states, interventions and capacity eviction are
not modelled; a word in a state the protocol does not produce is left
alone and logged ``LOG_UNIMP``. Every access to a watched page is
dispatched to the unit as a device access.

.. _sgi-bedrock-md-ecc:

ECC and the error records
-------------------------

The ECC backdoor at five eighths of the node holds one check byte per
memory doubleword (``BDECC_ENTRY`` in ``sn/addrs.h`` at v2.5.0),
a *software expectation*; the scrub carrier at four eighths indexes the same bytes
(``BDECC_SCRUB``). The memory error register keeps valid and overrun
bits for read-correctable (52, 53), write-uncorrectable (56, 57) and
read-uncorrectable (60, 61) errors, an 8-bit syndrome at 47:40 and the
address at 32:3; the directory error register keeps the same for its
kinds with a 7-bit syndrome (``hubmd.h``),
a *software expectation*. The check codes themselves are not public. The model
uses the column tables the Origin 300 field diagnostics' memory and
directory tests write, *inferred* from their behaviour.

The model regenerates a check byte on every data write. A backdoor
write of a byte that disagrees with its data marks that doubleword, and
a read of a marked doubleword is checked:

- a single-bit error is corrected and written back, latched as
  read-correctable, and sets pending 1 bit 61 on both interfaces;
- any other nonzero syndrome is latched as read-uncorrectable and sets
  error-pending bit 24 on both interfaces. An uncached processor read
  then fails with a bus error. A cached read completes with the stored
  word and raises no Cache Error exception.

A directory word that is not a codeword is checked when its memory line
is read: a correctable error sets pending 1 bit 61 and an uncorrectable
one error-pending bit 24, both on both interfaces. Setting the ignore
bit of either configuration register stops checking. Which interface the
chip signals is *unknown*; the field diagnostics need the pending bits,
a *software expectation*. Write-uncorrectable errors are never latched.

.. _sgi-bedrock-md-fetchop:

Memory special space (fetch-and-op)
-----------------------------------

The processor's uncached attribute 2 reaches node memory through the
unit's fetch-and-op operations. The header gives only the operation
cache: a 64-byte variable per entry (``hubmd.h``),
a *software expectation*. The model uses the encoding of the public SN2 successor
header (Linux ``arch/ia64/include/asm/sn/mspec.h``): a load at offset 0
returns the value, at 8 fetches and increments, at 16 fetches and
decrements, at 24 fetches and clears; a store at 0 stores, at 8 increments and
at 16 decrements (the store data is ignored), at 24 ands and at 32 ors.
The SN2 header does not define stores at 8 and 16, so the model's
meaning for them is an *approximation*. That the Bedrock uses the same offsets is *inferred*:
the Origin 300 field diagnostics use offsets 0 and 8 with those
meanings. A 32-bit access operates on one word. Any other offset,
including the SN2 cache-clear store at 56, fails with
``LOG_GUEST_ERROR``. There is no operation cache.

.. _sgi-bedrock-io:

I/O interface
=============

The I/O interface is the Bedrock's crosstalk port. It turns a device's
DMA request into the internal crossbar's format, forwards it to memory
and holds the two block-transfer engines (:src:`108-0240-002 p. 2-16`),
*established*. It connects to an :term:`XBridge`'s host
port 0xa (:doc:`xbridge`), and the ``host-widget`` property sets the value
the widget control register reads (below). Processor I/O-space accesses leave through
it; the inbound side takes DMA and interrupt packets from the widgets.

.. sgi-registers: base=0x10000000 access=64
   skip=0x400000:identity,0x400020:host-widget
   map=/machine/peripheral/bedrock:0:0x10000000
   qemu="-M none -cpu R10000 -m 256M -object memory-backend-ram,id=bedrock-dma,size=2M -device sgi-bedrock,id=bedrock,processor-count=4,dma-memory=/objects/bedrock-dma/bedrock-dma[0] -S"

.. list-table::
   :header-rows: 1

   * - Offset
     - Name
     - Width
     - Reset
     - Access
     - Notes
   * - ``0x400000``
     - widget identification
     - 32
     - see :ref:`sgi-bedrock-identity`
     - read-only
     - Also read at crosstalk widget 1 offset 0. Writes log
       ``LOG_GUEST_ERROR``.
   * - ``0x400008``
     - widget status
     - 64
     - ``0``
     - read
     - Reads zero; writes log ``LOG_UNIMP``.
   * - ``0x400020``
     - widget control
     - 23
     - ``host-widget`` (default ``0xa``)
     - read/write
     - Bits 3:0 the Bedrock's own crossbar port; bit 17 direct connect.
   * - ``0x400100``
     - local access protection
     - 64
     - all ones
     - read/write
     - Reset to all ones is a *software expectation* (``hubio.h``).
       Gates processor I/O-space accesses by the node's region.
   * - ``0x400108``
     - protection override
     - 64
     - none
     - write-only
     - The key ``0x53474972756c6573`` sets the node's region bit.
   * - ``0x400110``, ``0x400118``
     - outbound and inbound widget access
     - 64
     - all ones
     - read/write
     - :ref:`sgi-bedrock-io-access`.
   * - ``0x400120``
     - inbound device error mask
     - 64
     - all ones
     - write-one-to-set
     - :ref:`sgi-bedrock-io-access`.
   * - ``0x400128``
     - link control and status
     - 42
     - ``0x3fc0fff6403``
     - read/write
     - Bit 13 link up; :ref:`sgi-bedrock-io-link`.
   * - ``0x400130``
     - link log
     - 64
     - ``0``
     - read; write 0
     - A write of zero clears; any other write logs ``LOG_UNIMP``.
   * - ``0x400138``
     - interrupt destination
     - 25
     - ``0``
     - read/write
     - Stored; reads omit the sent bit (28). The model raises no
       error interrupt through it.
   * - ``0x400140``, ``0x400148``
     - graphics node-widget map 0, 1
     - 64
     - ``0``
     - read/write
     - Widget 3:0, PI 4, node 12:5, slot 16 (``hubio_next.h``); :ref:`sgi-bedrock-pi-gfx`.
   * - ``0x400150``, ``0x400158``
     - scratch 0, 1
     - 64
     - ``0``
     - read/write
     - Plain storage. The stock PROM publishes each interface's fitted
       processors in scratch 0; :ref:`sgi-bedrock-rendezvous`.
   * - ``0x400160``-``0x400190``
     - translation entries 0-6
     - 13
     - ``0``
     - read/write
     - :ref:`sgi-bedrock-io-pio`.
   * - ``0x400198``, ``0x4001a0``-``0x4001d8``
     - PIO request buffers, widget 0 and widgets 8-F
     - 64
     - ``0``
     - read/write
     - Stored whole; no flow control.
   * - ``0x4001e0``, ``0x4001f0``
     - credit-count timeout; tail and read-response timeouts
     - 26, 60
     - ``0``
     - read/write
     - Stored; no timeout runs.
   * - ``0x4001e8``
     - miscellaneous error mask
     - 13
     - ``0x111``
     - write-one-to-set
     - Widget 0 and both engines enabled; :ref:`sgi-bedrock-io-bte`.
   * - ``0x4001f8``
     - error clear
     - 64
     - n/a
     - write
     - Reads zero. A nonzero write logs ``LOG_UNIMP``: there is no
       error state to clear.
   * - ``0x400200``
     - BTE control
     - 9
     - ``0``
     - read/write
     - Stored.
   * - ``0x4003a8``-``0x4003c0``
     - request-buffer management, control, timeout, prescaler
     - masked
     - ``0``
     - read/write
     - Stored to their defined fields.
   * - ``0x400208``-``0x400228``, ``0x400300``-``0x400340``,
       ``0x400388``-``0x4003a0``, ``0x400400``-``0x4005d8``,
       ``0x400600``-``0x400610``, ``0x430000``, ``0x430008``
     - spurious message, link test, error headers, PIO read table,
       arbitration, request-buffer entries, debug, performance
     - 64
     - ``0``
     - read/write
     - Stored whole with no effect.
   * - ``0x410000``, ``0x420000``
     - block-transfer engines 0, 1
     - 64
     - ``0``
     - see :ref:`sgi-bedrock-io-bte`
     -

Other offsets in the unit read zero and drop writes with ``LOG_UNIMP``.
Except for local access protection, every reset value is an
*approximation*. On reset the header initialises only some fields of
the request-buffer entries (``hubio.h``); the model
zeroes them.

.. _sgi-bedrock-io-link:

Link status and widget control
------------------------------

Link control and status bit 13 is link up; both Linux ports skip the
node's crosstalk while it is clear (linux-ip35 ``ip35-xtalk.c``), a *software expectation*. The stock PROM also tests it before
it touches any widget (:src:`ip35-prom-6.210 crosstalk bring-up`). The
model resets the register to ``0x3fc0fff6403``: link up, remote power,
link enabled, null timeout 3, maximum retry ``0x3ff``, maximum burst
``0x3fc`` and data-available select 3, decoded by the header's layout
(``hubio.h``). The value at power-on is *unknown*. A
node built without a crosstalk peer reads bit 13 clear and no write can
set it, *inferred*. Writes keep the defined bits.

The interface has an ``xio-peer-power`` input that a cabled XBridge drives
(its ``host-link`` output). While the line is low, because the bridge is
held in reset or without power, the register reads bits 13 and 14 clear
and the rest of the word unchanged. Bit 13 (link up) clearing is
*inferred* from the bit's name and the PROM's use of it; bit 14 (remote
power) is *unknown* beyond its name; the physical word is *unknown*. The
PROM then skips every widget access. The stored bits are kept, so a
write-back of the word as read does not leave the link down once the
bridge returns. That is an *approximation*: whether a write-back matters
is *unknown*, and a real link may stay down until its port is reset.
Nothing on a node's own board drives the line, so its link is unchanged.
The line is a wire level: a reset leaves it, and the bridge re-drives it
after a migration load.

Widget control bits 3:0 reset to the ``host-widget`` property, ``0xa``
unless the board sets another value, and a reset restores it. The
property is the crossbar port where the Bedrock enters its XBridge. The
value must be below 16. The ``onyx350`` machine sets ``0xb``, which a
published PROM trace of a four-processor IP53 module prints as the
Bedrock's attachment widget (*observed*; that the module is an Onyx 350
is *inferred*). A Fuel trace and a Tezro deskside session print ``0xa``,
and no Origin 350 server trace is published, so the other IP35 and IP53
products use ``0xa``. The property changes the register only: the
XBridge the node builds stays wired to port ``0xa``. The out-of-tree port reads the field as the port
to skip when walking the crossbar, a *software expectation*. Bit 17
reads 0 at reset: every consumer then assumes a crossbar, not a directly
attached widget. Whether any IP35 PROM sets it is *unknown*.

.. _sgi-bedrock-io-pio:

Processor I/O and the translation entries
-----------------------------------------

Processor accesses to the node's I/O space go through this unit. Below
``0x10000000`` the I/O-space offset selects a small window: widget
``offset >> 24`` at ``offset & 0xffffff``. From ``0x20000000`` each
512 MB window uses translation entry ``(offset >> 29) - 1``: the widget
in bits 11:8, five offset bits in 4:0 placed above bits 28:0, and bit 12
selecting crosstalk I/O space (bit 47). This is the header's M-mode
decode (``hubio.h``), a *software expectation*. The model refuses
an access with a bus error when local access protection lacks the node's
region bit, when outbound access lacks the widget's bit or when no
widget is attached there. A read of widget 1 offset 0 returns the
Bedrock's own identification.

.. _sgi-bedrock-io-access:

Inbound access and the device mask
----------------------------------

Inbound widget access qualifies every request a widget launches,
including DMA and interrupt writes; the device error mask adds one bit
per DMA master of widgets 8-F, and a device needs both bits
(``hubio.h``), a *software expectation*. No public source gives a
reset value for outbound access, inbound access or the device mask. The
model resets all three to all ones, an *approximation*: a zero would
block every XBridge DMA and interrupt on every guest. The stock PROM
writes ``0xff01`` to both access registers, opening widget 0 and widgets
8-F, and never writes the device mask; IRIX clears and sets single
device bits, a *software expectation*
(:src:`ip35-prom-6.210 I/O bring-up`).

The header says device-mask bits are set by writing 1; the model
implements write-one-to-set. IRIX clears a device by writing the old
value with that bit cleared, which under write-one-to-set clears
nothing. Which behaviour the chip has is *unknown*.

A request that names its widget and device is checked against both
registers. Requests through QEMU's shared inbound window do not carry
their source. The model then refuses the request if any configured
candidate widget is denied, a conservative *approximation*. A refused
request logs ``LOG_GUEST_ERROR`` and fails with a transaction error;
what the chip does is *unknown*. Inbound requests with crosstalk bit 47
clear reach node memory through the node's fabric view. With bit 47 set
they address the registers of the node in bits 46:39, and only the two
interrupt-pending-modify writes are decoded
(:ref:`sgi-bedrock-pi-interrupts`). Other register reads and writes log
``LOG_UNIMP`` and fail.

.. _sgi-bedrock-io-bte:

Block-transfer engines
----------------------

Each engine has six registers at its base: length and status (15:0
length in 128-byte lines, 16 error, 20 busy), source, destination
(39:7), control (0 zero-fill, 4 notify, 8 poison), notification address
(39:7) and interrupt address (0 PI select, 8:1 node, 22:16 level).
Writing control starts a transfer and reading it terminates one
(``hubio.h``), a *software expectation*.

The model moves every line of a transfer before the starting write
returns, an *approximation*. The stock PROM's engine check starts both
engines back to back, waits for engine 0's completion interrupt alone,
then compares both copies (:src:`ip35-prom-6.210 engine check`). Engine 1
must therefore have finished by the time engine 0's interrupt is
taken. A start needs busy set with a nonzero
length, then the control write:

- with notify set, the engine writes its final length and status word
  to the notification address;
- with notify clear, it sends ``0x100 | level`` to the pending-modify
  register its interrupt address names, from a bottom half after the
  starting access returns.

Source reads and destination writes use the node's memory decoder, as
processor accesses do. A failed line stops the engine with error set and
the failed and remaining lines still counted; completion is then
signalled the same way. A transfer that would cross 2^40 fails before
moving data. A zero-length start returns the engine to idle and logs
``LOG_UNIMP``. Poison logs ``LOG_UNIMP`` and starts nothing. Reading
control clears busy on an engine armed but never started. After a
transfer, source and destination read one line past the last line
moved. None of these is a measured hardware result.

The miscellaneous error mask enables widget 0 and both engines (bits 0,
4, 8); the chip clears a bit to shut a source down after an error and
software sets it again (``hubio.h``), a *software expectation*. The
model resets it to ``0x111``, *inferred*: the vendor's system-level
diagnostic fails a freshly booted node whose engine bits are clear
without writing them first, and no PROM store sets them. The model never
shuts a source down.

.. _sgi-bedrock-ni:

Network interface
=================

The network interface is the Bedrock's NUMAlink 3 port
(:src:`108-0240-002 p. 2-14`), *established*. Two Origin 300 chassis
joined by a NUMAlink 3 cable form one system of four, six or eight
processors; the cable runs at 1.6 GB/s each way between the rear LINK
connectors (:src:`007-4365-002 p. 1, 8, 38`), *established*. No public
source holds the link protocol beyond a CRC-16 over 128 data bits and a
retry limit, and the model does not implement it. A machine joins two
Bedrocks with a cable object; the port carries the local block's vector
requests and port resets across it.

.. sgi-registers: base=0x10000000 access=64
   skip=0x680118:kept
   map=/machine/peripheral/bedrock:0:0x10000000
   qemu="-M none -cpu R10000 -m 256M -object memory-backend-ram,id=bedrock-dma,size=2M -device sgi-bedrock,id=bedrock,processor-count=4,dma-memory=/objects/bedrock-dma/bedrock-dma[0] -S"

.. list-table::
   :header-rows: 1

   * - Offset
     - Name
     - Width
     - Reset
     - Access
     - Notes
   * - ``0x680000``
     - port status
     - 64
     - n/a
     - read-only
     - Bit 1 link up; :ref:`sgi-bedrock-ni-link`.
   * - ``0x680008``
     - port reset
     - 3
     - n/a
     - write-only
     - Bit 0 link reset, 1 port reset, 2 local reset
       (``hubni_next.h``); :ref:`sgi-bedrock-reset`.
   * - ``0x680010``
     - reset enable
     - 1
     - ``0``
     - read/write
     - Lets a reset arriving over the link reset this Bedrock
       (``hubni_next.h``).
   * - ``0x680018``
     - diagnostic parameters
     - 3
     - ``0``
     - read/write
     - Stored.
   * - ``0x680020``
     - channel control
     - 9
     - ``0``
     - read/write
     - Stored.
   * - ``0x680030``
     - port parameters
     - 35, bit 28 not stored
     - ``0``
     - read/write
     - Stored.
   * - ``0x680038``
     - channel age
     - 16
     - ``0``
     - read/write
     - Stored.
   * - ``0x680100``
     - port errors
     - 53
     - ``0``
     - read; writes store
     - Bits 52:24 drive pending 1 bit 62.
   * - ``0x680108``, ``0x680110``
     - port header A, B
     - 64
     - ``0``
     - read-only
     -
   * - ``0x680118``
     - port sideband
     - 20
     - kept
     - read; writes store
     - Not cleared on reset (``hubni.h``), a *software expectation*.
   * - ``0x680120``
     - port error clear
     - 53
     - n/a
     - read-to-clear
     - Returns port errors and zeroes them.
   * - ``0x681000``-``0x6813f8``, ``0x682000``
     - local routing table (128 entries), global routing table
     - 5
     - ``0``
     - read/write
     - Stored, never consulted.

Reads of port reset, and reads and writes of the channel test register
at ``0x680028``, log ``LOG_UNIMP`` and return zero. Writes to read-only
registers log ``LOG_GUEST_ERROR``. The header states no reset value for
this unit except sideband's; the zeros are each an *approximation*. A companion
header gives a software constant it believes is the port-parameters
default (``hubni_next.h``); nothing uses it and the model
does not.

The stock PROM programs the port once at hub initialisation: reset
enable 1, diagnostic parameters 0, channel control ``0x166``, port
parameters ``0xffffffffa3ff1990``, channel age ``0xc800``, and zero to
port errors and sideband, a *software expectation*
(:src:`ip35-prom-6.210 hub initialisation`). What a write to port errors
or sideband does on the chip is *unknown*; the model stores it.

.. _sgi-bedrock-ni-link:

Port status and link-up
-----------------------

Port status bit 1 is link up (``hubni_next.h``),
a *software expectation*, and it is the only bit the stock PROM tests. The PROM
writes nothing to bring the link up: it programs the port, then polls
bit 1 for one second of the real-time counter before it reports the
local link down and boots alone, a *software expectation*. The model
computes bit 1 on every read, set while a cable joins this port to a
peer, is not marked down and has main power at both ends
(:ref:`sgi-bedrock-power`), clear otherwise. The header makes bits 1:0
the link-level port status and bit 2 remote power (``hubni.h``). What bit 0 and bit 2 read on a live link is *unknown*; the
model reads both as 0, an *approximation*.

OpenBSD reads bit 18 as "more nodes than M mode holds" (``sginode.c``); in the Bedrock's register file that bit is reserved
(``hubni.h``). The model reads it as 0, so OpenBSD
prints "Machine is in M mode", as a published boot of two cabled Origin
300s does (:src:`marc-openbsd-sgi-139223952727384`), *observed*.

.. _sgi-bedrock-lb:

Local block
===========

The local block services processor accesses to the network, crossbar and
memory units, holds its own registers, generates the real-time clock the
processor interfaces count, replies to programmed I/O, launches vector
operations across the fabric during initialisation and services local
invalidates (:src:`108-0240-002 p. 2-17`), *established*. The Origin
200's Hub has no such unit. The header states three reset facts: scratch
0 and its write-if-zero twin reset to zero, scratch 1 and 2 are not
reset, and the error bits survive reset. Every other reset value is
*unknown*.

.. sgi-registers: base=0x10000000 access=64
   skip=0x600000:see-notes,0x604040:kept,0x60c038:read-to-clear
   skiprow="scratch 0-4:see-notes"
   map=/machine/peripheral/bedrock:0:0x10000000
   qemu="-M none -cpu R10000 -m 256M -object memory-backend-ram,id=bedrock-dma,size=2M -device sgi-bedrock,id=bedrock,processor-count=4,dma-memory=/objects/bedrock-dma/bedrock-dma[0] -S"

.. list-table::
   :header-rows: 1

   * - Offset
     - Name
     - Width
     - Reset
     - Access
     - Notes
   * - ``0x600000``
     - revision and identity
     - 48
     - see notes
     - read; write latches node and region size
     - :ref:`sgi-bedrock-lb-node`.
   * - ``0x604000``, ``0x604010``
     - CPU permission, I/O permission
     - 64
     - all ones
     - read/write
     - Stored only. A value other than all ones logs ``LOG_UNIMP``.
   * - ``0x604008``
     - CPU permission override
     - 64
     - none
     - write-only
     - ``0x53474972756c6573`` sets the writer's region bit
       (``bedrock.h``); the model takes region 0.
   * - ``0x604018``
     - soft reset
     - 1
     - none
     - write-only
     - Bit 0 resets the Bedrock; :ref:`sgi-bedrock-reset`.
   * - ``0x604020``, ``0x604028``
     - region present, nodes absent
     - 64, four 16-bit entries
     - ``0``
     - read/write
     - Stored.
   * - ``0x604030``
     - MicroLAN control
     - 34
     - ``0``
     - read/write
     - :ref:`sgi-bedrock-lb-microlan`.
   * - ``0x604040``
     - error bits
     - 11
     - kept
     - read; a write sets bits
     - Any set bit raises pending 1 bit 55 on both interfaces.
   * - ``0x604048``
     - error mask clear
     - 11
     - none
     - write-one-to-clear
     - Clears error bits.
   * - ``0x604050``, ``0x604058``, ``0x604060``
     - error header 1, 2; error data
     - masked
     - ``0``
     - read/write
     - Stored; nothing captures an error.
   * - ``0x604100``, ``0x604108``
     - debug select, debug pins
     - 6
     - ``0``
     - read/write; pins log ``LOG_UNIMP``
     -
   * - ``0x604200``, ``0x604208``
     - real-time clock local and filter control
     - masked
     - ``0``
     - read/write
     - :ref:`sgi-bedrock-lb-rtc`.
   * - ``0x608000``-``0x608020``
     - scratch 0-4
     - 64, 64, 64, 1, 1
     - see notes
     - read/write
     - Scratch 0, 3 and 4 reset to zero; 1 and 2 are kept.
   * - ``0x608040``-``0x608050``
     - scratch 0-2 write-if-zero
     - 64
     - n/a
     - write
     - A write takes effect only while the register is zero.
   * - ``0x608058``, ``0x608060``
     - scratch 3, 4 read-set-if-zero
     - 1
     - n/a
     - read
     - A read returns the old bit and sets it. The Itanium port uses
       scratch 3's twin as a hub-wide lock.
   * - ``0x60c000``-``0x60c038``
     - vector parameters, route, data, status, return route, read data,
       status clear
     - 64
     - ``0``
     - see :ref:`sgi-bedrock-lb-vector`
     -

Reads of write-only registers log ``LOG_GUEST_ERROR`` and return zero.
Other offsets read zero and drop writes with ``LOG_UNIMP``.

OpenBSD writes its reboot request to scratch 2: magic ``0x0ead0000``
with a command and options (``ip27_machdep.c``;
:ref:`sgi-ip35-address-map-handoff`), a *software expectation*. The
IRIX 6.5.30 kernel writes its restart request there too. Because scratch
2 survives reset, it can serve as a mailbox the firmware reads after the
reset; no public source says the firmware reads it. The stock PROM uses
scratch 1 as its reset-time rendezvous mailbox and later for
node-number assignment (:ref:`sgi-bedrock-rendezvous`).

.. _sgi-bedrock-lb-node:

Revision word and node number
-----------------------------

The revision word holds manufacturer (11:1), part (27:12), revision
(31:28), node number (39:32) and region size (47:46): 0 for one node per
region, 1 for two, 2 for four (N mode), 3 invalid (``hublb.h``,
``hublb_next.h``), a *software expectation*. The Itanium port reads it
as a fence before each posted-write flush (``pio_flush.h``),
a *software expectation*.

The stock PROM reads its node number from bits 39:32 wherever it needs
one. It stores zero to the whole word at hub initialisation, and assigns
a number by storing the word back with bits 39:32 and 47:46 replaced, a
*software expectation* (:src:`ip35-prom-6.210 node number assignment`).
The identity fields must then be read-only, *inferred*: the early store
of zero would otherwise erase them. The model latches node number and
region size from an aligned 64-bit write and keeps the identity fields.
A write naming region size 3 is refused whole with ``LOG_GUEST_ERROR``.
The latched number is the one every unit of the chip decodes by, and the
node's processor and fabric views follow it. Every reset restores the
machine's configured node number and region size, an *approximation*;
the hardware value is *unknown*.

.. _sgi-bedrock-lb-vector:

Vector engine
-------------

A vector operation sends a read or write along an explicit port-by-port
route. Software waits for busy to clear, writes route and data, then
writes parameters last; the reply loads status, return route and read
data (``hublb.h``), a *software expectation*. Route nibbles are
consumed from the least significant; route 0 reaches the device at the
other end of the node's cable, a *software expectation* from the stock
PROM's discovery code (:src:`ip35-prom-6.210 fabric discovery`). The
PROM reads status-clear before every launch and fails an operation whose
status stays valid, so status-clear is read-to-clear, *inferred*; the
Origin 200's equivalent is read-and-clear.

The model performs a route-0 launch on the peer Bedrock's own registers
at once:

- a reply sets status valid with the request id, write id and address
  echoed and bits 2:0 equal to the request type (0 read, 1 write), which
  is what the PROM accepts; a read also loads read data;
- a launch while status is valid sets overrun and does nothing else;
- no cable, a cable marked down or a nonzero route on a direct pair
  latches nothing and logs ``LOG_GUEST_ERROR``, so the firmware's poll
  times out;
- a peer that refuses the offset latches nothing and logs the refusal;
- a peer whose main power is off answers a route-0 request with status
  valid, error code 4 and return route 0, leaving read data alone;
- a request type other than read or write logs ``LOG_GUEST_ERROR`` and
  latches nothing.

Routes through a router are on :doc:`sn1-router`. A direct reply leaves
return route 0. A write to status or status-clear clears it when zero
and logs ``LOG_UNIMP`` otherwise; return route and read data take any
written value. These outcomes are each an *approximation*: what the chip
answers with no link or for a refused offset is *unknown*. A remote
request applies no permission check.

.. _sgi-bedrock-lb-rtc:

Real-time clock controls
------------------------

The local block supplies the clock both processor interfaces count. With
use-internal set it divides the Bedrock clock by a maximum count; with
it clear it filters an incoming global clock (``hublb.h``), a *software expectation*. No public source gives the resulting frequency
or the firmware's setting. The stock PROM converts the counter to
microseconds as ``counter * (((control & 0x3ff0) >> 3) + 2) / 200``,
reading the maximum count back from local control, a *software
expectation*. For that to agree with the 1,250,000 Hz every consumer
assumes, the field would have to read 79, *inferred*.

The model resets both registers to zero, an *approximation*, and keeps
written values to their defined fields. Nothing feeds them into the
counter rate. With the field at zero the PROM's own conversion runs
about eighty times short of real time.

.. _sgi-bedrock-lb-microlan:

MicroLAN controller
-------------------

``0x604030`` is the controller for the node board's add-only serial
identity part: bit 0 received data, bit 1 completion, sample offset
9:2, low-pulse duration 19:10 and two phase dividers to bit 33, data
least significant bit first (``hublb.h``),
a *software expectation*. The XBridge's MicroLAN register matches the low 20 bits
field for field, and there a write starts one slot that software polls
for completion (:src:`xbridge-asic-spec-0.5 p. 79-80`); that the Bedrock
works the same way is *inferred*.

The model starts one slot per write, clears completion and publishes
completion and the sampled level 1 µs of virtual time later. Pulse
classification reuses QEMU's SGI one-wire slot helper. A write while a
slot is pending logs ``LOG_UNIMP`` and is discarded. These are
each an *approximation*. No identity part is attached: which part IP45 fits,
its commands and its contents are *unknown*, so a slot sees the bare
wire.

.. _sgi-bedrock-xb:

Internal crossbar
=================

The crossbar is an 8-input, 6-output switch connecting the Bedrock's
units, with arbitration and some error handling
(:src:`108-0240-002 p. 2-11`), *established*. This is the crossbar inside
the Bedrock; the one at crosstalk widget 0 is the XBridge's
(:doc:`xbridge`).

.. sgi-registers: base=0x10000000 access=64
   map=/machine/peripheral/bedrock:0:0x10000000
   qemu="-M none -cpu R10000 -m 256M -object memory-backend-ram,id=bedrock-dma,size=2M -device sgi-bedrock,id=bedrock,processor-count=4,dma-memory=/objects/bedrock-dma/bedrock-dma[0] -S"

.. list-table::
   :header-rows: 1

   * - Offset
     - Name
     - Width
     - Reset
     - Access
     - Notes
   * - ``0x700000``
     - parameters
     - 52
     - ``0x0008_0000_8000_0021``
     - read/write
     - Bypass grants, age, deadlock and tail timeout wraps. The reset is
       the companion header's software constant (``hubxb_next.h``); the chip's value is *unknown*.
   * - ``0x700008``, ``0x700010``, ``0x700018``
     - slow grant, spew control, I/O arbitration trigger
     - 32, 11, 4
     - ``0``
     - read/write
     - Stored. A nonzero spew control logs ``LOG_UNIMP``.
   * - ``0x700020``-``0x700088``
     - queue error registers, fourteen
     - see notes
     - ``0``
     - read; write injects
     - :ref:`sgi-bedrock-xb-errors`.
   * - ``0x700090``
     - first error
     - 13
     - ``0``
     - read-only
     - 3:0 type, 7:4 channel, 11:8 source, 12 valid.
   * - ``0x700098``, ``0x7000a0``
     - debug data and arbitration control
     - 16, 3
     - ``0``
     - read/write
     - Stored. A nonzero value logs ``LOG_UNIMP``.
   * - ``0x700120``-``0x700188``
     - queue error clear, one per queue register
     - 64
     - none
     - write
     - Any write clears that queue.
   * - ``0x700190``
     - first error clear
     - 64
     - none
     - write
     - :ref:`sgi-bedrock-xb-errors`.

The Itanium port writes zero to all fifteen clear registers at probe
(``huberror.c``), a *software expectation*. Reads of clear
registers and of holes return zero with ``LOG_UNIMP``. Writes to first
error log ``LOG_GUEST_ERROR``. Reset values other than parameters are
each an *approximation*.

.. _sgi-bedrock-xb-errors:

Error injection and first error
-------------------------------

The header says the PI0 output queue error register "can be written to
test software, will cause an interrupt". The first error latches while
valid is clear, interrupts both processor interfaces and persists until
its queue register and then first error are cleared (``hubxb.h``), a *software expectation*. Seven queues have published layouts:
PI0 output and input, MD-to-PI0 output, MD input, NI output, and LB
output and input. The source codes are in ``hubxb_next.h``;
type and channel codes are not published. The model numbers the sources
down from 0xf in address order, queue n having source 0xf - n. Only PI0
output (0xf) and LB input (0x2) are confirmed against the header, so the
values between are an *approximation*.

The model ORs a write's documented bits into a structured queue and
marks that queue unserviced. The first such write while first error is
clear latches that queue's source with type 0 and channel 0 and raises
pending 1 bit 54 on both interfaces. A write to a queue's clear register
zeroes it and marks it serviced. First error clear succeeds only after
the recorded queue has been serviced; a premature clear logs
``LOG_GUEST_ERROR`` and changes nothing. After a clear, the
lowest-address queue still holding errors latches next. The seven
queues the header lists without structure read zero and drop writes with
``LOG_UNIMP``. Treating any write to a clear register as a clear, and
the relatch order, are each an *approximation*. Arbitration, timeouts and
traffic errors are not modelled.

.. _sgi-bedrock-flash:

Boot flash
==========

SGI describes a 16 MiB logical flash behind the Bedrock, reached through
a local and a remote boot space (:src:`us-patent-6751698`),
*established*. Linux SN1 defines a 128 MiB local boot space, a remote one
per node and two PROMs on 32 MiB boundaries (``sn/addrs.h`` at ``7570df54ef8c``), a *software expectation*. The fitted parts, their
capacities and their wiring are *unknown*. QEMU models two 8 MiB parts as
``sgi-bedrock-flash``, an *approximation* that reconciles the 16 MiB
total with two PROM positions.

Each position is a QEMU ``pflash_cfi02`` device: AMD command set,
16-bit wide, big-endian, 128 sectors of 64 KiB, unlock addresses
``0x555`` and ``0x2aa``. Its CFI identifiers are QEMU's defaults and name
no part. The stock PROM's probe accepts this: it requires the ``QRY``
signature, primary command set 2, 16-bit capability and at most four
erase regions, a *software expectation* (:src:`ip35-prom-6.210 flash
probe`).

- Position 0 holds the lower 8 MiB of the backing image; position 1 the
  upper 8 MiB.
- Reads at boot-space offsets 0 to 8 MiB reach position 0, and offsets
  32 to 40 MiB reach position 1. A 64-bit read takes two 32-bit flash
  cycles. Other reads fail with ``LOG_UNIMP``; a read crossing the end of
  a part fails with ``LOG_GUEST_ERROR``.
- Writes must be 64-bit, aligned to four bytes, and carry a value no
  wider than 16 bits. A store at offset ``4n`` above a position's base
  presents its 16-bit word to that part at byte offset ``n``. The patent
  describes a compressed write interface; this factor of four is the
  model's reading of it, *inferred*. Any other store fails without a
  flash cycle and logs ``LOG_GUEST_ERROR``, or ``LOG_UNIMP`` for a
  width other than 64 bits.
- Both boot spaces reach the same two parts, so a command begun through
  one completes through the other. Each part has its own command state.

The stock PROM issues CFI query and reset commands to position 1 while
it executes from position 0, so the two must not share command state, a
*software expectation*. The processor's reset view and the bounded high
local view show position 0 through read-only execution views that refuse
stores and are refreshed after every accepted flash cycle.

The machine takes a 16 MiB raw image through the ``node<N>-flash``
property, or boots the stock payload from ``-bios`` when none is given
(:ref:`sgi-sn-firmware-supply`). A fresh image written by
``qemu-sgi-machine-init`` carries the payload, a position-1 outer label
the PROM's own blank-media formatter would write. The tool
writes no CPU-disable environment records. The stock PROM writes them
itself after failed starts of an empty processor position, and a
two-processor machine's flash starts without them. That is a model
limit: no factory flash dump is available to seed them from.

.. _sgi-bedrock-interrupts:

Interrupts
==========

The Bedrock has no separate interrupt controller; the processor
interfaces' pending banks and masks are its interrupt logic
(:ref:`sgi-bedrock-pi-interrupts`). Each fitted processor receives five
level outputs and an NMI from its slot. Every unit's error reaches the
processor interfaces:

.. list-table::
   :header-rows: 1

   * - Source
     - Destination
   * - L1 UART
     - pending 0 bit 4, both interfaces
   * - graphics credit below bias
     - pending 0 bit 1 or 2, the slot's own interface
   * - crossbar first error
     - pending 1 bit 54, both interfaces
   * - local block error bits
     - pending 1 bit 55, both interfaces
   * - memory or directory correctable error
     - pending 1 bit 61, both interfaces
   * - network port errors 52:24
     - pending 1 bit 62, both interfaces
   * - memory or directory uncorrectable error
     - error pending bit 24, both interfaces
   * - widget interrupt write
     - the pending bit it names, on the interface it addresses
   * - block-transfer completion
     - the pending bit the engine's interrupt address names

The I/O interface's own error interrupt, the processor nack and spool
interrupts and the clock-error bit are never raised.

.. _sgi-bedrock-power:

Main power
==========

The ``main-power`` input stands for the node's switched supply. It is
high unless something drives it low, so a Bedrock on its own is powered.
A reset does not change it: whatever owns the supply drives it, and
nothing in the Bedrock switches it. This is a model construct with no
register behind it, an *approximation*.

While the input is low the Bedrock answers nothing that arrives through
the fabric. A processor on another node reading or writing its
registers, a peer's vector request and a crosstalk packet bound for it
all fail the way they do when the Origin 300 PCI expansion is off. The
vector engine of the peer gets no reply, and a block transfer or a
widget write bound for it gets an error. What a powered-down or held
Bedrock does on hardware is *unknown*; the model treats the two alike,
an *approximation*. Registers keep their values while the input is low.

Without power the network port reads disabled to the cable, so the
link is down at both ends. A directly cabled peer reads port status bit
1 clear, as for a cable marked down (:ref:`sgi-bedrock-ni-link`), and a
router's port link bit for the Bedrock clears. No capture shows a
NUMAlink 3 port facing an unpowered node, and the landed sheets say what a
port reads with an unpowered or held peer is *unknown*, so reading it as
down is an *approximation*, with the value *unknown*. The Bedrock's own port
status reads the same, clear. The Origin 300 PCI expansion's own power
gate does not take its link down: a packet to a powered-off expansion is
still refused on a link that reads up.

Power going away lowers every level and NMI output into the processors,
and nothing raises one while the Bedrock is off: a line left high would
keep an off node's processor interrupted. What the chip does with those
outputs without power is *unknown*; holding them low is an
*approximation*. The pending banks keep their state, and power returning
raises exactly what is still pending and unmasked. An NMI request that
arrives while the Bedrock is off is dropped, also an *approximation*.
Stopping the processors themselves is the node's business, not the
Bedrock's.

A compute node (``sgi-sn1-node``) forwards its own ``power`` input,
high by default, to its Bedrock's ``main-power``. The L1 of the node's
brick drives it from its ``node-power`` output (:ref:`sgi-l1-power`).
When the node is off, its memory leaves
every other node's map. The cabled peer's claim on it is dropped on a
direct pair, and a routed alias is disabled, so a processor read gets a
bus error, and a block-transfer read or write fails with the transfer
status a cable marked down gives. The Bedrock's DMA view also holds a
low-priority alias of the whole system map under the node claims; the
transfer tests pin that a copy cannot fall through it to an off node's
RAM. The node's own RAM keeps its bytes while off, an *approximation*:
DRAM loses them on hardware. A level driven into the node before its
Bedrock exists is kept and handed over once it does, so the Bedrock
starts unpowered.

The node also stops and starts its own processors with the input. When
the supply goes, each processor of that node is reset and halted by work
queued on its own thread, the way the Origin 200 Hub stops one, so no
other node's processor is paused and nothing a stopped processor was
executing survives. The reset leaves Status.ERL set, which masks every
interrupt, and nothing else wakes it: the interrupt and NMI lines are held
low (above), a processor interface NMI store cannot reach a Bedrock that
answers nothing, and a park release queued for a processor of an off node
does nothing. The loss of the supply also ends every park. A stop and
continue of the machine, or a migration, leaves the node stopped, because
the halt is processor state and not a pause; a load that finds the Bedrock
off holds the node's processors again when the machine runs, in case the
source stopped before its queued stop ran. What the processors of an
unpowered node do on hardware is not recorded, so the reset and halt is an
*approximation*. The ``halted-cpus`` property of the node reports which of its
processors are halted, as a mask by number within the node.

When the supply returns the node starts cold: the Bedrock and every unit in
it, the L1 UART window and, on a board that carries its own, the XBridge
with the devices behind it are reset, the Bedrock is powered, and the
processors are restarted at the reset vector from work queued on their own
threads. The node's controller and processors are not part of that reset
list, and no other node is reset. Which registers survive a supply cycle on
hardware is *unknown*; the model resets them all, an *approximation*, so the
node's number, memory configuration and routes are the firmware's to
program again. The model re-arms each processor's cache-as-RAM window
(:ref:`sgi-prom-image-early-window`) with that reset, an *approximation*:
on hardware the window is part of the PROM's cache-only phase, which the
restarted processor runs again. Each processor's start is owed until its
work runs, and the work takes the debt: a start queued twice, by a power cycle before the
first ran or by a resume that finds one still queued, starts the
processor once, and never resets it again after it has run
(``sn1/cross-node/owed-start-runs-once`` leaves a power-up's starts
unqueued with the test-only ``x-test-start-delay-ms`` property). The
processors whose start has not yet run when the machine is saved travel in
the stream as a mask (the ``sn1-node/start`` subsection, sent only while a
start is owed) and the destination finishes them.

The level migrates while it is low, and the stream is the whole truth
about it: a Bedrock starts a load from on, so a destination that began with
its node off ends on when the source was on, and its processors, which the
stream carries too, are not restarted. The controller does not drive the
node's input again as it loads, because that would run a supply cycle in
the middle of the load and reset parts the stream has already filled
(``sn1/cross-node/load-into-off-nodes``, ``load-into-on-nodes`` and
``load-keeps-running-processors`` migrate between machines whose controllers
start at different levels). A Bedrock loaded off keeps its lines low and
re-derives its node's decode and a directly cabled Bedrock's, as a live
change does, and so does one that was off and is loaded on. That re-derive is
defensive against load order; load order follows realize order, so no qtest
exercises it.

What the machine decides from the supplies is not migrated: that the last
compute node went off and ended the machine, that the check for it is
queued, and the count of processor stops still owed after a loss. A machine
that ended under ``-no-shutdown`` and was then migrated does not resume
when a node is powered up on the destination, because the destination never
knew it had ended; and a migration taken between a loss and its
announcement loses the shutdown it would have raised, the processors being
held off by the loaded level but the machine not ending. Both are an
*approximation*. A loss that is announced is raised only after every
processor of the node has stopped, and work left over from a loss that the
supply's return cut short is ignored by the next loss. A stop and
continue of the machine while a loss is still settling holds the
processors again and keeps the loss's announcement, which is then raised
once they have all stopped again
(``sn1/cross-node/stop-cont-keeps-the-pending-announcement`` leaves a
loss's stops unqueued with the test-only ``x-test-stop-queue-delay-ms``
property). The test-only delay properties are not migrated, and
``x-test-stop-delay-ms``, which sleeps with the big lock dropped, is
refused unless the accelerator is TCG
(``sn1/cross-node/last-brick-power-down-announces-after-the-stop`` and
``repeated-loss-announces-when-the-last-stop-ran`` hold each stop back with
the test-only ``x-test-stop-delay-ms`` property, since the race is
otherwise too short to observe).

.. _sgi-bedrock-node-restart:

Node restart
============

A node has a ``reset`` input, a pulse, that restarts that one node. Its
brick's L1 drives it for an L1 ``reset``, for the reset request a kernel
sends the L1, and for the peer's side of ``* reset`` (see the L1 page).
Nothing else is touched: no other node, no router and no L1. A node
without power is never reached, so the reset leaves it off and does not
start it, and a node whose supply is on is restarted whatever its state
was. When the nodes a reset reaches are every powered compute node the
machine resets as a whole instead (:ref:`sgi-bedrock-reset`).

The restart is the supply loss's stop and the power-up's start joined by a
reset of the parts. Each processor is reset and halted from work queued on
its own thread, so a processor never runs between the reset and the reset of
the parts it uses. When the last has stopped, the node resets in one memory
transaction: the Bedrock and its units, the L1 UART window and, on a board
that carries its own, the XBridge are cold-reset as a supply cycle resets
them, and the reset input of an XBridge in another brick cabled to the
node's I/O interface is pulsed. That bridge is reset cold with the PCI
devices behind it only if it takes the pulse, and it ignores one while it is
held, which it is when its main power is off or its reset hold is asserted.
A node with no processors has nothing to stop and resets its parts at once.
Then the processors are started at the reset vector from work queued on
their own threads. The Bedrock's supply level is
not touched, so the node's port keeps reading up to its neighbour through
the restart, and the memory keeps its bytes. Each processor's cache-as-RAM
window (:ref:`sgi-prom-image-early-window`) is re-armed with the cold reset,
an *approximation*: on hardware the window is part of the PROM's cache-only
phase, which the restarted processor runs again. The node's identity word
returns to the machine's configured number and region size, as at a machine
reset; no other node's identity word, registers or memory configuration is
touched, so a running neighbour keeps its number across the restart.

The restart's debts follow the power-up's. A newer restart or a loss
makes the stops and starts a restart queued before it do nothing (a count
and an epoch, shared with the supply loss), so a node restarts once and a
loss that overtakes a restart leaves it off. A load that finds a restart
whose stops had not all run restarts the node again when the machine runs,
because the queued work is not in the stream: the ``sn1-node/restart``
subsection is sent only while one is owed, and the processors' starts are
the existing owed-start mask.

Which registers the L1's reset group leaves in the Bedrock is *unknown*
for every register; the headers' survivors are stated for a soft reset, or
for a reset that names no line. The model resets everything a supply cycle
resets, so the survivors are what the units keep through any reset, an
*approximation*: local block scratch 1 and 2, the error bits and the
processor interfaces' protection, region present, cached-alias size, CPU
enable and error stack registers. Scratch 2 is the one survivor with a
software reason, the kernel's restart word, which the PROM reads after
the reset; the other survivors follow from the units. Which of the L1's
reset lines reaches which Bedrock input is *unknown* too: the reset,
the reset hold and release pair, the kernel's request and the front-panel
switch drive one control group, and the model joins that group to this
input. No L1 path reaches an I-brick's bridge (*software expectation*): the PROM
warm-resets it over the link after it runs (*software expectation* for the write,
*inferred* for the response). Resetting the bridge when the node restarts
is an *approximation*, kept because a bridge left running could send
transfers into a node that is restarting. A restart through an IX-brick's or
PX-brick's PIC, which has no reset input, leaves the PIC alone. A processor
that an NMI or a park release wakes between its stop and the restart's start
is reset again by the start. The Bedrock's own resets reach nodes by the
rule in :ref:`sgi-bedrock-reset` and use the same restart.

The kernel's reset request may come from every processor of the node,
repeatedly, and from the boot processor later than from the others. Each
request restarts the node again, so one that arrives while the processors
are stopping only starts the restart over, one while the node is off does
nothing, and a request never reaches another node
(``sn1/cross-node/l1-reset-request-restarts-its-node``).

The node's ``reset`` input is exercised by
``sn1/cross-node/restart-one-node``,
``restart-does-not-start-an-off-node``, ``loss-overtakes-a-restart``,
``restart-in-flight-restarts-once``, ``restart-survives-migration``,
``restart-keeps-the-survivors``, ``restart-ignores-an-off-node`` and
``restart-rearms-the-cache-as-ram-window``, and,
through the L1, by ``l1-reset-restarts-its-node``, ``l1-reset-request-restarts-its-node``,
``l1-reset-leaves-an-off-brick-off`` and
``l1-reset-restarts-a-counting-node``. The tests that run guest code hold
the restarted node's starts back with the test-only
``x-test-start-delay-ms`` so the held processor can be seen;
``loss-overtakes-a-restart`` and
``restart-in-flight-restarts-once`` hold each stop with
``x-test-stop-delay-ms``; ``restart-survives-migration`` holds the stops
back with ``x-test-stop-queue-delay-ms``.

.. _sgi-bedrock-reset:

Reset
=====

Four requests reset the Bedrock in the model:

- a machine reset applies every unit's reset;
- port reset bit 2, local reset, resets the node whatever reset enable
  holds (*software expectation* for the register meaning; the enable not blocking it
  is the reviewed reading, an *approximation*);
- local block soft reset bit 0 does the same;
- port reset bit 1 sends a warm reset over the cable, and the Bedrock at the
  far end takes it when its own reset enable bit 0 is set. A router between
  the two passes it on by its own rule
  (:ref:`sgi-sn1-router-warm-reset`). With no link the write logs
  ``LOG_GUEST_ERROR``.

Port reset bit 0, link reset, reaches nothing. The header calls reset
enable a warm-reset enable that lets the link-level protocol reset the
Bedrock (``hubni.h``, ``hubni_next.h``),
a *software expectation*. Whether it also gates a local reset is *unknown*; the
model does not gate it.

**The reach of a reset.** A write of the port reset register or of the
soft reset register collects a set of nodes once, from the registers as they
stand when it is written. The set holds the writer's node when bit 2 (or the
soft reset) is set, and the Bedrocks that take the warm reset when bit 1 is
set: a directly cabled Bedrock whose reset enable bit 0 is set joins, a
router passes the reset on through its permit and masks, and each
chip is visited once per reset. A powered-off node is never reached. The
set follows the registers the firmware programmed and nothing else; the
partition number the L1 reports plays no part, so there is no partition
approximation here. Then:

- when the set is every powered compute node, the machine resets as a whole,
  keeping each L1's power state, and the ``RESET`` event is emitted. This is
  the unpartitioned case: a one-node machine, or a pair with both enables
  set, which is what the stock firmware leaves and a reboot writes. It
  is an *approximation*: it also restarts the routers and every L1, which a
  node reset does not. It applies because an unpartitioned guest cannot
  survive one brick restarting (*inferred*; the firmware finds the survivor
  outside its reset space and the two then disagree about the node
  numbering, and what a running kernel does then is *unknown*). A
  machine reset starts an L1 whose node was off as it was, so an off node
  stays off;
- otherwise each node in the set restarts alone (:ref:`sgi-bedrock-node-restart`)
  and the others, which are not in the set, run on. No ``RESET`` event is
  emitted.

A Bedrock that receives a warm reset it does not take, because its reset
enable bit 0 is clear, keeps running: it is not reset, its registers and its
identity word are left alone, its link stays up, and it latches port errors
bit 52, "link reset in", which raises its network interface error interrupt.
The bit's name is a *software expectation*; that a refused reset sets it is *inferred*,
and the model latches it, an *approximation*. No other error bit is set and
no interrupt is raised without one. A kernel that receives it with the reset
enable clear (and a partition number of zero) prints a notice and goes on;
that is the kernel's behaviour, not the model's, and the model sets only
bit 52. A powered-off Bedrock is not reached and latches nothing. No
link-up interrupt follows a peer's restart, and the model does not drop the
survivor's link across it. Which of the survivor's registers an arriving
warm reset leaves is *unknown*; the model leaves all of them.

Which Bedrock registers a full reset clears beyond a soft reset, and whether
the reset enable survives either, are *unknown*. The reset enable register
returns to zero with the interface's registers, so the firmware writes it
again before it resets, as the stock firmware and IRIX do.

A Bedrock with no board, as in a register test, resets the units it reaches
directly (the writer's, and a peer that takes the warm reset) and pulses each
one's ``node-reset`` output once, for a board to restart the processors.

A local reset applies each unit's reset, the memory unit's soft reset in
place of its full one. The stock PROM and the IRIX 6.5.30 kernel write reset
enable and then local plus port reset. OpenBSD writes local plus link
reset (OpenBSD ``sys/arch/sgi/sgi/ip27_machdep.c`` at ``2df91d80c104``). None
jumps to the firmware itself, a *software expectation*. A published Origin
350 console running PROM 6.210 prints "Resetting the system..." after a POD
``reset`` and goes straight to the PROM banner (:src:`graphica-o350-notes`),
*observed*. A restarted node therefore returns to the firmware's reset vector.
What else a local reset reaches on the chip is *unknown*. A node that
restarts is reset the way a supply cycle resets it (see Node restart), so
the memory unit's soft-reset survivors do not outlast it.

The reach is exercised by ``sn1/cross-node/bedrock-reset-reaches-every-node``
(each node writing), ``bedrock-reset-leaves-a-disabled-peer`` (each node
writing), ``bedrock-local-reset-ignores-the-enable``,
``bedrock-port-reset-alone-restarts-the-peer`` and
``bedrock-reset-never-reaches-an-off-node``, and, with no machine, by
``sn1/bedrock-link/refused-port-reset-latches-link-reset-in`` and
``port-reset-crosses-two-routers``.

What each reset keeps, from the unit sections above:

- processor interfaces: protection, region present, cached-alias size,
  CPU enable, error pending, error stack, error status and spurious
  headers are never reset; everything else returns to zero and the
  counter restarts from zero;
- local block: scratch 1 and 2 and the error bits are kept; the node
  number and region size return to the machine's configured values;
- network interface: sideband is kept;
- memory unit: its soft reset keeps memory configuration, refresh and
  directory configuration; a machine reset clears them and zeroes the
  directory, page-counter, protection and ECC stores.

RAM contents survive every reset.

.. _sgi-bedrock-rendezvous:

Boot rendezvous approximations
==============================

The stock IP35 PROM brings up a node's processors assuming they left
reset on the same clock edge and have run continuously since. Four of
its early handshakes order one processor's stores before another's
reads by nothing more than both running at hardware speed, a *software
expectation*. Under QEMU's multi-threaded TCG a processor's host thread
can fall seconds behind its peers. At each of the four points the model
halts the processor that arrived early until the ones it must follow
catch up, or until 2 s of virtual time pass. A halt changes when a
processor next runs, never a value it reads. These are
each an *approximation* with no hardware counterpart; they apply under
multi-threaded TCG, round-robin TCG and ``-icount`` alike.

- Reset barrier. Every processor clears local block scratch 1 after
  reset, then the boot master posts its release there. A processor's
  second or later write to scratch 1 is held until every other running
  processor has made its first. An access to scratch 1 from outside the
  node, such as the peer's vector engine, first lands every held write
  in order.
- Pair liveness. Each processor of a pair writes a status value to its
  own real-time compare register and polls its partner's. The first of
  a pair to write is held until its partner writes.
- Presence publication. The lowest fitted processor of each interface
  publishes its interface's processors in I/O interface scratch 0 under
  the hub lock. A publisher is held at its next hub-lock release until
  the other interface has published.
- Partner wait. The other processor of an interface is held at its next
  real-time counter read until its publisher has released the lock.

A local reset wakes a held processor. A migration carries any held
scratch 1 values and releases every held processor on load.

.. _sgi-bedrock-migration:

Migration
=========

Each unit migrates its registers and its pending timers: the real-time
counter's value and phase, the comparator and profile timers, the PIC
re-assertion times, the MicroLAN slot in flight, the self-test in
progress with its remaining time, and any block-transfer completion
interrupt not yet delivered. The memory unit carries the directory,
page-counter, protection and ECC stores, the marked doublewords and
lines, the processors' copies of followed lines and the 74-word
compatibility store; watched pages are cleared on load. The flash parts
carry their stored bytes and command state, and each execution view its
own copy of position 0. Outputs and memory decode are recomputed on
load. Population, revision, region and bank sizes are machine
configuration and must match on both sides. Migration works only
between identical builds.

.. _sgi-bedrock-measurements:

Measurements
============

No Bedrock register has been read on a physical machine for this model.
Every value above comes from SGI's headers, published captures, the
stock firmware or the model itself. Reads at PROM hand-off on an Origin
300 or Fuel of the two identity words, the local block's clock controls,
the I/O interface's access registers and miscellaneous error mask, and
the processor interfaces' CPU enable would settle most of the *unknown*
reset values here.

.. _sgi-bedrock-limits:

Model limits
============

- No cache coherence: processors share RAM directly. Directory words
  follow processor requests only on watched pages, and cached
  uncorrectable reads raise no Cache Error exception.
- No refresh, page migration, performance counting, error spooling,
  error capture in the status and header registers, request-buffer
  flow control or timeouts. Those registers are stored only.
- CPU and I/O protection, the local block's permissions and region
  present are stored but not enforced, except I/O protect on crosstalk
  interrupt writes and the current-cell check on the L1 UART windows.
- CPU enable does not stop a processor. The processor interface's
  remap and error-forward fields route nothing.
- The real-time counter runs at a fixed 1,250,000 Hz, independent of
  the local block's clock controls.
- The graphics write stall and the interrupt counter are not
  modelled.
- Block transfers finish inside the starting write; nothing can be
  terminated part way. Poison is unsupported.
- Inbound requests without a source identity are checked against every
  candidate widget at once. Inbound register accesses decode only the
  two interrupt-pending-modify writes.
- The link-level protocol, routing tables, link reset and the
  network interface's error counting are not modelled. Without a router
  the vector engine reaches only the far end of the node's own cable.
- No MicroLAN identity part is attached.
- A reset that reaches every powered compute node resets the whole machine,
  routers and L1s included, with each L1 keeping its power state.
- Unexpected accesses: holes between units, wrong widths and refused
  memory, directory, ECC, fetch-and-op or flash accesses fail with a
  bus error. Reads and writes of undecoded register offsets inside a
  unit log ``LOG_UNIMP`` and return zero. Guest mistakes such as writes
  to read-only registers or refused inbound requests log
  ``LOG_GUEST_ERROR``.
- Migration works only between identical builds.

.. _sgi-bedrock-tests:

Tests
=====

Register-contract qtests, one per unit:

- ``tests/qtest/sn1-bedrock-pi-test.c``: CPU number with and without a
  processor, presence and enable on sparse populations, pending
  reflection and modify, coherency pending, masks, power-on and local
  reset values, error capture, the NMI predicate, the comparators and
  profile wrap, 32-bit counter reads, lines held low without main
  power, and migration of outputs, counter phase and enables.
- ``tests/qtest/sn1-bedrock-md-test.c``: the register contract, bank
  decode, fitted populations and bank-select remapping, the self-test
  phases, completion without the timer, cancellation and migration,
  the directory, page-counter, protection and ECC backdoors, ECC and
  directory checking with their interrupts, and fetch-and-op.
- ``tests/qtest/sn1-bedrock-io-test.c``: identity, widget status, PIO
  translation, inbound protection with and without a source identity,
  inbound register writes, the block-transfer engines including errors,
  programming edges and back-to-back starts, the Itanium probe sequence,
  write-one-to-set, reset and migration.
- ``tests/qtest/sn1-bedrock-lb-test.c``: identity, permissions, the
  revision write, reset values of permissions, clock controls and
  scratch registers, the vector registers with no link, the MicroLAN
  slot and its reset and migration.
- ``tests/qtest/sn1-bedrock-xb-test.c``: control storage, PI0 output
  injection, service order, premature clear, relatch, the probe replay,
  structureless queues, widths, reset and migration.
- ``tests/qtest/sn1-bedrock-flash-test.c``: one-to-one reads, distinct
  positions, query mode, program and erase, both boot spaces, the store
  decode, cycle widths, execution views, reset and migration.

Composition and machine tests:

- ``tests/qtest/sn1-bedrock-test.c``: the composed identity and PIO
  path, crosstalk interrupt writes, the L1 UART windows and current
  cell, the node number decode, the CPU-enable lines, and the network
  interface's controls, routing storage, reset requests and the stock
  PROM's power-on programming.
- ``tests/qtest/sn1-bedrock-link-test.c``: link-up from the cable,
  route-0 vector reads and writes to the peer, refused offsets, port
  reset across the link, held scratch 1 writes against a peer's
  vector write, an unpowered peer reading link down
  (``sn1/bedrock-link/unpowered-peer-reads-link-down``), a router port
  clearing its link bit for an unpowered node
  (``sn1/bedrock-link/unpowered-node-clears-the-router-port``) and a
  routed target that loses and regains main power
  (``sn1/bedrock-link/routed-powered-off-target-no-reply``).
- ``tests/qtest/sn1-cross-node-test.c``: node-number decode, the
  block-transfer engines' routes, and an unpowered node's memory absent
  from both the processors' and the engines' view, direct and routed,
  also across migration, including a migration while the first node is
  off, which guards that path and not device load order
  (``sn1/cross-node/unpowered-node-memory-is-absent``,
  ``sn1/cross-node/unpowered-node-survives-migration``,
  ``sn1/cross-node/unpowered-first-node-survives-migration``), a
  supply cycle that halts and restarts only its own node's processors
  (``sn1/cross-node/node-power-halts-only-its-processors``), counting
  code that runs on both nodes while one is off, ignores the Bedrock's NMI
  input and a stop and continue, and restarts when the supply returns
  (``sn1/cross-node/node-power-stops-one-node``), an off node that
  stays off across a migration
  (``sn1/cross-node/off-node-survives-migration``), a supply cycle that
  re-arms a processor's cache-as-RAM window
  (``sn1/cross-node/power-return-rearms-the-cache-as-ram-window``), a
  power-up's start that is owed and runs once (``sn1/cross-node/owed-start-runs-once``),
  the supply levels of source and destination differing in either
  direction (``sn1/cross-node/load-into-off-nodes``,
  ``sn1/cross-node/load-into-on-nodes``,
  ``sn1/cross-node/load-keeps-running-processors``), and the order of a
  last node's loss and the shutdown
  (``sn1/cross-node/last-brick-power-down-announces-after-the-stop``,
  ``sn1/cross-node/repeated-loss-announces-when-the-last-stop-ran``,
  ``sn1/cross-node/stop-cont-keeps-the-pending-announcement``).
- ``tests/qtest/sgi-pci-expansion-ni-test.c``: the powered-off direct
  peer reply (``/sgi/pci-expansion-ni/power-vector``).
- ``tests/qtest/sn1-pi-gfx-test.c``: graphics credits counted and
  returned across interfaces and nodes, map matching, wrap and the
  graphics interrupt.
- ``tests/qtest/sn1-directory-state-test.c``: directory transitions on
  watched pages, prefetch, flushes, premium words, and reset and
  migration of followed lines.
- ``tests/qtest/sn1-address-map-test.c``: the initial CPU-enable grant
  for two and four processors
  (``sn1/address-map/origin300-cpu-c-initial-enable``,
  ``sn1/address-map/origin300-four-cpu-initial-enable``).
- ``tests/qtest/sn1-fuel-test.c``: the L1 NMI reaching fitted
  processors (``fuel/l1-nmi-reaches-processors``).
- ``tests/functional/mips64/test_sn1_warm_reset.py``: a local reset
  restarts the processors under multi-threaded and round-robin TCG.
- ``tests/functional/mips64/test_sn1_presence.py``: the presence
  publication and partner waits under multi-threaded, round-robin and
  ``-icount`` execution.
