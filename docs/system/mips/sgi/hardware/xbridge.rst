.. SPDX-License-Identifier: BSD-3-Clause

.. _sgi-xbridge:

==========================
XBridge: XIO-to-PCI bridge
==========================

Used in :ref:`sgi-ip35` (the Origin 300 IO-8 assembly, the Fuel system
board and the Origin 300 PCI expansion module) and in the Origin 3000
I-, N-, P-, V- and X-bricks (:ref:`sgi-bricks`). The ``origin3000``
machine builds the I-brick's, the N-brick's and the V-brick's. The P-
and X-bricks are reference compositions no machine builds.

.. seealso::

   Platform pages: :ref:`sgi-ip35`.

.. seealso::

   - :term:`Bridge` (:doc:`bridge`): the earlier XIO-to-PCI bridge, whose
     style the XBridge's PCI heads follow.
   - :term:`PIC` (:doc:`pic`): the XIO-to-PCI-X bridge of the IP53 boards.
   - :term:`Crossbow` (:doc:`xbow`): the :term:`XIO` crossbar whose style the XBridge's
     crossbar half follows.

The XBridge is the SN1-generation XIO-to-PCI chip: a Crossbow-style
crossbar fused with two Bridge-style PCI host bridges ("heads"), one PCI
bus each :src:`xbridge-asic-spec-0.5 p. 17, 44`. An Origin 300 or Fuel
node has one between the :term:`Bedrock` and its PCI buses, and the Origin 300
PCI expansion module has three. QEMU models it in three types:

- ``sgi-xbridge-head`` in ``hw/pci-host/sgi-xbridge.c``: one PCI head,
  with its register file, configuration and device windows, the PCI
  ranges above its register face, DMA translation and interrupt pins;
- ``sgi-xbridge-xbow`` in ``hw/misc/sgi-xbridge-xbow.c``: the crossbar
  half as widget 0 presents it;
- ``sgi-xbridge``, also in ``hw/pci-host/sgi-xbridge.c``: the chip, one
  crossbar and two heads at widgets ``0xe`` and ``0xf``, and the routing
  between them, the host port and six external ports.

Both halves report revision 2, the revision real Origin 300s and Fuels
report. The only public specification covers revision A as designed in
1998 :src:`xbridge-asic-spec-0.5 title page`. Facts cited to it are
*established* for revision A; that they also hold for revision 2 is
*inferred* unless the page says otherwise. Everything the
:doc:`bridge` page says about a Bridge carries over to a head unless
this page says otherwise. Facts are rated as described in
:ref:`sgi-ratings`.

.. _sgi-xbridge-sources:

Sources
=======

- *XBridge ASIC Specification*, revision 0.5 for revision A silicon,
  :src:`xbridge-asic-spec-0.5`. Locators are printed pages. It is the
  only register-level source.
- *Crossbow ASIC Specification*, revision 12.0,
  :src:`crossbow-asic-spec-12.0`, for the crossbar baseline.
- *SGI Origin 300 User's Guide*, :src:`007-4365-002 p. 8, 9, 12`: the
  rear XIO connector and the riser card's 64-bit universal PCI slots.
- *sMDK-based Field Diagnostics*, :src:`108-0264-004`: names the
  Origin 300 XBridge diagnostic ``xbg`` and the IO-8 board test, whose
  shipped binaries this page cites as software expectations.
- Published captures: an Origin 300 OpenBSD boot,
  :src:`marc-openbsd-sgi-125897882029839 lines 55-91`; an Origin 300
  :term:`PROM` and IRIX inventory,
  :src:`marc-openbsd-sgi-139223952727384 lines 64-131`; a Fuel OpenBSD
  boot, :src:`miod-openbsd-sgi-story lines 5676-5695`.
- OpenBSD ``sys/arch/sgi/xbow/xbridgereg.h``, ``xbridge.c``,
  ``xbow.c`` and ``xbowdevs`` at ``2df91d80c104``; Linux
  ``arch/mips/include/asm/pci/bridge.h`` and
  ``arch/mips/pci/pci-xtalk-bridge.c`` at ``db2ddb871435``; the
  out-of-tree IP35 port's ``arch/mips/pci/pci-sgipic.c`` and
  ``arch/mips/sgi-ip35/ip35-xtalk.c`` at ``linux-ip35:9f613a321ea0``,
  as software expectations.
- SGI's Linux :term:`SN1` port at ``linux-history:7570df54ef8c`` (v2.5.40):
  ``include/asm-ia64/sn/pci/bridge.h``, ``sn/xtalk/xbow.h`` and
  ``arch/ia64/sn/io/sn1/pcibr.c``, with the same header on the
  ``linux-mips`` tree's ``linux-2.4`` branch. SGI wrote these, so they
  are a register-definition source and an Itanium consumer, not an
  independent check on the specification.
- The IRIX 6.5.30 IP35 kernel, the stock IP35 PROM 6.210 and the
  Origin 300 diagnostics, read as shipped binaries, as software
  expectations.

.. _sgi-xbridge-identity:

Identity and placement
======================

A mode pin straps the chip either as a crossbar with the two heads on
internal ports ``0xe`` and ``0xf`` and six external ports ``8`` to
``0xd`` (XBridge mode), or as two independent heads on external links
with the crossbar disabled and inaccessible (Super Bridge mode)
:src:`xbridge-asic-spec-0.5 p. 41, 44-45`, *established*.

The Origin 300 runs XBridge mode, *observed*: the PROM inventory lists
``ASIC XBOW Rev 2`` and ``ASIC BRIDGE Rev 2`` at widgets 14 and 15
(:src:`marc-openbsd-sgi-139223952727384 lines 64-67, 111-131`), and
OpenBSD attaches ``XXBow revision 2`` with ``XBridge revision 2`` at
widgets 15 and 14 (:src:`marc-openbsd-sgi-125897882029839 lines 55-56,
82`). A Fuel reports ``XBridge revision 2`` at widget 15
(:src:`miod-openbsd-sgi-story lines 5676-5695`). The Bedrock sits on
crossbar port ``0xa`` in the ``origin300`` and ``fuel`` machines.

.. list-table::
   :header-rows: 1

   * - Face
     - Identification word
     - Basis
   * - Head (widgets ``0xe`` and ``0xf``)
     - ``0x2d002049``: revision 2, part ``0xd002``, manufacturer
       ``0x024``, bit 0 set
     - Part and manufacturer *established*
       (:src:`xbridge-asic-spec-0.5 p. 132`), agreed by OpenBSD
       ``xbowdevs`` and Linux ``bridge.h``. Revision 2 *observed* in the
       captures above.
   * - Crossbar (widget 0)
     - ``0x2d000001``: revision 2, part ``0xd000``, manufacturer 0,
       bit 0 set
     - *established* :src:`xbridge-asic-spec-0.5 p. 71`; OpenBSD names
       part ``0xd000`` "XXBow". Revision 2 *observed*.

OpenBSD attaches a head only for manufacturer ``0x024`` with part
``0xd002``; a Bridge's ``0x036``/``0xc002`` would attach nothing
(OpenBSD ``xbridge.c``), a *software expectation*. Linux matches the part
alone. The head's identification word is fixed at revision 2. The
crossbar's revision comes from its ``revision`` property, 1 to 15,
default 2.

The model puts the Origin 300 riser's option slots at head E devices 1
and 2 and the IO-8's own devices on head F, *observed*: OpenBSD attaches
two BCM5701s at widget 14 devices 1 and 2 and the ISP12160, :term:`IOC3` and USB
controller at widget 15 devices 1, 4 and 5
(:src:`marc-openbsd-sgi-125897882029839 lines 57-91`).

.. _sgi-xbridge-head-registers:

Head register map
=================

Each register is 32 bits wide in the low half of an eight-byte slot and
is reached by a 32-bit access at the slot offset plus 4
:src:`xbridge-asic-spec-0.5 p. 123`, *established*. Every other width
or alignment, and any slot below ``0x300`` the map does not define, is
an invalid-address error (:ref:`sgi-xbridge-errors`). Names follow
OpenBSD ``xbridgereg.h``. "Storage" means the model keeps the written
bits and gives them no effect. Reset values are the specification's
unless the notes say otherwise.

.. sgi-registers: base=0xf00000000000 width=32 lane=4 stride=8 label=head-e expect=0x20:0xf00321f
   skiprow="Device 0-7:per-slot-base"
   map=/machine/peripheral/xbridge:0:0x100000000000
   qemu="-M none -cpu R10000 -S -device sgi-xio-test-target,id=fabric -device sgi-xbridge,id=xbridge,host-port=0xb,xio-port=0xc,fabric=/machine/peripheral/fabric/fabric.ram[0]"

.. sgi-registers: base=0x1000000000000 width=32 lane=4 stride=8 label=head-f
   skiprow="Device 0-7:per-slot-base"
   map=/machine/peripheral/xbridge:0:0x100000000000
   qemu="-M none -cpu R10000 -S -device sgi-xio-test-target,id=fabric -device sgi-xbridge,id=xbridge,host-port=0xb,xio-port=0xc,fabric=/machine/peripheral/fabric/fabric.ram[0]"

.. list-table::
   :header-rows: 1

   * - Offset
     - Name
     - Reset
     - Access
     - Notes
   * - ``0x00``
     - Identification
     - ``0x2d002049``
     - read-only
     - See :ref:`sgi-xbridge-identity`.
   * - ``0x08``
     - Status
     - ``0x00000020``
     - read-only
     - Bits 7:0 are input pins on the XBridge
       :src:`xbridge-asic-spec-0.5 p. 133`. Bit 5 reads one; the SN1
       driver refuses a head whose bit 5 is clear, a *software expectation*. Credit and retry
       counts read zero, an *approximation*.
   * - ``0x10``, ``0x18``
     - Error address upper, lower
     - 0
     - read-only
     - Latched by the first invalid access.
   * - ``0x20``
     - Control
     - ``0x0f00320f``
     - mask ``0x0fe1f20f``
     - Bits 5:4, PCI speed, read-only from the ``pci-speed`` property.
       Bits 3:0 are the widget ID, reset ``0xf`` in both heads
       :src:`xbridge-asic-spec-0.5 p. 134-135`.
   * - ``0x28``
     - Request timeout
     - ``0xfffff``
     - bits 19:0
     - Storage.
   * - ``0x30``, ``0x38``
     - Interrupt destination upper, lower
     - 0
     - bits 19:0; all
     - Target widget in 19:16, address bits 47:32 in 15:0; a head has
       no vector field.
   * - ``0x40``
     - Error command word
     - 0
     - read-only
     - A write logs ``LOG_GUEST_ERROR``. The model never sets it.
   * - ``0x48``
     - LLP configuration
     - ``0x03ff1810``
     - bits 25:0
     - Storage; no effect in XBridge mode
       :src:`xbridge-asic-spec-0.5 p. 44`.
   * - ``0x50``
     - Target flush
     - 0
     - read zero
     - Writes log ``LOG_UNIMP``.
   * - ``0x58``-``0x68``
     - Aux error, response buffer error
     - 0
     - read zero
     - Writes log ``LOG_UNIMP``.
   * - ``0x70``
     - Test pin control
     - 0
     - bits 23:0
     - Storage.
   * - ``0x80``
     - Direct map
     - ``0x00020000``
     - bits 23:20, 17:0
     - Bit 17 (add 512 MB) resets set; bits 19:18 are reserved
       :src:`xbridge-asic-spec-0.5 p. 142`.
   * - ``0x90``
     - Map fault address
     - 0
     - read zero
     - No field layout is published. Writes log ``LOG_UNIMP``.
   * - ``0xa0``
     - Arbitration
     - ``0x00020008``
     - mask ``0x3ff7f``
     - Storage.
   * - ``0xb0``
     - NIC
     - 0
     - see notes
     - See :ref:`sgi-xbridge-nic`.
   * - ``0xc0``
     - PCI timeout
     - ``0x00010000``
     - mask ``0x1f03ff``
     - Storage.
   * - ``0xc8``
     - Type 1 configuration
     - 0
     - bits 23:11
     - Bus in 23:16, device in 15:11.
   * - ``0xd0``, ``0xd8``
     - PCI error upper, lower
     - 0
     - read zero
     - Writes log ``LOG_UNIMP``.
   * - ``0x100``
     - Interrupt status
     - 0
     - read-only
     - Bits 7:0 follow the pins. Bit 24 invalid address, bit 30 page
       fault. Writes are ignored.
   * - ``0x108``
     - Interrupt enable
     - 0
     - bits 30:0
     - See :ref:`sgi-xbridge-interrupts`.
   * - ``0x110``
     - Reset interrupt status
     - none
     - write
     - Bit 1 clears the page fault, bit 3 the invalid address, bit 6
       the whole error view. Other bits do nothing.
   * - ``0x118``
     - Interrupt mode
     - 0
     - bits 7:0
     - Clear packets per pin.
   * - ``0x120``
     - Interrupt device
     - 0
     - bits 23:0
     - Storage.
   * - ``0x128``
     - Host error field
     - 0
     - bits 7:0
     - Interrupt number of the head's error packet.
   * - ``0x130``-``0x168``
     - Host address 0-7
     - 0
     - bits 17:0
     - Bits 17:8 overlay packet address bits 47:38, bits 7:0 are the
       interrupt number.
   * - ``0x170``
     - Error interrupt view
     - 0
     - read-only
     - New on the XBridge :src:`xbridge-asic-spec-0.5 Table 37`.
       Writes log ``LOG_UNIMP``.
   * - ``0x178``
     - Multiple interrupt
     - 0
     - read-only
     - Never set; writes log ``LOG_UNIMP``.
   * - ``0x180``-``0x1b8``
     - Force always 0-7
     - none
     - write
     - New on the XBridge.
   * - ``0x1c0``-``0x1f8``
     - Force with pin 0-7
     - none
     - write
     - New on the XBridge.
   * - ``0x200``-``0x238``
     - Device 0-7
     - ``0x18001000`` plus base
     - mask ``0x1eefffff``
     - See :ref:`sgi-xbridge-device-registers`.
   * - ``0x240``-``0x278``
     - Write request buffer flush 0-7
     - 0
     - read zero
     - Writes log ``LOG_UNIMP``.
   * - ``0x280``, ``0x288``
     - Even and odd read response buffers
     - 0
     - all
     - Storage.
   * - ``0x290``, ``0x298``
     - Read response buffer status and clear
     - 0
     - read zero
     - Writes log ``LOG_UNIMP``.

Head E's ``pci-speed-e`` property defaults to 1, so the model reads
``0x0f00321f`` from Control at power-on, where the table's reset has speed
bits 5:4 clear. Head F's property defaults to 0 and reads the table's
value. The default is an *approximation*.

A write to the identification, status or error address registers logs
``LOG_UNIMP`` and changes nothing. Reads of the write-only registers
return zero.

.. _sgi-xbridge-face:

The register face and the space above it
----------------------------------------

A head decodes a 44-bit widget space, the most the chip passes a widget.
The first 16 MB is its register face:

.. list-table::
   :header-rows: 1

   * - Widget offset
     - Contents
   * - ``0x0`` to ``0x2ff``
     - Registers, above.
   * - ``0x300`` to ``0x5ff``
     - Address-match buffers and performance counters, deferred.
   * - ``0x1_0000`` to ``0x1_1fff``
     - Translation table, written as 64-bit entries, upper words read
       at ``+4``.
   * - ``0x1_2000`` to ``0x1_3fff``
     - Translation table lower-word view.
   * - ``0x2_0000`` to ``0x2_7fff``
     - Type 0 configuration, ``0x1000`` per device, ``0x100`` per
       function.
   * - ``0x2_8000`` to ``0x2_8fff``
     - Type 1 configuration through register ``0xc8``.
   * - ``0x3_0000`` to ``0x3_0007``
     - Interrupt acknowledge, deferred.
   * - ``0x20_0000`` to ``0xbf_ffff``
     - Device windows: two of 2 MB, then six of 1 MB.
   * - ``0xc0_0000`` to ``0xff_ffff``
     - The 4 MB section, deferred.

Configuration, device windows and the 4 MB section sit where the
specification's widget map puts them :src:`xbridge-asic-spec-0.5 p. 179`,
*established*. Above the face, three ranges reach PCI space
:src:`xbridge-asic-spec-0.5 p. 176-177`, *established*:

- ``0x4000_0000`` to ``0x7fff_ffff``: PCI memory ``0x0`` to
  ``0x3fff_ffff`` as a 32-bit target;
- ``0x8000_0000`` to ``0xbfff_ffff``: the same gigabyte as a 64-bit
  target;
- ``0x1_0000_0000`` to ``0x1_ffff_ffff``: all of PCI I/O space.

The offset within a range is the PCI address. Every other address in the
widget space, the gaps between the ranges included, is an
invalid-address error. That step from "reserved" to "invalid" is
*inferred* from the rule that any undefined address is one
:src:`xbridge-asic-spec-0.5 p. 123`. The XBridge has no external SSRAM
window: ``0x8_0000`` to ``0x1f_ffff`` is reserved and is an invalid
address here.

.. _sgi-xbridge-device-registers:

Device registers and the direct map
-----------------------------------

The device register layout is the Bridge's, with bit 24 (page-mapped
write gather) and bit 20 (page-mapped swap) removed
:src:`xbridge-asic-spec-0.5 p. 25, 161`, *established*. The model drops
both bits on write. OpenBSD and the SN1 driver still write them, a
*software expectation* the model accepts.

The specification gives no reset value for the window base field. The
model resets device *n* with error-lock, page-cross and memory space set
and window base 0, 2, 4, 5, 6, 7, 8 and 9, the Bridge's layout of two
2 MB windows then six of 1 MB. The stock IP35 PROM's sanity diagnostic
expects device registers 1 to 7 to hold window bases 2, 4, 5, 6, 7, 8
and 9 at reset, a *software expectation*. That the seven values it
reports belong to devices 1 to 7 in order is *inferred* from the report
order.

A direct-window DMA address takes its target widget from direct map bits
23:20 and XIO address bits 47:31 from bits 16:0
:src:`xbridge-asic-spec-0.5 p. 142`. The model adds 512 MB only when
bit 17 is set and the offset field is zero, as on the Bridge. The
specification states bit 17 without that condition, so the model's rule
is an *approximation*. OpenBSD, Linux and the IP35 port write widget and
offset and so clear bit 17, each a *software expectation*.

.. _sgi-xbridge-nic:

The NIC register
----------------

In XBridge mode the specification wires the chip's one-wire pin to
widget 0's NIC register and leaves each head's NIC register unused
:src:`xbridge-asic-spec-0.5 p. 44, 143`, *established* for revision A.
The model departs from that. A head given a ``board-part`` property
carries a DS2505-type board-identity part on its own one-wire bus behind
``0xb0``. Each write runs one bit slot and sets bit 1 (done) and bit 0
(the sampled level) in the readback. The record is a placeholder: family
``0x0b``, the text ``NOT-OBSERVED`` in the part field and a marked
synthetic serial from ``onewire-serial``. No real IO-8 identity bytes
are known, so this is an *approximation* that claims no hardware bytes.
A head without a board part logs ``LOG_UNIMP`` on a write to ``0xb0``
and stays inert. The Origin 300 machines give the IO-8's head F a board
part and leave head E without one.

.. _sgi-xbridge-pio:

Configuration and programmed I/O
================================

Configuration
-------------

Type 0 configuration space is ``0x2_0000`` plus device times ``0x1000``
plus function times ``0x100``; type 1 is ``0x2_8000`` with bus and device
from register ``0xc8`` :src:`xbridge-asic-spec-0.5 p. 179`,
*established*. A narrow access is placed big-endian within its word, at
the register offset XOR (4 minus size). A configuration read of an empty
device returns all ones and sets bit 11, PCI master timeout, in the
error-interrupt view only. The SN1 driver probes with a guarded read and
then reads the view for that bit, a *software expectation*. The model
does not set the bit in the status register or send an error packet.

The specification calls each configuration register a 32-bit word and
says nothing of a 64-bit access. The Origin 300 IO-8 board test reads
the IOC3's header with a 64-bit load and expects the word at the lower
address in the upper half, a *software expectation*. The model performs
a doubleword configuration read as two word reads, lower address first
into the upper half, and splits a doubleword write the same way. Each
is an *approximation*. A doubleword configuration read captured on an
Origin 300 would settle it.

Device windows
--------------

A device window maps to PCI memory space when device bit 12 is set and
to I/O space when it is clear. In memory space the top two base bits are
ignored, so the base supplies PCI bits 29:20, and devices 0 and 1 ignore
base bit 0 for their 2 MB windows. In I/O space all twelve bits map
:src:`xbridge-asic-spec-0.5 p. 178-179`, *established*. The model also
clears base bit 0 for devices 0 and 1 in I/O space, which the
specification does not state, an *approximation*.

Device bit 22 gives the target's width, 4 or 8 bytes. A doubleword to a
32-bit target is two word cycles, lower address first, carrying the more
significant half first. Device bit 13 selects the byte placement. With
it clear, a narrow access lands at offset XOR (width minus size) with its
value unchanged. With it set, the address is kept and the bytes travel
in address order. These rows follow the Bridge's appendix, which the
XBridge appendix repeats table for table
(:src:`xbridge-asic-spec-0.5 p. 229, 233`), *established* for 32-bit
data phases. :doc:`bridge` tabulates every width.

The ranges above the face
-------------------------

Placement in the ranges above the face follows control bit 22 for the
two memory ranges and bit 23 for the I/O range, not a device register
:src:`xbridge-asic-spec-0.5 p. 134, 178`, *established*. With the bit
clear a narrow access lands mirrored within the target's width, as a
device window places it with bit 13 clear. With it set the address is
kept and the bytes travel in address order.

The 32-bit memory alias and the I/O range use a 4-byte width. The
64-bit alias uses 8 bytes when no single mapped memory BAR owns the
access, and otherwise the owning slot's width settled at reset (see
:ref:`sgi-xbridge-dma-placement`). That the alias rather than device bit
22 selects the width is *inferred* from the specification naming the
two aliases for 32-bit and 64-bit targets. Using the slot's reset width
for a 32-bit card behind the 64-bit alias is an *approximation*: the
chip learns the width from ``ACK64#`` on each transfer
:src:`xbridge-asic-spec-0.5 p. 37`, which QEMU cannot observe. OpenBSD
and Linux clear both control swap bits and the IP35 port sets them, a
*software expectation*. OpenBSD maps PCI memory through the 32-bit alias
and never uses the 64-bit alias, a *software expectation*.

In the Origin 300 PCI expansion topology, with packet transport and
revision 2, head E of the first chip narrows a 4-byte aligned access through the 64-bit alias to 4 bytes
when the slot is in the head's wide-slot mask and device bit 22 is clear.
The stock ``xbg`` diagnostic expects the four words it reads there in
the order the 32-bit alias returns them. This is an *approximation*
confined to that topology.

.. _sgi-xbridge-dma:

DMA
===

A card's PCI address space divides as follows
:src:`xbridge-asic-spec-0.5 p. 174, 181-183`, *established*:

.. list-table::
   :header-rows: 1

   * - PCI address
     - Path
   * - ``0x0`` to ``0x3ffe_ffff``
     - No response. Logs ``LOG_GUEST_ERROR``.
   * - ``0x3fff_0000`` to ``0x3fff_ffff``
     - Buffer flush. Completes at once with nothing to drain.
   * - ``0x4000_0000`` to ``0x407f_ffff``
     - Page-mapped through the translation table.
   * - ``0x6000_0000`` to ``0x607f_ffff``
     - The same pages with a swap request (revision 2, below).
   * - Other addresses from ``0x4000_0000`` to ``0x7fff_ffff``
     - No response. Logs ``LOG_UNIMP``.
   * - ``0x8000_0000`` to ``0xffff_ffff``
     - Direct window through the direct map. Swap from device bit 19.
   * - ``0x1_0000_0000`` to ``0x0fff_ffff_ffff_ffff``
     - No response. Logs ``LOG_GUEST_ERROR``.
   * - ``0x1000_0000_0000_0000`` and up
     - 64-bit direct. Target widget in bits 63:60, XIO address in bits
       47:0, swap in bit 55.

A 64-bit address with target widget 0 is not supported
:src:`xbridge-asic-spec-0.5 Table 87`; it falls below the 64-bit window
and gets no response. Attribute bits 59:56 of a 64-bit address
(prefetch, precise, virtual, barrier) and the device register's
coherence and barrier bits have no effect in the model. A target widget
with nothing linked to it gets no response and logs ``LOG_GUEST_ERROR``.
Translation is uncached: the head refuses QEMU IOMMU notifiers.

.. _sgi-xbridge-ate:

Translation table
-----------------

An entry holds the XIO address in bits 47:12, the target widget in
bits 11:8, swap in bit 5, barrier, prefetch, precise and coherent in
bits 4:1, and valid in bit 0 :src:`xbridge-asic-spec-0.5 Table 92`,
*established*. Translation is per page, 4 KB or 16 KB by control bit 21.
A 16 KB page ignores entry bits 13:12. Target widget 0 is allowed here.
Bits 4:1 are stored and have no effect. Entries are stored as 48 bits,
so bits 63:48 read back zero.

The specification gives 512 entries written from ``0x1_0000`` and only
the Bridge's carried-over read view at ``0x1_1004``
:src:`xbridge-asic-spec-0.5 p. 181, 252`. SGI's SN1 header describes
1024 entries written from ``0x1_0000`` to ``0x1_1fff``, upper words read
at each entry's ``+4`` and lower words read from ``0x1_2000`` to
``0x1_3fff``, and names revisions 1 and 2 as A and B (``bridge.h`` lines
264-290 and 929-930). The Origin 300 ``xbg`` diagnostic writes 512
entries and reads the lower words at ``0x1_2004`` upward, a *software
expectation*. The model follows the header's layout, *inferred* to be
revision B's from the header and the diagnostic agreeing.

Only a 64-bit aligned write inside the write range and a 32-bit read at
``+4`` in either range are served. Every other access there, a write
into the lower-word view included, is an invalid-address error. Only the
first 512 entries map pages. Entries 512 to 1023 are stored and read
back, an *approximation*: whether revision B's page mapper reaches 1024
entries is *unknown*. Writing distinct values to entries 0 and 512 on an
Origin 300 and reading both views would settle the size.

The page index is PCI bits 28:12 for 4 KB pages or 28:14 for 16 KB
pages. A candidate index of 512 or more, or an entry with bit 0 clear,
sets interrupt status bit 30 and its view mirror, logs
``LOG_GUEST_ERROR`` and issues no transaction. Reset interrupt status bit
1 clears it. The model sends no error packet for a page fault.

The specification puts PCI bit 29 into the index and assigns swapping to
entry bit 5 alone. On revision 2 the model drops bit 29 from the index
and treats it as a swap request, so ``0x6000_0000`` reaches the same
page as ``0x4000_0000`` with swapping on. Entry bit 5 also requests
swapping. This is an *approximation*. SGI's SN1 driver swaps a mapped
page by setting PCI bit 29 and never sets entry bit 5 (``bridge.h``
lines 1398-1402, ``pcibr.c`` lines 5156-5171), and the IRIX 6.5.30
kernel does the same, each a *software expectation*. The stock IP35
PROM 6.210 programs entry 1 with bit 5 clear and issues USB DMA at
``0x6000_581c``, a *software expectation*. Only the bit-29 reading maps
it. A revision-B specification or a DMA capture on an
Origin 300 would settle the hardware rule.

.. _sgi-xbridge-dma-placement:

Byte placement
--------------

The swap control for each path is device bit 19 for the 32-bit direct
window, address bit 55 for 64-bit direct, and entry bit 5 or PCI bit 29
for page-mapped DMA. With swap set the transfer is a byte stream. With
swap clear the model reverses each aligned 32-bit word for a slot that
runs 32-bit data phases and each aligned doubleword for a slot that runs
64-bit data phases. The swap controls are *established*
:src:`xbridge-asic-spec-0.5 p. 161, 181-183`. The width-dependent
placement is *inferred* from the processor-to-PCI appendix tables,
since the specification does not tabulate the DMA direction. The shared
placement code is ``hw/pci-host/sgi-dma-placement.c``, as on the
:doc:`bridge`.

A slot runs 64-bit data phases when the board drives its ``REQ64#`` at
reset (the head's ``req64-slots``) and every function on the card
declares a 64-bit interface. The model settles this at the end of each
reset. The specification lists driving ``REQ64#`` during reset as a fix
over the Bridge :src:`xbridge-asic-spec-0.5 p. 25`, *established*. The
Origin 300 machine wires head E devices 1 and 2, the riser's 64-bit
universal slots :src:`007-4365-002 p. 9, 12`; that the riser carries the
64-bit lines is *inferred*. Head F is left 32-bit, an *approximation*
that makes no byte difference under IRIX, which runs the ISP12160
swap-set.

The IRIX 6.5.30 kernel identifies a head by part ``0xd002`` and
manufacturer ``0x024``. It requests swapping with device bit 19 for the
32-bit direct window, with address bit 55 for 64-bit direct and with PCI
bit 29 for page-mapped DMA, a *software expectation* read from the
shipped binary. The IP35 port records that the Origin 300's SCSI
controller needs bit 55 (``pci-sgipic.c``), a developer's report.

Origin 300 PCI expansion placement
----------------------------------

The Origin 300 PCI expansion topology adds placement rules on head E of
its first chip, for the slots in that chip's ``wide-slot-mask-e``
(devices 1 and 2). They apply only with packet transport and act only on
aligned 8-byte transfers whose destination is ordinary RAM:

- With swap set, writes through the direct window, the page map, or the
  64-bit window with device bit 19 set exchange the two 4-byte halves.
  Reads through the direct window, the page map's bit-29 alias, or the
  64-bit window from a 64-bit slot with device bit 19 set do the same.
- With swap clear on a 64-bit slot, direct-window, 64-bit window
  (device bit 19 clear) and page-mapped (bit 29 clear) transfers reverse
  each 32-bit word rather than the doubleword.

Shorter, unaligned and non-RAM transfers keep the ordinary placement.
These rules are each an *approximation* chosen so that the stock ``xbg``
diagnostic's Tigon DMA comparisons match. No public source states
revision 2's placement on a wide slot, and repeated test patterns do not
fix every byte. A PCI transaction capture on an Origin 300 that records
``ACK64#`` and the delivered lanes would settle it. :doc:`pci-expansion`
describes the topology.

.. _sgi-xbridge-interrupts:

Interrupts
==========

Each head has eight interrupt pins, the unnamed GPIO inputs of
``sgi-xbridge-head``. Status bits 7:0 follow the pins whether or not
they are enabled :src:`xbridge-asic-spec-0.5 interrupt chapter`,
*established*. A pin's rising edge, while enabled, sends a set packet:
an 8-byte big-endian write of the host address register's bits 7:0 with
bit 8 set, to the widget in destination upper bits 19:16, at the address
formed from the destination registers with host address bits 17:8 over
address bits 47:38. The falling edge sends a clear packet, bit 8 clear,
only when both the enable bit and the mode bit are set. OpenBSD, Linux
and the IP35 port set the mode bit; SGI's SN1 driver clears it,
each a *software expectation*.

Force-always register *n* sends pin *n*'s set packet on any write.
Force-with-pin register *n* sends it only while pin *n* is asserted
:src:`xbridge-asic-spec-0.5 Table 37`, *established*. OpenBSD writes the
force-with-pin register after servicing each interrupt on an XBridge. The
specification does not say whether a force on a disabled pin is
delivered. The model logs ``LOG_UNIMP`` and sends nothing, an
*approximation*.

Two revision-2 behaviours are each an *approximation* with no hardware source:

- Interrupt enable stores bits 8 and 16, which the specification
  reserves :src:`xbridge-asic-spec-0.5 p. 150-151`, so an all-ones
  write reads back ``0x7fffffff``. The stock ``xbg`` diagnostic writes
  ``0x7ffffffd`` and expects it back. The bits have no effect.
- Setting an enable bit while that pin is already asserted sends one set
  packet. ``xbg`` asserts a pin while it is masked and then waits for a
  packet after unmasking it. The specification does not cover the case.

The multiple-interrupt register at ``0x178`` is never set: the
specification does not define what detects a recurrence.

Which slot's INTx drives which pin is board wiring the specification
leaves open :src:`xbridge-asic-spec-0.5 interrupt chapter`. The head
therefore exposes 32 raw outputs, ``pci-intx``, numbered slot times 4
plus pin (A = 0), carrying each card's level unchanged. A level raised
on an output with nothing connected logs ``LOG_UNIMP``. The machine
connects outputs to pins; the Fuel's wiring is in
:ref:`sgi-sn-fuel-interrupts`.

With packet transport each interrupt packet carries the head's widget
ID, control bits 3:0, as its source. Without it the packet is a plain
write into the target's address space.

.. _sgi-xbridge-errors:

Errors
======

An undefined register slot, a wrong access width, an access to the
translation table other than those served, or an address above the face
outside the three ranges is an invalid-address error
:src:`xbridge-asic-spec-0.5 p. 123`, *established*. The model sets
interrupt status bit 24 and its view mirror and logs
``LOG_GUEST_ERROR``. The access completes: reads return zero and writes
are dropped, with no bus error.

The first such access latches its widget-relative address, bits 47:32 in
error address upper and bits 31:0 in lower, whether or not bit 24 is
enabled :src:`xbridge-asic-spec-0.5 p. 133-134, 222`, *established*. If
bit 24 is enabled, the head also sends one set packet carrying the host
error field's bits 7:0 with bit 8 set to the interrupt destination. A
later invalid access neither replaces the record nor sends again until
reset interrupt status bit 3 clears bit 24. The latched value carries no
widget number. Inside the face it is the face offset, above the face the
full offset, at most 44 bits through the chip. The specification also
puts the request's command word in the error command word register; the
model does not.

The deferred ranges, ``0x300`` to ``0x5ff``, ``0x3_0000`` to
``0x3_0007`` and the 4 MB section, read zero and drop writes with a
``LOG_UNIMP`` message, and are not invalid addresses.

.. _sgi-xbridge-crossbar:

Crossbar half
=============

``sgi-xbridge-xbow`` presents one 16 MB MMIO view per crossbar port,
8 to ``0xf``, because widget 0's status register reports in bits 9:6 the
port a read arrived on :src:`xbridge-asic-spec-0.5 p. 75`,
*established*. The SN1 consumer and the IP35 port read it to learn the
host's port, a *software expectation*. Only the first ``0x300`` bytes of
a view hold registers.

Every register is 32 bits, reached by a 32-bit access at its slot offset
plus 4. Another width, an offset from ``0xd0`` to ``0xf8`` or at
``0x300`` and above, a write to a read-only or read-and-clear register,
or a read of a link reset register sets status bit 5, the register
access error :src:`xbridge-asic-spec-0.5 p. 58, 60, 63, 75`,
*established*. The model logs ``LOG_GUEST_ERROR`` and completes the
access with zero; the data a real read returns is *unknown*.

.. list-table::
   :header-rows: 1

   * - Offset
     - Name
     - Reset
     - Access
     - Notes
   * - ``0x00``
     - Identification
     - ``0x2d000001``
     - read-only
     - With the default revision 2.
   * - ``0x08``
     - Widget 0 status
     - 0
     - read-only
     - Bits 9:6 report the reading port. Bit 23 is the widget 0
       request, bits 5:0 the conditions.
   * - ``0x10``, ``0x18``
     - Error address upper, lower
     - 0
     - read-only
     - Never set by the model.
   * - ``0x20``
     - Widget 0 control
     - ``0x80``
     - bits 7:1
     - Bit 7 enables the link timeout counters. Each condition's enable
       sits at the condition's bit.
   * - ``0x28``
     - Packet timeout
     - ``0xfffff``
     - bits 19:0
     - Storage.
   * - ``0x30``
     - Interrupt destination upper
     - ``0x00080000``
     - mask ``0xff07ffff``
     - Vector 31:24, target 19:16 with bit 19 fixed at one, address
       47:32.
   * - ``0x38``
     - Interrupt destination lower
     - 0
     - all
     - Address 31:0.
   * - ``0x40``
     - Error command word
     - 0
     - write
     - A write clears it and both error address registers.
   * - ``0x48``
     - LLP control
     - ``0x03ff1810``
     - bits 25:0
     - Storage.
   * - ``0x50``
     - Widget 0 status, clearing alias
     - none
     - read
     - See below.
   * - ``0x58``
     - Arbitration reload
     - 2
     - bits 5:0
     - Storage.
   * - ``0x60``, ``0x68``
     - Performance counters A, B
     - 0
     - bits 22:20
     - Link select only; nothing counts.
   * - ``0x70``
     - NIC
     - 2
     - bits 19:2
     - Bit 1 (data valid) reads one. A pulse logs ``LOG_UNIMP``.
   * - ``0x78``
     - Widget 0 reset fence
     - ``0xff``
     - bits 7:0
     - Ports widget 0 accepts a warm reset from.
   * - ``0x80``-``0xb8``
     - Link 8-F reset fences
     - ``0xff``
     - bits 7:0
     - Every bit writable, the link's own included.
   * - ``0xc0``
     - Lock
     - 0
     - bits 7:0
     - A write takes effect only while the lock reads zero.
   * - ``0xc8``
     - Lock clear
     - none
     - read, write
     - A write clears the lock; a read returns it.
   * - link ``+0x00``
     - Input buffer flush
     - none
     - read
     - Reads zero: nothing is queued.
   * - link ``+0x08``
     - Link control
     - ``0x00080000``
     - mask ``0xbf7f01ff``
     - Credit 2. Force bad packet, bit 23, reads zero.
   * - link ``+0x10``, ``+0x28``
     - Link status, clearing alias
     - see below
     - read
     - The alias clears nothing the model holds.
   * - link ``+0x18``, ``+0x20``
     - Arbitration upper, lower
     - 0
     - all
     - Storage.
   * - link ``+0x30``
     - Link reset
     - none
     - write
     - One-shot; see :ref:`sgi-xbridge-reset`.
   * - link ``+0x38``
     - Link auxiliary status
     - see below
     - read
     -

Link registers start at ``0x100``, ``0x40`` per link from link 8. The
reset values that differ from the Crossbow (control ``0x80``,
destination ``0x00080000``, three-bit counter selects, all-ones fences)
are *established* :src:`xbridge-asic-spec-0.5 p. 73, 76-78, 81-82`. The
fences, lock and lock clear are new on the XBridge
:src:`xbridge-asic-spec-0.5 p. 27, 82-83`.

An enabled condition sets the widget 0 request bit and sends one set
packet, the destination's vector with bit 8 set, to the destination
widget. Further conditions accumulate without another packet. Reading
the clearing alias at ``0x50`` returns the status, clears bits 5:0 and
the request, and sends a clear packet if a set packet was outstanding.
The only condition the model raises is the register access error.

A link with a widget behind it reads alive (link status bit 31) and
widget present (aux status bit 5). An empty port reads link status bit 5
and aux status bit 6, failure mode, and stays there. Link training is
not modelled, an *approximation*. Aux status bit 4 reports the
``links-8bit`` property.

.. _sgi-xbridge-chip:

The chip
========

``sgi-xbridge`` owns the crossbar and two heads as children named
``xbow``, ``head-e`` and ``head-f``, and is what a machine instantiates.
In XBridge mode it refuses to realize without ``fabric``, the memory
region that receives everything sent toward the host, and ``host-port``,
the host's crossbar port, 8 to ``0xd``. The crossbar's presence map is
the host port and both heads, plus any attached external port.

Properties: ``pci-speed-e`` (default 1) and ``pci-speed-f`` (default 0)
feed each head's control bits 5:4, which OpenBSD decodes as 33, 66, 100
or 133 MHz. ``req64-slots-e`` and ``req64-slots-f`` set the 64-bit slots.
``board-part-e``, ``board-part-f``, ``onewire-serial-e`` and
``onewire-serial-f`` give a head its NIC record. ``bus-prefix`` names the
PCI buses ``<prefix>head-e.0`` and ``<prefix>head-f.0``. ``xio-port``
selects the external port the machine attaches a widget to (default
``0xb``). ``superbridge`` selects the strap mode.

MMIO region 0 is the XIO space as the host sees it, sixteen slots of
16 TB. Slot 0 is the crossbar view for the host's port, so a host read
of status reports the host's port. Slots ``0xe`` and ``0xf`` are the
heads' whole widget spaces. An external slot reaches its widget once one
is attached. Every other slot reaches nothing: reads return zero and
writes are dropped, both with ``LOG_GUEST_ERROR``. Regions 1 to 3 are
the views of widgets 0, ``0xe`` and ``0xf``. They exist from instance
init, so a host and the chip can link each other before either realizes.
``sgi_xbridge_widget_view`` and ``sgi_xbridge_pci_bus`` return a view
and a head's root bus.

Widgets ``0xe`` and ``0xf`` are fixed destinations. Rewriting a head's
widget ID changes its readback and the source ID in its packets, not
where it answers. That a head does not compare a packet's destination
with its widget ID is *inferred*: the specification gives the widget ID
no role but the source ID of packets the head originates
:src:`xbridge-asic-spec-0.5 p. 134`.

The chip's XIO port interface presents widget 0's view as the processor
face of the host's own port. The Origin 300 machine therefore answers a
request for widget ``0xa`` with widget 0's registers, an
*approximation*. Whether the chip answers a request for the port it
arrived on is *unknown*; the specification says only that a link cannot
send packets to itself :src:`xbridge-asic-spec-0.5 p. 68`. SGI's sMDK
I/O discovery walks the Bedrock's ports and fails with nothing at
``0xa``, a *software expectation*. A read at the host's own port number
on an Origin 300 would settle it.

External ports
--------------

``sgi_xbridge_attach_xio_port`` attaches a widget to any external port 8
to ``0xd`` except the host's, before or after realization. The XIO port
interface does the same for a linked peer. The crossbar then reports the
link present and alive. A packet from an external widget is routed to
widget 0, the host port, either head or another external port, but not
back to its own port. A packet for its own port or an empty port fails
with ``LOG_GUEST_ERROR``.

The Origin 300's rear XIO connector is a cable port
:src:`007-4365-002 p. 8`. That the crossbar serves it is *inferred*:
the Bedrock already holds port ``0xa``. Which port it uses is *unknown*;
the model uses ``0xb``, the lowest free port, an *approximation*. The
machine offers no XIO board there. The Fuel's graphics board answers at
widget ``0xd``, which the ``fuel`` machine selects as its external port.

Packet transport
----------------

A machine may select packet transport before realization. Each head then
sends DMA and interrupts as packets that name the source widget, the
head's control bits 3:0, and, for DMA, the requesting slot. Inbound
packets to a head are delivered through the chip's XIO space. A packet
that would re-enter a transfer already in progress from the same source
is refused with ``LOG_GUEST_ERROR``. The Origin 300 topologies that route
I/O through the modelled :term:`NUMAlink` fabric use this mode.

Super Bridge mode
-----------------

With ``superbridge`` set the chip exposes two independent heads, named
by their E and F origins. Widget 0 is not accessible, the heads cannot
reach each other, and each takes its own upstream link before
realization :src:`xbridge-asic-spec-0.5 p. 45`. Both widget ID fields
reset to ``0xf`` :src:`xbridge-asic-spec-0.5 p. 135`. Only the F head
may carry a board part, since the head that powers up as ``0xf`` owns
the one-wire pin. Setting ``board-part-e`` or ``xio-widget`` is refused.
Warm-reset inputs are ignored. Applying these revision-A clauses to the
revision-2 head is *inferred*. Renumbered packet filtering, source
identity after renumbering and reset propagation across links are not
modelled. The Origin 300 PCI expansion topology straps its second and
third chips this way, an *inferred* composition
(:ref:`sgi-pci-expansion-origin300`).

.. _sgi-xbridge-reset:

Reset
=====

A machine reset restores every register in both halves, clears pin and
INTx levels, and then settles each slot's DMA width.

A warm reset arriving on a crossbar port is accepted if widget 0's fence
admits that port. It resets the crossbar core and is forwarded to every
port whose fence admits the source, the source's own port included
:src:`xbridge-asic-spec-0.5 p. 27, 81, 122`, *established*. The forward
to ``0xe`` or ``0xf`` resets that head. External ports leave on the
chip's ``warm-reset-out`` lines, index port minus 8, and enter on its
``warm-reset`` lines. The model reads the fences as they stood before
the core reset. The specification does not settle three points, and the
model's answers are each an *approximation*:

- whether accepting a warm reset returns the fences to all ones (the
  model does) or keeps them, since the change list says power-on reset
  alone resets them;
- whether a warm reset that widget 0's fence rejects still reaches the
  links (the model forwards nothing);
- the ignore window. After a reset pattern a port ignores further ones
  for 1024 cycles :src:`xbridge-asic-spec-0.5 p. 122`. The model keeps
  one window for the chip, 10.24 microseconds of virtual time at the
  100 MHz core clock :src:`xbridge-asic-spec-0.5 p. 86`, opened even by
  a rejected pattern. A machine reset closes it.

A write to a link reset register pulses that link's reset output alone.
The register's page says the one-shot sends a link reset and the reset
chapter says a warm reset :src:`xbridge-asic-spec-0.5 p. 70, 122`. The
heads reset alike for either, so the model has one line.

.. _sgi-xbridge-hold:

Main power and reset hold
-------------------------

The chip has two level inputs and one pulse input that a brick's
controller drives, and one output that tells the host whether the chip
answers:

- ``main-power`` (default high) is the board's switched supply;
- ``reset-hold`` (default low) is the controller holding the bridge in
  reset;
- ``reset`` restarts a running chip cold on a rising edge;
- ``host-link`` follows the held state: it goes low when the chip becomes
  held and high when it is released. The chip drives it on each change and
  not at realize, when nothing is connected to it yet, so a consumer joined
  to a chip that is not held keeps its own default of up.

The chip is held while ``main-power`` is low or ``reset-hold`` is high.
Entering the hold restarts the crossbar, both heads and their PCI
devices cold, then refuses every access through the chip's XIO space: a
processor read or write through any widget, and an access that arrives
through an external port's routes, each fail with an access error and log
``LOG_GUEST_ERROR``. With packet transport the chip also fails the
packets it sends on the host's or a widget's behalf: a head's DMA or
interrupt request, and a widget's packet through the crossbar. Without
packet transport, which includes a bridge an I-brick supplies, the heads
send to the host fabric directly and the hold does not gate them; the cold
restart on entering the hold is what stops them, by clearing the heads'
registers and devices. What a held bridge does with a request already on
the wire is *unknown*. Leaving the hold needs no second reset. Both causes must be gone for the chip to answer again. A
``reset`` pulse while held does nothing. The two levels describe wires,
not device state, so a machine reset does not release a hold; they
migrate in a subsection that exists only while one is active.

On an I-brick the controller's ``node-power`` output drives ``main-power``,
its ``reset-hold`` output drives ``reset-hold``, and its ``node-reset``
output drives ``reset``. The L1 starts the brick on with its system, so the
bridge runs from the start; an L1 started off holds it. The chip takes the
L1's levels when the machine is built, and a machine reset leaves them
alone, as it leaves the L1's power state. A migration load takes them from
the L1's saved state, so a brick that was running is not switched off. The
L1's ``reset`` command raises the hold, which restarts the bridge cold, and
releases it; the ``reset`` pulse that follows arrives at a held chip and
does nothing more.

What the controller's reset bits do is graded as follows. That the I-brick
controller holds three bits of one expander low to hold the bridge and
releases two of them, then the third ten ticks later, is *established*;
that low asserts is *inferred*; which XBridge input each bit drives is
*unknown*. The model therefore has one hold that stops the whole chip.
That an unpowered or held bridge answers nothing, so the host reads its
link down, is *inferred*; what a bridge behind a dead link answers on
hardware is *unknown*. Not modelled: the ordering of the three bits'
release, the ten tick gap, and a warm reset sent out of the external
ports on entering the hold.

.. _sgi-xbridge-migration:

Migration
=========

A head's stream carries its register file, translation table, pin levels
and raw INTx levels; the INTx levels are re-driven on load. The
crossbar's stream carries its register file, its status latch, so an
outstanding assertion still clears on the far side, and the reset ignore
window. The chip's stream checks that both sides chose the same mode
and transport. Migration works only between identical builds.

.. _sgi-xbridge-limits:

Model limits
============

- A V-brick projection exists in code but no topology selects it. It
  joins external port ``0xb`` to a second XBridge's host port and
  projects that chip's widget 8 and widget 0 into the first and second
  8 MB of widget ``0xb``. It is a QEMU address choice, not a hardware
  route.
- Only revision 2 is presented. Code paths keyed to other head revisions
  are unreachable.
- Deferred, reading zero and logging ``LOG_UNIMP``: the address-match
  buffers, performance counters, interrupt acknowledge and the 4 MB
  section with the flash behind head F.
- Target flush, aux and response buffer errors, map fault address, PCI
  error, write request buffer flush and read response buffer status read
  zero and log writes as ``LOG_UNIMP``.
- The head raises only the invalid-address, page-fault and empty-slot
  conditions. Reset interrupt status bits 0, 2, 4 and 5 clear nothing,
  and the error command word is never written.
- The crossbar raises only the register access error and never fills
  its error registers. Link training, arbitration, timeouts, performance
  counting, buffer flushing with traffic and widget 0's one-wire pulse
  are not modelled.
- The head NIC record is a placeholder, not IO-8 data.
- No register of this chip has been read on a physical machine for the
  model.

.. _sgi-xbridge-tests:

Tests
=====

- ``tests/qtest/sn1-xbridge-test.c``: the head's register masks, reset
  values and access errors, the translation table and its views,
  configuration and doubleword configuration, PIO placement at every
  width, the ranges above the face and the 64-bit alias width, DMA
  routes, page faults and the revision-2 bit-29 rule, byte placement and
  swap controls, interrupt packets and force registers, unmasking an
  asserted pin, raw INTx outputs, the NIC placeholder, Origin 300
  ``REQ64#`` wiring and revisions, and migration.
- ``tests/qtest/sn1-xbridge-xbow-test.c``: crossbar identity, reset
  values, the source-port field, control and destination, the lock,
  fences, link registers and eight-bit mode, access errors, interrupt
  packets, warm reset, the link reset one-shot and the ignore window, and
  migration.
- ``tests/qtest/sn1-xbridge-chip-test.c``: the chip's widget views,
  routing, fixed destinations, head interrupts and DMA reaching the
  host, warm reset reaching the heads and external ports, external port
  attachment and routes, two chained chips, realize checks, packet
  transport, main power and reset hold, Super Bridge heads and migration. It also drives
  ``sgi-xbridge-assembly-fixture`` from
  ``tests/qtest/xbridge-assembly-fixture.c``: one crossbar chip and two
  Super Bridge chips joined by cables the test chooses, giving six
  heads.
- ``tests/qtest/sgi-pci-expansion-ni-test.c``: the Origin 300 PCI
  expansion placement rules through the routed topology.
- ``tests/qtest/sn1-address-map-test.c``, ``sn1-fuel-test.c`` and
  ``sgi-migration-roundtrip-test.c`` exercise the chip inside the
  Origin 300 and Fuel machines.
