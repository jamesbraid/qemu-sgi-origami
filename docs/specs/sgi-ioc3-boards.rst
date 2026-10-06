.. SPDX-License-Identifier: BSD-3-Clause

.. _sgi-ioc3-boards:

===========================================
IOC3-bearing I/O boards and the MENET board
===========================================

The IOC3 is one chip that SGI fitted to many boards, each wiring a
different subset of its functions (:doc:`sgi-ioc3`). This page lists
the IOC3-bearing boards of the ``sgi-sn`` machines, how guests decide each
chip's population, and specifies the MENET XIO board (a Bridge and four
IOC3s giving a module four Fast Ethernet ports and six serial ports) and
SGI's dual-serial PCI card. QEMU builds the MENET as the ``menet``
preset of ``sgi-baseio`` (``hw/mips/sgi/baseio.c``) and the dual-serial
card as the ``sio-slot`` option of ``sgi-pci-shoehorn``. Ratings follow
:ref:`sgi-ip30`.

Sources: *IRIS 4-Port Fast Ethernet Adapter with Asynchronous Serial XIO
Board Installation Instructions* (108-0151-001); an Origin 2000 owner's
``hinv -vm`` with two MENET boards (``web:mimgfx-origin2000-inventory``); an Origin 2000
``hinv -v`` with a MENET in io6 under IRIX 6.5.4m (``web:usenet-7psh8j``); an Onyx2
deskside ``hinv -m`` with a MENET part ``030-0873-004``
(``web:usenet-40c20497``); an SGI engineer's statement that GIGAchannel XIO
slots take MENET boards (``web:usenet-9gjuq1``); a stock list naming the
dual-serial card (``web:usenet-a47377f9``); Linux
``arch/mips/pci/pci-xtalk-bridge.c`` lines 466-545 and
``drivers/mfd/ioc3.c`` lines 388-536 at
``linux:db2ddb87143519e20a95aa36c60b36107b736a58``; OpenBSD sgi
``sys/arch/sgi/pci/ioc.c`` lines 196-250 at
``openbsd-sgi:2df91d80c104636deeef8124252ddd5951b55fbf``.

.. _sgi-ioc3-boards-population:

How a guest decides an IOC3's population
========================================

Linux and OpenBSD select an IOC3's functions from a board identity: Linux
substitutes a subsystem id chosen by the Bridge's board part prefix, then
dispatches on it; OpenBSD reads the board part from the chip's own
one-wire bus, and tests prefixes. The IRIX 6.5.30 kernel selects them with
an eight-bit enable word attached to the chip's PCI slot (bit 0 Ethernet,
3 keyboard and mouse, 4 serial A, 5 serial B, 7 clock and interrupt), set by
a per-board routine matched on the Bridge board record's part prefix
or, for add-in cards, the card's own record. The words and which prefix
sets which, read from the shipped kernel by disassembly, are:

.. list-table::
   :header-rows: 1
   :widths: 30 18 52

   * - Board (prefix)
     - IRIX 6.5.30 kernel word (observed in the shipped binary)
     - Linux and OpenBSD population (public sources)
   * - IP29 system board (``030-1025-``, ``-1244-``, ``-1389-``)
     - ``0xff``
     - Linux ``bridge_setup_ip29_baseio`` (sets Bridge pin 3 for line B).
   * - IP29 system board (``030-1291-``, ``-1308-``)
     - ``0xff``
     - No public source names these prefixes; the kernel observation is
       the only basis.
   * - BaseIO IO6 (``030-1023-``, ``-1124-``)
     - ``0xb7``
     - Linux ``bridge_setup_ip27_baseio``, line B to pin 4: Ethernet,
       serial, clock.
   * - BaseIO-G (``030-0734-``)
     - ``0xbf``
     - Linux ``bridge_setup_ip27_baseio6g``: as IO6 plus keyboard and
       mouse.
   * - MediaIO daughtercard (``030-0880-``)
     - ``0x7a``
     - Linux ``ip27_mio_setup``: serial and keyboard and mouse, no
       Ethernet or clock; line B to pin 6.
   * - MENET (``030-0873-``)
     - ``0x31`` for devices 0-2, ``0x01`` for device 3
     - Linux ``bridge_setup_menet`` (``MENET`` for slots 0-2,
       ``MENET4`` for slot 3); OpenBSD ``ioc.c``: Ethernet, plus serial
       for ``pa_device != 3``.
   * - Cad Duo card (``030-1155-``)
     - ``0x09``
     - OpenBSD ``ioc.c``: keyboard and mouse and Ethernet.
   * - Dual-serial card (``030-1657-``, ``-1664-``)
     - ``0x30``
     - OpenBSD ``ioc.c`` ("PCI_SIO_UFC dual serial board"): serial A
       and B only.

The IRIX words are observations of a shipped binary, not independently
citable: no public source states them. Where Linux and OpenBSD describe
the same board the populations agree; Linux's population for the
BaseIO-G and IP29 omits the parallel port and Linux treats any
unmatched identity as a Cad Duo, so the model follows the owners'
inventories and the IRIX behaviour, not Linux's narrower populations.
The PROM types a board from a part-prefix table (``030-1250-``,
``-1252-`` and ``-1062-`` PCI; ``-0734-`` and the BaseIO prefixes
BaseIO; ``-0872-``, ``-1243-``, ``-1281-`` MSCSI; ``-0873-`` Ethernet
``0x23``; ``-0927-`` Fibre Channel; a ``030-0880-`` record sets a flag
rather than a type), measured from the stock image
(``firmware:ip27-prom-6.156``); Linux's ``klconfig.h`` names type ``0x23``
``KLTYPE_ETHERNET`` with the alias ``KLTYPE_MENET``.

A second SuperIO interrupt line is needed by any function other than
Ethernet. Where a board record declares a second slot, the interrupt comes
from that slot's line; otherwise it is line B of the chip's own slot,
which the Bridge routes to pin ``slot ^ 4`` (inferred: Linux
``int_mapping``, OpenBSD ``xbridge_intr_map``; not a capture).

.. _sgi-ioc3-boards-catalogue:

Board catalogue
===============

.. list-table::
   :header-rows: 1
   :widths: 24 22 54

   * - Board
     - Model
     - IOC3 placement and source
   * - Origin 200 IP29 system board
     - ``sgi-baseio`` preset ``ip29``
     - Device 2, SuperIO on Bridge pin 3 (Linux ``int_mapping[2][1] =
       3``).
   * - Origin 2000 BaseIO (IO6), BaseIO-G, MediaIO
     - presets ``io6``, ``io6g``
     - Device 2, pin 4; MediaIO IOC3 at device 6, pin 6
       (:doc:`sgi-baseio`).
   * - MENET
     - preset ``menet``
     - Devices 0 to 3 (below).
   * - Cad Duo and dual-serial PCI cards
     - ``sgi-pci-shoehorn`` ``cad-duo-slot``, ``sio-slot``
     - The card's slot (:doc:`sgi-pci-expansion`).
   * - Origin 300 IO8, Origin 3000 I-brick IO7, Fuel
     - ``io8`` preset; I-brick and Fuel board record layers
     - Device 4 on the XBridge (Linux ``bridge_setup_io8``, ``_io7``,
       ``_ip34_fuel_sysboard``). The I-brick has a layer but no QEMU
       board.
   * - Octane IP30 system board
     - not built
     - Device 2, pin 4 (Linux ``bridge_setup_ip30_sysboard``); out of
       family.

.. _sgi-ioc3-boards-menet:

MENET
=====

.. list-table::
   :header-rows: 1
   :widths: 46 12 42

   * - Fact
     - Rating
     - Source
   * - One XIO board: a Bridge and four IOC3s on its PCI bus, 128 KB of
       SRAM per Ethernet path. IOC3s 1 to 3 each drive a serial
       transceiver (two ports) and a 10/100 transceiver; IOC3 4 drives
       only a 10/100 transceiver.
     - established
     - 108-0151-001 printed pages 1-6 and 1-7, Figures 1-4 and 1-5.
   * - Four RJ-45 ports labelled ef0 to ef3 and six mini-DIN8 serial
       ports labelled tty_1 to tty_6; each Ethernet port has a socketed
       NIC holding its MAC address.
     - established
     - 108-0151-001 printed pages 1-6 and 1-9 to 1-11.
   * - The IOC3s are at PCI devices 0 to 3, each vendor ``0x10a9`` device
       ``0x0003``.
     - established
     - ``web:usenet-7psh8j`` (``PCI Adapter ID (vendor 4265, device 3)
       pci slot 0`` to ``3`` under ``Origin MENET board``);
       ``web:mimgfx-origin2000-inventory`` (the same four slots under
       each of two MENETs); Linux ``bridge_setup_menet``.
   * - Devices 0 to 2 enable Ethernet and both serial ports; device 3
       enables Ethernet only. No clock, keyboard, mouse or parallel
       port.
     - established
     - Linux ``bridge_setup_menet``; OpenBSD ``ioc.c`` lines 205-216;
       the manual lists no such connectors.
   * - The SuperIO interrupt of device ``d`` (0 to 2) reaches Bridge pin
       ``d + 4``; device 3 raises none; each chip's Ethernet interrupt
       uses its own line A, pin ``d``.
     - inferred
     - Linux's default ``slot ^ 4`` mapping, which it does not override
       for the MENET; the second-slot interrupt IRIX takes agrees. No
       capture shows a MENET interrupt register.
   * - Fits any XIO slot; the manual recommends odd slots before even,
       low before high. A GIGAchannel's XIO slots take MENET boards.
     - established
     - 108-0151-001 printed pages 2-4 and 2-5; ``web:usenet-9gjuq1`` (an
       SGI engineer; stated, not tested).
   * - Board record on the Bridge's one-wire line: part ``030-0873-003``
       with revisions C and J seen, and ``030-0873-004`` rev A on an
       Onyx2; name ``MENET``. The hardware-graph slot name is ``menet``
       and the PROM types it type ``0x23``.
     - established
     - ``web:mimgfx-origin2000-inventory`` (``Location:
       /hw/module/1/slot/io8/menet``, ``MENET Board: barcode HLZ655 part
       030-0873-003 rev J``, io7 ``rev C``); ``web:usenet-40c20497``;
       the PROM prefix table from ``firmware:ip27-prom-6.156``.
   * - The Bridge revision is 3 or 4 (printed as the board revision:
       ``Origin MENET board ... Revision 3`` and ``Revision 4``).
     - established
     - ``web:usenet-7psh8j`` (slot 6, revision 3);
       ``web:mimgfx-origin2000-inventory`` (slots 7 and 8, revisions 3
       and 4).
   * - Whether the real board's IOC3s carry a per-chip one-wire record
       that OpenBSD would find.
     - unknown
     - OpenBSD reads the MENET part from each chip's own bus
       (``ioc.c`` line 206), IRIX does not, and no inventory shows one.
       The model puts none there, so OpenBSD prints an unknown flavour
       for these chips; a real MENET's OpenBSD ``dmesg`` would settle it.
   * - What each chip's own PCI subsystem register returns.
     - unknown
     - Linux substitutes ``MENET``/``MENET4`` ids from the Bridge
       record; the model presents those in the chip's configuration
       space (an emulator choice).

Model choices: ``030-0873-003`` revision J with Bridge revision 4, the
pairing one real board reports, and a placeholder barcode; the
DP83840A transceiver at MII address 31 as on the BaseIO (the real part
is unknown); no Bridge flash (the manual lists none); IOC3 PCI revision
1; each chip's one-wire MAC part serial derived from the catalogue's
synthetic IOC3 serial. The PROM and IRIX lists of the model show the
boards as the real listings do; the serial ports take no host backend
in the catalogue-placed board.

.. _sgi-ioc3-boards-dual-serial:

Dual-serial PCI card
====================

SGI's dual-serial PCI card carries one IOC3 wired for serial ports A and
B only. OpenBSD enables exactly those two for part prefixes ``030-1657-``
and ``030-1664-`` and calls it "PCI_SIO_UFC dual serial board"
(established, ``ioc.c`` lines 235-241). The assembly exists:
a stock list names ``030-1657-003`` "SERIAL I/O CARD (UFC), PCI"
(``web:usenet-a47377f9``). The record revision, subsystem id and the
card's record name beyond OpenBSD's comment are unknown; the model
presents revision ``A``, subsystem ``0xffff`` and name ``PCI_SIO_UFC``
(emulator choices), with the SuperIO output wired to Bridge pin
``slot ^ 4`` (inferred).
