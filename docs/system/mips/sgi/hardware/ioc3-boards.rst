.. SPDX-License-Identifier: BSD-3-Clause

.. _sgi-ioc3-boards:

==============================================
IOC3 boards: IOC3-bearing I/O boards and MENET
==============================================

Used in :ref:`sgi-ip27` (the IP29, BaseIO, BaseIO-G and MENET boards and
the dual-serial PCI card), :ref:`sgi-ip30` (the Octane and Octane2
system board),
:ref:`sgi-ip35` (the Origin 300 IO-8 and the Fuel system board) and the
Origin 3000 I-brick's IO7, which the ``origin3000`` machine builds
(:ref:`sgi-bricks-ibrick`).

.. seealso::

   Platform pages: :ref:`sgi-ip27`, :ref:`sgi-ip30`, :ref:`sgi-ip35`.

SGI fitted one IOC3 chip to many boards, and each board wires up a
different subset of its functions (:doc:`ioc3`). This page lists the
boards that carry an IOC3, says how a guest
decides what each chip fits, and specifies two boards that have no page of
their own: the MENET XIO board, with a Bridge and four IOC3s that give a
module four Fast Ethernet ports and six serial ports, and SGI's
dual-serial PCI card. QEMU builds the MENET as the ``menet`` preset of
``sgi-xio-baseio`` (``hw/mips/sgi/xio-baseio.c`` and
``hw/mips/sgi/baseio.c``) and the dual-serial card as
the ``sio-slot`` property of ``sgi-pci-shoehorn``
(``hw/mips/sgi/board-pci-shoehorn.c``). The chip's population comes from
the ``subsystem-id`` and ``population`` properties of ``sgi-ioc3``
(``hw/misc/sgi-ioc3.c``). Facts are rated as described in
:ref:`sgi-ratings`.

.. _sgi-ioc3-boards-sources:

Sources
=======

- *IRIS 4-Port Fast Ethernet Adapter with Asynchronous Serial XIO Board
  Installation Instructions*, :src:`108-0151-001`.
- An Origin 2000 ``hinv -vm`` with two MENET boards,
  :src:`mimgfx-origin2000-inventory`.
- An Origin 2000 ``hinv -v`` with a MENET in ``io6`` under IRIX 6.5.4m,
  :src:`usenet-7psh8j`.
- An Onyx2 deskside ``hinv -m`` with a MENET, part ``030-0873-004``,
  :src:`usenet-40c20497`.
- An SGI engineer's statement that GIGAchannel XIO slots take MENET
  boards, :src:`usenet-9gjuq1`.
- A stock list that names the dual-serial card, :src:`usenet-a47377f9`.
- The stock IP27 PROM, :src:`ip27-prom-6.156`.
- Linux ``arch/mips/pci/pci-xtalk-bridge.c``, ``drivers/mfd/ioc3.c`` and
  ``arch/mips/include/asm/sn/klconfig.h`` at ``db2ddb871435``, and
  OpenBSD sgi ``sys/arch/sgi/pci/ioc.c`` at ``2df91d80c104``, as software
  expectations.

.. _sgi-ioc3-boards-population:

How a guest decides an IOC3's population
========================================

The three guests choose from different inputs.

Linux substitutes a PCI subsystem id for the chip, chosen by the part
prefix in the Bridge's board record, and dispatches on it
(``drivers/mfd/ioc3.c``, ``pci-xtalk-bridge.c``). The chip has no
subsystem register of its own, *inferred* (:ref:`sgi-ioc3-measurements`),
so there is no id of its own to use. The model matches: its configuration
space repeats 32 bytes, a guest reading ``0x2c`` gets the latency word,
and the ``subsystem-id`` property only keys the population the model
fits.
OpenBSD reads the board part from the chip's own one-wire bus and tests
prefixes (``ioc.c``).

The IRIX 6.5.30 kernel attaches an eight-bit enable word to the chip's PCI
slot instead. Bit 0 enables Ethernet, bit 3 keyboard and mouse, bit 4
serial A, bit 5 serial B and bit 7 the real-time clock and interrupt. A
per-board routine sets it, matched on the part prefix of the Bridge's
board record or, for add-in cards, of the card's own record. The words
below are the stock kernel's behaviour, *software expectation*. No public
source states them.

.. list-table::
   :header-rows: 1
   :widths: 30 18 52

   * - Board (part prefix)
     - IRIX 6.5.30 enable word
     - Linux and OpenBSD population
   * - IP29 system board (``030-1025-``, ``-1244-``, ``-1389-``)
     - ``0xff``
     - Linux sets Bridge pin 3 for line B of the IOC3.
   * - IP29 system board (``030-1291-``, ``-1308-``)
     - ``0xff``
     - No public source names these two prefixes.
   * - BaseIO IO6 (``030-1023-``, ``-1124-``)
     - ``0xb7``
     - Linux, line B to pin 4: Ethernet, serial, clock.
   * - BaseIO-G (``030-0734-``)
     - ``0xbf``
     - Linux: as IO6 plus keyboard and mouse.
   * - MediaIO daughtercard (``030-0880-``)
     - ``0x7a``
     - Linux: serial and keyboard and mouse, no Ethernet or clock; line B
       to pin 6.
   * - MENET (``030-0873-``)
     - ``0x31`` for devices 0 to 2, ``0x01`` for device 3
     - Linux gives slots 0 to 2 the ``MENET`` id and slot 3 ``MENET4``.
       OpenBSD enables Ethernet, plus serial when ``pa_device`` is not 3.
   * - Cad Duo card (``030-1155-``)
     - ``0x09``
     - OpenBSD: keyboard and mouse and Ethernet.
   * - Dual-serial card (``030-1657-``, ``-1664-``)
     - ``0x30``
     - OpenBSD, "PCI_SIO_UFC dual serial board": serial A and B only.

Where Linux and OpenBSD describe the same board, their populations agree.
Linux's population for the BaseIO-G and the IP29 board omits the parallel
port, and Linux treats any unmatched identity as a Cad Duo. The model
follows the board manuals, published inventories and the IRIX words, not
Linux's narrower populations. The IRIX words agree with Linux's prefix
table wherever Linux has the prefix, *inferred*.

The stock PROM types a board from a part-prefix table in the shipped
image :src:`ip27-prom-6.156`, a *software expectation*. ``030-1250-``, ``-1252-`` and ``-1062-``
are PCI boards. ``030-0734-`` and the BaseIO prefixes are BaseIO.
``030-0872-``, ``-1243-`` and ``-1281-`` are MSCSI (:doc:`mscsi`).
``030-0873-`` is type ``0x23``, which Linux ``klconfig.h`` names
``KLTYPE_ETHERNET`` with the alias ``KLTYPE_MENET``. ``030-0927-`` is
Fibre Channel. A ``030-0880-`` record sets a flag on the board and does not
set a type.

A function other than Ethernet needs a second interrupt line. Where the
Bridge's board record declares a second slot for the chip, the IRIX 6.5.30
kernel takes that interrupt from the second slot's line. Otherwise it
takes line B of the chip's own slot, which the Bridge routes to pin
``slot ^ 4``. The exception is a chip with no Ethernet and no
dual-serial card record, which takes line A, as the MediaIO chip does.
The second-slot pins agree with Linux's overrides for the
IP29 board (``int_mapping[2][1] = 3``) and the BaseIO (``4``) and with its
default for the MENET. That the chip's line B reaches ``slot ^ 4`` is
*inferred* from Linux ``int_mapping`` and OpenBSD ``xbridge_intr_map``;
no capture shows it.

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
     - Device 2, SuperIO on Bridge pin 3. Ethernet, both serial ports,
       keyboard and mouse, parallel, one-wire identity and an M48T35
       Timekeeper (Linux ``int_mapping[2][1] = 3``).
   * - Origin 2000 BaseIO (IO6) and BaseIO-G, with the MediaIO
       daughtercard
     - presets ``io6``, ``io6g``
     - Device 2, pin 4. IO6 fits Ethernet, both serial ports, one-wire
       identity and the Timekeeper. BaseIO-G adds keyboard and mouse.
       The MediaIO chip sits at device 6 on pin 6 with serial, keyboard
       and mouse and parallel (:doc:`baseio`).
   * - MENET
     - preset ``menet``
     - Devices 0 to 3 (:ref:`sgi-ioc3-boards-menet`).
   * - Cad Duo and dual-serial PCI cards
     - ``sgi-pci-shoehorn`` properties ``cad-duo-slot`` and ``sio-slot``
     - The card's slot (:doc:`pci-expansion`).
   * - Origin 300 IO8 and Fuel
     - ``sgi-baseio`` presets ``io8`` and ``fuel``
     - Device 4 on the XBridge (Linux ``bridge_setup_io8``,
       ``bridge_setup_ip34_fuel_sysboard``). Both fit Ethernet, both
       serial ports, parallel and an M48T35 Timekeeper, with a DP83843
       transceiver. Fuel also fits keyboard and mouse (:doc:`xbridge`).
   * - Origin 3000 I-brick IO7
     - none
     - Device 4 on the XBridge (Linux ``bridge_setup_io7``). The model has
       no I-brick board.
   * - Octane IP30 system board
     - built by the ``octane`` machine, not an ``sgi-xio-baseio`` preset
       (:doc:`/system/mips/sgi/ip30`)
     - Device 2, pin 4 (Linux ``bridge_setup_ip30_sysboard``). Outside
       the ``sgi-sn`` family.

``sgi-ioc3`` keeps a table from the Linux ids ``0xc300`` (BaseIO-G),
``0xc301`` (MediaIO), ``0xc302`` (BaseIO), ``0xc303`` (IP29), ``0xc304``
(Octane), ``0xc305`` (MENET devices 0 to 2) and ``0xc306`` (MENET device 3)
to populations. It uses the table only when a board leaves the population
automatic. An id the table does not list gets the Cad Duo population, the
fall-through Linux uses. The presets set the population explicitly. They
carry ``0xc303`` on the IP29 chip, ``0xc301`` on the MediaIO chip and
``0xc305`` and ``0xc306`` on the MENET chips. The BaseIO and BaseIO-G
chips carry ``0xffff``, a model value with no hardware counterpart that
no guest reads (*approximation*), because the chip has no subsystem
register (:ref:`sgi-ioc3-measurements`).

.. _sgi-ioc3-boards-menet:

MENET
=====

The MENET is an XIO board with a Bridge and four IOC3s. The
rows below are the board's hardware facts.

.. list-table::
   :header-rows: 1
   :widths: 46 12 42

   * - Fact
     - Rating
     - Source
   * - One XIO board: a Bridge and four IOC3s on its PCI bus, 128 KB of
       SRAM per Ethernet path. IOC3s 1 to 3 each drive a serial
       transceiver (two ports) and a 10/100 transceiver. IOC3 4 drives
       only a 10/100 transceiver.
     - *established*
     - :src:`108-0151-001 pp. 1-6, 1-7, Figures 1-4 and 1-5`.
   * - Four RJ-45 ports labelled ef0 to ef3 and six mini-DIN8 serial
       ports labelled tty_1 to tty_6. Each Ethernet port has a socketed
       NIC holding its MAC address.
     - *established*
     - :src:`108-0151-001 pp. 1-6, 1-9 to 1-11`.
   * - The IOC3s are at PCI devices 0 to 3, each vendor ``0x10a9`` device
       ``0x0003``.
     - *established*
     - :src:`usenet-7psh8j` (``PCI Adapter ID (vendor 4265, device 3)
       pci slot 0`` to ``3`` under ``Origin MENET board``),
       :src:`mimgfx-origin2000-inventory` (the same four slots under each
       of two MENETs) and Linux ``bridge_setup_menet``.
   * - Devices 0 to 2 enable Ethernet and both serial ports. Device 3
       enables Ethernet only. There is no clock, keyboard, mouse or
       parallel port.
     - *software expectation*
     - Linux ``bridge_setup_menet``, OpenBSD ``ioc.c`` and the IRIX
       6.5.30 words above.
   * - The board has no clock, keyboard, mouse or parallel connector.
     - *established*
     - The manual lists none (:src:`108-0151-001`).
   * - The SuperIO interrupt of device ``d`` (0 to 2) reaches Bridge pin
       ``d + 4``. Device 3 raises none. Each chip's Ethernet interrupt
       uses its own line A, pin ``d``.
     - *inferred*
     - Linux's default ``slot ^ 4`` mapping, which it does not override
       for the MENET. The second-slot interrupt the IRIX 6.5.30 kernel
       takes agrees. No capture shows a MENET interrupt register.
   * - Fits any XIO slot. The manual recommends odd slots before even,
       low before high. A GIGAchannel's XIO slots take MENET boards.
     - *established*
     - :src:`108-0151-001 pp. 2-4, 2-5`. For the GIGAchannel,
       :src:`usenet-9gjuq1`, an SGI engineer's statement, not a test.
   * - Board record on the Bridge's one-wire line: part ``030-0873-003``
       with revisions C and J seen, and ``030-0873-004`` rev A on an
       Onyx2. Name ``MENET``. The hardware-graph slot name is ``menet``
       and the PROM types it ``0x23``.
     - *established*
     - :src:`mimgfx-origin2000-inventory` (``Location:
       /hw/module/1/slot/io8/menet``, ``MENET Board: barcode HLZ655 part
       030-0873-003 rev J``, ``io7`` rev C), :src:`usenet-40c20497` and
       :src:`ip27-prom-6.156`.
   * - The Bridge revision is 3 or 4. IRIX prints it as the board
       revision (``Origin MENET board ... Revision 3`` and ``Revision
       4``).
     - *established*
     - :src:`usenet-7psh8j` (slot 6, revision 3);
       :src:`mimgfx-origin2000-inventory` (slots 7 and 8, revisions 3 and
       4).
   * - Whether the real board's IOC3s carry a per-chip one-wire record.
     - *unknown*
     - OpenBSD reads the MENET part from each chip's own bus and the IRIX
       6.5.30 kernel does not. No inventory shows one. A real MENET's
       OpenBSD ``dmesg`` would settle it.
   * - What each chip's own PCI subsystem register returns.
     - The chip has none, *inferred* (:ref:`sgi-ioc3-measurements`), and
       the model presents none. The ``subsystem-id`` column is the key
       that selects the model's population.
     - Linux substitutes ``MENET`` and ``MENET4`` ids from the Bridge
       record.

Model
-----

``sgi-xio-baseio`` with ``preset=menet`` builds the board's own Bridge at
any XIO widget, with Bridge revision 4 and no flash part, and four
``sgi-ioc3`` chips:

.. list-table::
   :header-rows: 1

   * - PCI device
     - Population
     - Subsystem id
     - SuperIO output to
   * - 0 to 2
     - Ethernet, serial A, serial B, one-wire identity
     - ``0xc305``
     - Bridge input ``d + 4``
   * - 3
     - Ethernet, one-wire identity
     - ``0xc306``
     - not connected

Each chip has PCI revision 1 and no Timekeeper. Its Ethernet interrupt is
its own INTA, which the Bridge's per-slot rule sends to pin ``d``. The
chips have no keyboard or mouse device, parallel port or clock.

The board-identity record is part ``030-0873-003``, revision ``J`` and
name ``MENET`` with Bridge revision 4, the pairing one real board reports
(*approximation*). The barcode is a marked synthetic placeholder. Each
chip's one-wire station-address part takes the board's synthetic IOC3
serial plus the device number, so the four parts differ.

The Ethernet transceiver is the model's DP83840A at MII address 31, as
on the BaseIO. The real part and address are *unknown*.

The preset refuses a console, record, MediaIO, parallel, clock or NVRAM
backend instead of dropping it. The serial ports take no host backend:
the console belongs to the module's BaseIO. The machine's
``baseio-preset`` property refuses ``menet``, so the board cannot replace
a module's BaseIO in IO1.

.. _sgi-ioc3-boards-dual-serial:

Dual-serial PCI card
====================

SGI's dual-serial PCI card carries one IOC3 wired for serial ports A and
B and nothing else. OpenBSD enables exactly those two ports for part
prefixes ``030-1657-`` and ``030-1664-`` and calls it "PCI_SIO_UFC dual
serial board" (``ioc.c``), a *software expectation*. The IRIX 6.5.30
kernel does the same and, because the chip has no Ethernet, takes the
SuperIO interrupt from line B of the card's slot. A stock list names
``030-1657-003`` "SERIAL I/O CARD (UFC), PCI" :src:`usenet-a47377f9`, so
the assembly exists, *established*. Line B reaches Bridge pin ``slot ^ 4``,
*inferred* from the Bridge's general line-B rule, as for the Cad Duo.

The record revision, the model's ``subsystem-id`` for the chip and the record name beyond
OpenBSD's comment are *unknown*. Nothing a guest does depends on them
beyond the text it prints, since the guests match the part prefix.

``sgi-pci-shoehorn`` fits the card with ``sio-slot=N`` instead of the
Cad Duo's ``cad-duo-slot``. Setting both is refused. The card is an
``sgi-ioc3`` with serial A and B only, ``subsystem-id`` ``0xffff``, and a
board record on its one-wire bus: part ``030-1657-003``, revision ``A``
and name ``PCI_SIO_UFC``, all *approximation*. The barcode is a marked
synthetic placeholder. With no Ethernet there is no station-address part,
so the board record is the only part on the bus. The SuperIO output goes
to Bridge pin ``slot ^ 4``. No machine composition places the card. It is
fitted with ``-device``.

Model limits
============

- The I-brick IO7 has no QEMU board.
- The Octane system board and any Octane placement of a MENET are outside
  the ``sgi-sn`` family.
- The MENET's Ethernet transceiver, its IOC3 PCI revision and the absence
  of a Bridge flash part are choices, *unknown* in hardware.
- Neither board's serial ports reach a host backend.

Tests
=====

``tests/qtest/sn0-menet-test.c`` runs the following for a board added with
``-device``. The catalogue-placed board (``placed/``) runs only
``widget-alive``, ``ioc3s-enumerate``, ``populations`` and
``superio-reaches-hub``. A board in the Origin 200 GIGAchannel cabinet
runs ``ioc3s-enumerate`` and ``populations``:

- ``ioc3s-enumerate``: IOC3s answer ``0x000310a9`` at devices 0 to 3,
  and each reads the repeated latency word at ``0x2c``, with no subsystem
  register;
- ``populations``: every chip has Ethernet and its one-wire part, devices
  0 to 2 have both serial ports, and no chip has a clock, keyboard or
  mouse device, parallel port or board record;
- ``onewire-serials-distinct``: the four station-address parts carry
  four serials;
- ``superio-reaches-hub``: a serial interrupt on each of devices 0 to 2
  raises Bridge pin ``d + 4``, not ``d``, and reaches the node's Hub;
- ``placed/widget-alive``: the placed board's widget is in the Crossbow's
  startup-alive population;
- ``not-a-baseio-preset``, ``refuses-backends-it-cannot-take`` and
  ``io6-refuses-mediaio-ports``: the refusals described above.

``tests/qtest/sn0-bridge-identity-test.c`` reads the MENET's Bridge
record back byte for byte and checks revision 4.

``tests/qtest/sn0-pci-shoehorn-test.c`` covers the dual-serial card:
``sio-enumerates`` (IOC3 identity, no subsystem register, both serial
ports and no Ethernet, keyboard, parallel, clock or station-address
part), ``sio-board-record`` (the board record is the bus's only part),
``sio-serial-interrupt-reaches-hub`` (a serial interrupt raises pin
``slot ^ 4``, not ``slot``) and ``one-card-only``.

``tests/qtest/sn0-ioc3-test.c`` pins the chip's subsystem-id-to-population
table for each id above.
