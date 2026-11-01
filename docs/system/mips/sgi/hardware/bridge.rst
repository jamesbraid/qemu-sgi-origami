.. SPDX-License-Identifier: BSD-3-Clause

.. _sgi-bridge:

=========================
Bridge: XIO-to-PCI bridge
=========================

Used in :ref:`sgi-ip27` (the Origin 200 IP29 system board, the Origin
2000 and Onyx2 BaseIO card, the GIGAchannel cabinet and the :term:`XIO` PCI
option boards) and :ref:`sgi-ip30` (the Octane and Octane2 system
board).

.. seealso::

   Platform pages: :ref:`sgi-ip27`, :ref:`sgi-ip30`.

.. seealso::

   - :term:`XBridge` (:doc:`xbridge`): the :term:`SN1` XIO-to-PCI chip.
   - :term:`PIC` (:doc:`pic`): the XIO-to-PCI-X bridge of the IP53 boards.
   - :term:`Crossbow` (:doc:`xbow`): the XIO crossbar that Bridge widgets sit behind
     where a module has one.

Bridge is SGI's XIO-to-PCI bridge. One side is an XIO widget with the
common widget register block at the bottom of 16 MB of widget space. The
other side is a PCI host bridge with eight device slots, a configuration
window, a programmed-I/O window per slot, an eight-pin interrupt
controller and an address translator for transfers a device initiates.
QEMU
models it as ``sgi-bridge`` in ``hw/pci-host/sgi-bridge.c``, with DMA
byte placement in ``hw/pci-host/sgi-dma-placement.c`` (shared with
:doc:`xbridge`) and the programmed-I/O forwarding hop in
``hw/misc/sgi-pio-forward.c``. The model follows the specification for
revision D silicon. The revision the identification register reports is a
machine property (:ref:`sgi-bridge-identity`).

Facts are rated as described in :ref:`sgi-ratings`. No public header
gives a Bridge reset value, so every reset value rated *established* here
rests on one source, the specification. Offsets and bit positions also
agree with the Linux and OpenBSD headers.

.. _sgi-bridge-sources:

Sources
=======

- SGI's *Bridge ASIC Specification*, revision 4.0 for revision D
  silicon, 26 April 1998, :src:`bridge-asic-spec-4.0`. The copy is a scan
  without a text layer. Pages given here are printed page numbers; the
  PDF page is the printed page plus 14. The document is marked
  proprietary and is cited, not reproduced. It is the only source for
  reset values, error capture rules, the byte-placement tables and the
  flash port.
- The *Origin and Onyx2 Theory of Operations Manual*,
  :src:`007-3439-002 p. 69`, Figure 2-25: the Origin 2000 BaseIO card's
  Bridge, its 32-bit PCI bus, a 1 MB flash EPROM on the Bridge and a
  one-wire identity part.
- The *Origin 200 and Origin 200 GIGAchannel Owner's Guide*,
  :src:`007-3708-002 p. 6`, Figure 1-4: the Origin 200's base Bridge, one
  PCI bus and the two SCSI controllers and :term:`IOC3` behind it.
- SGI's *IRIX Device Driver Programmer's Guide*,
  :src:`007-0911-090 p. 780`: the two DMA byte orders IRIX offers.
- The DS2505 datasheet, :src:`ds2505-datasheet`, for the board identity
  part.
- Linux ``arch/mips/include/asm/pci/bridge.h``,
  ``arch/mips/pci/pci-xtalk-bridge.c``,
  ``arch/mips/include/asm/mach-ip27/mangle-port.h`` and
  ``drivers/w1/masters/sgi_w1.c`` at ``db2ddb871435``, as software
  expectations: register layout, error groups, start-of-day programming,
  the one-wire master and the board-part table.
- OpenBSD ``sys/arch/sgi/xbow/xbridgereg.h``, ``xbridge.c``,
  ``widget.h`` and ``xbowdevs.h`` at ``2df91d80c104``, as software
  expectations: register layout, interrupt handling, byte-order
  programming and the identity.
- The IP27 :term:`PROM`, :src:`ip27-prom-6.156`, and the IRIX 6.5.30 kernel, read
  as shipped binaries, as software expectations.
- Published inventories: :src:`nekonomicon-16726704`,
  :src:`irixnet-post-32252`, :src:`usenet-95oo9r` and
  :src:`usenet-3a23907e`.
- Register and flash window reads taken on the reference Octane2
  (:ref:`sgi-bridge-measurements`).

.. _sgi-bridge-identity:

Identity and placement
======================

The identification register holds the revision in bits 31:28, the part
number in bits 27:12 and the manufacturer in bits 11:1, with bit 0 reading
one. Bridge is part ``0xc002``, manufacturer ``0x036``, *established*
(:src:`bridge-asic-spec-4.0 register tables`; OpenBSD ``xbowdevs.h``).
The model composes the word from its ``revision``, ``part`` and
``manufacturer`` properties at reset. The machines set the revision:

.. list-table::
   :header-rows: 1

   * - Where
     - Widget
     - Revision field
     - Basis
   * - Origin 200 IP29 system board
     - 8 on a direct :term:`Hub` link
     - 4
     - *observed*: a published Origin 200 GIGAchannel inventory prints
       ``ASIC BRIDGE Rev 4`` for the base I/O Bridge
       (:src:`nekonomicon-16726704`). SGI's upgrade guide prints
       ``Rev 3`` in its example listing (:src:`108-0187-001 p. 3-8`).
       Whether revision 3 IP29 boards shipped is *unknown*.
   * - Origin 2000 and Onyx2 BaseIO and BaseIO-G, MENET card,
       GIGAchannel cabinet, PCI-XTALK adapter
     - the board's XIO slot
     - 4
     - *approximation* for each board; see :doc:`baseio`,
       :doc:`gigachannel` and :doc:`pci-expansion`.
   * - MSCSI card, Origin 2000 PCI XIO cardcage
     - the board's XIO slot
     - 3
     - *observed* for the cardcage, ``ASIC BRIDGE Rev 3``
       (:src:`irixnet-post-32252`); see :doc:`mscsi` and
       :doc:`pci-expansion`.
   * - Bare ``sgi-pci-expansion`` board
     - the board's XIO slot
     - 1
     - *approximation*: the board leaves the property at its default
       unless it is given a ``bridge-revision`` (:doc:`pci-expansion`).
   * - Octane IP30 system board
     - 15
     - 3
     - *observed*: an OpenBSD boot of a 175 MHz Octane prints "Bridge
       revision 3" (:src:`miod-openbsd-sgi-story-4 lines 1308-1352`;
       :ref:`sgi-ip30-components`).
   * - Octane2 IP30 system board
     - 15
     - 4
     - *established*: the reference Octane2 reads ``0x4c00206d``
       (:ref:`sgi-bridge-measurements`). The Octane2 machine sets the
       property from the module's revision.

The model implements one behaviour, the specification's revision D, for
every revision value.

On an Origin 200 there is no Crossbow. The Hub's one XIO link ends at the
Bridge, so every widget number the Hub sends on its link reaches the same
chip. The model answers the whole 16 MB widget face at widget 0 and at
widgets 2 to 15; widget 1 stays the Hub's own registers. Guests read the
identification register at widget 0 first, so on this machine widget 0
reports the Bridge's part and manufacturer, not a Crossbow's. This follows
from the direct link, *inferred*; :doc:`hub` describes the Hub side.
Behind a Crossbow the Bridge answers only at the port it is cabled to
(:doc:`xbow`).

.. _sgi-bridge-address-map:

Widget address map
==================

Offsets are from the start of the Bridge's widget space, *established*
(:src:`bridge-asic-spec-4.0 p. 55-56`, Tables 43 and 44; Linux
``bridge.h``; OpenBSD ``xbridgereg.h``).

.. list-table::
   :header-rows: 1

   * - Offset
     - Size
     - Contents
     - Model
   * - ``0x000000``
     - 64 KB
     - Registers; the first ``0x300`` bytes are defined
     - :ref:`sgi-bridge-registers`
   * - ``0x010000``
     - 1 KB
     - Internal translation table, 128 eight-byte entries
     - :ref:`sgi-bridge-ate`
   * - ``0x011000``
     - 512 bytes
     - Read-only view of each entry's lower half
     - :ref:`sgi-bridge-ate`
   * - ``0x020000``
     - 32 KB
     - Type 0 configuration space, eight devices of 4 KB
     - :ref:`sgi-bridge-config`
   * - ``0x028000``
     - 4 KB
     - Type 1 configuration space
     - Not decoded
   * - ``0x030000``
     - 8 bytes
     - PCI interrupt acknowledge
     - Not decoded
   * - ``0x080000``
     - 512 KB
     - External SSRAM for an extended translation table
     - :ref:`sgi-bridge-flash`
   * - ``0x200000``, ``0x400000``
     - 2 MB each
     - Device 0 and device 1 windows
     - :ref:`sgi-bridge-pio`
   * - ``0x600000``-``0xbfffff``
     - 1 MB each
     - Device 2 to device 7 windows
     - :ref:`sgi-bridge-pio`
   * - ``0xc00000``
     - 4 MB
     - External flash, two 2 MB chip selects
     - :ref:`sgi-bridge-flash`
   * - ``0x40000000``
     - 1 GB
     - PCI memory ``0`` to ``0x3fffffff`` for 32-bit targets, outside the
       device windows
     - :ref:`sgi-bridge-pio`
   * - ``0x80000000``
     - 1 GB
     - The same PCI memory for 64-bit targets
     - Not decoded
   * - ``0x100000000``
     - 4 GB
     - PCI I/O space
     - Not decoded

The three ranges above the 16 MB face are *established* from
:src:`bridge-asic-spec-4.0 p. 52-53`. An access the model does not
decode, anywhere in the widget space outside the regions above, fails the
transaction, so a processor load takes a bus error. The reserved range
``0x100000``-``0x1fffff`` behaves the same way.

.. _sgi-bridge-registers:

Register map
============

Every register is 32 bits or narrower and sits in data bits 31:0 of an
eight-byte slot. The specification allows only an XIO doubleword packet
with four byte enables, which from a big-endian processor is a 32-bit
access at the slot's offset plus four, *established*
(:src:`bridge-asic-spec-4.0 register tables`). Linux ``bridge.h`` names
that plus-four address; OpenBSD ``xbridgereg.h`` names the slot and adds
four when it accesses it. The table gives slot offsets. A reset marked
*spec* is *established* from the specification; one marked *model* is the
model's choice where the specification gives none, an *approximation*.

.. list-table::
   :header-rows: 1

   * - Slot
     - Name
     - Width
     - Reset
     - Access
     - Notes
   * - ``0x000``
     - Identification
     - 32
     - per machine
     - read-only
     - :ref:`sgi-bridge-identity`
   * - ``0x008``
     - Status
     - 32
     - ``0x20`` or ``0x60``; spec
     - read-only
     - Bit 5 reads one for PCI mode. Bit 6 is the flash-select strap, the
       ``flash-select`` property. Retry counters and the
       outstanding-request count read zero. ``octane2`` reads ``0x60``
       and ``octane`` ``0x20``. The reference Octane2 reads ``0x62``
       (:ref:`sgi-bridge-octane2-status`).
   * - ``0x010``, ``0x018``
     - Error address upper, lower
     - 32
     - ``0``; model
     - read-only
     - Never written by the model. Hardware keeps the last logged
       address (:ref:`sgi-bridge-octane2-errors`).
   * - ``0x020``
     - Control
     - 32
     - ``0x7f0033ff``; spec
     - read/write
     - :ref:`sgi-bridge-control`
   * - ``0x028``
     - Request time-out
     - 32
     - ``0x000fffff``; spec
     - read/write
     - Bits 19:0 stored; nothing times out.
   * - ``0x030``, ``0x038``
     - Interrupt destination upper, lower
     - 32
     - ``0``; model
     - read/write
     - The Bridge's own error-interrupt destination
       (:ref:`sgi-bridge-interrupts`).
   * - ``0x040``
     - Error command word
     - 32
     - ``0``; model
     - read-only
     - Never written by the model.
   * - ``0x048``
     - Link configuration
     - 32
     - ``0x03ff1810``; spec
     - read/write
     - Maximum retry ``0x3ff``, null time-out ``0x06``, maximum burst
       ``0x010``. Bits 25:0 stored with no effect.
   * - ``0x050``
     - Target flush
     - 32
     - ``0``
     - read-only
     - A read completes once earlier programmed accesses have. The model
       completes accesses synchronously, so the read returns zero at once.
   * - ``0x058``
     - Auxiliary error command word
     - 32
     - ``0``; model
     - read-only
     - Never written by the model.
   * - ``0x060``, ``0x068``
     - Response buffer error address upper, lower
     - 32
     - ``0``; model
     - read-only
     - Read time-out capture (:ref:`sgi-bridge-errors`).
   * - ``0x070``
     - Test pin control
     - 32
     - ``0``; model
     - read/write
     - All 32 bits stored with no effect.
   * - ``0x080``
     - Direct map
     - 32
     - ``0x00020000``; spec
     - read/write
     - Widget in bits 23:20, remote-map mode bit 18, add-512 MB bit 17,
       XIO address bits 47:31 in bits 16:0 (:ref:`sgi-bridge-dma`).
   * - ``0x090``
     - SSRAM parity error
     - 32
     - ``0``; model
     - read-only
     - No SSRAM is fitted.
   * - ``0x0a0``
     - Arbitration
     - 32
     - ``0x00020008``; spec
     - read/write
     - Mask ``0x0003ff7f`` stored with no effect.
   * - ``0x0b0``
     - One-wire bus master
     - 32
     - ``0``
     - read/write
     - :ref:`sgi-bridge-onewire`
   * - ``0x0c0``
     - Bus time-out
     - 32
     - ``0x00010000``; spec
     - read/write
     - Retry hold in bits 20:16 (five bits), GIO time-out bit 12, PCI
       retry count bits 9:0. Stored with no effect.
   * - ``0x0c8``
     - PCI type 1 configuration
     - 32
     - ``0``; model
     - read/write
     - Bus number bits 23:16, device bits 15:11
       (:src:`bridge-asic-spec-4.0 p. 32`, Table 26). Stored; the type 1
       window is not decoded.
   * - ``0x0d0``, ``0x0d8``
     - PCI error address upper, lower
     - 32
     - ``0``; model
     - read-only
     - PCI error capture (:ref:`sgi-bridge-errors`).
   * - ``0x100``
     - Interrupt status
     - 32
     - ``0``; spec
     - read-only
     - Composed on each read (:ref:`sgi-bridge-interrupts`).
   * - ``0x108``
     - Interrupt enable
     - 32
     - ``0``; spec
     - read/write
     - Mask ``0x7fffffff``: bit 31 has no enable.
   * - ``0x110``
     - Reset interrupt status
     - 32
     - reads ``0``
     - write strobe
     - Seven group bits (:ref:`sgi-bridge-errors`).
   * - ``0x118``
     - Interrupt mode
     - 32
     - ``0``; spec
     - read/write
     - Bits 7:0, one per pin.
   * - ``0x120``
     - Interrupt device
     - 32
     - ``0``; spec
     - read/write
     - Three bits per pin, bits 23:0. Stored only.
   * - ``0x128``
     - Host error field
     - 32
     - ``0``; model
     - read/write
     - Bits 7:0, the interrupt number the Bridge sends for its own errors.
   * - ``0x130`` + 8 *n*
     - Interrupt host address, pins 0-7
     - 32
     - ``0``; spec
     - read/write
     - Bits 17:0 (:ref:`sgi-bridge-interrupts`).
   * - ``0x200`` + 8 *n*
     - Device(*n*), slots 0-7
     - 32
     - see below
     - read/write
     - Mask ``0x1fffffff`` (:ref:`sgi-bridge-device-registers`).
   * - ``0x240`` + 8 *n*
     - Device(*n*) write buffer flush
     - 32
     - ``0``
     - read-only
     - Returns zero at once.
   * - ``0x280``, ``0x288``
     - Even and odd read response buffers
     - 32
     - ``0``; spec
     - read/write
     - All 32 bits stored with no effect.
   * - ``0x290``
     - Read response buffer status
     - 32
     - ``0``
     - read-only
     - No buffer is ever in use.
   * - ``0x298``
     - Read response buffer clear
     - 32
     - reads ``0``
     - write strobe
     - Accepted and ignored.

Register offsets, field positions and the error bit numbering are
*established* by the specification and agree with Linux ``bridge.h`` and
OpenBSD ``xbridgereg.h``. A write to a read-only register is ignored and
logs ``LOG_GUEST_ERROR``. A write to a read/write register keeps only the
bits in its mask.

.. _sgi-bridge-access:

Access rules
------------

The register block takes only 32-bit accesses. A 1-, 2- or 8-byte access
anywhere in the 64 KB block fails the transaction before it reaches the
register file; QEMU's memory core logs it under ``-d invalid_mem``. This
follows the specification's packet rule, *established*. Which error the
chip records for such a packet is *unknown*, so the model records none.

A 32-bit access at an undefined address in the block is a different case.
The specification calls it an address invalid for the widget: the Bridge
does not return an error response, it sets interrupt status bit 24 and
reports the error by interrupt, *established*
(:src:`bridge-asic-spec-4.0 p. 34-36`, Tables 29 and 30). The model
treats a 32-bit access to an undefined slot, to the upper (even) half of a
defined slot, or to any offset from ``0x300`` to ``0xffff`` this way. The
access completes, a read returns ``0xffffffff``, bit 24 latches and, if
its enable bit is set, the error interrupt is sent
(:ref:`sgi-bridge-errors`). It logs ``LOG_UNIMP``.

One reserved address is an exception. A 32-bit store of zero to
``0x19c``, the data half of undefined slot ``0x198``, completes with no
effect and records no error. The IP27 PROM issues that store
(:src:`ip27-prom-6.156`), a *software expectation*. A non-zero store or a
read there fails the transaction and logs ``LOG_UNIMP``. What the chip
does at that address is *unknown*.

.. _sgi-bridge-control:

Control register
----------------

.. list-table::
   :header-rows: 1

   * - Bits
     - Field
     - Reset
     - Model
   * - 31
     - Flash write enable
     - 0
     - Gates flash writes (:ref:`sgi-bridge-flash`).
   * - 30, 29, 28
     - 50, 40 and 33 MHz clock output enables
     - 1, 1, 1
     - Stored.
   * - 27:24
     - PCI reset outputs
     - ``0xf``
     - Stored; no PCI device is reset by them.
   * - 23
     - I/O swap outside the device windows
     - 0
     - Stored; governs nothing, since PCI I/O space above the face is not
       decoded.
   * - 22
     - Memory swap outside the device windows
     - 0
     - Byte placement of the ``0x40000000`` range (:ref:`sgi-bridge-pio`).
   * - 21
     - 16 KB translation pages
     - 0
     - Selects the translation page size (:ref:`sgi-bridge-ate`).
   * - 20, 19, 18:17
     - SSRAM parity force, parity enable, SSRAM size
     - 0
     - Stored.
   * - 16
     - Force bad link packet
     - 0
     - Stored.
   * - 15:12
     - Crossbar credit
     - 3
     - Stored.
   * - 11, 10
     - Clear link retry counters
     - 0
     - Strobes; always read zero.
   * - 9
     - System big-endian
     - 1
     - Stored; the model always places bytes for a big-endian system.
   * - 8:4
     - Maximum outstanding transactions
     - ``0x1f``
     - Stored.
   * - 3:0
     - This widget's number
     - ``0xf``
     - Stored.

The fields and reset values are *established*
(:src:`bridge-asic-spec-4.0 p. 21-23`, Table 11). The whole register
resets to ``0x7f0033ff``. The widget number resets to ``0xf``, not to the
widget the Bridge answers at; firmware writes the real number. The model
keeps whatever is written and decodes the same addresses either way.

.. _sgi-bridge-device-registers:

Device registers
----------------

Each slot has one Device(*n*) register, *established*
(:src:`bridge-asic-spec-4.0 p. 41-43`, Table 36):

.. list-table::
   :header-rows: 1

   * - Bits
     - Field
     - Reset
     - Model
   * - 28
     - Error lock enable
     - 1
     - Stored.
   * - 27
     - Page-cross check enable
     - 1
     - Stored. Linux names the same bit a disable; the descriptions agree.
   * - 26, 25, 24, 23
     - Force parity, virtual device, write gather (translated, direct)
     - 0
     - Stored.
   * - 22
     - Device size: 64-bit device
     - 0
     - Selects programmed-I/O placement (:ref:`sgi-bridge-pio`).
   * - 21
     - Real time
     - 0
     - Stored.
   * - 20
     - Byte swap, translated DMA
     - 0
     - :ref:`sgi-bridge-dma-byte-order`
   * - 19
     - Byte swap, direct DMA
     - 0
     - :ref:`sgi-bridge-dma-byte-order`
   * - 18, 17, 16, 15, 14
     - Prefetch, precise, coherent, barrier, guaranteed bandwidth
     - 0
     - Stored.
   * - 13
     - Byte swap, programmed I/O
     - 0
     - :ref:`sgi-bridge-pio`
   * - 12
     - Memory space (one) or I/O space (zero)
     - 1
     - Selects which PCI space the slot's window reaches.
   * - 11:0
     - Window base, PCI address bits 31:20
     - none given
     - Aims the slot's window.

The specification gives no power-up value for the window base. The model
resets slots 0 and 1 to base 2\ *n* and slots 2 to 7 to *n* + 2, so the
default windows lie end to end from PCI address zero. The reset values
are ``0x18001000``, ``0x18001002`` and ``0x18001004`` to ``0x18001009``.
The base is an *approximation*. The reference Octane2 reads slots 4 to 7,
which hold no function, as ``0x18001006`` to ``0x18001009`` after its
firmware has run, the model's values, but whether the firmware wrote them
is not shown (:ref:`sgi-bridge-octane2-devices`). Bits 31:29 read zero
and are not writable.

.. _sgi-bridge-config:

PCI configuration space
=======================

Type 0 configuration space starts at ``0x020000``: device in address bits
14:12, function in bits 10:8, register in bits 7:0, *established*
(:src:`bridge-asic-spec-4.0 p. 55`, Table 43; Linux
``pci-xtalk-bridge.c``). The window takes 1-, 2- and 4-byte accesses; an
8-byte access fails the transaction.

On this big-endian machine a byte or halfword configuration access aims at
the address whose byte positions carry the register. Linux computes the
register offset exclusive-or (4 − size), and OpenBSD issues only 32-bit
configuration reads, *software expectation*. The model undoes the
exclusive-or before it addresses the PCI device, so a byte read of a
vendor ID returns the vendor's low byte. A 32-bit access needs no
correction.

A configuration cycle no device claims is a master time-out: the
specification sets interrupt status bit 11 and records the PCI address and
device number in the PCI error address registers, *established*
(:src:`bridge-asic-spec-4.0 p. 82-83`, Table 57). The model counts a
function as claimed when a device model sits at its number, so an empty
slot and a function a card does not have both time out. It records the
time-out as :ref:`sgi-bridge-errors` describes, with the cycle's PCI
address: the ID-select line, address bit 24 plus the device number
(:src:`bridge-asic-spec-4.0 p. 105`, Table 61), with the function and
register word below it.

What the processor sees when such a read fails is not specified. Every
guest reads empty slots inside a bus-error guard. The IP27 PROM treats a
fault as an absent device and, after a read that completes, also tests
status bit 11 (:src:`ip27-prom-6.156`). The IRIX 6.5.30 kernel clears the
master time-out enable around its probe and takes the bus error as the
answer. Linux reads every configuration word through ``get_dbe()``.
OpenBSD clears the interrupt enable register around the read and uses a
guarded read. These are each a *software expectation*. The PROM and OpenBSD
probe with the time-out enable clear, so a fault, not the status bit, is
what tells them a slot is empty. The model therefore fails an unclaimed
read back to the requester, which a processor takes as a bus error, and
returns all ones to a caller that sees the value without the failure. An
unclaimed write completes. Both are *inferred*.

Type 1 configuration space and the interrupt-acknowledge cycle are not
decoded: no board this model serves has a PCI-to-PCI bridge or a PCI
interrupt controller. Their register, ``0x0c8``, is storage.

.. _sgi-bridge-pio:

Device windows and programmed I/O
=================================

Each slot's window sits at a fixed widget offset: 2 MB windows for slots 0
and 1, 1 MB windows for slots 2 to 7. The Device(*n*) register aims it.
Bit 12 picks PCI memory or I/O space and bits 11:0 supply PCI address bits
31:20, *established* (:src:`bridge-asic-spec-4.0 p. 55-56`, Tables 43 and
44). For a 2 MB window, address bit 20 comes from both the base and the
window offset. The model adds them, as OpenBSD does, so a 2 MB window
based on an odd megabyte reaches into the next one, *inferred*. I/O
windows reach the full 32-bit PCI I/O space; the Bridge does not narrow it
to 64 KB.

The windows take accesses of 1 to 8 bytes. Placement follows the
specification's big-endian tables (:src:`bridge-asic-spec-4.0 p. 132-140`,
Appendix A, Tables 65 to 80). Below, *k* is the offset of a host byte in
its aligned unit and *o* the offset of a naturally aligned access of *w*
bytes:

.. list-table::
   :header-rows: 1

   * - Device(*n*) bit 22
     - Device(*n*) bit 13
     - Host byte *k* lands on PCI byte
     - Access at *o* reaches PCI offset
     - Tables
   * - clear, 32-bit device
     - clear
     - *k* XOR 3, within the aligned word
     - *o* XOR (4 − *w*)
     - 73 to 76
   * - clear, 32-bit device
     - set
     - *k*
     - *o*
     - 65 to 68
   * - set, 64-bit device
     - clear
     - 7 − *k*, within the aligned doubleword
     - *o* XOR (8 − *w*)
     - 77 to 80
   * - set, 64-bit device
     - set
     - *k*
     - *o*
     - 69 to 72

Every row is *established* for naturally aligned accesses. The
specification says the Bridge does no halfword or word swapping and that
the processor must align word and halfword transfers
(:src:`bridge-asic-spec-4.0 p. 132`). A doubleword to a 32-bit device is
two word cycles, the lower address first, carrying host bytes 0 to 3 and
then 4 to 7. With bit 13 clear on a 32-bit device, an access at the
exclusive-or address returns the register's numeric value. OpenBSD's
narrow accessors and Linux ``mangle-port.h`` compute *o* XOR (4 − *w*) in
software, and neither guest sets bit 13 or bit 22, *software
expectation*.

The model applies the table to every slot, whatever device sits there; it
reads no PCI identity. The device's own declared register byte order then
decides the value it sees.

The ``0x40000000`` range above the face reaches PCI memory from address
zero without a device window. The model places its bytes by the 32-bit
rows, with control bit 22 in place of Device(*n*) bit 13. That the range,
not a device register, selects the 32-bit rows is *inferred*: the
specification names this range the alias for 32-bit device targets and
gives the swap for memory outside the device windows to control bit 22
(:src:`bridge-asic-spec-4.0 p. 52-55`). Linux and OpenBSD clear control
bits 22 and 23 at start of day. The IRIX 6.5.30 kernel reaches PCI BARs
too large for a device window through this range. Each is a *software
expectation*.

A window cycle that no PCI target claims, because no enabled BAR of the
selected space covers the address, is a master time-out (status bit 11).
A cycle a device model claims and then fails is a PCI abort (status bit
15). Both are recorded with the cycle's PCI address and the slot number.
For the ``0x40000000`` range, which no slot selects, the model records
device 0, an *approximation*. A failed read fails back to the requester; a
write completes. If the first word cycle of a doubleword read fails, the
second is not issued. An address inside a BAR that the device model leaves
without a register region completes, reads zero and logs ``LOG_UNIMP``.
The real device decodes its whole BAR, and what it returns there is
*unknown*.

Window cycles reach the PCI address spaces through a forwarding hop that
remembers which region the last 4 KB page resolved to and forgets it on
every memory map change. It changes no result.

.. _sgi-bridge-dma:

DMA
===

A device sees a 32-bit PCI address space divided by its top two bits,
*established* (:src:`bridge-asic-spec-4.0 p. 57-58`, Figure 4 and section
4.3.1):

.. list-table::
   :header-rows: 1

   * - PCI address
     - Specification
     - Model
   * - ``0x00000000``-``0x3ffeffff``
     - Local: the Bridge does not respond.
     - Forwarded unchanged into the XIO fabric as widget 0.
   * - ``0x3fff0000``-``0x3fffffff``
     - The Bridge responds and flushes the initiating device's buffers.
     - Not implemented; treated as local.
   * - ``0x40000000``-``0x7fffffff``
     - Translated through the table.
     - :ref:`sgi-bridge-ate`
   * - ``0x80000000``-``0xffffffff``
     - Direct map.
     - Below.
   * - ``0x100000000`` and above
     - 64-bit address cycle.
     - Below.

Each slot has its own IOMMU region over one shared translation table,
because the swap bits that steer a transfer live in the requesting slot's
Device(*n*) register. The region translates one page at a time (4 KB, or
16 KB with control bit 21 set), so a transfer that crosses pages is
translated once per page. Transfers leave the Bridge as packets into the
XIO fabric, with the target widget in bits 51:48 above the 48-bit XIO
address. A Crossbow routes them by widget, and a Hub on a direct link takes
them all.

The direct window takes the target widget from direct map bits 23:20 and
XIO address bits 47:31 from bits 16:0, and passes PCI address bits 30:0
through, *established* (:src:`bridge-asic-spec-4.0 p. 61`, Table 47). When
the offset field is zero and bit 17 is set, the XIO address is 512 MB
higher, which steps over the processor's low compatibility segments; bit
17 resets set. Bit 18, which selects how the remote-map field is formed,
is stored with no effect, as are the per-transfer attributes in the
Device(*n*) register.

A 64-bit address cycle bypasses the offset registers. Bits 63:60 name the
target widget and bits 47:0 are the XIO address, *established*
(:src:`bridge-asic-spec-4.0 64-bit addressing`). The model keeps the node
number in the upper XIO address bits, so a transfer that names another
node reaches that node's memory. Attribute bits 59:48 are ignored. The
specification says widget 0 cannot be reached from 64-bit space; the model
logs a target of widget 0 as ``LOG_GUEST_ERROR`` and still sends it.

A device transfer that the fabric refuses is refused to the device. A read
a Crossbow drops is recorded as a read time-out
(:ref:`sgi-bridge-errors`). A dropped write is never reported, since the
Bridge requests no response for DMA writes, *inferred*.

.. _sgi-bridge-ate:

Translation table
-----------------

The table has 128 internal entries at ``0x010000``, *established*
(:src:`bridge-asic-spec-4.0 p. 65-66`, Tables 51 and 52):

.. list-table::
   :header-rows: 1

   * - Bits
     - Field
     - Model
   * - 63:48
     - Remote map
     - Stored; no effect.
   * - 47:12
     - XIO address bits 47:12 of the page
     - Used.
   * - 11:8
     - Target widget
     - Used.
   * - 4, 3, 2, 1
     - Barrier, prefetch, precise, coherent
     - Stored; no effect.
   * - 0
     - Valid
     - Used.

An entry is written whole, as a 64-bit store at its eight-byte offset. A
32-bit read at the entry's offset plus four returns its upper half. A
32-bit read in the second region, at ``0x011000`` plus four times the
entry number, returns its lower half. Any other access to either region,
including a 32-bit write, a 64-bit read and any write to the lower-half
view, fails the transaction and logs ``LOG_GUEST_ERROR``. Reset clears
whole entries, not only their valid bits.

A translated transfer indexes the table with PCI address bits 29:12, or
29:14 with 16 KB pages. An index of 128 or more needs the external table,
which no modelled board fits, so the transfer is refused and logs
``LOG_GUEST_ERROR``. So is a transfer through an entry whose valid bit is
clear. The model refuses IOMMU map notifiers, since nothing invalidates a
cached mapping when a guest rewrites an entry.

.. _sgi-bridge-dma-byte-order:

DMA byte order
--------------

Device(*n*) bit 20 controls DMA through the translated window and bit 19
controls the direct window and 64-bit cycles. Both reset clear, and local
space has no swapper, *established* (:src:`bridge-asic-spec-4.0 p. 41-43`,
Table 36). Table 36 applies the Appendix A placement to DMA through those
windows. Read that way, a set bit gives a byte stream, and a clear bit
reverses each aligned 32-bit word on a 32-bit data phase (Tables 73 and
76) and each aligned doubleword on a 64-bit data phase (Tables 77 and 80).

SGI's driver guide defines the two orders IRIX offers for DMA. *Byte
stream* keeps byte order. *Word values* puts a byte at offset *o* at *o*
XOR 3 and leaves 64-bit values with their 32-bit halves exchanged
(:src:`007-0911-090 p. 780`), with no condition on data-phase width. The
two documents agree for byte stream and for 32-bit data phases. For a
swap-clear 64-bit data phase they disagree, so the hardware placement
there is *unknown*. Which width each Origin 200 slot runs is also
*unknown*: the revision D anomaly list says the chip does not drive REQ64#
at reset (:src:`bridge-asic-spec-4.0 p. 161`), and no public board
document names the external driver.

The model takes the specification's width-dependent rule, an
*approximation*:

- The swap bit is chosen from the PCI address the device issued, before
  any translation: bit 20 below ``0x80000000``, bit 19 at and above it.
  Local transfers are never placed.
- Set: the transfer is a byte stream. Clear: each aligned 32-bit word is
  byte-reversed, with the words in place; in a 64-bit slot each aligned
  doubleword is reversed instead, and a shorter transfer lands mirrored
  within its doubleword.
- A slot is 64-bit when the board wires it to the 64-bit data path (the
  ``req64-slots`` property, one bit per slot) and every function in it
  implements ``INTERFACE_PCI_64BIT_DEVICE``. The Bridge settles this at
  the end of each reset and tells each card what its slot drove. This
  stands in for PCI's per-transaction REQ64# and ACK64# handshake. A
  32-bit function narrows a 64-bit function in the same slot. The Origin
  200 wires devices 5 to 7; every other board wires none. The Origin 200
  Owner's Guide says the option slots take 32-bit or 64-bit cards and
  gives their PCI IDs as 5, 6 and 7 (:src:`007-3415-003 pp. 7 and 12`), *established*. That
  those IDs are Bridge devices 5 to 7, and that the slots drive REQ64#,
  is *inferred*.
- The placement reads no PCI identity. A card whose bytes must arrive in
  another order models its own swap controls or converts at its own PCI
  face, as :doc:`ioc3` does.

Mainline Linux sets bit 19 on every slot at start of day and clears it for
the IOC3. OpenBSD sets bits 19 and 20 on every device except the IOC3,
:term:`IOC4` and RAD1 and confines DMA to the direct window. The IRIX 6.5.30
kernel sets a window's swap bit for a byte-stream mapping and clears it
for a word-values mapping. Each of the three is a *software expectation*.

.. _sgi-bridge-interrupts:

Interrupts
==========

The chip has eight interrupt pins. The model wires PCI INTA and INTC of
slot *n* to pin *n* and INTB and INTD to pin *n* XOR 4, which Linux and
OpenBSD both compute, *software expectation*. A board can also drive a pin
through the Bridge's eight GPIO inputs, as the IP29 board does for the
IOC3's second interrupt (:doc:`baseio`). PCI and GPIO levels are kept
separately and ORed per pin, so each source holds the pin on its own.

Interrupt status bits 7:0 report which pins are asserted, *established*
(:src:`bridge-asic-spec-4.0 p. 34-36`, Table 30). The model composes the
register on each read from the live pin levels and the latched error bits
31:8, so a pin bit follows its level and never latches. Guests depend on
this: OpenBSD re-reads the register at the end of each interrupt and, if
its pin is still set, raises the interrupt again at the Hub without
touching the Bridge, *software expectation*.

A pin raises an interrupt by an XIO write to the host. The per-pin host
address register holds a compressed destination: bits 17:8 replace XIO
address bits 47:38 of the widget interrupt destination, and bits 7:0 are
the interrupt number, *established* (:src:`bridge-asic-spec-4.0 p. 39-40`,
Tables 33 to 35; Linux ``pci-xtalk-bridge.c``; OpenBSD ``xbridge.c``).
The widget interrupt destination pair supplies the rest: the target widget
in upper bits 19:16, XIO address bits 47:32 in upper bits 15:0 and bits
31:0 in the lower register. The packet is a 64-bit big-endian store of the
interrupt number, with data bit 8 set to raise and clear to lower.

The model sends packets on transitions:

- A rising pin with its enable bit set sends a raise packet.
- Setting the enable bit of a pin that is already high sends a raise
  packet.
- A falling pin with its enable bit set sends a lower packet only if its
  interrupt mode bit is set (:src:`bridge-asic-spec-4.0 p. 39`);
  otherwise the guest clears the Hub bit itself.
- A pin whose host address register is zero sends nothing and logs
  ``LOG_GUEST_ERROR``.

The interrupt device register is stored and read back; guests
read-modify-write it and the mode register when they add a pin. The
specification uses it to order and flush the owning device's writes before
the interrupt. The model does not, since it completes writes
synchronously.

The Bridge's own errors are sent as a raise packet carrying the host-error
field's number, to the full address in the widget interrupt destination
pair. A destination address of zero sends nothing and logs
``LOG_GUEST_ERROR``. Mainline Linux and OpenBSD build the same per-pin
words, *software expectation*.

.. _sgi-bridge-errors:

Errors
======

Each error status bit belongs to one reset group, and a one in a group's
bit of the reset interrupt status register clears that group only,
*established* (:src:`bridge-asic-spec-4.0 p. 36-39`, Tables 31 and 32;
Linux ``BRIDGE_IRR_*_GRP``). The specification sets a status bit only
when its enable bit is set. The first error in a group keeps its capture
registers, and a later one sets bit 31, multiple errors, instead
(:src:`bridge-asic-spec-4.0 p. 77`). The model raises four errors:

.. list-table::
   :header-rows: 1

   * - Status bit
     - Error
     - Group (reset bit)
     - Model
   * - 24
     - Address invalid for this widget
     - Request dispatch (3)
     - :ref:`sgi-bridge-access`. Latches whatever its enable bit says;
       sends the error interrupt only when enabled. Captures no address
       or command and never sets bit 31.
   * - 11
     - PCI master time-out
     - PCI (0)
     - Configuration or window cycle no target claimed. Captures the
       device in PCI error upper bits 18:16, PCI address bits 47:32 in
       bits 15:0 and bits 31:0 in the lower register.
   * - 15
     - PCI abort
     - PCI (0)
     - Window cycle a device claimed and failed. Captured as bit 11.
   * - 9
     - XIO read request time-out
     - Response buffer (4)
     - A device read a Crossbow dropped. Captures the requesting device
       in response buffer error upper bits 22:20, PCI address bits 47:32
       in bits 15:0 and bits 31:0 in the lower register. The buffer
       field, bits 19:16, is zero.

Bits 11, 15 and 9 follow the specification's rules. With the enable bit
clear nothing is recorded. A second error in the group sets bit 31 and
keeps the first record. Each enabled error sends the error interrupt.
Reset bit 6 clears bit 31. Reset bits 1, 2 and 5 clear groups the model
never raises. That bit 24 latches without its enable is an
*approximation*.

Table 58 names the Bridge error address registers as where the read
time-out is recorded. The model uses the response buffer error registers,
whose fields are a device, a buffer and 48 address bits
(:src:`bridge-asic-spec-4.0 p. 83`, Tables 18, 19 and 58), *inferred*.
Which device number the chip records for a cycle it mastered itself is
*unknown*: the specification names the field the device number of the PCI
master (Table 27), and the Bridge is the master of such a cycle. The model
stores the addressed slot, an *approximation*. The reference Octane2 held a
PCI error record inside the IOC3's BAR with device field 0, but the event
that made it is not known (:ref:`sgi-bridge-octane2-errors`).

The error address registers at ``0x010`` and ``0x018``, the error command
words and the SSRAM parity register always read zero. On hardware the
error address registers keep the last logged value until the group is
cleared and enabled (:src:`bridge-asic-spec-4.0 p. 20`), and the
reference Octane2 read ``0x00cc1100`` at ``0x018`` with no error latched
(:ref:`sgi-bridge-octane2-errors`).

.. _sgi-bridge-flash:

External SSRAM and flash
========================

The external SSRAM port can hold an extended translation table. No
modelled board fits one. The model completes every access to the 512 KB
window, reads all ones and drops writes, so a write-and-read-back size
probe finds no memory, and the control register's SSRAM size field resets to
zero and has no effect. That an unfitted SSRAM port completes accesses is an
*approximation*.

The same port can carry a flash part instead: 16 data lines, two 2 MB
chip selects, a strap that chooses which select answers at ``0xc00000``
(status bit 6), reads of any XIO size, writes of 16 bits only, and no
write completed while control bit 31 is clear, *established*
(:src:`bridge-asic-spec-4.0 p. 115`, section 10.2.1, and p. 21-23, Table
11). The Origin 2000 BaseIO card wires a 1 MB flash EPROM to its Bridge
(:src:`007-3439-002 p. 69`), *established*.

What the model fits:

- The Origin 2000 and Onyx2 BaseIO cards fit an AMD-command-set part
  through QEMU's ``pflash_cfi02`` (:doc:`baseio`). The Octane and Octane2
  fit a 1 MiB Intel-command-set part through ``pflash_cfi01``
  (:ref:`sgi-ip30-flash-wiring`). The GIGAchannel cabinet fits the BaseIO
  card's part on select 0 when it is given an image, and answers its
  status word on select 1 (:doc:`gigachannel`). The Origin 200
  IP29 board fits nothing.
- The ``flash-select`` property is the strap. Clear puts select 0 at
  ``0xc00000`` and select 1 at ``0xe00000``; set swaps them.
- ``octane2`` fits its boot part on select 1 with the strap set, as the
  reference Octane2's status reads, so its Bridge status reads ``0x60``
  and the part still answers at ``0xc00000``
  (:ref:`sgi-bridge-octane2-status`). ``octane`` keeps the part on
  select 0 with the strap clear, status ``0x20``, an *approximation*:
  no Octane board has been read.
- A read of 1 to 8 bytes returns the part's bytes in address order, each
  16-bit word read as one chip cycle. A part smaller than 2 MB repeats
  across its select. The reference Octane2's 1 MiB boot select repeats
  this way at every sampled offset (:ref:`sgi-bridge-octane2-flash`),
  *inferred* for its whole range. How other boards wire the upper
  address lines is *unknown*, so for them the repeat is an
  *approximation*.
- A 16-bit write with control bit 31 set reaches the part. A write of any
  other width, or any write with bit 31 clear, does not reach it and logs
  ``LOG_GUEST_ERROR``. By default the write still completes. A board that
  sets ``flash-reject-invalid-writes``, as the Octanes do, fails it
  instead and also refuses a 16-bit write at an odd address. How the chip
  reports a refused write is *unknown*.
- A select with no part fails the transaction and logs
  ``LOG_GUEST_ERROR``, unless the board sets ``flash-empty-reads-high``.
  Then a read of the empty select completes with ``0xff`` in every byte
  lane and logs nothing, and a write still fails; the ``octane`` and
  ``octane2`` machines set it (*approximation* beyond the measured reads
  below). The reference Octane2's alternate select, chip
  select 0 on that board, returns all ones to 16- and 32-bit reads at
  four places across its 2 MB with no exception
  (:ref:`sgi-bridge-octane2-flash`). Whether a part is fitted there is
  *unknown*, so what an unpopulated select returns is *unknown* too.

The IP27 PROM loads the BASEIO PROM from the BaseIO card's flash, a
*software expectation* (:src:`ip27-prom-6.156`). It sets control bit 31,
identifies the part with the AMD autoselect sequence as 16-bit stores, and
reads the manufacturer and device words at offsets 0 and 2. It accepts AMD
``0x01``/``0x22da``, ST ``0x20``/``0xd7`` and Fujitsu ``0x04``/``0x22da``
as 16-bit parts, and several byte-wide parts. For any other identity it
falls back to the copy inside its own image, so a read-only image cannot
stand in for the part.

.. _sgi-bridge-onewire:

Board identity one-wire part
============================

The register at slot ``0x0b0`` is a one-wire bus master for the board's
identity part. Bit 0 is the level sampled from the wire, bit 1 says the
slot has finished, bits 9:2 are the sample offset and bits 19:10 the
low-drive time in microseconds; a write starts one bit slot,
*established* (:src:`bridge-asic-spec-4.0 register tables`; Linux
``sgi_w1.c``). The model completes the slot inside the write and sets bit
1. It decodes the slot from the low-drive time alone: 400 µs or more is a
reset, after which bit 0 reads zero when a part answers; less than 40 µs
writes a one or reads a bit; anything between writes a zero. These
thresholds are model choices. Linux resets with 520 µs, and OpenBSD's
master for the same SGI register layout on other chips with 500 µs,
*software expectation*. The sample offset is stored and unused.

Behind the master sits a DS2505, the 16 Kbit add-only EPROM, family code
``0x0b`` (:src:`ds2505-datasheet`), modelled in ``hw/1wire/ds2505.c``. It
answers Search ROM, Match ROM, Read ROM and Skip ROM, then Read Memory
(``0xf0``), Extended Read Memory (``0xa5``) and Read Status (``0xaa``).
Write commands and the overdrive ROM commands log ``LOG_UNIMP`` and leave
the wire high; an unknown ROM command logs ``LOG_GUEST_ERROR``. Its
identity is family ``0x0b``, the 48-bit ``onewire-serial`` property and
the Dallas CRC-8. Memory past the stored record reads ``0xff``, as
unprogrammed EPROM does, and every status byte reads ``0xff``.

Mainline Linux reads this part before it attaches the Bridge. It looks for
family ``0x0b``, reads 64 bytes as two 32-byte pages, checks each page's
stored CRC-16 against the residue ``0xb001``, and builds a part number from
bytes 11-29 and 32-37 with spaces dropped. If the read fails, the probe is
deferred and the PCI bus never appears. A valid record with an unknown
part number attaches with no board fixups. Each is a *software
expectation*.

When the machine gives a ``board-name``, the model stores a fixed-offset
record:

.. list-table::
   :header-rows: 1

   * - Bytes
     - Field
     - Contents
   * - 0
     - Format
     - ``0x01``
   * - 1-10
     - Barcode
     - ``board-barcode``, space-padded
   * - 11-29
     - Part, first part
     - Ten spaces, then the nine-character base number
   * - 30-31
     - Page 0 check
     - Inverted CRC-16 over bytes 0-29, low byte first
   * - 32-37
     - Part, dash number
     - Three spaces, then the three digits
   * - 38-41
     - Revision
     - ``board-revision``, space-padded
   * - 42-47
     - Group, capability, variety
     - ``0xff``
   * - 48-61
     - Name
     - ``board-name``, space-padded
   * - 62-63
     - Page 1 check
     - Inverted CRC-16 over bytes 32-61

The layout satisfies Linux's reader and OpenBSD's ``owserial`` checks
(format byte, printable serial and name, digits and dashes in the part,
``0xff`` in bytes 42-47), *software expectation*. That SGI's own Bridge
records use this layout is *inferred*. At realize the model refuses a
field that does not fit, a ``:`` or ``;``, a non-printable character, or a
part that is not twelve digits and dashes. Without a ``board-name``, only
the part number is stored, after ``;Part:`` in an older text layout, which
only a bare ``sgi-bridge`` or a board that names no record uses. A board
can add a second record to the same bus for a daughtercard.

The Origin 200 IP29 record is part ``030-1244-001``, revision ``H``, name
``IP29``: two published Origin 200 inventories report that part and
revision for the board (:src:`usenet-95oo9r`, :src:`usenet-3a23907e`),
*observed*, and one places the record at the Bridge's own widget. Other
IP29 part numbers exist (:src:`preterhuman-origin-200`), *observed*. The barcode is a
marked synthetic value, ``EMUN`` and an instance number. Linux maps the
``030-1244-`` prefix to the IP29 setup, which puts the IOC3's second
interrupt on Bridge pin 3, and OpenBSD reaches the same pin as the first
empty PCI device number, each a *software expectation*. Other boards' records are on their own pages.

.. _sgi-bridge-reset:

Reset
=====

A cold reset loads every register from the reset column above, composes
the identification and status words, clears the latched errors and every
translation entry, re-aims the device windows and clears the pin levels.
At the end of reset the Bridge settles each slot's DMA width
(:ref:`sgi-bridge-dma-byte-order`).

Revision D feeds a reset received on its XIO link into its core logic and
releases the core only once the link connection is determined,
*established* (:src:`bridge-asic-spec-4.0`). Which registers survive is
*unknown*. The model treats the whole core as lost, an *approximation*. A
rising ``link-reset`` input, driven by a Crossbow's per-link reset or a
Hub's I/O link warm reset, reloads the register file, clears the
translation entries and latched errors and re-aims the windows. The core
is released at once, not held until the link is determined, and the event
logs ``LOG_UNIMP``. Pin levels are kept, because the devices driving them
are not reset. Nothing reaches the PCI side: no source says the Bridge
pulses its PCI reset outputs, so cards keep their programming. The
Octane machines wire the Crossbow's widget 15 reset to the onboard
Bridge the same way XIO boards are wired. The path is *established* for
the Octane2's revision 4 (D) Bridge and *inferred* for the Octane's
revision 3.

The slots do not support hot plug; a card must be present when the machine
is built. A device past slot 7 is refused when it is added.

.. _sgi-bridge-migration:

Migration
=========

The ``sgi-bridge`` stream carries the register file, the three pin-level
words, the latched error bits and the 128 translation entries. Loading it
re-aims the device windows from the restored registers. The slot DMA
widths are settled again by the destination's own reset. The record of the
last DMA translation, meaningful only inside one access, is not carried.
The identity part migrates in its own ``ds2505`` stream: its memory image
and the position of any one-wire conversation. Migration works only
between identical builds.

.. _sgi-bridge-measurements:

Measurements
============

These reads come from the :ref:`sgi-measured-octane2`, IP30 board
revision B, whose Bridge at widget 15 reports revision 4. The
specification letters revisions from 1 as A, so 4 is D, the silicon it
describes (:src:`bridge-asic-spec-4.0 p. 19`, Table 7). Every read was
taken in the PROM :term:`Command Monitor` after the firmware had initialised the
machine, so none is a reset value. Each register read is one 32-bit load
at the slot offset plus four, physical ``0x1f000000`` plus the widget
offset.

.. list-table::
   :header-rows: 1

   * - Address
     - Width
     - Value
     - Register
     - Revision
     - When
   * - ``0x1f000004``
     - 32
     - ``0x4c00206d``
     - Identification
     - 4
     - PROM monitor, after initialisation
   * - ``0x1f00000c``
     - 32
     - ``0x00000062``
     - Status
     - 4
     - PROM monitor, after initialisation
   * - ``0x1f000014``, ``0x1f00001c``
     - 32
     - ``0``, ``0x00cc1100``
     - Error address upper, lower
     - 4
     - PROM monitor, after initialisation
   * - ``0x1f000024``
     - 32
     - ``0x7f2043ff``
     - Control
     - 4
     - PROM monitor, after initialisation
   * - ``0x1f00002c``
     - 32
     - ``0x000fffff``
     - Request time-out
     - 4
     - PROM monitor, after initialisation
   * - ``0x1f000034``, ``0x1f00003c``
     - 32
     - ``0x00080000``, ``0x00000080``
     - Interrupt destination upper, lower
     - 4
     - PROM monitor, after initialisation
   * - ``0x1f00004c``
     - 32
     - ``0x03ff1810``
     - Link configuration
     - 4
     - PROM monitor, after initialisation
   * - ``0x1f000084``
     - 32
     - ``0x00800000``
     - Direct map
     - 4
     - PROM monitor, after initialisation
   * - ``0x1f0000c4``
     - 32
     - ``0x00010000``
     - PCI/GIO time-out
     - 4
     - PROM monitor, after initialisation
   * - ``0x1f0000d4``, ``0x1f0000dc``
     - 32
     - ``0``, ``0x00520178``
     - PCI error address upper, lower
     - 4
     - PROM monitor, after initialisation
   * - ``0x1f000104``
     - 32
     - ``0``
     - Interrupt status
     - 4
     - PROM monitor, after initialisation
   * - ``0x1f00010c``
     - 32
     - ``0x7fcbffc0``
     - Interrupt enable
     - 4
     - PROM monitor, after initialisation
   * - ``0x1f00011c``, ``0x1f000124``
     - 32
     - ``0``, ``0``
     - Interrupt mode, interrupt device
     - 4
     - PROM monitor, after initialisation
   * - ``0x1f00012c``
     - 32
     - ``0x00000039``
     - Host error field
     - 4
     - PROM monitor, after initialisation
   * - ``0x1f000134``-``0x1f00015c``
     - 32
     - ``0`` each
     - Interrupt host address, pins 0-5
     - 4
     - PROM monitor, after initialisation
   * - ``0x1f000164``, ``0x1f00016c``
     - 32
     - ``0x00000029``, ``0x00000039``
     - Interrupt host address, pins 6 and 7
     - 4
     - PROM monitor, after initialisation
   * - ``0x1f000204``-``0x1f00023c``
     - 32
     - see below
     - Device(0) to Device(7)
     - 4
     - PROM monitor, after initialisation
   * - ``0x1fc00000``-``0x1fffffff``
     - 16 and 32
     - see :ref:`sgi-bridge-octane2-flash`
     - Flash window, both selects
     - 4
     - PROM monitor, after initialisation

Each read was a command of this form, one register at a time; the
monitor prints the word without leading zeros:

.. code-block:: text

   >> dump -w -x 0xffffffffbf000004#1
   0xffffffffbf000004:   4c00206d
   >> dump -w -x 0xffffffffbf00000c#1
   0xffffffffbf00000c:         62
   >> dump -w -x 0xffffffffbf000024#1
   0xffffffffbf000024:   7f2043ff

The interrupt status word read zero, so no pin was asserted and no error
was latched while these reads ran.

.. _sgi-bridge-octane2-status:

Identification, status and control
----------------------------------

The identification word decodes as revision 4, part ``0xc002`` and
manufacturer ``0x036``, *established* for this board's Bridge only;
other Octane2 boards may differ.

Status ``0x62`` decodes against the specification's status table
(:src:`bridge-asic-spec-4.0 p. 20`, Table 8): bit 6, the strapped flash
select, is 1, so chip select 1 answers at ``0xc00000``; bit 5 is 1, PCI
mode; the outstanding-read count in bits 4:0 is 2; both link retry
counters in bits 31:16 are zero. The strap value and PCI mode are
*established* for this board. The field read 2 at the moment of the
monitor's read. That it is a live count, not a fixed value, is *inferred*
from the monitor's read being itself an outstanding request; one sample
cannot separate the two.

The ``octane2`` model matches the strap and PCI mode: its status reads
``0x60`` before and after a machine reset, with chip select 1 at
``0xc00000``. The outstanding-read count reads zero there
(:ref:`sgi-bridge-limits`).

Control ``0x7f2043ff`` decodes against Linux ``bridge.h`` and the
specification's control table (:src:`bridge-asic-spec-4.0 p. 21-23`,
Table 11):

.. list-table::
   :header-rows: 1

   * - Bits
     - Value
     - Reset
     - Meaning
   * - 31
     - 0
     - 0
     - Flash writes disabled
   * - 30:28
     - ``0x7``
     - ``0x7``
     - 50, 40 and 33 MHz clock outputs on
   * - 27:24
     - ``0xf``
     - ``0xf``
     - PCI reset outputs
   * - 23, 22
     - 0, 0
     - 0, 0
     - No I/O or memory swap outside the device windows
   * - 21
     - 1
     - 0
     - 16 KB translation pages
   * - 20:19
     - 0
     - 0
     - SSRAM parity checking off
   * - 18:17
     - 0
     - 0
     - SSRAM size field 0
   * - 16
     - 0
     - 0
     - Force bad LLP packet off
   * - 15:12
     - 4
     - 3
     - Crossbar credit
   * - 11:10
     - 0
     - 0
     - Clear LLP counters not asserted
   * - 9
     - 1
     - 1
     - Big-endian system
   * - 8:4
     - ``0x1f``
     - ``0x1f``
     - Maximum outstanding transactions
   * - 3:0
     - ``0xf``
     - ``0xf``
     - Widget number 15

Bit 21 and the credit of 4 differ from the specification's reset values
and the other fields match them. That the firmware wrote bit 21 and the
credit is *inferred*: the read is of the state after firmware
initialisation. The SSRAM size field reads 0. With a
flash part on the port this says nothing about an SSRAM.

The direct map reads ``0x00800000``: widget 8, offset 0 and the
add-512 MB bit clear, the value both public drivers write and the model's
PROM was seen writing (:ref:`sgi-heart-dma`), *observed*. The request time-out
``0x000fffff``, link configuration ``0x03ff1810`` and PCI/GIO time-out
``0x00010000`` are the specification's reset values
(:src:`bridge-asic-spec-4.0 register tables`), so the firmware
either left them or wrote them back.

.. _sgi-bridge-octane2-devices:

Device registers
----------------

.. code-block:: text

   >> dump -w -x 0xffffffffbf000204#1
   0xffffffffbf000204:   100d1002
   >> dump -w -x 0xffffffffbf00020c#1
   0xffffffffbf00020c:   100d1004
   >> dump -w -x 0xffffffffbf000214#1
   0xffffffffbf000214:   10011005
   >> dump -w -x 0xffffffffbf00021c#1
   0xffffffffbf00021c:   10011006
   >> dump -w -x 0xffffffffbf000224#1
   0xffffffffbf000224:   18001006
   >> dump -w -x 0xffffffffbf00022c#1
   0xffffffffbf00022c:   18001007
   >> dump -w -x 0xffffffffbf000234#1
   0xffffffffbf000234:   18001008
   >> dump -w -x 0xffffffffbf00023c#1
   0xffffffffbf00023c:   18001009

.. list-table::
   :header-rows: 1

   * - Slot
     - Value
     - Function
     - Fields set
     - Window
   * - 0
     - ``0x100d1002``
     - QL1040B
     - error lock (28), direct-DMA byte swap (19), prefetch (18),
       coherent (16), memory space (12)
     - PCI memory ``0x00200000``
   * - 1
     - ``0x100d1004``
     - QL1040B
     - as slot 0
     - PCI memory ``0x00400000``
   * - 2
     - ``0x10011005``
     - IOC3
     - error lock, coherent, memory space
     - PCI memory ``0x00500000``
   * - 3
     - ``0x10011006``
     - RAD1
     - error lock, coherent, memory space
     - PCI memory ``0x00600000``
   * - 4 to 7
     - ``0x18001006`` to ``0x18001009``
     - none
     - error lock, page-cross check (27), memory space
     - PCI memory ``0x00600000`` to ``0x00900000``

Each base matches the BAR the function in that slot reads
(:ref:`sgi-qlogic-isp-measurements`, :ref:`sgi-ioc3-measurements`,
:ref:`sgi-rad1-measurements`). No slot has the 64-bit device bit (22),
the programmed-I/O swap bit (13) or the translated-DMA swap bit (20)
set, so the firmware runs every slot as a 32-bit device with swap-clear
programmed I/O. The page-cross check disable bit (27) is clear on the four
fitted slots, so the check is enabled there, and set on slots 4 to 7. Slots 4 to 7 read exactly the values the model resets them
to. Whether those are the chip's power-up values or values the firmware
wrote is not shown, so the window-base reset stays an *approximation*.

.. _sgi-bridge-octane2-interrupts:

Interrupt programming
---------------------

Pins 6 and 7 are enabled and every other pin is disabled
(enable bits 7:0 are ``0xc0``); that the firmware set this is *inferred*
from the read being taken after initialisation. Pin 6 sends interrupt number ``0x29``
(41) and pin 7 ``0x39`` (57). Bits 17:8 of both host address words are
zero, so each packet goes to the address the widget interrupt
destination pair names: widget 8, XIO address ``0x80``, HEART's
interrupt destination (:ref:`sgi-heart-interrupts`). The host error
field also holds ``0x39``, so the Bridge's own errors and pin 7 raise the
same HEART bit. OpenBSD names HEART bit 41 the power switch and bit 57
the widget 15 error (``xheartreg.h``); Linux names 57 the same way
(``heart.h``). Mode and device registers read zero: no pin sends a lower
packet when it falls. Error enables in bits 30:8 are all set except bits
18, 20 and 21, the link transmit-retry and receive check-bit and
sequence-number errors (Linux ``bridge.h`` names). Bit 31, the
multiple-error flag, has no enable and reads clear.

These are the register contents after firmware initialisation,
*established*; that the firmware programmed them is *inferred*. They are software state, not board wiring. What drives pins 6
and 7 is not shown by them. That pin 6 carries the timekeeper's
interrupt, which OpenBSD clears through the clock for the power button,
is *inferred* (:doc:`ds1685`).

.. _sgi-bridge-octane2-errors:

Error capture at idle
---------------------

Interrupt status read zero, yet the error address lower register held
``0x00cc1100`` and the PCI error lower register ``0x00520178``, with zero
upper halves. The specification says the error address registers hold
the last logged value until the group is cleared and enabled
(:src:`bridge-asic-spec-4.0 p. 20`, Table 9), which this agrees with;
the PCI error registers kept a value the same way. ``0x00cc1100`` lies in this Bridge's own flash window, offset
``0x0c1100`` into the boot select. ``0x00520178`` lies in the IOC3's
memory BAR at offset ``0x20178``, the serial port A register file. Which
events left these records is *unknown*. The PCI error upper word's device
field, bits 18:16, reads 0, not the IOC3's slot 2. If that record came
from a programmed-I/O cycle the Bridge mastered toward the IOC3, the chip
records 0 rather than the addressed slot for its own cycles, but the
event is not known, so the field's rule stays *unknown*
(:ref:`sgi-bridge-errors`).

.. _sgi-bridge-octane2-flash:

Flash selects
-------------

.. code-block:: text

   >> dump -w -x 0xffffffffbfc00000#4
   0xffffffffbfc00000:    bf000f0          0    bf000f8          0
   >> dump -w -x 0xffffffffbfd00000#4
   0xffffffffbfd00000:    bf000f0          0    bf000f8          0
   >> dump -w -x 0xffffffffbfcffff0#4
   0xffffffffbfcffff0:   ffffffff   ffffffff   ffffffff   ffffffff
   >> dump -w -x 0xffffffffbfdffff0#4
   0xffffffffbfdffff0:   ffffffff   ffffffff   ffffffff   ffffffff
   >> dump -w -x 0xffffffffbfc80000#4
   0xffffffffbfc80000:   dfb10008    3e00008   67bd0030   1000fff8
   >> dump -h -x 0xffffffffbfe00000#1
   0xffffffffbfe00000:   ffff
   >> dump -w -x 0xffffffffbfe80000#4
   0xffffffffbfe80000:   ffffffff   ffffffff   ffffffff   ffffffff
   >> dump -w -x 0xffffffffbff00000#4
   0xffffffffbff00000:   ffffffff   ffffffff   ffffffff   ffffffff

The ``0xffffffffbfe00000`` region was also read as ``dump -w -x
0xffffffffbfe00000#8`` and ``dump -w -x 0xffffffffbffffff0#4``, all
``0xffffffff``, *observed*. Every read completed without an exception.

.. list-table::
   :header-rows: 1

   * - Address
     - Width
     - Value
     - Region
   * - ``0x1fc00000``-``0x1fc0000f``
     - 32
     - ``0x0bf000f0``, ``0``, ``0x0bf000f8``, ``0``
     - Boot select (chip select 1), start
   * - ``0x1fd00000``-``0x1fd0000f``
     - 32
     - the same four words
     - Boot select, 1 MiB in
   * - ``0x1fcffff0``-``0x1fcfffff``
     - 32
     - ``0xffffffff`` each
     - Boot select, last 16 bytes of the first MiB
   * - ``0x1fdffff0``-``0x1fdfffff``
     - 32
     - ``0xffffffff`` each
     - Boot select, last 16 bytes
   * - ``0x1fc80000``-``0x1fc8000f``
     - 32
     - ``0xdfb10008``, ``0x03e00008``, ``0x67bd0030``, ``0x1000fff8``
     - Boot select, 512 KiB in
   * - ``0x1fe00000``
     - 16
     - ``0xffff``
     - Alternate select (chip select 0), start
   * - ``0x1fe00000``-``0x1fe0001f``, ``0x1fe80000``-``0x1fe8000f``,
       ``0x1ff00000``-``0x1ff0000f``, ``0x1ffffff0``-``0x1fffffff``
     - 32
     - ``0xffffffff`` each
     - Alternate select, four places across its 2 MB

With status bit 6 set, chip select 1 is the boot select at ``0xc00000``
and chip select 0 the alternate at ``0xe00000`` on this board,
*established*. The boot select returns the same words 1 MiB in as at its
start, and all ones at the top of both halves, *established* at those
offsets. That the select repeats every 1 MiB throughout, so the fitted
part decodes 1 MiB, is *inferred* from these samples. It matches the
specification's 512K by 16 target part (:src:`bridge-asic-spec-4.0 p.
115`), and it cannot separate a 1 MiB part from a 2 MiB part with its top
address line unconnected. The words at 512 KiB are inside the 682448-byte
primary image the same machine's ``flash -V`` reports
(:ref:`sgi-measured-octane2-prom`).

The alternate select reads all ones at every sampled offset and width
with no exception. Whether a part is fitted there, erased or absent, is
*unknown*, as is what a write there does.

.. _sgi-bridge-limits:

Model limits
============

- One behaviour for every revision; the identification revision is the
  only difference.
- Status reports the strap and PCI mode only. Its outstanding-request
  count and link retry counters always read zero, where the reference
  Octane2 read a count of 2 while serving the read. The model completes
  each read before the next access, so the count has nothing to report
  (:ref:`sgi-bridge-octane2-status`).
- The error address registers never record anything, where hardware
  keeps the last logged address.
- GIO mode, the external SSRAM table, the read response buffers,
  arbitration, prefetch, write gathering, the virtual-device split, the
  bus and request time-outs and the link layer are not modelled; their
  registers are storage.
- Type 1 configuration, the interrupt-acknowledge cycle, the 64-bit PCI
  memory alias at ``0x80000000`` and the PCI I/O range above the face are
  not decoded. An access there fails the transaction, so an IRIX mapping
  that asks for the 64-bit memory space or the I/O range fails rather than
  being misplaced. Control bit 23 governs nothing.
- The DMA flush window at ``0x3fff0000`` is not implemented, and local DMA
  below ``0x40000000`` is forwarded to widget 0 at the same address, where
  real hardware does not respond.
- Swap-clear DMA in a 64-bit slot uses whole-doubleword reversal, which no
  document settles for DMA. Slot width is settled per slot at reset, not
  per transaction or per function.
- QEMU refuses a memory-only DMA access unless it ends in RAM, so a
  memory-only transfer through a swap-clear slot fails. Byte-stream slots
  are unaffected. No card an SGI guest drives uses such transfers.
- Translation-table attributes, the remote map and the direct map's
  remote-map mode are ignored. A 64-bit DMA to widget 0 is sent rather
  than refused.
- The interrupt device register does not order or flush writes.
- Only the invalid-address, PCI master time-out, PCI abort and read
  time-out errors are raised. The invalid-address error latches without
  its enable and captures no address. Wrong-width register accesses record
  no error.
- An access at an undefined register slot completes and reads all ones,
  logging ``LOG_UNIMP``. Accesses of the wrong width, and undecoded widget
  ranges, fail the transaction. A non-zero store or a read at ``0x19c``
  fails and logs ``LOG_UNIMP``.
- A flash select with no part fails the transaction on the Origin
  boards. The Octanes' Bridge reads it as all ones, matching the
  reference Octane2, but the reads span only the sampled offsets and
  widths, and what a write does is *unknown*; the model fails writes.
- An inbound link reset releases the core at once.
- The one-wire part has no write or status-programming commands, and slot
  timing is decoded from fixed thresholds.
- No hot plug. Migration works only between identical builds.

.. _sgi-bridge-tests:

Tests
=====

``tests/qtest/sn0-bridge-test.c`` drives the Origin 200's Bridge and
standalone instances. It covers identity, the slot convention and the
slot's upper half, the ``0x19c`` store, reset values and each slot's
window, field widths, read-only registers and target flush, configuration
space layout and byte positions, window aiming and decode, the translation
table and its lower-half view, page-straddling and invalid-entry
transfers, the level-following status, the OR of wire and PCI sources,
raise and lower packets with and without their enable and mode bits, the
per-pin host and widget destination composition, the one-wire master and
part (reset, search, match, both memory reads, status, data past the
record, record validation), DMA placement on both windows with every swap
setting, mixed 32-bit and 64-bit functions, cross-node direct DMA, 64-bit
cycles, the 64-bit device window, inbound and Hub warm resets, and the
``0x40000000`` range.

``tests/qtest/sn0-bridge-invalid-addr-test.c`` covers the posted
invalid-address error, its latch, the per-group reset strobes, delivery
to the Hub, and the SSRAM window completing accesses.
``tests/qtest/sn0-bridge-master-timeout-test.c`` covers the configuration
master time-out: capture, the enable, multiple errors, delivery and the
bus error on a processor read. ``tests/qtest/sn0-pi-error-test.c`` covers
window master time-outs and a BAR hole that is not one.
``tests/qtest/sn0-bridge-flash-test.c`` covers the flash port: every read
width, select decode, the write enable, write widths, programming, the
strap, both selects and the Origin 200's empty window.
``tests/qtest/sn0-bridge-identity-test.c`` covers board record
validation. ``tests/qtest/sn0-widget-decode-test.c`` covers the Origin 200
Bridge answering at widget 0 and widgets 2 to 15, doubleword window
accesses split into two word cycles, and widget 1 staying the Hub.
``tests/qtest/sgi-bridge-bus-test.c`` drives a standalone Bridge with test
parts. ``tests/qtest/sn0-pio-forward-test.c`` checks that window accesses
follow a moved BAR. ``tests/qtest/octane-test.c`` covers the Octane flash
wiring and strap combinations and the status word each machine reads
across a reset (``/octane2/bridge-status-strap``).
``tests/qtest/sgi-ioc3-eth-test.c``
covers IOC3 DMA through both windows with both swap settings.
