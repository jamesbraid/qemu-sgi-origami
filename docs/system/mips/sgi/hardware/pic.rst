.. SPDX-License-Identifier: BSD-3-Clause

.. _sgi-pic:

=================
PIC: PCI-X bridge
=================

Used in :ref:`sgi-ip53` (the Origin 350, Onyx 350, Tezro and Onyx4 host
machines) and in the Origin 3000 IX- and PX-bricks (:ref:`sgi-bricks`),
which QEMU describes for reference only.

.. seealso::

   Platform pages: :ref:`sgi-ip53`.

.. seealso::

   - :term:`Bridge` (:doc:`bridge`) and :term:`XBridge` (:doc:`xbridge`): the earlier XIO-
     to-PCI bridges.

PIC is SGI's XIO-to-PCI-X bridge. One PIC is one :term:`XIO` widget
with two bridge heads, each with its own register block and its own PCI
bus. QEMU models it as ``sgi-pic`` in ``hw/pci-host/sgi-pic.c``, with the
two heads as internal ``sgi-pic-head`` children. The model reports PIC
revision 3, the revision a published Origin 350 boot reports. No PIC
specification is public, so the model is a compatibility profile: it
gives public drivers, the stock IP35 :term:`PROM` and IRIX 6.5.30 the registers
and transfers they use, and it is not a revision-matched description of
the chip. Facts are rated as described in :ref:`sgi-ratings`.

The machine-level choices for these products, such as which widgets carry
a PIC and which IO9 devices sit behind it, are on :ref:`sgi-ip53-pic` and
:ref:`sgi-ip53-serial-profiles`.

.. _sgi-pic-sources:

Sources
=======

- *SGI Origin 350 Server System User's Guide*,
  :src:`007-4566-001 p. 69-70`: the PIC on the PCI riser, four
  PCI/PCI-X slots as two buses of two slots, and the factory IO9 card in
  bus 1, slot 1.
- *Tezro Hardware User's Guide, Tower Configuration*,
  :src:`007-4564-001 p. 29, 38-39`: the Tezro slot layout.
- Miod Vallat's OpenBSD Origin 350 announcement, part 5 of
  :src:`miod-openbsd-sgi-story part 5`: an OpenBSD dmesg reporting
  ``PIC revision 3`` at widget 15 on a 1 GHz R16000 Origin 350, and the
  author's account of an interrupt address truncated to 32 bits.
- OpenBSD ``sys/arch/sgi/xbow/xbridgereg.h``, ``xbridge.c``,
  ``widget.h``, ``xbow.c``, ``xbow.h``, ``xbowdevs`` and
  ``sys/arch/sgi/sgi/ip27_machdep.c`` at ``2df91d80c104``, as software
  expectations: the register offsets, the second head at ``0x800000``,
  the identity codes, the direct DMA window, interrupt programming, the
  PCI configuration layout and the PIO byte-address rules.
- The out-of-tree Linux IP35 port's ``arch/mips/include/asm/sn/pic.h``,
  ``arch/mips/pci/pci-sgipic.c`` and ``arch/mips/sgi-ip35/ip35-xtalk.c``
  at ``linux-ip35:9f613a321ea0``, as software expectations: the register
  structure, direct-map programming, the 64-bit DMA address fields,
  interrupt destinations and the slot-to-device numbering.
- Linux/MIPS ``linux-2.4`` branch ``include/asm-ia64/sn/pci/pic.h``,
  ``include/asm-ia64/sn/pci/pcibr_private.h`` and
  ``arch/ia64/sn/io/sn2/pcibr/`` at ``8ef7d46c94d5``, as software
  expectations: the identity codes, the register access rule and the
  full-width store workaround.
- *XBridge ASIC Specification*, revision 0.5,
  :src:`xbridge-asic-spec-0.5 p. 71, 132`: bit 0 of the sibling chip's
  identity words.
- The stock IP35 PROM, :src:`ip35-prom-6.210`, run on the ``origin350``
  model. What it reads and writes is a software expectation observed on
  the model.

.. _sgi-pic-identity:

Identity and placement
======================

One PIC is one XIO widget. Its two heads share the widget: head 0's
registers start at widget offset 0 and head 1's at ``0x800000``, a
*software expectation* from OpenBSD ``xbridgereg.h`` and ``xbridge.c``
and the Linux IP35 port. The PIC of one 1 GHz Origin 350 is widget 15
at revision 3, *observed* in the one published dmesg
(:src:`miod-openbsd-sgi-story part 5`). Whether every Origin 350 places
its PIC at widget 15, and whether every IP53-family board carries
revision 3, is *unknown*; further published inventories or boot logs
would settle it.

The Origin 350 riser has four PCI/PCI-X slots on two buses, two slots
each, and IO9 sits in bus 1, slot 1 :src:`007-4566-001 p. 69`,
*established*. Which head drives bus 1 is *unknown*; the model puts IO9
behind head 0. The Tezro tower has four slots on two buses with one
processor, and eight slots on four buses with two or four
:src:`007-4564-001 p. 38-39`, *established*. That the four-bus tower
carries two PICs is *inferred* from two buses per PIC.

The model places the PIC wherever the machine profile says. The
``origin350``, ``onyx350`` and ``onyx4`` machines fit one PIC at
widget 15. The ``tezro`` machine fits one PIC at widget 15 on its
``tezro-rack-ip53`` and ``tezro-rack-ip59`` topologies and on the
one-processor board of ``tezro-one-processor``, and PICs at widgets 11
and 15 on the two/four-processor board of ``tezro-ip53`` and
``tezro-ip59``.
IO9 is behind head 0 of the widget-15 PIC on every Tezro board
(:ref:`sgi-ip53-serial-profiles`).

No register of a physical PIC has been read for this model.

.. _sgi-pic-faces:

Views of the widget
===================

The model presents each PIC through two address views of one 4 GiB
widget window:

- the *processor view*, which processor programmed I/O reaches through
  the :term:`Bedrock` and the crossbar. It is the device's system-bus region;
- the *packet view*, which XIO packets from other widgets reach.

Both views hold the same per-head state. In the two runtime profiles the
processor view adds three behaviours the packet view lacks: the 32-bit
word view (:ref:`sgi-pic-word-view`), the processor byte-lane rule for
PCI programmed I/O (:ref:`sgi-pic-pio-lanes`) and the link-error write
(:ref:`sgi-pic-error-interrupt`). These are each an *approximation*: whether the
Bedrock or PIC performs any of them, and how, is *unknown*.

.. list-table::
   :header-rows: 1
   :widths: 26 20 54

   * - Widget offset
     - Size
     - Contents
   * - ``0x000000``
     - 8 MiB
     - Head 0: registers, configuration windows, request RAM and short
       device windows (layout below).
   * - ``0x800000``
     - 8 MiB
     - Head 1, same layout.
   * - ``0x40000000``
     - 1 GiB
     - Head 0 PCI memory window. The offset is the PCI memory address.
   * - ``0xc0000000``
     - 1 GiB
     - Head 1 PCI memory window.

The two large windows follow OpenBSD's PIC address map, a *software
expectation*. OpenBSD marks PIC's direct PCI I/O window unsupported; the
model has none. PCI I/O space is reachable only through the short device
windows.

.. _sgi-pic-profiles:

Profiles
========

The ``compatibility-profile`` property selects one of three profiles:

``pic-rev3-bedrock-experimental``
  The runtime profile of the Origin 350, Onyx 350 and Onyx4 host.
``pic-rev3-bedrock-tezro-experimental``
  The Tezro runtime profile. Its PIC behaviour is identical to the first;
  only the name differs, so migration can tell the two apart.
``pic-rev3-bedrock``
  A synthetic register fixture for qtest, with ``test-fixture=on``. It
  refuses to realize outside qtest.

The rest of this page describes the runtime profiles unless it says
otherwise. The fixture keeps simpler masks, zero reset values, a
synthetic PCI function at device 0, and 32-bit direct DMA only.

.. _sgi-pic-registers:

Register map
============

Offsets are relative to a head; add ``0x800000`` for head 1. Every
register is a 64-bit slot. The offsets follow OpenBSD ``xbridgereg.h``,
``widget.h`` and the Linux IP35 ``pic.h`` structure, each a *software
expectation*. No public source gives PIC reset values; every reset value
below is a model choice, an *approximation*, unless the notes say
otherwise. "64" means aligned 64-bit access on both views; "+4" means the
processor view's 32-bit word view also reaches the slot.

.. sgi-registers: base=0x100000000000 width=64
   map=/machine/peripheral/fabric:1:0x100000000000
   skip=0x0:per-head-value,0x200:see-below,0x208:see-below,0x210:see-below,0x218:see-below
   qemu="-M none -cpu R10000 -S -device sgi-xio-test-target,id=fabric,size=65536,pic-peer=on,pic-runtime-profile=on,pic-linked=off,pic-bedrock-interrupts=off"

.. list-table::
   :header-rows: 1
   :widths: 12 18 10 18 12 30

   * - Offset
     - Name
     - Width
     - Reset
     - Access
     - Notes
   * - ``0x000``
     - Widget identity
     - 64, +4
     - ``0x3d102049``, head 1 ``0x3d112049``
     - read-only
     - See :ref:`sgi-pic-identity-word`. Writes are refused.
   * - ``0x008``
     - Widget status
     -
     -
     - unsupported
     - OpenBSD reads PCI-X mode and speed here. Reads return 0.
   * - ``0x020``
     - Widget control
     - 64, +4
     - ``0x0f00320f``
     - read/write
     - Write mask ``0x0f00f20f``. Bits 3:0 are the head's widget number.
   * - ``0x028``
     - Request timeout
     - 64, +4
     - ``0xfffff``
     - read/write
     - Stored, no timeout is modelled.
   * - ``0x030``
     - Interrupt destination, upper
     - 64, +4
     - ``0``
     - read/write
     - Used only by the link-error interrupt.
   * - ``0x038``
     - Interrupt destination, lower
     - 64, +4
     - ``0``
     - read/write
     - Used only by the link-error interrupt.
   * - ``0x050``
     - Widget flush
     - 64
     - ``0``
     - read-only
     - Reads return 0 at once. Writes and word reads are refused.
   * - ``0x078``
     - Link error address
     -
     -
     - unsupported
     - A 32-bit processor write at ``+0x7c`` raises the link error.
   * - ``0x080``
     - Direct map
     - 64, +4
     - ``0``
     - read/write
     - Write mask ``0xf1ffff``. See :ref:`sgi-pic-direct32`.
   * - ``0x0c8``
     - Type-1 configuration selector
     - 64, +4
     - ``0``
     - read/write
     - Bus in bits 23:16, device in bits 15:11.
   * - ``0x100``
     - Interrupt status
     - 64, +4
     - ``0``
     - read-only
     - Pins in bits 7:0, link error in bit 24.
   * - ``0x108``
     - Interrupt enable
     - 64, +4
     - ``0``
     - read/write
     - One bit per pin, bit 24 for the link error.
   * - ``0x110``
     - Interrupt clear
     - 64, +4
     - ``0``
     - write-one-to-clear
     - Reads return 0.
   * - ``0x118``
     - Interrupt mode
     - 64, +4
     - ``0``
     - read/write
     - A set bit sends a clear packet when that pin falls.
   * - ``0x120``
     - Device-to-pin map
     - 64, +4 read
     - ``0``
     - stored
     - Writes log ``LOG_UNIMP`` and have no effect.
   * - ``0x128``
     - Host error interrupt
     - 64, +4
     - ``0``
     - read/write
     - Number sent by the link-error interrupt.
   * - ``0x130``-``0x168``
     - Pin destinations 0-7
     - 64, +4
     - ``0``
     - read/write
     - Level in bits 63:48, address in bits 47:0.
   * - ``0x180``-``0x1b8``
     - Force always 0-7
     - 64, +4
     - none
     - write
     - Sends the pin's set packet. See :ref:`sgi-pic-force`.
   * - ``0x1c0``-``0x1f8``
     - Force with pin 0-7
     - 64, +4
     - none
     - write
     - Sends the set packet while the pin is high.
   * - ``0x200``-``0x218``
     - Device 0-3
     - 64, +4
     - see below
     - read/write
     - Write mask ``0x18583fff``. See :ref:`sgi-pic-device`.
   * - ``0x600``-``0x638``
     - PCI-X error and timeout
     -
     -
     - unsupported
     -
   * - ``0x10000``-``0x13fff``
     - Address translation entries
     -
     -
     - unsupported
     - 1024 entries and a parity-write view in the public header.
   * - ``0x18000``-``0x197ff``
     - Write-request RAM
     - 64, +4
     - ``0``
     - read/write
     - Three banks of 256 entries. Stored, no effect.
   * - ``0x20000``-``0x28fff``
     - Configuration windows
     - 8, 16, 32
     -
     -
     - See :ref:`sgi-pic-config`.
   * - ``0x30000``
     - Interrupt acknowledge
     -
     -
     - unsupported
     -
   * - ``0x200000``-``0x7fffff``
     - Short device windows
     - 8 to 64
     -
     -
     - See :ref:`sgi-pic-short-io`.

The register map is *software expectation* for offsets and field names
and *approximation* for everything the model does with them. In the
fixture profile the slots at ``0x28``, ``0x30`` and ``0x38`` are
unsupported, control keeps only bits 3:0 with reset 0, the Device
registers keep bits 22, 20, 19, 13, 12 and 11:0 with reset 0, and the
identity word has bit 0 clear.

.. _sgi-pic-identity-word:

Widget identity
---------------

The common widget identity word packs the revision in bits 31:28, the part
number in bits 27:12 and the manufacturer in bits 11:1 (OpenBSD
``widget.h`` lines 34-45; Linux ``arch/mips/include/asm/xtalk/xwidget.h``
at ``db2ddb871435``), a *software expectation*. The public PIC definitions
give manufacturer ``0x24`` and parts ``0xd102`` for head 0 and ``0xd112``
for head 1 (OpenBSD ``xbowdevs``; Linux/MIPS ``linux-2.4``
``include/asm-ia64/sn/pci/pic.h`` lines 98-105). The manufacturer field
therefore holds ``0x48``. The model reports revision 3.

Bit 0 is set in the runtime profiles and clear in the fixture, an
*approximation*. The XBridge specification hardwires bit 0 to one on
its own identity words :src:`xbridge-asic-spec-0.5 p. 71, 132`,
*established* for XBridge; no source gives the PIC bit. The stock IP35
PROM rejects the identity when bit 0 is clear, a *software expectation*
observed on the model. The physical revision-3 word is *unknown*: a raw
identity read from a real PIC would settle it.

OpenBSD's IP35 discovery reads the identity with a 32-bit load at head
base plus 4 (``ip27_machdep.c`` lines 589-632), a *software expectation*.
Both views answer the 64-bit read at offset 0 and the 32-bit read at
offset 4 with the same word on both heads. The head-1 read at
``0x800004`` is an extrapolation; OpenBSD reads only head 0.

.. _sgi-pic-control:

Widget control
--------------

Bits 3:0 hold the head's widget number. The model uses them as the source
widget on every DMA and interrupt packet the head sends. The runtime reset
value ``0x0f00320f`` makes that widget 15 on every PIC, including the
Tezro two/four-processor board's PIC attached at widget 11, until
software rewrites
the field. Other stored bits, in the
credit and endianness fields Linux ``xwidget.h`` names for the common
widget, have no modelled effect. Bits outside the mask are logged
(``LOG_UNIMP``) and dropped. The reset value and mask are chosen from the
stock PROM's bridge checks, an *approximation*.

.. _sgi-pic-device:

Device registers
----------------

Each head has four Device registers, one per internal slot. Internal slot
*n* is PCI configuration device *n* + 1 (Linux IP35 ``pic.h`` and
``pci-sgipic.c``), a *software expectation*. The model reads them as
follows:

.. list-table::
   :header-rows: 1
   :widths: 14 86

   * - Bits
     - Model use
   * - 11:0
     - Short-window base in 1 MiB units.
   * - 12
     - Short window targets PCI memory when set, PCI I/O when clear.
   * - 13
     - Programmed-I/O byte swap. A short-window access with it set is
       refused.
   * - 19
     - 32-bit direct DMA byte swap for this slot's requests.
   * - 20
     - Stored; no modelled effect.
   * - 22
     - Target size. A short-window access with it set is refused.
   * - 28:27
     - Stored; no modelled effect.

The runtime reset values are ``0x18001000``, ``0x18001002``,
``0x18001004`` and ``0x18001005`` for Device 0 to 3: PCI memory at bases
0, 2 MiB, 4 MiB and 5 MiB. These are chosen to satisfy the stock PROM, an
*approximation*.

.. _sgi-pic-word-view:

Processor word view
-------------------

The public drivers and the stock PROM access these registers
differently. OpenBSD's PIC
driver uses only aligned 64-bit register accesses, which it attributes to
hardware bugs, and the Linux IA64 PIC header calls a 32-bit access at
``+4`` unsupported. The stock IP35 PROM nevertheless reads and writes
32-bit words at slot plus 4: the timeout at ``+0x2c``, the destinations at
``+0x34`` and ``+0x3c``, and the host-error field at ``+0x12c``, a
*software expectation* observed on the model.

The runtime profiles answer a 32-bit access at slot plus 4 on the
processor view, an *approximation*. A read returns bits 31:0 of the
slot. A write replaces bits 31:0, keeps bits 63:32 and then applies the
slot's normal 64-bit write, including its mask and side effects. A word
write to the clear register passes only the written word, never the
stored upper half. The view reaches every slot marked "+4" in the table
and the write-request RAM. It refuses writes to identity, flush, status
and the device-to-pin map, and both directions at flush. A 32-bit access
at the aligned slot, byte or halfword control accesses and misaligned
accesses stay refused. The packet view never has this word view, except
the identity read at ``+4``. How the Bedrock turns a 32-bit processor
load into an XIO request is *unknown*.

.. _sgi-pic-buses:

PCI buses
=========

Each head drives one PCI bus. The products' slots are PCI-X capable
:src:`007-4566-001 p. 69`, *established*. The model's buses are
conventional PCI: no PCI-X mode, capability or split transaction is
modelled, and the widget status register that reports the bus mode is
unsupported. Each bus takes devices 1 to 4; device 0 is reserved for the
bridge itself, a *software expectation* from the Linux IP35 ``pic.h``.
The model presents no PCI function for PIC itself because no PIC PCI
vendor and device identity is known, *unknown*.

The Linux IA64 PIC header reserves device 0 and names four internal
slots. The Origin 350 has two external connectors per bus
:src:`007-4566-001 p. 69`. Internal slots are not connectors, so the
four internal slots do not show that there are more connectors,
*inferred*. On the IO9 bus the model uses internal slots 0, 2 and 3 for
the IO9's own devices (:ref:`sgi-pic-interrupts`).

.. _sgi-pic-config:

Configuration space
-------------------

Type-0 cycles use a ``0x1000``-byte window per device starting at
``0x20000``: the address is ``0x20000 + (device << 12) + (function << 8)
+ register``. Type-1 cycles use the window at ``0x28000``, with the bus
and device taken from the selector at ``0xc8``. Both layouts follow
OpenBSD ``xbridge.c``, a *software expectation*. Accesses of 8, 16 and 32
bits are accepted. The model forms the PCI register address by XORing the
low two bits of the offset with ``4 - width``, so a big-endian processor
sees each 32-bit configuration register in its natural numeric order.

A cycle that finds no device logs ``LOG_GUEST_ERROR``. A read then
returns all ones, as OpenBSD's guarded probe expects; a write is dropped.
What PIC's own error status records on a miss is *unknown*.

.. _sgi-pic-short-io:

Short device windows
====================

Each head has four short windows for programmed I/O to PCI devices,
following OpenBSD ``xbridgereg.h``, a *software expectation*:

.. list-table::
   :header-rows: 1

   * - Window
     - Head offset
     - Size
     - Device register
   * - 0
     - ``0x200000``
     - 2 MiB
     - ``0x200``
   * - 1
     - ``0x400000``
     - 2 MiB
     - ``0x208``
   * - 2
     - ``0x600000``
     - 1 MiB
     - ``0x210``
   * - 3
     - ``0x700000``
     - 1 MiB
     - ``0x218``

The window's Device register supplies the base. For PCI memory the model
uses base bits 9:0, PCI address bits 29:20; for PCI I/O it uses all
twelve. Windows 0 and 1 clear base bit 0 so the base is 2 MiB aligned.
The PCI address is the base plus the offset within the window. The
window index selects only the translation; PCI BAR decoding and each
device's Command register decide who answers.

A request that would cross the end of its window is refused before any
PCI cycle and logs ``LOG_GUEST_ERROR``. Device bit 13 or 22 set refuses
the request with ``LOG_UNIMP``; their physical effect is *unknown*. A
failed PCI cycle logs ``LOG_GUEST_ERROR``; a failed read returns zero.
These are each an *approximation*: PIC's real error response is *unknown*.

.. _sgi-pic-pio-lanes:

Processor byte lanes
--------------------

OpenBSD's PIC bus accessors place a byte at address XOR 3 and a halfword
at address XOR 2 for ordinary PCI devices, without swapping the value,
and use the address unchanged for :term:`IOC3`, :term:`IOC4` and RAD1
(``xbridge.c``), a *software expectation*. The stock IP35 PROM's 16-bit
writes to the IO9 SCSI controller arrive two bytes off their registers
unless the same correction applies, a *software expectation* observed on
the model.

In the runtime profiles the model applies this rule to aligned 1-, 2- and
4-byte processor accesses through the short windows and the large PCI
memory windows, an *inferred* placement at the processor interface:

.. list-table::
   :header-rows: 1

   * - Width
     - Ordinary device
     - Self-ordered device
   * - 1 byte
     - address XOR 3, value kept
     - address and bytes kept
   * - 2 bytes
     - address XOR 2, value kept
     - address and bytes kept
   * - 4 bytes
     - address kept, value kept
     - address and bytes kept

For an ordinary device the value is written to PCI as little-endian
bytes, so a 32-bit write of ``0x12345678`` produces PCI bytes ``78 56 34
12``. A self-ordered device receives the processor's byte order, ``12 34
56 78``. A device declares itself self-ordered with the read-only QOM
property ``byte-lane-self-ordered``; the IOC4 model does. The model
decides by finding which PCI function's BAR actually answers the address.
An aligned eight-byte processor access in the runtime profiles, through
a short window or a large PCI memory window, is two 32-bit cycles with the
more significant half at the lower PCI address, each word placed by the
four-byte rule above. A failed first cycle ends the access, and a failed
read returns zero. The half order is *inferred*: the vendor's published
driver guide and the shipped network drivers expect it, but no PIC
specification is held. One
little-endian eight-byte transfer would put the less significant half at
the lower address instead. The model gives a PCI function no 64-bit data
phase. Unaligned eight-byte accesses, the packet view and the fixture
profile use plain little-endian transfers with no address change. Which
chip performs the physical correction is *unknown*.

.. _sgi-pic-dma:

DMA
===

A PCI requester's DMA address selects one of two decoders. Requests from
device 0 are refused silently; requests from a device above 4 are refused
with ``LOG_GUEST_ERROR``. A refused translation returns an error to the
requesting device.

.. _sgi-pic-direct32:

32-bit direct DMA
-----------------

PCI addresses ``0x80000000`` to ``0xffffffff`` form a 2 GiB direct
window (OpenBSD ``xbridge.c``; Linux IP35 ``pci-sgipic.c``), a *software
expectation*. The direct-map register at ``0x80`` supplies the target:
bits 23:20 name the destination widget, and bits 16:0 become destination
address bits 47:31. The destination address is::

    (direct_map[16:0] << 31) | (pci_address & 0x7fffffff)

The Linux IP35 port programs the master widget shifted left by 20, a
*software expectation* that agrees. The runtime mask keeps only these two
fields; other bits are logged and cleared, an *approximation*.

The swap control for this window is the requesting slot's Device bit 19,
a per-device setting, not an address bit. With the bit clear, the model
moves bytes unchanged. With it set, each byte moves to address XOR 3,
reversing every 32-bit word, one write per word as for 64-bit direct
DMA below. The XBridge uses the
bit the other way round: set gives a byte stream and clear reverses
(:ref:`sgi-xbridge-dma-placement`). The model's sense is an
*approximation* carried from the original fixture; which sense PIC uses
is *unknown*. A guest whose 32-bit direct DMA depends on the bit would
settle it. PCI addresses below ``0x80000000``, including the translated
window at ``0x40000000``, log ``LOG_UNIMP`` and are refused.

.. _sgi-pic-direct64:

64-bit direct DMA
-----------------

In the runtime profiles a PCI address above 4 GiB is decoded as a 64-bit
direct address. The Linux IP35 port's producer places the target widget
in bits 63:60 and the attribute bits below it over the physical address
(``pci-sgipic.c``), a *software expectation*. The model reads the fields
as:

.. list-table::
   :header-rows: 1
   :widths: 14 30 56

   * - Bits
     - Field
     - Model behaviour
   * - 63:60
     - Destination widget
     - Selects the packet destination. Zero is refused.
   * - 59
     - Prefetch
     - Accepted. Moves only the requested bytes.
   * - 58
     - Precise
     - Accepted. Failures always reach the requester.
   * - 57
     - Virtual channel 1
     - Accepted. Same path as channel 0.
   * - 56
     - Barrier
     - Accepted. Transfers are already synchronous.
   * - 55
     - Swap
     - Set: bytes move unchanged. Clear: byte placement below.
   * - 54
     - Unknown on PIC
     - Refused.
   * - 53:48
     - Undocumented
     - Refused.
   * - 47:0
     - Physical address
     - Sent unchanged. Bit 47 set (Bedrock register space) is refused.

A refusal logs ``LOG_GUEST_ERROR`` with the head, requester device, full
address and reason, and the transfer fails. A transfer that would run
past bit 47 is refused. The direct-map register and Device bit 19 do not
affect 64-bit direct DMA.

With bit 55 clear the model maps each PCI byte address *a* to memory
address *a* XOR 3, so 32-bit numeric values keep their meaning between a
little-endian device and big-endian memory. For a device whose slot
drives ``REQ64#`` and which accepts it at PCI reset, the model uses *a*
XOR 7 instead, reversing each 64-bit doubleword. The board sets the
``REQ64#`` slots through the ``req64-slots-head0`` and
``req64-slots-head1`` properties; the IO9 profiles set it for the
BCM5701 at device 4. The model samples each device's width at the end of
reset.

The model receives a device's transfer as naturally aligned pieces of 1,
2, 4 or 8 bytes, and writes each piece to memory once per data phase it
touches: an aligned 4-byte word, or an 8-byte doubleword for a
``REQ64#`` slot. No aligned field of up to 8 bytes is split between two
writes, and a burst as a whole is not atomic. A partial data phase of 3,
5, 6 or 7 bytes at a ragged end is written as two or three pieces, where
the hardware is expected to write only its enabled bytes in one access,
an *approximation*. Transfers with bit 55 set are cut into the same
pieces.

These placements are each an *approximation*. They rest on two software
expectations: the Linux IP35 port's producer, which names bit 55 the
swap attribute (``pci-sgipic.c``), and the IRIX 6.5.30 BCM5701 driver,
which writes big-endian descriptors with the chip's own swap bits
clear. The XBridge uses bit 55 the same way, a byte stream when set and
a byte exchange within each 32-bit or 64-bit data phase when clear, with
no halfword or word swap (:ref:`sgi-xbridge-dma-placement`), and its
DMA write packets carry an enable for each byte, so a ragged end writes
only the enabled bytes. No PIC specification is held, so that data
path and the one-write-per-phase rule are *inferred* for PIC from its
sibling. PIC's physical byte placement, the IO9 ``REQ64#`` wiring and
the real effect of prefetch, precise, barrier and virtual channel are
*unknown*.

.. _sgi-pic-interrupts:

Interrupts
==========

Each head has eight interrupt inputs. The board wires PCI INTx lines to
them: output ``pci-intx`` line ``4 * device + pin`` (INTA is pin 0) goes
to one input of the head. An asserted line with no board wiring logs
``LOG_UNIMP``. The Origin 350, Onyx 350 and Onyx4 host profiles connect
INTA of IOC4 at device 1 to input 0, the ISP12160 at device 3 to input 2
and the BCM5701 at device 4 to input 3. That follows the Linux IP35
port's rule that internal slot *n*, configuration device *n* + 1, uses
input *n*, a *software expectation*. The physical IO9 wiring is
*unknown*.

A rising input sets its status bit. If the input is enabled, the head
sends a set packet to the input's destination register. If the input's
mode bit is set, a falling input sends a clear packet; OpenBSD enables
the clear packet this way, a *software expectation*. Writing the clear
register clears the written status bits, then sets again the bit of
every input still high, and sends a new set packet for each written bit
whose input is high and enabled. Enabling an input that is already high
and pending sends one set packet. Changing mode or destination sends
nothing. OpenBSD clears PIC error bits by writing them to ``0x110``, a
*software expectation* that agrees.

A destination register holds the Bedrock interrupt level in bits 63:48
and the target address in bits 47:0, following the Linux IP35 port, a
*software expectation*. The head writes an eight-byte big-endian packet
to that address: ``0x100 | level`` to set, ``level`` to clear. Levels
outside 7 to 127 are refused with ``LOG_GUEST_ERROR`` before any packet
is sent. The Linux IP35 port allocates levels above 6, a *software
expectation*. Bit 6 of the value selects the Bedrock pending bank and
bit 8 sets or clears, *established* on :ref:`sgi-bedrock-pi-interrupts`.
A destination the Bedrock
refuses also logs ``LOG_GUEST_ERROR``. Every packet logs ``LOG_UNIMP``
with its destination. The packet format and address use are each an *approximation*:
PIC's physical interrupt packet is *unknown*.

An OpenBSD account of Origin 350 bring-up reports that interrupts stopped
when the destination was truncated to 32 bits and resumed when all 64
bits were written (:src:`miod-openbsd-sgi-story part 5`), *observed*. The
model keeps all 64 bits.

.. _sgi-pic-force:

Force registers
---------------

Each input *n* has two write-only force registers, force-always at
``0x180 + 8n`` and force-with-pin at ``0x1c0 + 8n``, named in the Linux
IA64 PIC header (``pic.h``), a *software expectation*. A write ignores
its data and sends input *n*'s set packet to its destination without a
new edge: force-always whatever the input's level, force-with-pin only
while the input is high. Neither changes the status register. Both views
accept the write: a 64-bit write at the register and, in the runtime
profiles, a 32-bit processor write at ``+4``. Reads return zero, log
``LOG_UNIMP`` and send nothing.

The Linux SN2 PIC driver writes force-with-pin at the end of each
interrupt dispatch (``pcibr_intr.c``), a *software expectation*, so an
input still high after its handler runs is serviced again. The register
behaviour is *inferred* from the XBridge, whose force registers sit at
the same offsets (:ref:`sgi-xbridge-interrupts`); no PIC specification
is held. Whether a force reaches a disabled input is *unknown*. The
model sends nothing and logs ``LOG_UNIMP``, an *approximation*, as the
XBridge model does. A delivered force updates the input's Bedrock level
source the same way a rising edge does (:ref:`sgi-pic-reassert`).

.. _sgi-pic-reassert:

Held inputs
-----------

A PCI device keeps its line high until serviced, but a PIC sends one set
packet per rising edge. If the processor clears the Bedrock pending bit
while the line is still high, no new edge arrives. The model therefore
registers each delivered, enabled and still-high input as a level source
with the Bedrock processor interface chosen by destination bit 21. When
the processor clears that pending bit, the Bedrock sets it again 1 ms of
virtual time later, while the source stays active. Lowering, disabling
or re-targeting the input removes the source. This is an *approximation*:
the 1 ms delay is a model choice placed after the IRIX 6.5.30 kernel's
rapid clear sequence, not a measured latency, and what revision-3 PIC
does when a cleared input stays high is *unknown*. A destination
rewrite while an input stays high loses the source until the next edge
or force; the public drivers program destinations before enabling inputs.

The drivers' own way to service a held input is the force-with-pin write
at the end of each dispatch (:ref:`sgi-pic-force`). The model keeps both
mechanisms; the 1 ms re-set has no hardware source.

.. _sgi-pic-error-interrupt:

Link-error interrupt
--------------------

The Linux IA64 PIC header marks the link-side error address at ``0x78``
read-only. The stock IP35 PROM enables status bit 24, which OpenBSD
names the invalid-address error, then writes the 32-bit word at
``+0x7c``, a *software expectation* observed on the model.

In the runtime profiles a 32-bit processor write at ``+0x7c`` stores
nothing and sets status bit 24. If bit 24 is enabled, every such write
sends one set packet, even while the status bit stays set. Enabling bit
24 later does not replay an earlier error; writing bit 24 to ``0x110``
clears it. The packet carries ``0x100 | n``, where *n* is the host-error
field at ``0x128``; values above 127 are refused and logged. The
destination comes from the widget destination pair, upper at ``0x30``
and lower at ``0x38``:

- If the lower word uses bits 47:32, the address is the lower word, when
  the upper word's bits 15:0 are zero or equal to those bits.
- If the lower word uses only bits 31:0, the address is the upper word's
  bits 15:0 above the lower 32 bits.
- Conflicting high fields, lower-word bits above 47, upper-word bits
  above 31 and a zero address are refused with ``LOG_GUEST_ERROR``. The
  status bit stays set.

All of this is an *approximation* chosen for the stock PROM. Upper-word
bits 31:16 are stored and ignored. OpenBSD puts the target widget in
lower-word bits 63:48; the model refuses that form. Every other access at
``0x78`` and ``+0x7c`` is unsupported.

.. _sgi-pic-pxbow:

Widget 0
========

The IP53 board has no :term:`Crossbow`, but software expects one at widget 0.
Public Linux headers name the IP53 crossbar PXBOW, with manufacturer 0
and part ``0xd100``, a *software expectation*. In the runtime profiles
the PIC owns an ``sgi-xbow`` child that answers as widget 0 with that
identity and revision 5, connects host widget 10 to the PIC's widget,
and carries processor I/O to the PIC (:doc:`xbow`). Reusing the Crossbow
model is an *approximation*: PXBOW's register set, revision and routing
are *unknown*. The ``pxbow-revision``, ``pxbow-host-widget`` and
``pxbow-pic-widget`` properties set it. Every runtime PIC builds this
child, but a machine maps only one as widget 0: when a machine has two
PICs, as the Tezro two/four-processor board does, the PXBOW of the PIC
that carries IO9 is widget 0.

The stock IP35 PROM's crossbar check stores to the two read-only error
address registers and expects each store to complete with the
register-access error set, a *software expectation*. The PXBOW child
completes such a store and records the access error
(``complete-posted-error-address-writes``). The XBridge's built-in
crossbar does the same: it completes every store and latches the access
error for one it does not decode. Only the Crossbow of the Origin 2000
and Octane refuses the store. The evidence for completion on the IP53
crossbar is the model's own note that a real deskside Tezro passes the
PROM check; no public capture of that check is cited, so limiting
completion to these crossbars is an *approximation*, and whether those
addresses are registers is *unknown*.

.. _sgi-pic-reset:

Reset
=====

No public source gives a PIC reset table, and the drivers program
interrupt enables, device maps, the direct map and request RAM
themselves after firmware. On reset each head clears every register, the
request RAM, its input state and its held-input sources, then loads the
identity word and, in the runtime profiles, the control, timeout and
Device values in the register table. The unsupported-access log limit
also resets. Each device's ``REQ64#`` width is sampled at the end of
reset. All reset values are each an *approximation*; warm-reset scope and
whether request RAM survives reset are *unknown*.

.. _sgi-pic-migration:

Migration
=========

Each head migrates its registers, request RAM, input levels, packet
counters and its unsupported-access log count. The PIC migrates no
mutable state of its own, but records its transport, interrupt mode,
profile, PXBOW composition, processor-lane policy, 64-bit DMA policy and
``REQ64#`` slots, and refuses a stream
whose values differ from the destination. Identity reads are computed
from the profile, so a stream cannot change them. The device loads
PIC stream versions 1 to 9. An older stream lacks the newer policy
fields, which load as their earlier defaults, and a destination whose
policy differs refuses it. Only migration between identical builds is
supported.

.. _sgi-pic-limits:

Model limits
============

- No PCI-X bus mode, split transactions or PCI-X error registers.
  Accesses to ``0x600``-``0x638`` log ``LOG_UNIMP``.
- No translated DMA. The address translation entries and their parity
  view are unsupported, and PCI DMA below ``0x80000000`` logs
  ``LOG_UNIMP``.
- No PIC PCI function at device 0 outside the fixture.
- Widget status, the device-to-pin map, the flush register's ordering,
  the request timeout and write-request RAM have no effect.
- The interrupt-acknowledge cycle and the direct PCI I/O window do not
  exist.
- Unsupported register accesses log ``LOG_UNIMP``, up to 32 per head
  until reset; reads return 0. Misaligned control accesses are refused by
  the memory core and log under ``LOG_INVALID_MEM``. Accesses to the
  guard after head 1's last short window log ``LOG_GUEST_ERROR``.
- A failed access through the large PCI memory windows logs
  ``LOG_UNIMP`` under the same 32-per-head limit; a failed read returns
  0. The short windows log ``LOG_GUEST_ERROR`` instead, without a limit.
- PCI transaction failures never set PIC status or send an error
  interrupt.
- The stock IP35 PROM's bridge checks shape several reset values and
  the processor word view; a passing PROM run shows the model satisfies
  that software, not that the values are right.

.. _sgi-pic-tests:

Tests
=====

``tests/qtest/sgi-pic-test.c`` covers both heads and both views: the
identity word on both widths and its migration, configuration windows,
type-1 cycles and slot bounds, register widths and unsupported
registers, the processor word view, the widget timeout and destination
slots, the runtime reset values, the short windows with their attribute
refusals and boundaries, processor byte lanes against conventional
devices, the IOC4 and the ISP12160, 32-bit and 64-bit direct DMA with the
attribute matrix, requester width and refusals, swapped transfers written
a data phase at a time, interrupt pins, edges, clear and enable effects,
the force registers on both views, packets to a Bedrock, held-input
reassertion with migration, the link-error interrupt, the direct-map
mask, and migration between matching and mismatched profiles and older
stream versions.

``tests/qtest/sn-builder-test.c`` builds IP53 machines with one and two
PICs and IO9 on either head, and checks reset, independent per-widget
state and composition migration. The functional test
``tests/functional/mips64/test_irix_io9_tg_perf.py`` runs IRIX 6.5.30
network traffic through the BCM5701 behind PIC head 0 on the
``origin350`` machine.
