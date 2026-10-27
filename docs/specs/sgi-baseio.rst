.. SPDX-License-Identifier: BSD-3-Clause

.. _sgi-baseio:

===================================
SGI Origin 2000 BaseIO and BaseIO-G
===================================

BaseIO is the standard XIO I/O board of the Origin 2000 and Onyx2: a
Bridge joins it to a Crossbow, and one 32-bit PCI bus joins the Bridge
to an IOC3 and two SCSI controllers. The BaseIO-G variant adds a MediaIO
daughtercard. QEMU builds both from the shared Bridge, IOC3 and ISP1040
models (``hw/mips/sgi/baseio.c``); the ``io6`` preset builds the plain
board and ``io6g`` builds BaseIO-G. ``preset`` has no default. The Origin 200 has an integrated
system board, not this card, and its population and wiring must not be
read from here (:doc:`sgi-sn`). Ratings follow :ref:`sgi-ip30`.

Sources: *Origin and Onyx2 Theory of Operations* (007-3439-002), BaseIO
functions on printed page 67 (PDF 81), Figures 2-24 and 2-25 on printed
pages 68-69 (PDF 82-83), the physical layout on page 70 and the
MediaIO description with Figure 2-27 on pages 71-72; *Onyx2 Deskside
Workstation Owner's Guide* (007-3454-005), the IO6G panel on printed
pages 26-27; an owner's
``hinv -v`` and ``hinv -vm`` of a populated BaseIO-G Origin 2000
(``web:nekonomicon-17194``); an Origin 2000 ``hinv -m`` that lists
a plain BaseIO as ``030-1124-002`` revision J
(``web:usenet-9uhh6o``); Linux at
``linux:db2ddb87143519e20a95aa36c60b36107b736a58`` and the OpenBSD sgi port at
``openbsd-sgi:2df91d80c104636deeef8124252ddd5951b55fbf``.

.. _sgi-baseio-composition:

Board composition
=================

.. list-table::
   :header-rows: 1
   :widths: 44 12 44

   * - Fact
     - Rating
     - Source
   * - A Bridge connects the board to the Crossbow; the IOC3 and both
       SCSI controllers are PCI children of the Bridge, not separate
       crosstalk widgets, and a MIO expansion connector sits on the
       same PCI bus.
     - established
     - 007-3439-002 Figures 2-24 and 2-25, printed pages 68-69.
   * - The standard board provides one 10/100 Base-TX Ethernet link
       (through an MII link to an MC34440 PHY), two independent
       460 Kbaud serial ports, one external single-ended wide SCSI
       port, one internal Fast-20 SCSI port, a real-time interrupt input
       and frame-sync output, flash, NVRAM and a time-of-day clock.
     - established
     - 007-3439-002 printed pages 67 and 69.
   * - The IOC3 carries 64K by 18 SSRAM, a time-of-day clock with 32 KiB
       NVRAM, a Super I/O, and a board-identity NIC; the Bridge carries
       a separate NIC and a 1 MiB flash EPROM with a serial-number
       region. The Super I/O is a separate part, not a fourth PCI
       endpoint.
     - established
     - 007-3439-002 Figure 2-25, printed page 69. The figure does not
       say what family the NIC parts are or what they store.
   * - The Bridge identifies itself as part 0xc002, manufacturer 0x36,
       with the board's PCI devices behind it.
     - established
     - Bridge page (:doc:`sgi-bridge`); revision selection is
       described there.
   * - The two SCSI controllers are the QLogic ISP10x0 family,
       ``0x1077:0x1020``, and report single-ended. Figure 2-25 still
       draws a differential transceiver on one controller's path; the
       figure is standard BaseIO and the ``hinv`` lines are BaseIO-G,
       so a product difference is one explanation. Unresolved; no
       guest-visible effect is known.
     - established, with an open conflict
     - ``web:nekonomicon-17194`` ``hinv -v`` (vendor 4215, device
       4128, ``QL1040B (rev. 2), single ended``); 007-3454-005 ("the
       BaseIO single-ended SCSI connector"); 007-3439-002 Figure 2-25.
   * - The ISP1040 revision modelled is an ISP1040C-compatible part
       (revision 6, 60 MHz clock, PCI revision 5).
     - approximation
     - Emulator choice recorded on :doc:`sgi-qlogic-isp`; no historical
       package identification is known.
   * - The board's real-time interrupt input reaches the Bridge's
       ``INT_IN`` through an opto-isolator and the Bridge drives the
       output at the board edge. The numbered Bridge input for the
       external signal is not documented; the jacks are not modelled.
     - established / unknown
     - 007-3439-002 Figure 2-25; Onyx2 guide panel table. The input
       number would be settled by a Bridge interrupt-routing capture.

.. _sgi-baseio-pci:

PCI placement
=============

.. list-table::
   :header-rows: 1
   :widths: 44 12 44

   * - Fact
     - Rating
     - Source
   * - The two SCSI controllers are PCI devices 0 and 1; IOC3s are at
       devices 2 and 6; the RAD1 audio ASIC is at device 7; nothing
       answers at device 4.
     - established (BaseIO-G); inferred for plain BaseIO
     - ``web:nekonomicon-17194``, two ``hinv -v`` listings
       (``0x1077:0x1020`` at slots 0 and 1, ``0x10a9:0x0003`` at
       slots 2 and 6, ``0x10a9:0x0005`` at slot 7). The standard board
       is inferred to match; no plain-BaseIO ``hinv -v`` with PCI slots
       is cited.
   * - On a standard BaseIO the IOC3 is at device 2, so the
       real-time-interrupt vertex is ``.../baseio/pci/2/ei``.
     - established
     - Linux ``arch/mips/pci/pci-xtalk-bridge.c`` lines 469-482 and
       530-533 (board parts ``030-0734-``/``030-0880-`` give device 2
       the BaseIO6G identity and device 6 the MIO identity;
       ``030-1023-``/``030-1124-`` the plain BaseIO identity);
       ``web:nekonomicon-17194``.
   * - The IOC3's SuperIO interrupt rides Bridge input 4 on the board
       IOC3 and input 6 on the MediaIO IOC3.
     - established by observation only
     - Observed in the stock IRIX 6.5.30 kernel image and a live boot
       of this model; no public source states it, so it is not
       independently citable. A Bridge interrupt-register capture of a
       real board would settle it.
   * - The IOC3 populations: plain BaseIO has Ethernet, serial and the
       clock; BaseIO-G adds the keyboard and mouse controller; the
       MediaIO IOC3 has serial and keyboard and mouse but no Ethernet
       or clock.
     - established
     - Linux ``drivers/mfd/ioc3.c`` lines 388-446
       (``ip27_baseio_setup``, ``ip27_baseio6g_setup`` and
       ``ip27_mio_setup``); OpenBSD ``sys/arch/sgi/pci/ioc.c``.
   * - The model's IRIX enable words for those chips (``0xb7`` plain,
       ``0xbf`` BaseIO-G, ``0x7a`` MediaIO) agree with those
       populations.
     - approximation
     - The words were read from the stock IRIX 6.5.30 kernel binary, a shipped image rather than vendor source; they
       are not independently citable, and the populations above are the
       public statement.

.. _sgi-baseio-g:

BaseIO-G and MediaIO
====================

MediaIO is a daughtercard fitted only to the BaseIO-G. It adds a second
IOC3, a Mach5 Super I/O, a RAD audio ASIC, a parallel port, serial ports,
keyboard and mouse connections and the audio interfaces; those devices
are not part of the standard board (established, 007-3439-002 Figure
2-27, printed pages 71-72). The keyboard and mouse leave the IOC3, the
parallel port and serial ports leave the Mach5, and the RAD ASIC sits on
the PCI bus. The IO6G panel carries two PS/2 keyboard and mouse pairs, a
36-pin IEEE 1284-C parallel connector and four BNCs (two AES3 audio,
two video-sync loop-through) (established, 007-3454-005 printed pages 7,
26-27, 55 and 61).

The base board is assembly ``030-0734-002`` revision N and the MediaIO
card ``030-0880-003`` revision G in one owner's ``hinv -vm``
(``web:nekonomicon-17194``), both under ``/hw/module/1/slot/io1/baseio``
as two records at the Bridge's location (established). That the MediaIO
record is a second part on the Bridge's own wire is inferred. The SGI
part-number table also lists ``030-0734-001`` as an io6g base board and
``030-0880-003`` as the MediaIO card (``web:sgi-pca-numbers``). The
plain board's record is ``030-1124-002`` revision J (``web:usenet-9uhh6o``).
Barcodes are per-unit and are placeholders in the model; the record
layout is on :doc:`sgi-bridge`. The RAD1 audio ASIC's revision, the division of the audio paths between it
and the codec parts, and the MediaIO's Mach5 part number have no source
cited here and are unknown for this page; the board's PCI identity and
IOC3 population are on :doc:`sgi-ioc3`.

.. _sgi-baseio-flash:

Bridge flash
============

The Bridge's external flash port is sixteen bits wide and takes a
sector-erasable 256K by 16 or 512K by 16 part; the board diagram draws
1 MB, so the part is 512K by 16 (established, :doc:`sgi-bridge` and 007-3439-002 Figure 2-25; the part is
not named).
The IP27 PROM accepts the AMD ``0x01``/``0x22da``, ST ``0x20``/``0xd7``
and Fujitsu ``0x04``/``0x22da`` identities for a sixteen-bit part here
(measured from the stock image, ``firmware:ip27-prom-6.156``). The model
fits the AMD part, which Linux's JEDEC table names AM29LV800BT: 1 MiB, top
boot, fifteen 64 KiB sectors then 32 KiB, two 8 KiB and 16 KiB, with
unlock addresses ``0x555`` and ``0x2aa`` (approximation; which part SGI
soldered on a given board is unknown). The strap behind the Bridge's
``FLASH_SELECT`` pin and whether the second chip select is a socket are
unknown. The image the PROM loads is the BASEIO PROM that the install
media carry beside the node PROM.

.. _sgi-baseio-limits:

Open items
==========

The numeric Crossbow widget used by BaseIO in every enclosure is not
established; the Origin 2000 model puts it at widget 8 on Crossbow 0
(:ref:`sgi-xbow-role`). Whether a multi-enclosure system fits one BaseIO
per enclosure or one for the whole system is configuration-dependent; SGI
states a deskside holds at most one BaseIO (007-3439-002 printed page
69, after Figure 2-25). The panel LEDs have no register in any source and are not modelled.
