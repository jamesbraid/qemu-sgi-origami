.. SPDX-License-Identifier: BSD-3-Clause

.. _sgi-tsb12lv22:

=======================================================
TI TSB12LV22 IEEE 1394 OHCI link controller (I-brick)
=======================================================

The TSB12LV22 is Texas Instruments' PCI-to-1394 link controller, an IEEE
1394 OpenHCI 1.00 part with the PCI identity ``104c:8009``. Published
IRIX inventories of the Onyx 3000 and Origin 3000 I-brick list it at PCI
slot 6 of the base I/O bus (:ref:`sgi-sn-family-ibrick`), *observed*. QEMU models revision 01 as
``hw/misc/tsb12lv22.c``, the ``tsb12lv22`` type, a standalone register model.
No SGI machine model instantiates it. It can be added to any PCI bus with
``-device tsb12lv22``. Facts are rated as described in :ref:`sgi-ratings`.

The model has no PHY, no 1394 bus, no configuration ROM, no DMA engine and
no cycle timer. Software can read and write the register file, take software
interrupts, and reset the part. Anything that would start the link, the PHY
or a DMA context is refused and logged (see :ref:`sgi-tsb12lv22-limits`).

.. _sgi-tsb12lv22-sources:

Sources
=======

- *TSB12LV22 OHCI-Lynx PCI-Based IEEE 1394 Host Controller*, Texas
  Instruments SLLS290, July 1998, :src:`ti-tsb12lv22-slls290`. It supplies
  the PCI configuration, the register map, access types and reset
  diagrams. Page numbers below are its printed pages, which equal the PDF
  pages for the technical part.
- Linux ``drivers/firewire/ohci.c`` at ``db2ddb871435``, as a software
  expectation. It matches device ID ``8009``, accesses registers as aligned
  32-bit little-endian words, and restores the GUID registers before it
  soft-resets the controller on resume.

The datasheet describes a link controller that needs a separate PHY. The
Behaviour that only the 1394 OHCI 1.00 specification would settle, and
that the datasheet does not state, is rated *unknown* or *approximation*.

Identity and placement
======================

.. list-table::
   :header-rows: 1
   :widths: 24 24 52

   * - Field
     - Model value
     - Basis
   * - Vendor, device
     - ``0x104c``, ``0x8009``
     - *Established* :src:`ti-tsb12lv22-slls290 p. 8`.
   * - Class and revision
     - ``0x0c001001``
     - *Established*: serial bus, IEEE 1394, OHCI programming interface
       ``0x10``, revision 01 :src:`ti-tsb12lv22-slls290 pp. 9-10`.
   * - Interrupt pin
     - INTA (``1``)
     - *Inferred* from the interrupt-pin table and the part's single INTA
       pin :src:`ti-tsb12lv22-slls290 pp. 5, 12`. The datasheet's reset
       diagram shows zero, an apparent editorial error.
   * - Status after reset
     - ``0x0210``
     - *Established*: medium DEVSEL and capability list present
       :src:`ti-tsb12lv22-slls290 p. 9`. Error bits 15-11 and 8 are
       write-one-clear.
   * - Command
     - writable ``0x0156``
     - *Established*: bits 8, 6, 4, 2 and 1 :src:`ti-tsb12lv22-slls290 p. 8`.
       Reset is zero.
   * - Cache line, latency timer, interrupt line
     - read/write bytes
     - *Established* :src:`ti-tsb12lv22-slls290 pp. 10, 12`. Latency timer
       is cleared by device reset.
   * - BAR 0, BAR 1
     - 2 KiB memory each
     - BAR 0 is the OHCI register block, *established* (sizing returns
       ``0xfffff800``) :src:`ti-tsb12lv22-slls290 p. 11`. BAR 1 is TI's
       extension block. Its 2 KiB size is *inferred* from the datasheet's
       cross-reference to BAR 0. Its contents are *unknown*.
   * - Capability pointer
     - ``0x44``
     - *Established*; one capability, power management, ID ``01``, next
       ``00`` :src:`ti-tsb12lv22-slls290 pp. 12-13`.
   * - PM capabilities (``0x46``)
     - ``0x4001``
     - *Inferred* from the reset diagram and per-bit descriptions: D0 and
       D3hot, PME from D3hot only. The summary table's ``01001`` is
       taken as an editorial error :src:`ti-tsb12lv22-slls290 pp. 13-14`.
   * - PM control and status (``0x48``)
     - writable ``0x0100``, write-one-clear ``0x8000``
     - Enable bit 8 and status bit 15 are *established*; write-one-clear
       on the status bit is *inferred* from the access tag
       :src:`ti-tsb12lv22-slls290 pp. 7, 14`. The power-state bits 1:0
       are not writable (see limits).
   * - MIN_GNT, MAX_LAT
     - ``0x02``, ``0x04``
     - *Approximation*. The prose gives 2 and 4 with no ROM, the reset
       diagram 2 and 2 :src:`ti-tsb12lv22-slls290 p. 12`. The model takes the
       prose. The bytes are read-only and survive reset.
   * - Link enhancements (``0xf4``)
     - writable ``0x86``
     - *Established*: bits 7, 2 and 1, reset zero
       :src:`ti-tsb12lv22-slls290 p. 15`. The bits have no effect.
   * - Subsystem identity (``0x2c``, ``0xf8``)
     - reset zero
     - *Established* that ``0xf8`` is a read/write alias of the read-only
       ``0x2c`` :src:`ti-tsb12lv22-slls290 pp. 11, 15-16`. A write to
       ``0xf8`` is mirrored into ``0x2c``. With no ROM the value is
       whatever software wrote, which the model keeps until device reset.
   * - GPIO control (``0xfc``)
     - reset ``0x1010``, writable ``0x3131b1b1``
     - The reset value is *established*
       :src:`ti-tsb12lv22-slls290 pp. 16-17`. The writable mask is the
       model's. The GPIOs drive nothing.

Config bytes outside the table read zero and ignore writes. PCI reset
clears Command, the BARs' decode and the other writable fields as for any
QEMU PCI device. With memory decode off the BARs do not respond.

.. _sgi-tsb12lv22-registers:

Register map
============

Offsets are relative to BAR 0. Access tags follow the datasheet: ``set``
and ``clr`` are a write-one-to-set and write-one-to-clear pair that share
one stored value, and writing zero to either has no effect
:src:`ti-tsb12lv22-slls290 p. 7`. A reader of the clear address of an event
register sees the pending bits ANDed with the mask. Every other read returns
the stored value, and both addresses of a pair read the same value, except
that interrupt event reads include the derived bits 7 and 6. The
writable column is the mask of bits software can change. Offsets and
access types are *established*
:src:`ti-tsb12lv22-slls290 table 20, pp. 17-19`. Where the datasheet leaves
reset bits unspecified (an ``X`` in its reset diagram) the model resets them to zero, an *approximation* that is not
a claim about silicon.

.. list-table::
   :header-rows: 1
   :widths: 10 20 12 14 44

   * - Offset
     - Name
     - Reset
     - Writable
     - Notes
   * - ``0x000``
     - Version
     - ``0x00010000``
     - none
     - Major 1, minor 0. The ROM-present bit 24 reads zero.
   * - ``0x004``
     - GUID ROM
     - ``0``
     - none
     - Reads zero. Any write logs ``LOG_UNIMP``. No ROM is present.
   * - ``0x008``
     - Async retries
     - ``0``
     - ``0x00000fff``
     - Stored, no effect.
   * - ``0x00c``, ``0x010``
     - CSR data, compare
     - ``0``
     - all
     - Stored, no effect.
   * - ``0x014``
     - CSR control
     - ``0x80000000``
     - none
     - The done bit stays set. Any write logs ``LOG_UNIMP``.
   * - ``0x018``
     - Config ROM header
     - ``0``
     - all
     - Stored, no ROM.
   * - ``0x01c``
     - Bus ID
     - ``0x31333934``
     - none
     - ASCII ``1394``.
   * - ``0x020``
     - Bus options
     - ``0x0000a002``
     - ``0xf8fff0c0``
     - Link speed 2 in bits 2:0, max_rec 10 in bits 15:12 at hard reset.
       Bit 27 resets zero.
   * - ``0x024``, ``0x028``
     - GUID high, low
     - ``0``
     - write once
     - See below.
   * - ``0x034``
     - Config ROM map
     - ``0``
     - ``0xfffffc00``
     - Stored.
   * - ``0x038``, ``0x03c``
     - Posted-write error
     - ``0``
     - none
     - Never updated.
   * - ``0x040``
     - Vendor ID
     - ``0``
     - none
     -
   * - ``0x050`` / ``0x054``
     - HC control set/clr
     - ``0``
     - ``0x40ce0000``
     - Bit 16 (software reset) is not stored: a write of it to the set
       address resets the controller. Bit 23 can only be cleared.
   * - ``0x064``
     - Self-ID buffer
     - ``0``
     - ``0xfffff800``
     - Stored. No self-ID packets arrive.
   * - ``0x068``
     - Self-ID count
     - ``0``
     - none
     - Never updated.
   * - ``0x070`` / ``0x074``, ``0x078`` / ``0x07c``
     - Isochronous receive channel mask high, low, set/clr
     - ``0``
     - all
     - Stored.
   * - ``0x080`` / ``0x084``
     - Interrupt event set/clr
     - ``0``
     - ``0x07fb033f``
     - Bits 26-19, 17-16, 9-8 and 5-0. Bits 7 and 6 are derived. Bit 30
       is not writable.
   * - ``0x088`` / ``0x08c``
     - Interrupt mask set/clr
     - ``0``
     - ``0x87fb03ff``
     - Bit 31 is master enable.
   * - ``0x090`` / ``0x094``, ``0x098`` / ``0x09c``
     - Isochronous transmit event, mask
     - ``0``
     - ``0xff``
     - Eight contexts.
   * - ``0x0a0`` / ``0x0a4``, ``0x0a8`` / ``0x0ac``
     - Isochronous receive event, mask
     - ``0``
     - ``0x0f``
     - Four contexts.
   * - ``0x0dc``
     - Fairness control
     - ``0``
     - none
     -
   * - ``0x0e0`` / ``0x0e4``
     - Link control set/clr
     - ``0``
     - ``0x00400600``
     - Bits 22, 10 and 9. Bits 21 and 20 (cycle master, cycle timer
       enable) log ``LOG_UNIMP`` and are not stored.
   * - ``0x0e8``
     - Node ID
     - ``0x0000ffc0``
     - ``0x0000ffc0``
     - Bus number all ones. No ID is ever assigned.
   * - ``0x0ec``
     - PHY control
     - ``0``
     - ``0x00000fff``
     - A write with bit 15 or 14 (read or write command) is dropped whole
       and logs ``LOG_UNIMP``. Read-done never sets.
   * - ``0x0f0``
     - Cycle timer
     - ``0``
     - all
     - Plain storage. The timer does not count.
   * - ``0x100`` / ``0x104``, ``0x108`` / ``0x10c``
     - Async request filter high, low, set/clr
     - ``0``
     - all
     - Stored.
   * - ``0x110`` / ``0x114``, ``0x118`` / ``0x11c``
     - Physical request filter high, low, set/clr
     - ``0``
     - all
     - Stored.
   * - ``0x120``
     - Physical upper bound
     - ``0``
     - none
     - Optional, not implemented.
   * - ``0x180``-``0x1ff``
     - Async contexts 0-3
     - ``0``
     - see below
     -
   * - ``0x200``-``0x27f``
     - Isochronous transmit contexts 0-7
     - ``0``
     - see below
     -
   * - ``0x400``-``0x47f``
     - Isochronous receive contexts 0-3
     - ``0``
     - see below
     -

An offset in the table's range that is not listed, such as ``0x2c``-``0x30``
or ``0x044``, reads zero and ignores writes, and logs ``LOG_UNIMP``.

GUID registers
--------------

Each of ``0x024`` and ``0x028`` accepts one write after a hard reset and then
ignores writes, including a first write of zero. This is *established* for
the no-ROM case :src:`ti-tsb12lv22-slls290 p. 23`. A hard reset clears both
values and both locks. A software reset keeps both, *inferred* from the
datasheet's statement that GUIDs are immutable once written and its
exception for reset behaviour specified elsewhere
:src:`ti-tsb12lv22-slls290 pp. 23, 25`. The Linux driver
restores missing GUIDs before it soft-resets the controller on resume,
which would be pointless if the reset cleared them.

Context registers
-----------------

The map is *established* :src:`ti-tsb12lv22-slls290 table 20, pp. 19, 35-39`
except where noted. Each context has a control set/clr pair at its base
and base + 4. Asynchronous contexts sit at ``0x180 + 0x20 n`` (n = 0-3).
Isochronous transmit contexts sit at ``0x200 + 0x10 n`` (n = 0-7).
Isochronous receive contexts sit at ``0x400 + 0x20 n`` (n = 0-3). The
command pointer is at base + ``0xc`` in every family, and a receive context
has a match register at base + ``0x10``. Other offsets, such as ``0x190``
inside a context or ``0x480`` past the last receive context, are
unsupported.

Control registers keep no run or active state. Asynchronous control bits
are not writable. Transmit control stores bits ``0xffff0000`` (cycle match
and its enable). Receive control stores bits ``0xf0000000``. A write with
bit 15 (run) or bit 12 (wake) set, at either address, logs ``LOG_UNIMP``.
Those bits are not stored. The write's other writable bits are. Neither
dead nor active ever sets. Command pointers store
all 32 bits. The receive match register stores ``0xf1ffff7f``.

The datasheet conflicts with itself on four points. The model's choice for
each is *inferred* from other statements in the same document:

- The receive map reads n = 0-4, but the event and mask diagrams and the
  receive table define only four contexts. The model has four.
- The first asynchronous command pointer is printed at ``0x18c`` in one
  place and ``0x19c`` in another. The model uses ``0x18c``, which fits the
  ``0x1ac``, ``0x1cc`` and ``0x1ec`` of the other three, and the table
  reserves ``0x190``-``0x19c``.
- The receive match register is printed at ``0x410`` and ``0x410c``. The
  model uses ``0x410 + 0x20 n``, since ``0x410c`` lies outside the 2 KiB
  aperture.
- The transmit event clear is printed at ``0x094`` and ``0x084``. The model
  uses ``0x094``, since ``0x084`` is the global event clear.

Access
======

Only aligned four-byte accesses reach the registers, in little-endian
order. This is the software interface the Linux driver uses. Behaviour of
the silicon for other widths is *unknown*. Any other width or alignment is
refused, logs ``LOG_UNIMP``, and does not change state. A wider access is
never split into several register accesses. A refused access reaches the
guest as the interconnect reports a failed transaction, so what the guest
sees depends on the bus above the card. An aligned four-byte access to
BAR 1 is not refused: it reads zero, ignores the write and logs
``LOG_UNIMP``. Other widths are refused as for BAR 0.

Interrupts
==========

An interrupt event register holds pending bits. Software can set a writable
bit through the set address, which is how the model raises an event, since
nothing in it raises one. Bits 7 and 6, the isochronous receive and
transmit summaries, are not stored. They read as one when the matching
context event register ANDed with its mask is non-zero, *established*
:src:`ti-tsb12lv22-slls290 p. 28`. Writes to them have no effect.

The INTA output is high while master enable (mask bit 31) is set and any
pending event, including a derived summary, is enabled in the mask. The
model recomputes it after every register write, every reset and every
migration load. A transmit or receive summary needs its own bit in the
interrupt mask as well as in the context mask.

The datasheet also says some events clear or block others, for example
bus reset sets one event and clears self-ID-complete, a fatal context error
sets unrecoverable-error and blocks context interrupts, and a cycle that runs
long clears cycle master. None of these has a source in the model, so none
is implemented.

Reset
=====

A device reset (PCI reset or system reset) zeroes the whole register file,
then sets the values in the table and clears the GUID locks. It also sets
the PCI fields to the values above.

Setting bit 16 at ``0x050`` runs a soft reset. It completes before the write
returns, so software never sees the bit set. It zeroes the register file as
a hardware reset does, keeps both GUID values and locks, and keeps max_rec
(bits 15:12 of bus options), *established* for max_rec
:src:`ti-tsb12lv22-slls290 p. 23`. It does not touch PCI configuration, and
INTA is recomputed. The datasheet gives no completion latency, so the
synchronous completion is an *approximation*.

Migration
=========

The device state is the PCI configuration, the register array and the two
GUID lock flags. Migration works only between identical builds. After load,
INTA is recomputed from the register state.

Measurements
============

No read of this part was taken on a physical machine. The datasheet is the
only source for the register contract.

.. _sgi-tsb12lv22-limits:

Model limits
============

- No PHY. PHY register reads and writes, bus reset, self-ID, node
  assignment and topology do not exist. Node ID is never valid.
- No DMA. Setting a context's run or wake bit starts nothing, and neither
  descriptors nor packet formats exist. The datasheet does not give them.
- No configuration ROM, and no SBP-2 target. The ROM-present bit stays
  zero. A ROM-equipped configuration is unsupported.
- No cycle timer, cycle master or cycle scheduling. Link control bits 21
  and 20 and the PHY command bits log ``LOG_UNIMP``.
- Power states other than D0 are unsupported. A write that requests D1, D2
  or D3hot leaves D0 and logs ``LOG_UNIMP`` at offset ``0x48``. No
  D3hot-to-D0 reset occurs and no PME is raised.
- The model adds no vendor-defined event bit 30, and the extension BAR has
  no registers. Any access to it logs ``LOG_UNIMP``.
- The reset zeros for unspecified bits, the MIN_GNT choice and the GPIO
  write mask are model choices, not hardware facts.
- Registers marked read/update in the datasheet (cycle timer, node ID, error
  address and self-ID size) are plain storage or fixed zero. No bus event
  updates them.
- Physical behaviour for subword, unaligned and wide accesses and for
  reserved addresses is *unknown*. Only the 1394 OHCI 1.00 specification
  would settle it.

Tests
=====

``tests/qtest/tsb12lv22-test.c`` runs when the device is built. It covers:

- ``tsb12lv22/pci``: identity, class, Status, interrupt pin, MIN_GNT,
  capability chain, BAR sizing, the Command mask, the ``0xf8``/``0x2c``
  alias, and the power-management control mask.
- ``tsb12lv22/pci-decode``: BAR relocation, and memory decode and
  bus-master control independently.
- ``tsb12lv22/access-width``: refused sizes and misaligned accesses, on a
  Malta machine because the SGI Bridge only passes 32-bit words.
- ``tsb12lv22/defaults``: reset values.
- ``tsb12lv22/masks``: writable masks, set/clear pairs, and the context
  register addresses including the ``0x19c`` and ``0x480`` non-aliases.
- ``tsb12lv22/interrupts``: pending bits, masks, master enable, the
  derived summaries and INTA at the Bridge.
- ``tsb12lv22/resets``: soft reset against hardware reset, GUID
  write-once behaviour and the max_rec exception.
- ``tsb12lv22/unsupported``: the refused PHY, link, CSR, context and BAR 1
  operations leave state alone and touch no guest memory.
- ``tsb12lv22/migration``: GUID locks, registers and INTA across an
  identical-build migration.
