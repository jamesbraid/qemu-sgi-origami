.. SPDX-License-Identifier: BSD-3-Clause

.. _sgi-pci-expansion:

============================================================
SGI PCI expansion boards and the Origin 300 expansion module
============================================================

These boards put PCI slots behind the SGI XIO and NUMAlink fabrics. This
page covers the Origin 2000 XIO PCI expansion board and its PCI Carrier
Assembly, the PCI-XTALK adapter (the "shoehorn") with the Cad Duo card that
fits it, and the Origin 300 PCI expansion module.

QEMU builds the first three boards as ``sgi-pci-expansion``,
``sgi-pci-carrier`` and ``sgi-pci-shoehorn``
(``hw/mips/sgi/board-pci-expansion.c`` and
``hw/mips/sgi/board-pci-shoehorn.c``). Each is an XIO board that holds one
Bridge and its PCI bus (:doc:`bridge`). The Cad Duo is an ``sgi-ioc3``
(:doc:`ioc3`). The Origin 300 module is
``sgi-origin300-pci-expansion-module``
(``hw/mips/sgi/origin300-pci-expansion.c``): a Bedrock with no processors or
memory and three XBridge chips that serve six PCI buses (:doc:`bedrock`,
:doc:`xbridge`). The model uses Bedrock and XBridge revision 2 for the
module, Bridge widget revision 3 for the carrier and Bridge widget revision
4 for the shoehorn. Facts are rated as described in :ref:`sgi-ratings`.

Sources
=======

- *Origin2000 Rackmount Owner's Guide*, :src:`007-3456-003`, for the
  carrier and the PCI option slot.
- *PCI Expansion Module User's Guide*, :src:`007-4499-002`, and *Origin 300
  System with NUMAlink Module User's Guide*, :src:`007-4498-002`, for the
  Origin 300 module.
- Real Origin 2000 inventories: :src:`irixnet-post-32252`,
  :src:`nekonomicon-16727716`, :src:`nekonomicon-5749` and
  :src:`mimgfx-origin2000-inventory`. A published Origin 300 inventory with
  an expansion module, :src:`nekonomicon-11542`.
- Seller photographs of a PCI-XTALK adapter, :src:`ebay-277925514107-image-1`,
  :src:`ebay-277925514107-image-3` and :src:`ebay-277925514107-image-5`.
- A published SGI part-number list, :src:`sgi-pca-numbers`, and the IP27
  PROM's board part-number table, :src:`ip27-prom-6.156`.
- Linux ``arch/mips/pci/pci-xtalk-bridge.c`` and ``drivers/mfd/ioc3.c`` at
  ``db2ddb871435``, OpenBSD sgi ``sys/arch/sgi/include/mnode.h``,
  ``sys/arch/sgi/pci/ioc.c`` and ``sys/arch/sgi/xbow/xbridge.c`` at
  ``2df91d80c104``, and the Linux SN1 ``arch/ia64/sn/io/sn1/pcibr.c`` at
  ``linux-history:7570df54ef8c``, as software expectations.

No physical machine was measured for these boards (see `Measurements`_).

XIO PCI boards
==============

``sgi-pci-expansion`` is the base class. ``sgi-pci-carrier`` is a subclass
that sets the carrier's identity as defaults. ``sgi-pci-shoehorn`` is a
separate board class with a fixed identity. All three fit an empty XIO slot
named with ``slot=``, for example ``-device sgi-pci-carrier,slot=io2``. The
machine gives the board the link to that slot's Crossbow port. A slot the
topology does not name, and a slot that is already filled, are refused. A
processor reaches the board's Bridge through its Crossbow, and the Bridge
sends DMA and interrupt packets back out over the same link.

The board names its PCI bus with the ``pci-bus-name`` property, or
``<id>.0`` from its ``id``. The tests address the board fitted at IO2 as
``bus=io2.0``.

``sgi-pci-expansion`` passes the properties ``board-part``,
``board-barcode``, ``board-revision``, ``board-name`` and
``bridge-revision`` to its Bridge. ``board-part`` must be exactly 12
characters. A bare ``-device sgi-pci-expansion`` keeps the placeholder part
``NOT-OBSERVED``, the Bridge's older text identity page and the Bridge's
default revision. No source gives such a board an identity, so it is an
*approximation* that exists to hold cards. When ``board-name`` is set and
``board-barcode`` is not, the barcode is a marked synthetic one made from
the letter ``O`` and the ``onewire-serial`` property. The record layout is
on :doc:`bridge`. ``slots=N`` limits PCI devices to 0 through N-1 and
refuses a card at any other device when it is realized. ``slots=0``, the
default, leaves all eight Bridge device positions open. A value above 8 is
refused.

A card's second interrupt follows the Bridge's line rule. INTA and INTC of
a device reach Bridge pin *device*, and INTB and INTD reach pin
*device* ^ 4 (:doc:`bridge`). Both OpenBSD ``xbridge_intr_map`` and Linux
``pci-xtalk-bridge.c`` (``int_mapping[slot][1]``) use that rule, a
*software expectation*. An add-in IOC3 raises its SuperIO interrupt on
INTB: OpenBSD ``ioc.c`` takes it from INTB for an add-in chip and from INTD
for an on-board one, also a *software expectation*. The IOC3 sends the
SuperIO interrupt out of a separate output, so the board connects that
output to Bridge pin *device* ^ 4. ``sgi-pci-expansion`` does this for each
``sgi-ioc3`` on its bus when machine creation finishes, and leaves an
output that is already connected. ``sgi-pci-shoehorn`` does it for the card
it fits. Without the connection, a keyboard byte latches in the chip and no
interrupt reaches the guest. No capture shows a real card's interrupt
wiring, so the connection is *inferred*.

.. _sgi-pci-expansion-carrier:

PCI Carrier Assembly
====================

The PCI Carrier Assembly turns one XIO slot into PCI slots for third-party
cards. QEMU builds it as ``sgi-pci-carrier``.

.. list-table::
   :header-rows: 1
   :widths: 46 12 42

   * - Fact
     - Rating
     - Source
   * - The carrier holds up to three third-party PCI boards, two full-size
       and one half-size, and provides up to 75 W.
     - *established*
     - :src:`007-3456-003 "PCI Carrier Assembly", Figure 2-11, printed
       p. 34`. The model has no power or board length.
   * - The "PCI module" goes in XIO slot 2, which is Crossbow 0 widget
       12.
     - *established*
     - :src:`007-3456-003 "XIO Board Slots", printed p. 98`. Widget
       numbering is on :doc:`xbow`.
   * - The "PCI module" of page 98 is this carrier, its converter is a
       Bridge, and SGI kernels call the product the PCI "shoebox".
     - *inferred*
     - The guide uses both names without equating them. It describes no
       other PCI option for the module, and every SGI XIO-to-PCI board
       with evidence uses a Bridge. OpenBSD ``mnode.h`` separates
       ``IP27_BRD_PCI1`` ("PCI shoebox") from ``IP27_BRD_PCI3`` ("PCI
       shoehorn").
   * - A real PCI XIO board in slot 2 has part ``030-1062-002``, record
       revision C and name ``PCI-XIO`` on a Bridge of revision 3. The
       PROM lists it as ``PCI_XIO IO Board`` on ``ASIC BRIDGE Rev 3
       (widget 12)``. IRIX prints ``Origin PCI XIO board, module 1 slot
       2: Revision 3``.
     - *observed*
     - :src:`irixnet-post-32252` and :src:`nekonomicon-16727716`. The
       part list names ``030-1062-002`` the Origin 2000 PCI cardcage,
       PCI_XIO :src:`sgi-pca-numbers`. The PROM's table types the
       ``030-1062-`` prefix as a PCI board :src:`ip27-prom-6.156`.
   * - This carrier is that PCI_XIO cardcage.
     - *inferred*
     - Both are the PCI option fitted in XIO slot 2. The guide names no
       part for the carrier.
   * - Every PCI_XIO board has a Bridge of revision 3.
     - *unknown*
     - Two captured boards report 3. More captures would settle it.
   * - The PCI device number of each physical slot.
     - *unknown*
     - No source gives it. A real carrier's PCI configuration reads, or
       its PROM inventory with cards fitted, would settle it.

The model builds a Bridge at the slot's widget with ``slots=3``, so PCI
devices 3 and up are reserved and a card there is refused when it is
realized. Devices 0, 1 and 2, one per physical slot, are an
*approximation*. The Bridge's widget identification register reports
revision 3 while the chip behaves as revision D. No difference a guest
exercises on this board is known.

The identity record on the Bridge's one-wire part has part
``030-1062-002``, revision ``C`` and name ``PCI-XIO``, with a marked
synthetic barcode. The part, revision and name are *observed* on real
boards. That this carrier carries them is *inferred*. The PROM's table
types the prefix as a PCI board, which is why a real board produces no
"Found an io Board" line for the part.

The Octane PCI shoebox is a different product for a different machine
family. The Origin 200 external expansion cabinet is a different product
too (:doc:`gigachannel`).

.. _sgi-pci-expansion-shoehorn:

PCI-XTALK adapter (the shoehorn)
================================

The adapter is a single-slot XIO board that carries one PCI card. QEMU
builds it as ``sgi-pci-shoehorn``.

.. list-table::
   :header-rows: 1
   :widths: 46 12 42

   * - Fact
     - Rating
     - Source
   * - The board is silkscreened "PCI-XTALK ADAPTER", SGI assembly
       ``030-1275-002`` revision B (also ``030-1275-004`` revision A),
       copyright 1997, with one Burndy card-edge connector for a PCI
       daughter card.
     - *established*
     - :src:`ebay-277925514107-image-1` and
       :src:`ebay-277925514107-image-3`.
   * - Its converter is a Bridge D, LSI L2A0710, marked SGI part
       ``099-0125-007``, widget revision 4.
     - *established*
     - :src:`ebay-277925514107-image-5`.
   * - It is a different board type from the shoebox.
     - *software expectation*
     - OpenBSD ``mnode.h``: ``IP27_BRD_PCI3`` "PCI shoehorn".
   * - On an Origin 2000 its record name is ``XTALK_PCI`` with part
       ``030-1275-00x``. The PROM prints ``Found an io Board with nic
       030-1275-00x`` and lists ``XTALK_PCI IO Board`` on a Bridge Rev 4.
       An owner's listing has two, parts ``030-1275-004`` rev A and
       ``030-1275-003`` rev A.
     - *observed*
     - :src:`nekonomicon-16727716` and
       :src:`mimgfx-origin2000-inventory`.
   * - The PROM prints no board line for it.
     - *observed*
     - The same captures. The PROM's table has no ``030-1275-`` prefix
       :src:`ip27-prom-6.156`.
   * - The card behind the adapter is at PCI device 1.
     - *observed*
     - A Cad Duo's keyboard and mouse at
       ``/hw/module/1/slot/io5/xtalk_pci/pci/1`` :src:`nekonomicon-5749`
       and a SCSI card at ``.../io5/xtalk_pci/pci/1/scsi_ctlr/0``
       :src:`nekonomicon-16727716`.
   * - Real adapters sat in XIO slot 5, and in one listing slots 3 and 9.
     - *observed*
     - The captures above.
   * - The 030-1275 board is a cross-platform XIO adapter that Octane also
       used. The Origin 2000 captures are the strongest placement. A
       reseller labelled one photographed unit Origin 200.
     - *inferred*
     - A real Origin 200's Bridge reports ``030-1244-001``, so 030-1275 is
       not that machine's own board. A capture that maps each part to its
       board would settle the platform.
   * - The modelled record revision is ``B``.
     - *inferred*
     - It is the photographed label. The captured records are other dash
       numbers.

The model builds a Bridge of widget revision 4 at the slot's widget. The
identity record has part ``030-1275-002``, revision ``B``, name
``XTALK_PCI`` and a marked synthetic barcode, so the PROM and IRIX type
the board as the captures show. The board fits any empty XIO slot. The
captures place real units in slot 5 and the tests fit it at IO2 and IO4,
so the slot is a choice, not a claim about a given unit.

The adapter has one PCI position. ``cad-duo-slot=N`` fits a Cad Duo at
device N and ``sio-slot=N`` fits SGI's dual-serial card
(:ref:`sgi-ioc3-boards-dual-serial`). Setting both is refused, and a
slot number of 8 or more is refused. The captures put the card at device 1,
so ``cad-duo-slot=1`` matches them. ``cad-duo-onewire-serial`` and
``sio-onewire-serial`` set the fitted card's serial. Their defaults are
marked synthetic. A card added with ``-device`` on the adapter's bus is
not stopped, so the one-position limit applies to the fitting properties
only.

A fitted card's SuperIO output goes to Bridge pin *slot* ^ 4, the pin the
Bridge routes INTB to. The Ethernet keeps INTA, which reaches pin *slot*.
The IRIX 6.5.30 kernel enables both pins for a Cad Duo at device 1, pins 1
and 5, a *software expectation*. The wire is *inferred* from the drivers
above, as for every board here. A guest services INTA with the Ethernet
handler alone, so a SuperIO source on INTA would never clear. The tests
check that a keystroke leaves INTA low. The interrupt then takes the
Bridge to Crossbow to Hub path of any card interrupt (:doc:`hub`).

.. _sgi-pci-expansion-cad-duo:

Cad Duo PCI card
================

The Cad Duo is SGI option board ``030-1155-002``. It is a PCI card with one
IOC3 that adds a second Ethernet interface and a PS/2 keyboard and mouse
pair. QEMU has no Cad Duo type of its own: it is an ``sgi-ioc3`` with the
card's board properties, which the shoehorn sets when ``cad-duo-slot`` is
given. The chip registers are the IOC3's (:doc:`ioc3`).

.. list-table::
   :header-rows: 1
   :widths: 46 12 42

   * - Fact
     - Rating
     - Source
   * - A real card reports record name ``PCI_ENET``, part
       ``030-1155-002``, revision D, at its own PCI vertex below its
       adapter's ``XTALK_PCI`` record.
     - *observed*
     - :src:`mimgfx-origin2000-inventory`
       (``/hw/module/1/slot/io3/xtalk_pci/pci/1``).
   * - A "CadDuo" PCI keyboard and mouse card served as a second seat's
       input in an Origin 2000.
     - *observed*
     - An owner's report, :src:`nekonomicon-5749`.
   * - The card's IOC3 has Ethernet, keyboard and mouse, and no serial
       ports, clock or parallel port.
     - *software expectation*
     - OpenBSD ``ioc.c`` enables keyboard, mouse and Ethernet for the
       ``030-1155-`` prefix. Linux ``drivers/mfd/ioc3.c`` gives any
       unlisted identity the same "CAD DUO" population. The IRIX 6.5.30
       kernel enables Ethernet and keyboard and mouse for that prefix
       (:ref:`sgi-ioc3-boards-population`).
   * - The record is read from the card's own IOC3 one-wire bus, not the
       adapter's Bridge line.
     - *software expectation*
     - OpenBSD reads it from the chip's bus. The IRIX 6.5.30 kernel reads
       a record from a part of family ``0x0b`` or ``0x09`` and ignores
       the station-address part.
   * - The card's bus holds a record part of family ``0x0b`` beside the
       station-address part.
     - *inferred*
     - Both readers accept family ``0x0b``. A one-wire search on a real
       card would settle the part list.
   * - The card's PCI subsystem identity, connectors and bill of
       materials.
     - *unknown*
     - No capture or photograph gives them.

The model gives the card subsystem id ``0xffff``, an *approximation* that
stands for the uncaptured value. No board in :doc:`ioc3-boards` uses
``0xffff``, so the chip's fall-through selects the Cad Duo population:
Ethernet with its one-wire station-address part, and the PS/2 keyboard and
mouse. Serial A and B, the clock and the parallel port are absent. The
chip is IOC3 PCI revision 1. Ethernet DMA is the IOC3's own and does not
depend on the slot.

The shoehorn gives the card its board record on the IOC3's one-wire bus.
A bare ``-device sgi-ioc3`` carries none. The record part uses the DS2505
command set the Bridge record parts use (:doc:`ioc3`). The part's ROM
identity has family ``0x0b`` and a marked synthetic serial. The
station-address part beside it keeps the IOC3's UniqueWare code. The
record uses the layout every SGI record shares (:doc:`bridge`):

.. list-table::
   :header-rows: 1
   :widths: 14 24 62

   * - Bytes
     - Field
     - Value
   * - 0
     - format
     - ``0x01``
   * - 1-10
     - barcode
     - ``EMU``, the letter ``P`` and a decimal instance number, padded
       with spaces. A real barcode names one unit, so this is marked
       synthetic.
   * - 11-29
     - first part
     - ten spaces and ``030-1155-``
   * - 30-31
     - check
     - computed from the barcode
   * - 32-37
     - dash number
     - three spaces and ``002``
   * - 38-41
     - revision
     - ``D`` and three spaces
   * - 42-47
     - group, capability, variety
     - ``0xff``
   * - 48-61
     - name
     - ``PCI_ENET`` and six spaces
   * - 62-63
     - check
     - computed

The part, revision and name are *observed*. The padding and the barcode are
an *approximation*. Memory past byte 63 reads unprogrammed.

The card's keyboard and mouse take host input like every ``sgi-ioc3``
does. QEMU delivers a host event to every PS/2 device that registered, so
the onboard chip and a Cad Duo on one machine both receive the host
keystrokes. Their registers stay independent.

.. _sgi-pci-expansion-origin300:

Origin 300 PCI expansion module
===============================

The module adds six PCI buses to an Origin 300 over NUMAlink. QEMU builds
it as ``sgi-origin300-pci-expansion-module``. The type is not
user-creatable. The ``origin300`` machine builds it from its
``origin300-pci-expansion`` topology (:doc:`/system/mips/sgi/sn`). Page
numbers below are printed pages.

.. list-table::
   :header-rows: 1
   :widths: 46 12 42

   * - Fact
     - Rating
     - Source
   * - The module adds six PCI buses of two slots each. Its one Bedrock
       interface card (BIC) has one external NUMAlink connector. The
       BIC's Bedrock uses its network interface (NI) for that link and
       its I/O interface (II) for the PCI motherboard. Its processor
       interfaces and memory interface are unused, because the module
       has no processors or memory.
     - *established*
     - :src:`007-4499-002 pp. 38-40, 50`.
   * - Direct connection: BIC NI to one compute module's LINK connector,
       by a one-metre cable within 8U.
     - *established*
     - :src:`007-4499-002 pp. 12, 36, 52`.
   * - Routed connection: BIC to port 2, 3, 4 or 5 of a NUMAlink module.
       The module does not function on ports 1, 6, 7 or 8, which carry
       USB for the rack L2 controller.
     - *established*
     - :src:`007-4499-002 p. 52` and
       :src:`007-4498-002 pp. 10-11, 96, 98`. Why the other ports fail is
       *unknown*.
   * - The NUMAlink module has eight ports. Up to four expansion modules
       are permitted in specified rack configurations.
     - *established*
     - :src:`007-4498-002 pp. 10-11, 96, 98`.
   * - The Origin 3000 P-brick is a different product, with a host
       interface card and Crosstalk.
     - *established*
     - :src:`007-4499-002 pp. 37-38`.
   * - The BIC's Bedrock II connects to XBridge U0 port ``0xa``. U0
       controls buses 3 and 4 and links to U1 (buses 1 and 2) and U2
       (buses 5 and 6). The XBridges are 600 MHz, each bus is 66 MHz and
       each slot takes a 33 or 66 MHz card with 32 or 64 bit addressing.
     - *established*
     - :src:`007-4499-002 pp. 39-41, Figures 3-5 and 3-6`: U0 ``0xd`` to
       U2 ``0xd``, U0 ``0xc`` to U2 ``0x8``, U0 ``0x9`` to U1 ``0x8``,
       U0 ``0x8`` to U1 ``0xd``, and the ``0xe`` and ``0xf`` heads of
       U0, U1 and U2 to buses 4/3, 2/1 and 6/5. Whether U0 port ``0xb``
       is unused is *unknown*.
   * - The module's six bus paths are XBridge widgets 8, 9, 12, 13, 14
       and 15, under a BIC part ``030-1740-001`` revision F and a
       motherboard ``030-1724-001`` revision E, with 0 MB and no
       processors in the module.
     - *observed*
     - :src:`nekonomicon-11542`.
   * - Widget 8 is bus 2, 9 is bus 1, 12 is bus 5, 13 is bus 6, 14 is
       bus 4 and 15 is bus 3.
     - *inferred*
     - The widget-to-bus table of the Linux SN1 driver
       (``linux-history:7570df54ef8c``, ``arch/ia64/sn/io/sn1/pcibr.c``
       lines 262-272) combined with the wiring above. It is not a
       register scan of six buses.
   * - Each slot's data width, including which slots wire REQ64#.
     - *unknown*
     - The guide's 512 MB/s peak per bus suggests 64-bit data phases,
       which is *inferred*. The guide states only 32 or 64 bit
       addressing. A board wiring document would settle it.
   * - A serial ID EEPROM holds component information. A separate L1
       controller monitors the module, runs on 12 V standby and controls
       PCI slot power from 48 V.
     - *established* / *unknown*
     - :src:`007-4499-002 pp. 36, 46, 48, 50`. The EEPROM contents and
       read path, the L1 protocol and the effect of slot power on
       firmware are *unknown*.

Chip composition
----------------

The model builds one Bedrock, the BIC, with no processors, no memory and no
installed banks. It also builds three XBridges. The BIC's II reaches U0
port ``0xa``. U0 runs in XBridge mode, which gives it the crossbar and two
local PCI heads, E and F. U1 and U2 run in Super Bridge mode. The
properties ``chip1-e-port``, ``chip1-f-port``, ``chip2-e-port`` and
``chip2-f-port`` name the U0 port that links to U1 E, U1 F, U2 E and U2 F.
Each must be 8, 9, ``0xc`` or ``0xd`` and no two may match. The machine's
default composition uses 8, 9, ``0xc`` and ``0xd`` in that order. The six
PCI heads, in model order, are U0 E and F, U1 E and F, and U2 E and F.
Each head is a PCI bus with devices 1 and 2.

The composition is an *approximation* chosen to reproduce the public wiring
and the six-widget set. It differs from the inferred widget-to-bus table
for U2. The table gives widget 12, bus 5, to U2 F and widget 13, bus 6, to
U2 E, while the default pairs U2 E with ``0xc``. The pairing is the
caller's choice and the tests change it. Which pairing a real module has is
*unknown*.

Bedrock revision 2 and XBridge revision 2 are *inferred*. The published
inventory shows a Hub revision 2 on the attached compute module and no
silicon revision for the BIC or the XBridges, and board revisions F and E
do not give ASIC revisions. The BIC Bedrock's revision, silicon identity
and reset values on this board are *unknown*. A revision-matched capture
would settle them. The module refuses to realize unless both revisions are
2, the placement and node number are below 128 and ``bus-prefix`` is set.
The prefix names the buses ``<prefix>u<n>-head-e`` and
``<prefix>u<n>-head-f``. The BIC accepts inbound traffic only from widgets
8, 9, 12, 13, 14 and 15.

Slot width and speed
--------------------

Each head's PCI speed field is 1, which the tests read as a 66 MHz head.
Both devices of every head request 64-bit data phases. A card without a
64-bit interface stays 32-bit and runs as a 33 MHz peer. The wide
data-placement rules for the Origin 300 apply to U0 head E only
(:doc:`xbridge`). The width of the real slots is *unknown*, so wide slots
are an *approximation* that the module selects.

PCI interrupt wiring
--------------------

A board map routes each head's 32 raw PCI interrupt outputs to the head's
eight interrupt inputs. The default composition maps, on every head, device
1 pins A, B, C and D to inputs 1, 5, 1 and 5, and device 2 pins A, B, C and
D to inputs 2, 6, 2 and 6. That is the Bridge's rule (INTA and INTC to
*device*, INTB and INTD to *device* ^ 4) applied to device numbers 1 and 2
(:doc:`bridge`). The Linux SN1 driver gives the same default rule
(``linux-history`` ``arch/ia64/sn/io/sn1/pcibr.c``). The numbers 1 and 2
do not correspond to physical slot 1 and slot 2, and the electrical wiring
of the module is *unknown*, so the map is an *approximation*.

Contributors combine with logical OR, so an input stays high until every
contributor mapped to it is low. A device index other than 1 or 2 drives
no input and logs ``LOG_UNIMP`` when it asserts. Reset clears every
output. The head's own enable, pending and destination registers then
apply as they do on any XBridge (:doc:`xbridge`).

NI link
-------

The BIC's NUMAlink traffic has no published packet encoding, address map or
discovery record. The model connects the BIC to its compute module with a
synchronous address-space route. This is an *approximation*. It reproduces
the guest-visible reads, writes, DMA and interrupt packets between the
compute node and the six heads. It does not model packet commands,
credits, ordering, timeouts, retry or the BIC's routing tables. A routed
connection through a NUMAlink module has no model.

A processor reaches the BIC's register block, its I/O space and the views
of its three XBridges through the node number the machine assigns. A
transaction for a node number that does not resolve, or for an address
outside the windows, fails with a decode error and reaches no other node.
Without the NI route, DMA from a head logs ``LOG_GUEST_ERROR``, names the
missing data route and fails with a decode error.

Processor presence and pending registers
----------------------------------------

The BIC has no processors, so both processor interfaces report absence
through the registers described in :ref:`sgi-bedrock-pi-presence`. When the
machine selects it, those two words are readable through the BIC's remote
register window as eight-byte aligned accesses. Whether a real BIC answers
the same values is *unknown*, and a revision-matched register capture would
settle it. Other processor-interface registers return a decode error.

A second selection exposes a bounded set of processor-interface registers
(offsets ``0x10``, ``0x90``, ``0x98``, ``0xa0`` and ``0x180`` of each
interface) to two or four compute origins. Another origin is refused with
an access error, and an access that is not an aligned eight-byte transfer
logs ``LOG_GUEST_ERROR``. These registers are an *approximation*. No source
says how a real BIC answers them.

Unconnected UART
----------------

When the machine selects it, the BIC's Hub-special window holds a 16550
register bank, eight registers with a register shift of 3, big-endian,
with no backend. A transfer that is not an aligned eight-byte access logs
``LOG_GUEST_ERROR`` and fails. Only the first two or four compute origins,
as selected, reach it. Without the selection, the window returns a decode
error. No source says a real BIC carries a UART, so this is an
*approximation* with no data path.

Configuration storage
---------------------

When the caller binds a raw image, the module serves two 8 MiB CFI flash
positions in the BIC's boot address class. The window is 128 MiB at
node-special offset ``0x38000000``, and the second position sits
``0x2000000`` above the first. The image must be a writable, regular,
unfiltered raw file of exactly 16 MiB, and the module keeps it reserved.
Reads of 1, 2, 4 and 8 bytes work. Only 8-byte writes work, and any other
size logs ``LOG_GUEST_ERROR`` and fails. With no image the window returns
a decode error. The model supplies no image and no contents. No source says
a BIC carries flash or gives its size. The machine page describes the
launch option (:doc:`/system/mips/sgi/sn`).

Main power
----------

The module can take its main power from the L1 that the machine binds to
it (:doc:`l1`). Only that L1 can change the state. The change completes
synchronously, and a request for the current state does nothing. While
power is off, accesses through the module's windows and the BIC's register
blocks fail with an access error, and processor transfers log
``LOG_GUEST_ERROR``. Chip state is kept across the change and power-up
applies no reset. On power-up the XBridges re-evaluate their pending
interrupt status against the current masks, so an input held high while off
is delivered once and an unchanged input does not repeat. This models
availability only and claims no rail, timing, power-good or reset
behaviour, an *approximation*. Slot power, the standby supply and hot-plug
are not modelled.

Reset and migration
-------------------

Reset restores main power to its initial state, resets the UART and clears
the interrupt contributors and outputs. The chips keep their own reset
behaviour. Migration works only between identical builds, and it refuses a
destination whose selections differ: placement, node number, revisions,
head ports, interrupt map, storage identity and the UART, pending-register,
processor-absence and power selections. A save is refused while a power
change is in progress. After a load the module redrives each head's
interrupt outputs to their saved levels.

Measurements
============

No register on these boards was read on a physical machine. The captures
cited above are published inventories and photographs. They establish
identity records, part numbers, placement and PCI device numbers. They do
not establish any register value, reset value or interrupt wiring.

Model limits
============

- The base ``sgi-pci-expansion`` has no source for its identity. It exists
  to hold cards.
- The carrier takes devices 0, 1 and 2 for its three slots. The real
  numbers are *unknown*. Power, board length and hot-plug are not modelled.
- The shoehorn fits any empty XIO slot and one card through its
  properties. The Cad Duo's subsystem id is a placeholder and its second
  interrupt line is *inferred*.
- A card fitted with ``-device`` on a Bridge bus that does not belong to
  an ``sgi-pci-expansion`` board or a shoehorn gets no SuperIO connection
  from this code.
- A host keystroke reaches every IOC3 keyboard on a machine, so a Cad Duo
  duplicates the onboard chip's input.
- The Origin 300 module is a direct, synchronous link to one compute node.
  It has no routed connection, no packet encoding, no L2 path and no
  discovery record beyond the identity the machine supplies.
- The module's chip revisions, head pairing, slot width, interrupt map,
  UART, flash, processor registers and power are *approximation* or
  *unknown* as stated above.
- A head device index other than 1 or 2 logs ``LOG_UNIMP`` on an interrupt
  and routes nowhere. A DMA the module cannot route logs
  ``LOG_GUEST_ERROR`` and fails.

Tests
=====

``tests/qtest/sn0-pci-carrier-test.c`` fits ``sgi-pci-carrier`` at IO2. It
checks that the widget reads present and alive, that a card answers its PCI
identity at devices 0, 1 and 2, that a card at device 3 is refused, and that
host input reaches an IOC3's keyboard and mouse through the carrier.

``tests/qtest/sn0-pci-shoehorn-test.c`` fits ``sgi-pci-shoehorn`` with a Cad
Duo and with the dual-serial card, and checks a Cad Duo on a plain
expansion board and on the carrier. It covers enumeration, the keyboard
interrupt reaching node 0's Hub on pin *slot* ^ 4 with INTA left low, the
Cad Duo's one-wire record by Search ROM and Match ROM, a bare card with no
record, the dual-serial card's record and serial interrupt, the one-card
limit and a board fitted by slot name.

``tests/qtest/sn0-bridge-identity-test.c`` compares the carrier's and the
shoehorn's Bridge records byte for byte and checks their formatted part,
name and widget revision. ``tests/qtest/sn0-ioc3-test.c`` checks an add-in
IOC3 at a free option slot and the independence of two chips' PS/2 ports.

``tests/qtest/sn1-pci-expansion-module-test.c`` builds the Origin 300
module alone. It covers the population with no processors or memory, the
register views of the six heads, explicit head pairing, a failed DMA, slot
width at both devices on all six heads, reset, migration, and the refusals
for a missing or invalid revision, placement, pairing, port and bus
prefix. ``tests/qtest/sgi-pci-expansion-ni-test.c`` adds the NI route to a
compute node. It covers PIO to each head, DMA and interrupt delivery, the
interrupt map with its OR combination, masking and reset, denied traffic,
the processor-absence and pending-register windows, the UART, the
configuration storage, main power and migration with its mismatch
refusals.

``tests/functional/mips64/test_irix_diag_origin2000_pci_carrier.py`` and
``test_irix_diag_origin2000_pci_shoehorn.py`` boot IRIX on an Origin 2000
with a carrier and a SCSI card, and with a shoehorn and a Cad Duo, and run
the online diagnostics and IOC3 port checks. No functional test boots a
guest with the Origin 300 module.
