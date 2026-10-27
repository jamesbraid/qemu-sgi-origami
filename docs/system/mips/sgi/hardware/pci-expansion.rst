.. SPDX-License-Identifier: BSD-3-Clause

.. _sgi-pci-expansion:

============================================================
SGI PCI expansion boards and the Origin 300 expansion module
============================================================

This page covers the boards that put PCI slots behind SGI's crosstalk
and NUMAlink fabrics: the Origin 2000 PCI carrier, the one-slot
PCI-XTALK adapter, the Cad Duo input card that fits it, and the Origin
300 PCI expansion module with its NUMAlink cable connections. QEMU
builds the first two as ``sgi-pci-carrier`` and ``sgi-pci-shoehorn``
(``hw/mips/sgi/board-pci-expansion.c``,
``hw/mips/sgi/board-pci-shoehorn.c``), each a Bridge and PCI bus the
model already has; the Origin 300 module is an isolated experiment
(``hw/mips/sgi/origin300-pci-expansion.c``). Ratings follow
:ref:`sgi-ip30`.

Sources: *Origin2000 Rackmount Owner's Guide* (007-3456-003); *PCI
Expansion Module User's Guide* (007-4499-002) and *Origin 300 System
with NUMAlink Module User's Guide* (007-4498-002); *Origin 3000 System
Architecture* (108-0240-002); real inventories ``web:irixnet-post-32252``,
``web:nekonomicon-16727716``, ``web:nekonomicon-5749`` and
``web:mimgfx-origin2000-inventory``; seller photographs of the adapter
(``web:ebay-277925514107-image-1``, ``-image-3``, ``-image-5``); the SGI
part-number list ``web:sgi-pca-numbers``; Linux and OpenBSD sgi at the
pinned commits given on :ref:`sgi-ip30-sources`.

.. _sgi-pci-expansion-carrier:

PCI carrier assembly (the "shoebox")
====================================

An XIO board that turns one crosstalk slot into three PCI slots.

.. list-table::
   :header-rows: 1
   :widths: 46 12 42

   * - Fact
     - Rating
     - Source
   * - It holds up to three third-party PCI boards, two full-size and one
       half-size, and provides up to 75 W.
     - established
     - 007-3456-003 "PCI Carrier Assembly" and Figure 2-11, printed
       page 34 (PDF 54). Power is outside the emulator's contract.
   * - The "PCI module" goes in XIO slot 2, which is Crossbow 0 widget 12.
     - established
     - 007-3456-003 "XIO Board Slots", printed page 98 (PDF 118); widget
       numbering on :ref:`sgi-xbow-role`.
   * - The module of page 98 is this carrier assembly, its converter is
       a Bridge, and it is the product SGI kernels call the PCI
       "shoebox".
     - inferred
     - The guide uses both names without equating them; it is the only
       PCI option the guide describes for the module, and every SGI
       XIO-to-PCI board with evidence uses a Bridge. OpenBSD
       ``sys/arch/sgi/include/mnode.h`` distinguishes ``IP27_BRD_PCI1``
       (shoebox) from ``IP27_BRD_PCI3`` (shoehorn).
   * - A real PCI XIO board in slot 2 is part ``030-1062-002``, record
       revision C, name ``PCI-XIO``, Bridge revision 3; the PROM types
       it ``PCI_XIO IO Board`` on ``ASIC BRIDGE Rev 3, (widget 12)``.
     - established
     - ``web:irixnet-post-32252`` (``PCI-XIO Board: ... part
       030-1062-002 rev C``, ``Origin PCI XIO board, module 1 slot 2:
       Revision 3``); ``web:nekonomicon-16727716``;
       ``web:sgi-pca-numbers`` lists ``030-1062-002`` as the Origin 2000
       PCI cardcage, PCI_XIO.
   * - This carrier is that PCI_XIO cardcage.
     - inferred
     - Both are the PCI option fitted in XIO slot 2; the guide names no
       part for the carrier.
   * - A slot's INTA and INTC reach Bridge pin ``device``; INTB and INTD
       reach pin ``device ^ 4``.
     - inferred
     - The Bridge's general mapping in the public Linux SN Bridge
       driver (``arch/ia64/sn/io/pcibr.c`` in Linux 2.5.0) and OpenBSD
       ``xbridge_intr_map``; not a carrier capture.
   * - The PCI device number of each slot.
     - unknown
     - No source read. The model selects devices 0, 1 and 2, one per
       slot, and refuses a card elsewhere. A real carrier's ``hinv -v``
       would settle it.

The model reports widget revision 3 but implements Bridge D behaviour;
no difference a guest exercises on this board is known. The identity
record is the PCI_XIO part above with a placeholder barcode, on the
Bridge's one-wire line (:doc:`/system/mips/sgi/hardware/bridge`). The Octane PCI shoebox and
the GIGAchannel XBOX are different products and are not covered.

.. _sgi-pci-expansion-shoehorn:

PCI-XTALK adapter (the "shoehorn")
==================================

A single-XIO-slot board that carries one PCI daughter card.

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
     - established
     - Seller photographs ``web:ebay-277925514107-image-1``, ``-image-3``.
   * - Its converter is a Bridge D, LSI L2A0710 marked SGI part
       ``099-0125-007``, the revision ``sgi-bridge`` models (widget
       revision 4).
     - established
     - ``web:ebay-277925514107-image-5``.
   * - It is a distinct board type from the shoebox.
     - established
     - OpenBSD ``mnode.h``: ``IP27_BRD_PCI3`` "PCI shoehorn".
   * - On an Origin 2000 the board's record name is ``XTALK_PCI`` with
       part ``030-1275-00x``; the PROM prints ``Found an io Board with
       nic 030-1275-00x`` and ``XTALK_PCI IO Board`` on a Bridge Rev 4,
       and prints no board line for it.
     - established
     - ``web:nekonomicon-16727716``; an owner's ``hinv -mv``
       (``web:mimgfx-origin2000-inventory``) lists two shoehorns as
       ``XTALK_PCI Board: part 030-1275-004 rev A`` at
       ``/hw/module/1/slot/io9/xtalk_pci`` and ``030-1275-003 rev A`` at
       ``.../io3/xtalk_pci``.
   * - The platform: the 030-1275 board is a cross-platform XIO adapter
       also used in Octane; the Origin 2000 captures are the strongest
       evidence and an Origin 200 reseller label the weakest. A real
       Origin 200's own Bridge reports ``030-1244-001``, so 030-1275 is
       not the Origin 200 board.
     - inferred
     - The captures above against the reseller listing; a fuller
       capture mapping each nic to a board would settle it.
   * - The card behind the adapter sits at PCI device 1.
     - established
     - A Cad Duo's ``pckb`` and ``pcms`` at
       ``/hw/module/1/slot/io5/xtalk_pci/pci/1/``
       (``web:nekonomicon-5749``) and a SCSI card at
       ``.../io5/xtalk_pci/pci/1/scsi_ctlr/0``
       (``web:nekonomicon-16727716``).
   * - Real shoehorns sat in XIO slot 5 (and, in one owner's listing,
       slot 3). The model fits it in any empty slot; the default
       composition places it at IO2.
     - established / approximation
     - The captures above; the placement at IO2 is a composition
       choice, not a captured position.
   * - The modelled record revision is ``B``.
     - inferred
     - The photographed label; captured records are other dash numbers.
   * - A fitted add-in IOC3's SuperIO interrupt reaches Bridge pin
       ``slot ^ 4``.
     - inferred
     - OpenBSD ``ioc.c`` puts an add-in IOC3's SuperIO on INTB; Linux
       ``pci-xtalk-bridge.c`` ``int_mapping[slot][1] = slot ^ 4`` for
       IP27. Drivers, not a hardware capture.

.. _sgi-pci-expansion-cad-duo:

Cad Duo PCI card
================

The Cad Duo is SGI option board ``030-1155-002``: an IOC3 with Ethernet
and a PS/2 keyboard and mouse pair, no serial ports, clock or parallel
port. It adds an independent second Ethernet and input pair to a
system. The population is the generic "CAD DUO" fall-through of Linux
``drivers/mfd/ioc3.c``, which treats every identity without a dedicated
entry that way (established). A real card reports record name
``PCI_ENET``, part ``030-1155-002``, revision D, at its own PCI vertex
(established, ``web:mimgfx-origin2000-inventory``, ``PCI_ENET Board:
barcode HKV898 part 030-1155-002 rev D`` at
``/hw/module/1/slot/io3/xtalk_pci/pci/1``). The record is read from the
card's own IOC3 one-wire bus, not the adapter's Bridge line (:doc:`/system/mips/sgi/hardware/ioc3-boards`).
The card's PCI subsystem identity, physical connectors and bill of
materials are unknown; the model uses a placeholder subsystem id and the
IOC3 revision 1 of :doc:`/system/mips/sgi/hardware/ioc3`.

.. _sgi-pci-expansion-origin300:

Origin 300 PCI expansion module
===============================

Facts in this section are from 007-4499-002 and 007-4498-002 unless
noted; page numbers are printed pages.

.. list-table::
   :header-rows: 1
   :widths: 46 12 42

   * - Fact
     - Rating
     - Source
   * - The module adds six PCI buses of two slots each to an Origin 300
       system. Its one Bedrock interface card (BIC) has one external
       NUMAlink connector; the BIC's Bedrock uses its network interface
       for that link and its I/O interface for the PCI motherboard. Its
       processor interfaces and memory are unused: it has no processors
       or memory.
     - established
     - 007-4499-002 pp. 38-40 and 50.
   * - Direct connection: BIC network interface to one compute module's
       LINK connector, by a one-metre cable within 8U.
     - established
     - 007-4499-002 pp. 12, 36 and 52.
   * - Routed connection: BIC to port 2, 3, 4 or 5 of a NUMAlink module;
       the module does not function on port 1, 6, 7 or 8. Those ports
       carry USB for communication with the rack L2 controller.
     - established
     - 007-4499-002 p. 52; 007-4498-002 pp. 10-11, 96 and 98. The failure
       mechanism on the other ports is unknown.
   * - The NUMAlink module has eight ports; up to four expansion modules
       are permitted in specified rack configurations.
     - established
     - 007-4498-002 pp. 10-11, 96 and 98.
   * - The Origin 3000 P-brick is a different product: it has a host
       interface card, uses crosstalk and connects to a C-brick.
     - established
     - 007-4499-002 pp. 37-38.
   * - The BIC's Bedrock II connects to XBridge U0 on the PCI
       motherboard; U0 controls buses 3 and 4 and links to U1 (buses 1
       and 2) and U2 (buses 5 and 6). The XBridges are 600 MHz, each bus
       66 MHz, each slot 33 or 66 MHz with 32 or 64 bit addressing.
     - established
     - 007-4499-002 pp. 39-41, Figures 3-5 and 3-6 (U0 port ``0xa`` to
       BIC; U0 ``0xd`` to U2 ``0xd``, U0 ``0xc`` to U2 ``0x8``, U0
       ``0x9`` to U1 ``0x8``, U0 ``0x8`` to U1 ``0xd``; U0/U1/U2 ``0xe``
       and ``0xf`` to buses 4/3, 2/1, 6/5). Whether U0 port ``0xb`` is
       unused is unknown.
   * - The six bus paths pair with XBridge widgets 8, 9, 12, 13, 14 and
       15, widget 8 as bus 2, 9 as bus 1, 12 as bus 5, 13 as bus 6, 14
       as bus 4 and 15 as bus 3.
     - inferred
     - A public owner inventory (a BIC ``030-1740-001`` rev F and a
       PEBRICK motherboard ``030-1724-001`` rev E under those widget
       paths, 0 MB of memory and no processors in the module) combined
       with the widget-to-bus table of the Linux SN1 driver
       (``linux-history:7570df54ef8c`` ``arch/ia64/sn/io/sn1/pcibr.c``
       lines 262-272) and the wiring above. It
       is not a six-bus register scan.
   * - Chip composition: U0 in XBridge mode, U1 and U2 in Super Bridge
       mode.
     - approximation
     - Chosen so the public wiring and widget set are reproduced; the
       XBridge page (:doc:`/system/mips/sgi/hardware/xbridge`) states both modes. The BIC's
       Bedrock revision, silicon identity and reset values are
       unknown; a revision-matched capture would settle them.
   * - A serial ID EEPROM on the board carries component information;
       its contents, attachment and read path are unknown. A separate L1
       controller monitors the module, runs on 12 V standby and
       controls PCI slot power from 48 V.
     - established / unknown
     - 007-4499-002 pp. 36, 46, 48 and 50. The L1 protocol, USB
       transport and slot-power effects visible to firmware are
       unknown.

The BIC announces itself to a compute node by NUMAlink with no
published packet encoding, address map or discovery record. The model's
direct-link experiment (compute LINK to the BIC's network interface
through a synchronous address-space route, selected explicitly by the
caller) is therefore a *functional approximation*: it can reproduce the
guest-visible read, write, DMA and interrupt transactions to the six
bridge heads, but it does not reproduce packet commands, credits,
ordering, timeouts or retry. The PI presence words of the BIC are
exposed as absent processors by the model's reuse of the Hub processor
interface policy; the hardware's value is unknown. A routed
configuration through a NUMAlink module has no model.
