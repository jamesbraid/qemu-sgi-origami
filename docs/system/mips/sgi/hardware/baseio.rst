.. SPDX-License-Identifier: BSD-3-Clause

.. _sgi-baseio:

==============================================================
SGI base I/O boards (BaseIO, BaseIO-G, IP29, IO-8 and Fuel)
==============================================================

Every SGI Origin and Onyx2 family machine carries its console, boot
disk and Ethernet on a small set of PCI devices behind an XIO-to-PCI
bridge: an IOC3 and one or two SCSI controllers, and on the newer
boards a USB controller. QEMU models all of these boards as one device,
``sgi-baseio`` (``hw/mips/sgi/baseio.c``), with a preset for each board
design. The device owns the PCI population, the interrupt wiring and the
board identity. It does not own the bridge. The carrier creates the
Bridge (Origin 200, Origin 2000, Onyx2) or XBridge (Origin 300, Fuel),
sets its identity, realizes it and passes its PCI bus in the ``pci-bus``
link property.

The presets are ``ip29`` (the Origin 200 system board's onboard I/O),
``io6`` (the Origin 2000 BaseIO), ``io6g`` (the Onyx2 BaseIO-G with its
MediaIO daughtercard), ``io7`` (the Origin 3000 I-brick's onboard I/O),
``io8`` (the Origin 300 IO-8), ``fuel`` (the Fuel
system board) and the two optional XIO boards ``menet`` and ``mscsi``,
which have their own pages (:doc:`ioc3-boards`, :doc:`mscsi`).
``preset`` has no default, and an unknown name is refused. The XIO cards
(``io6``, ``io6g``, ``menet``, ``mscsi``) are built by the carrier
``sgi-xio-baseio`` (``hw/mips/sgi/xio-baseio.c``), which adds a Bridge
and, for the BaseIO presets, the flash part. The Origin 200 has an
integrated system board and no BaseIO card, so the Origin 2000 board
description must not be read as the Origin 200's. Facts are rated as
described in :ref:`sgi-ratings`.

.. _sgi-baseio-sources:

Sources
=======

- *Origin and Onyx2 Theory of Operations*, :src:`007-3439-002 pp. 67-72`:
  BaseIO functions (p. 67), the block diagrams in Figures 2-24 and 2-25
  (pp. 68-69), the physical layout (p. 70) and the MediaIO description
  with Figure 2-27 (pp. 71-72).
- *Onyx2 Deskside Workstation Owner's Guide*, :src:`007-3454-005 pp. 26-27`:
  the IO6G panel and its connector table. It describes the BaseIO-G, not
  the plain BaseIO.
- A BaseIO-G Origin 2000 owner's ``hinv -v`` and ``hinv -vm``,
  :src:`nekonomicon-17194`, and an Origin 2000 ``hinv -m`` that lists a
  plain BaseIO, :src:`usenet-9uhh6o`.
- The SGI part-number list, :src:`sgi-pca-numbers`, and the *Hardware
  Quick-reference Booklet (Origin and Onyx2 Series)*,
  :src:`origin-onyx2-quick-reference-booklet`.
- Photographs of a BaseIO-G with its MediaIO and genlock daughtercards in
  two auction listings (eBay 277424701723 and 286153077499), for chip
  markings. The listings are not archived, so these markings cannot be
  rechecked.
- Linux ``arch/mips/pci/pci-xtalk-bridge.c`` and ``drivers/mfd/ioc3.c``
  at ``db2ddb871435``, and OpenBSD ``sys/arch/sgi/pci/ioc.c`` at
  ``2df91d80c104``, as software expectations.
- For the Origin 300 IO-8 and the Fuel: an Origin 300 OpenBSD boot,
  :src:`marc-openbsd-sgi-139223952727384`, the Fuel IRIX console capture
  :src:`tingo-fuel-irix-console`, and the IO-8 assembly description in
  :src:`007-4365-002 p. 12`.

.. _sgi-baseio-presets:

Presets
=======

Each preset fixes the PCI devices, their revisions and the board
identity that the carrier writes into the bridge. The identity is the
board part number, revision letter and name that IRIX prints in
``hinv -m``. The model supplies a synthetic barcode and serial for every
board, so an inventory from the model never reads as a real unit.

.. list-table::
   :header-rows: 1
   :widths: 10 28 62

   * - Preset
     - Board and carrier
     - Population and identity
   * - ``ip29``
     - Origin 200 system board. The node builds the Bridge and this
       device directly, with no board wrapper.
     - Bridge revision 4, board record ``030-1244-001`` revision H, name
       ``IP29``. Two ISP1040 at devices 0 and 1 (PCI revision 5). One
       IOC3 at device 2 (revision 1) with Ethernet, both serial ports,
       keyboard and mouse, parallel port, one-wire identity part and an
       M48T35-compatible Timekeeper. The model's IOC3 subsystem id is its
       "IP29 System Board" key.
   * - ``io6``
     - Origin 2000 BaseIO, built by ``sgi-xio-baseio``. XIO widget 8
       only, slot ``io1`` only.
     - Bridge revision 4, record ``030-1124-002`` revision J, name
       ``BASEIO``. ISP1040 at devices 0 and 1 (revision 5). IOC3 at
       device 2 (revision 1) with Ethernet, both serial ports, one-wire
       identity part and Timekeeper; no keyboard, mouse or parallel
       port. IOC3 subsystem id ``0xffff``, a model value with no hardware
       counterpart because the chip has no subsystem register
       (*approximation*). BASEIO
       PROM flash on the Bridge's flash port.
   * - ``io6g``
     - Onyx2 BaseIO-G with MediaIO, built by ``sgi-xio-baseio``. XIO
       widget 8 or 15, slot ``io1`` only.
     - Bridge revision 4, record ``030-0734-002`` revision N, name
       ``BASEIO``, plus a second record ``030-0880-003`` revision G, name
       ``MIO``. The ``io6`` devices, with keyboard and mouse added to the
       device 2 IOC3. A second IOC3 at device 6 (MediaIO, subsystem id
       ``0xc301``) with both serial ports, keyboard and mouse and a
       parallel port, but no Ethernet, clock or one-wire identity part.
       The RAD1 audio ASIC at device 7 (:doc:`rad1-audio`). BASEIO PROM
       flash.
   * - ``io7``
     - Origin 3000 I-brick onboard I/O, placed on head F (widget 15) of
       the I-brick's XBridge (:ref:`sgi-sn-family-ibrick`).
     - IOC3 at device 4 with Ethernet through a PHY at MII address 1,
       serial port A only and an M48T35-compatible Timekeeper; no
       keyboard, mouse or parallel port. AT&T/Lucent OHCI USB at
       device 5 (:doc:`lucent-ohci`), TI TSB12LV22 IEEE 1394 link at
       device 6 (:doc:`tsb12lv22`). No SCSI controller.
   * - ``io8``
     - Origin 300 IO-8, placed on XBridge head F (widget 15).
     - ISP12160 at device 1 (:doc:`qlogic-isp`). IOC3 at device 4
       (revision 1) with Ethernet through a DP83843 at MII address 1,
       both serial ports and a parallel port, and an M48T35-compatible
       Timekeeper; no keyboard, mouse or one-wire part. AT&T/Lucent OHCI
       USB at device 5 (:doc:`lucent-ohci`). Board part
       ``030-1673-003``.
   * - ``fuel``
     - Fuel system board, placed on XBridge head F.
     - The IO-8 devices, but the IOC3 also has keyboard and mouse and its
       parallel port is the Fuel's TL16PIR552 part. The carrier
       supplies the board identity.
   * - ``menet``
     - Four-IOC3 Ethernet and serial XIO board (:doc:`ioc3-boards`).
     - Bridge revision 4, record ``030-0873-003`` revision J, name
       ``MENET``.
   * - ``mscsi``
     - Four-ISP1040 SCSI XIO board (:doc:`mscsi`).
     - Bridge revision 3, record ``030-1243-001`` revision F, name
       ``MSCSI``.

The part numbers and revisions of the two Origin 2000 boards come from
published inventories (:src:`nekonomicon-17194`, :src:`usenet-9uhh6o`)
and are *observed*. The ``ip29`` record is the revision the model keeps
for every Origin 200 node. Inventories of faster Origin 200 nodes print
``030-1389-001`` (:doc:`/system/mips/sgi/sn`), so the model value is an
*approximation*. The Bridge revisions are model defaults, except the
MENET's and the MSCSI's, which published inventories show
(:doc:`ioc3-boards`, :doc:`mscsi`). The ``io8`` part prefix
``030-1673-`` is the one Linux matches for the IO-8, a *software
expectation*, and the ``-003`` suffix is a model choice. The Fuel's board
identity is *unknown* here.

.. _sgi-baseio-composition:

BaseIO board composition
========================

This section covers the plain BaseIO and the part of the BaseIO-G that is
the same board.

.. list-table::
   :header-rows: 1
   :widths: 46 12 42

   * - Fact
     - Rating
     - Source
   * - A Bridge connects the board to the Crossbow. The IOC3 and both
       SCSI controllers are PCI children of the Bridge, not crosstalk
       widgets of their own. An MIO expansion connector sits on the same
       PCI bus.
     - *established*
     - :src:`007-3439-002 Figs. 2-24, 2-25, pp. 68-69`
   * - The standard board provides one 10/100 Base-TX Ethernet link
       through an MII link to an MC34440 PHY, two independent 460 Kbaud
       serial ports, one external single-ended wide SCSI port, one
       internal Fast-20 SCSI port, a real-time interrupt input and
       frame-sync output, flash, NVRAM and a time-of-day clock.
     - *established*
     - :src:`007-3439-002 pp. 67, 69`
   * - The IOC3 carries 64K by 18 SSRAM, a time-of-day clock with 32 KiB
       NVRAM, a Super I/O and a board-identity part. The Bridge carries a
       separate board-identity part and a 1 MiB flash EPROM with a
       serial-number region. The Super I/O is a separate component, not a
       fourth PCI endpoint, and the serial connectors leave it, not the
       IOC3.
     - *established*
     - :src:`007-3439-002 Fig. 2-25, p. 69`. The figure does not give the
       identity parts' family or contents.
   * - The two SCSI controllers are QLogic ISP10x0 family devices,
       ``0x1077:0x1020``, and IRIX reports them single-ended
       (``QL1040B (rev. 2), single ended``). Figure 2-25 draws a
       differential transceiver on one controller's path. The figure is
       the standard board and the inventory is a BaseIO-G, so a product
       difference is one explanation. No guest-visible effect is known.
     - *established*, with an open conflict
     - :src:`nekonomicon-17194 hinv -v`; :src:`007-3454-005 p. 27`;
       :src:`007-3439-002 Fig. 2-25, p. 69`
   * - The model fits a part reporting PCI revision 5, the value the
       reference Octane2's onboard QL1040B parts read. Which ISP1040
       package revision 5 names is *unknown*.
     - *approximation*
     - :doc:`qlogic-isp`
   * - The external real-time interrupt input reaches the Bridge
       ``INT_IN`` through an opto-isolator, and the Bridge drives the
       output at the board edge. The numbered Bridge input for the
       external signal is not documented.
     - *established*; input number *unknown*
     - :src:`007-3439-002 Fig. 2-25, p. 69`. A Bridge interrupt-routing
       capture of a real board would settle the input number.

.. _sgi-baseio-pci:

PCI placement and interrupts
============================

.. list-table::
   :header-rows: 1
   :widths: 46 12 42

   * - Fact
     - Rating
     - Source
   * - The two SCSI controllers are PCI devices 0 and 1, the IOC3s are at
       devices 2 and 6, the RAD1 audio ASIC is at device 7, and no other
       function is listed. The model reads an empty slot as all ones.
     - *observed* on a BaseIO-G; *inferred* for the plain BaseIO
     - :src:`nekonomicon-17194 hinv -v`: ``0x1077:0x1020`` at slots 0
       and 1, ``0x10a9:0x0003`` at 2 and 6, ``0x10a9:0x0005`` at 7. No
       plain-BaseIO ``hinv -v`` with PCI slots is cited, so the plain
       board is inferred to match.
   * - On a plain BaseIO the IOC3 is at device 2, so the real-time
       interrupt vertex that IRIX creates below it is
       ``.../baseio/pci/2/ei``.
     - *software expectation*
     - Linux gives device 2 the BaseIO identity for parts
       ``030-1023-`` and ``030-1124-`` and the BaseIO-G identity for
       ``030-0734-`` in ``pci-xtalk-bridge.c``. An Origin 2000 inventory
       puts the Ethernet at ``pci 2`` (:src:`usenet-9uhh6o`).
   * - The IOC3 populations: the plain BaseIO has Ethernet, serial and the
       clock. The BaseIO-G board IOC3 adds the keyboard and mouse
       controller. The MediaIO IOC3 has serial ports, keyboard and mouse
       but no Ethernet or clock. The parallel port is on the MediaIO
       side.
     - *software expectation*
     - Linux ``drivers/mfd/ioc3.c`` (``ip27_baseio_setup``,
       ``ip27_baseio6g_setup``, ``ip27_mio_setup``). OpenBSD ``ioc.c``
       chooses by device number and gives the BaseIO-G board IOC3 the
       standard BaseIO set.
   * - The IRIX 6.5.30 kernel enables the keyboard and mouse of the
       device 2 IOC3 only when the Bridge's board record carries the
       BaseIO-G part prefix ``030-0734-``. It attaches no Ethernet to the
       device 6 IOC3 when the MediaIO record ``030-0880-`` is present.
     - *software expectation*
     - The kernel's enable words for these prefixes
       (:ref:`sgi-ioc3-boards-population`). Without the MediaIO record
       the kernel's default word enables Ethernet on device 6.
   * - On the plain BaseIO and the BaseIO-G board IOC3, the SuperIO
       interrupt (serial, keyboard and mouse, real-time) reaches Bridge
       input 4. On the MediaIO IOC3 it reaches input 6. Ethernet uses
       the Bridge's ordinary per-slot INTA rule.
     - *software expectation*
     - Linux ``bridge_setup_ip27_baseio`` (line B to input 4) and
       ``ip27_mio_setup`` (input 6), as summarized on
       :doc:`ioc3-boards`. The model connects the IOC3's SuperIO GPIO
       output to that Bridge input. A capture of a real board's
       interrupt register would confirm it.
   * - On the Origin 200 the IOC3's SuperIO interrupt reaches Bridge
       input 3, not the input matching its device number.
     - *software expectation*
     - Linux ``bridge_setup_ip29_baseio`` (:doc:`ioc3-boards`).
   * - Both ISP1040s on every Bridge-based preset use the Bridge's
       ordinary per-slot INTA mapping. RAD1 has no interrupt wiring in
       the model, because IRIX's RAD1 driver registers no data-path
       interrupt.
     - *approximation*
     - No source describes other wiring.
   * - The MENET's four IOC3s sit at devices 0 to 3. The SuperIO output
       of devices 0 to 2 reaches Bridge input device + 4. Device 3 has
       Ethernet only and raises none. The MSCSI's four ISP1040s sit at
       devices 0 to 3.
     - devices *established*; SuperIO inputs *inferred*
     - :doc:`ioc3-boards` and :doc:`mscsi`.
   * - The IO-8 and Fuel wire each function's interrupt explicitly onto
       an XBridge head F input: ISP12160 INTA to input 1, IOC3 Ethernet
       INTA to input 4, OHCI INTA to input 5, and the IOC3's SuperIO
       output to input 0. An XBridge head has no per-slot rule of its
       own.
     - OHCI input 5 *observed*; the rest *approximation*
     - :src:`marc-openbsd-sgi-139223952727384 lines 150-183` and
       :doc:`lucent-ohci` for input 5. :ref:`sgi-sn-fuel-interrupts`
       gives the Fuel table. No capture shows the other inputs.

.. _sgi-baseio-g:

BaseIO-G and MediaIO
====================

MediaIO is a daughtercard fitted only to the BaseIO-G. It adds a second
IOC3, a Mach5 Super I/O, a RAD audio ASIC, a parallel port, serial
ports, keyboard and mouse connections and the audio interfaces. None of
these is part of the standard board, *established*
(:src:`007-3439-002 Fig. 2-27, pp. 71-72`). The keyboard and mouse leave
the IOC3, the parallel and serial ports leave the Mach5, and the RAD ASIC
sits on the PCI bus and carries the audio ports. The Mach5 rests on the
figure alone: no photograph shows a legible marking for it.

Photographs of two MediaIO boards (assembly ``030-0880-003``, revisions
E and F) show an LSI L2A0366 IOC3, an LSI L2A0470 RAD1 REV B, a TI
TLC320AD58 stereo codec, a Cirrus CS8412-CS AES3 receiver and at least
one CS3310-KS volume control (auction photographs), *observed*.
The division of the AES3 and ADAT digital paths among RAD1, the CS8412
and the codec is *unknown*, and so is the presence of the balanced line
drivers that one source claims. The audio device is described on
:doc:`rad1-audio`.

The IO6G panel carries one RJ45 Ethernet jack, four serial connectors,
two PS/2 keyboard and mouse pairs with the primary pair on the right, a
36-pin IEEE 1284-C parallel connector, four BNCs (two AES3 audio, two
video-sync loop-through), optical audio connectors, a 68-pin SCSI
connector and two 3.5 mm jacks for the real-time interrupt input and
output, *established* (:src:`007-3454-005 pp. 7, 26-27, 55, 61`). The
BNCs are not interrupt paths. The jacks belong to the base board and the
BNCs to the MediaIO side. The panel table covers only the IO6G, and no
connector listing of the standard BaseIO panel is cited.

The base board of a BaseIO-G is assembly ``030-0734-002`` revision N and
the MediaIO is ``030-0880-003`` revision G in one published
``hinv -vm``, both under ``/hw/module/1/slot/io1/baseio``, *observed*
(:src:`nekonomicon-17194`). The part list also names ``030-0734-001`` as
an io6g base board (:src:`sgi-pca-numbers`). That the MediaIO record is a
second part on the Bridge's own one-wire line is *inferred*. The record
layout is on :doc:`bridge`. The model carries both records on the
Bridge's line, with the MediaIO record's serial distinct from the base
board's so that a one-wire search tells them apart. Barcodes are
synthetic.

Two keyboard and two mouse connectors work as separate inputs on at
least one real assembly (:src:`usenet-35ab0320`), *observed*. Which
connector maps to which IOC3 is *unknown*. The second pair on the board
IOC3 is *inferred*: each IOC3 has one keyboard controller, the MediaIO
adds one pair, and Linux and the IRIX 6.5.30 kernel both enable keyboard
and mouse on the device 2 IOC3 of a ``030-0734`` board. The model puts
one pair on each IOC3. A real BaseIO-G with one keyboard in the primary
port, listing ``/hw/input*``, would settle the primary connector.

SGI sold the plain BaseIO with the Origin 2000 and the BaseIO-G with the
Onyx2 (:src:`origin-onyx2-quick-reference-booklet`), *established*.
Owners who fitted an IMPACT graphics board to an Origin 2000 replaced the
BaseIO with a BaseIO-G to get keyboard, mouse and audio
(:src:`nekonomicon-5749`). That such a configuration needs the BaseIO-G
for its console is *inferred* from those reports and from the plain
board's missing PS/2 ports.

Origin 200 onboard I/O
======================

The Origin 200 system board has no XIO card. Its Bridge, two SCSI
controllers and IOC3 are fitted to the board, and the ``ip29`` preset
builds them as listed under :ref:`sgi-baseio-presets`. The node sets the
Bridge to drive REQ64 on its option slots 5 to 7 and not on the onboard
devices, *inferred* from the slots being sold as 64-bit. The Origin 200's
own Bridge fits no BASEIO PROM flash part, so its flash window decodes
as an error (:doc:`bridge`). In a GIGAchannel module the cabinet's
Bridge carries that part (:doc:`gigachannel`).

IO-8 and Fuel
=============

The ``io8`` and ``fuel`` presets populate head F of an XBridge with an
ISP12160 at device 1, an IOC3 at device 4 and an OHCI controller at
device 5. The IO-8 placement is *observed* in the Origin 300 captures
(:src:`marc-openbsd-sgi-139223952727384`) and the Fuel's in its console
capture (:src:`tingo-fuel-irix-console`). The identity details are model
choices:

- The IO-8's ISP12160 and OHCI subsystem identities are *unknown* and
  read ``0xffff``. The IOC3 has no subsystem register of its own,
  *inferred*, so its ``0xffff`` subsystem device is a model value with no
  hardware counterpart (*approximation*). The IOC3 subsystem vendor is
  ``0x10a9``. The Fuel needs the carrier to supply
  the IOC3 subsystem id and refuses a value above sixteen bits.
- Both presets put a National Semiconductor DP83843 PHY at MII address 1
  on the IOC3, where the Origin 200 and 2000 boards use a DP83840A at
  address 31 (:doc:`ioc3`). On the IO-8 the IOC3's GPIO 5 resets the PHY.
- The IO-8 IOC3 has no one-wire identity part. The model takes the
  board's Ethernet address from the brick EEPROM through the L1
  (:doc:`l1`), not from the IOC3.
- ``scsi-revision``, ``ioc3-revision`` and ``usb-revision`` select the PCI
  revision of the three devices. They default to the revision the device
  class implements, any other preset refuses them, and a value the class
  does not implement is refused.
- ``system-timekeeper-kind`` and ``system-timekeeper-year-base`` select
  the system Timekeeper. These two presets accept only the
  M48T35-compatible part with year base 1968, and every other preset
  refuses any value. The IO-8 always has the Timekeeper. The Fuel has it
  only when the carrier sets the kind.
- An XBridge head has no interrupt routing of its own, so the preset
  wires each function (:ref:`sgi-baseio-pci`).

Board identity records
======================

Each Bridge-based preset gives its Bridge a board record on the one-wire
line: part, revision and name, with a synthetic barcode made from the
board's letter and the Bridge's serial. The values are in the preset
table. IRIX names the slot in the hardware graph after the record's
name in lower case (``baseio``, ``menet``, ``mscsi``), *inferred* from
published inventories (:src:`nekonomicon-17194`,
:src:`preterhuman-origin-200`). A record with an empty name gives a slot
of underscores, so the name comes from the record, not the board type. The IRIX 6.5.30 kernel finds the
Timekeeper through a path that includes the slot name ``baseio``, so a
plain BaseIO whose slot lacked that name could not store NVRAM
variables, a *software expectation*. The ``io8`` XBridge carries no such
record.

``onewire-serial`` sets the serial of the onboard IOC3's one-wire part,
which is distinct from the Bridge's. The MENET gives each of its four
IOC3 parts the board's serial plus its device number.

Properties
==========

``sgi-baseio`` has no registers. Its properties select the preset and
attach host backends to the IOC3s it builds.

.. list-table::
   :header-rows: 1
   :widths: 28 72

   * - Property
     - Meaning
   * - ``preset``, ``pci-bus``
     - The preset name and the PCI bus of the carrier's Bridge or
       XBridge (a link). Both are required.
   * - ``part``, ``revision``
     - The preset's board part and Bridge silicon revision, read by a
       carrier before it builds its chip. They default from the preset.
   * - ``onewire-serial``, ``ioc3-mac``, ``ioc3-subsystem-id``
     - The IOC3 one-wire part serial, the board Ethernet address, and
       the IOC3 PCI subsystem device.
   * - ``chardev-a``, ``chardev-b``, ``record-a``, ``record-b``
     - Host backends for the onboard IOC3's two serial ports and their
       recording lines.
   * - ``mio-chardev-a``, ``mio-chardev-b``
     - Backends for the two serial ports of the MediaIO IOC3 (``io6g``
       only). They are peripheral lines with no recording line.
   * - ``parallel-chardev``
     - Backend for the preset's parallel port: the onboard IOC3 on
       ``ip29``, ``io8`` and ``fuel``, the MediaIO IOC3 on ``io6g``.
       ``io6`` has none and refuses the id.
   * - ``nvram-drive``, ``nvram-clock-drive``
     - Block backends for the Timekeeper data and the clock record the
       model keeps beside it. A clock drive needs a data drive.

A backend id that no chip of the preset takes is refused at realize, and
so is an id that names no backend, so a function is never left silently
unbound.

``sgi-xio-baseio`` adds ``preset`` (default ``io6``), ``fabric`` (the
Crossbow port link, required), ``pci-bus-name`` (one name per card when a
module holds more than one, because two buses with the same name collide
in the migration stream), ``onewire-serial`` (the Bridge's
board-identity part), ``ioc3-onewire-serial``, ``flash-drive`` and
``firmware``, and forwards the backend ids above. The machine property
``baseio-preset`` (``io6`` or ``io6g``) selects the card of an Origin 2000
topology, and ``io<N>-flash`` binds the flash of the BaseIO with I/O
board index N.

.. _sgi-baseio-flash:

Bridge flash
============

The Bridge's external flash port is sixteen bits wide and takes a
sector-erasable 256K by 16 or 512K by 16 part. The board diagram draws
1 MiB, so the part is 512K by 16, *established*
(:doc:`bridge` and :src:`007-3439-002 Fig. 2-25, p. 69`), though neither
names it. A photograph marks the flash AM29LV800B
(auction photographs), *observed* on one unit. The IP27 PROM
loads the BASEIO PROM from this part. It accepts the AMD
``0x01``/``0x22da``, ST ``0x20``/``0xd7`` and Fujitsu
``0x04``/``0x22da`` identities for a sixteen-bit part here
(:src:`ip27-prom-6.156`), a *software expectation*. The image it loads
is the I/O PROM image that the IRIX install media carry beside the node
PROM.

``hw/mips/sgi/baseio-flash.c`` builds the part as a CFI AMD
command-set flash (``cfi.pflash02``):

- 1 MiB, sixteen bits wide, big-endian, top boot: fifteen 64 KiB
  sectors, then one 32 KiB, two 8 KiB and one 16 KiB sector.
- Manufacturer ``0x01`` and device ``0x22da``, which Linux's JEDEC table
  names AM29LV800BT. The unlock addresses ``0x555`` and ``0x2aa`` are
  sixteen-bit word addresses.

Which of the three accepted parts SGI fitted on a given board is
*unknown*, and so is whether they differ in sector layout. The model's
choice is an *approximation*. The strap behind the Bridge's
``FLASH_SELECT`` pin and whether the second chip select is a socket are
also *unknown*. The model reads the strap as zero and leaves the second
select empty.

The contents come from one of two places. A drive (``flash-drive``, or
the machine's ``io<N>-flash``) supplies the whole 1 MiB, and a
``firmware`` file is then not read. Without a drive, ``firmware`` (or the
machine's ``io-prom`` option) names an original I/O PROM file. The part
stores it as supplied, container header included, followed by erased
bytes. A missing file, or one larger than 1 MiB, is an error. With
neither, the part is fitted blank and reads as zeros, not the ``0xff`` of
an erased chip, so the PROM's loader rejects it and falls back to its
internal copy (*approximation*). The Origin 200 GIGAchannel cabinet's
Bridge fits the same part. ``menet`` and ``mscsi`` fit no flash and
refuse ``flash-drive`` and ``firmware``.

Measurements
============

No register of this board has been read from an identified physical
machine. The facts rated *observed* above come from published
inventories and photographs of other people's machines:

.. list-table::
   :header-rows: 1
   :widths: 30 44 26

   * - Capture
     - What it shows
     - Board
   * - :src:`nekonomicon-17194` ``hinv -v``, ``hinv -vm``
     - PCI slots 0, 1, 2, 6, 7 and their identities, board records
       ``030-0734-002`` N and ``030-0880-003`` G, SCSI controllers
       reported single-ended
     - BaseIO-G, Origin 2000
   * - :src:`usenet-9uhh6o` ``hinv -m``
     - Board record ``030-1124-002`` J, Ethernet at ``pci 2``
     - plain BaseIO, Origin 2000
   * - Auction photographs (eBay 277424701723, 286153077499)
     - Chip markings: IOC3, RAD1, flash, Timekeeper, audio parts
     - BaseIO-G with MediaIO

These establish identities and placement. They read no Bridge, IOC3 or
flash register and say nothing about interrupt input numbers.

.. _sgi-baseio-limits:

Model limits
============

- The external real-time interrupt and frame-sync jacks are not modelled.
  The numbered Bridge input for the external signal is *unknown*.
- The panel LEDs have no register in any cited source and are not
  modelled.
- The 030-1101-001 genlock daughtercard is modelled only as a video
  reference that host configuration presents to RAD1, without its
  sync-present status bit (:doc:`rad1-audio`).
- A model run lists the BASEIO record before the MIO record in
  ``hinv -m``. A published BaseIO-G inventory lists MIO first. Whether
  the order follows the one-wire ROM numbers or something else is
  *unknown*, and the model's ROM numbers may not reproduce the real
  order.
- The numeric Crossbow widget for BaseIO in every enclosure is not
  established. The model places the plain BaseIO at widget 8 and the
  BaseIO-G at widget 8 or 15 (:ref:`sgi-xbow-role`). SGI states that a
  deskside holds at most one BaseIO (:src:`007-3439-002 p. 69`). Whether
  a multi-enclosure system has one per enclosure or one for the whole
  system depends on configuration.
- How a one-letter revision sits in its four-byte record field is a model
  choice that reproduces the published inventory output. A raw dump of a
  real board's record would settle it.
- The IOC3 ``0xffff`` subsystem ids are model values with no hardware
  counterpart (*approximation*); the ISP12160 and OHCI ``0xffff`` ids and
  the IO-8 board part are placeholders, not captured values. The flash part identity and the blank-flash
  contents are described under :ref:`sgi-baseio-flash`.
- ``sgi-baseio`` and ``sgi-xio-baseio`` have no registers and no
  guest-visible state of their own. Accesses to the devices behind them
  are handled and logged by the Bridge, XBridge, IOC3, ISP and OHCI
  models. A read of an unfitted flash port is an error from the Bridge.
  This code refuses only at realize: an unknown preset, a missing PCI
  bus, an unsupported Timekeeper policy or device revision, a backend
  id with no taker, and a card placed at a slot or widget its preset does
  not fit.
- Both devices add an empty migration stream. The devices behind them
  migrate in their own streams, and migration works only between
  identical builds.

Tests
=====

- ``tests/qtest/sn0-baseio-test.c`` covers the ``io6g`` board in a full
  Origin 2000: bad ROM files refused, Timekeeper storage and reopening,
  the MediaIO IOC3 subsystem id, the RAD1 slot, identity, registers, DMA
  and streams, PS/2 reaching Bridge inputs 4 and 6, MediaIO serial
  loopback, and the two board records.
- ``tests/qtest/sgi-board-test.c`` runs ``ip29``, ``io6`` and ``io8``
  through a standalone fixture: fixed devices and revisions, the
  interrupt chain, and refusal of an unresolved chardev or drive id.
- ``tests/qtest/sn0-bridge-identity-test.c`` reads the board records of
  the Bridge-based boards byte for byte and checks the realize-time
  refusals.
- ``tests/qtest/sn0-ioc3-kbc-routing-test.c`` checks keyboard routing
  between the two BaseIO-G IOC3s.
- ``tests/qtest/sn0-bridge-flash-test.c`` covers the flash window of the
  plain BaseIO: access widths, chip-select decode, write enable,
  autoselect, program and erase, the unbacked part, both chip selects,
  the select strap and the unpopulated Origin 200.
- ``tests/qtest/sn0-gigachannel-test.c`` covers the same part in the
  GIGAchannel cabinet and the ``io-prom`` image paths.
- ``tests/qtest/sgi-machine-storage-test.c`` covers ``io<N>-flash``
  binding, the legacy ``-drive`` form and refusals.
- ``tests/qtest/sn-builder-test.c`` checks that the card fits only slot
  ``io1`` and that an Origin 2000 builds an ``sgi-xio-baseio``.
- ``tests/qtest/sn0-menet-test.c`` and ``tests/qtest/sn0-mscsi-test.c``
  cover the two optional XIO presets.
- ``tests/qtest/sn1-fuel-test.c`` covers the Fuel's PCI population, IOC3
  identity and PHY, interrupt paths, PS/2, system Timekeeper and
  revision properties. ``tests/qtest/sn1-io8-scsi-test.c`` covers the
  ISP12160 on the IO-8 and the Fuel.
