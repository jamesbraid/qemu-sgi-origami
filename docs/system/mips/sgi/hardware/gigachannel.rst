.. SPDX-License-Identifier: BSD-3-Clause

.. _sgi-gigachannel:

==================================
GIGAchannel: XIO expansion cabinet
==================================

Used in :ref:`sgi-ip27` (the Origin 200, ``origin200-gigachannel`` and
``origin200-gigachannel-loopback`` topologies).

.. seealso::

   Platform pages: :ref:`sgi-ip27`.

The GIGAchannel is a separate I/O cabinet for the Origin 200. It holds one
:term:`Crossbow`, one :term:`Bridge` with four PCI slots and five XIO slots. Two XIO cables
join it to one Origin 200 CPU module. The upgrade removes the module's
passive XTalk jumper and fits a Crosstown adapter in its place, so after
conversion the module's own :term:`Hub` reaches its own IP29 Bridge only through the
cabinet's Crossbow. The node cannot boot without either the cabinet or a
loopback cable :src:`007-3708-002 pp. 19-24, 63`.

QEMU builds the cabinet for ``-M origin200,topology=origin200-gigachannel``
and the converted module without a cabinet for
``topology=origin200-gigachannel-loopback`` (:ref:`sgi-sn-gigachannel`).
The cabinet's own I/O is the type ``sgi-gigachannel`` in
``hw/mips/sgi/board-gigachannel.c``: a Bridge at widget revision 4, the
board record firmware reads from it, a BASEIO :term:`PROM` flash part and one
system-controller status word. The machine builds the cabinet's Crossbow
(:doc:`xbow`) and puts the module's IP29 Bridge on another of its links. The
Bridge is described in :doc:`bridge`. This page covers what is specific to
the cabinet.

SGI's names are GIGAchannel (the product), XBOX (the chassis model and
board-record prefix) and Crosstown (the cable adapters). The cabinet is
distinct from the PCI shoehorn, a one-slot XIO PCI adapter, and from the
Origin 2000 PCI carrier. Facts are rated as described in :ref:`sgi-ratings`.

.. _sgi-gigachannel-sources:

Sources
=======

- *Origin200 and Origin200 GIGAchannel Owner's Guide*,
  :src:`007-3708-002`.
- *Origin200 and Origin200 GIGAchannel Maintenance Guide*,
  :src:`007-3709-002`.
- *Origin200 GIGAchannel Upgrade Installation Instructions*,
  :src:`108-0187-001`.
- Published ``hinv -vm`` inventories: :src:`preterhuman-origin-200` (a
  one-module and a two-module system), a 2003 Usenet post by an Origin 200
  owner :src:`usenet-pigot-3edc9a71`, the 2012 thread *Building an Onyx 200*
  :src:`nekonomicon-16726704` and a two-module system with a cabinet on
  each module :src:`nekonomicon-16731033`. A 1999 boot log
  :src:`usenet-rasey-384c6090` shows the console lines of a one-module
  system.
- Photographs of a CMN A019 cabinet and of its interface board,
  :src:`ntc-tech-origin-200-cmn-a019` and
  :src:`ordi-spare-030-1252-001`.
- *Tensor Processing Unit (TPU) XIO Board Introduction*,
  :src:`007-4222-002`, for the physical XIO slot numbers.
- The stock IP27 PROM 6.156, :src:`ip27-prom-6.156`, and the IRIX 6.5.30
  kernel, as software expectations.

No physical GIGAchannel has been measured for this page. The reference
Octane2 (:ref:`sgi-measured-octane2`) is not an Origin 200. Public Linux,
OpenBSD and NetBSD trees hold no GIGAchannel support: a search of them for
the ``XBOX`` name finds only unrelated drivers.

.. _sgi-gigachannel-identity:

Identity
========

.. list-table::
   :header-rows: 1
   :widths: 22 46 32

   * - Item
     - Value
     - Rating
   * - Product name
     - Origin 200 GIGAchannel expansion cabinet (also "expansion chassis")
     - *established* :src:`007-3708-002 p. 5, Table 1-1`
   * - Chassis regulatory model
     - CMN A019, on the rear label. The label also prints ``XBOX-RM`` and a
       serial-like code. Which field each fills is not settled.
     - *observed* :src:`ntc-tech-origin-200-cmn-a019 image 03`
   * - Input rating
     - 100-240 VAC, 6-3 A, 50/60 Hz, 720 W on the label. Published input
       power for a fully loaded cabinet is 576 W.
     - *observed* (label); *established*
       :src:`007-3708-002 p. 25, Table 1-4`
   * - Cabinet interface board
     - 030-1252-001, silkscreen "XBOX XTALK INTERFACE". Board record
       ``XBOX_DUALXTOWN``, revision F in every capture.
     - *observed* :src:`preterhuman-origin-200 hinv -vm`,
       :src:`usenet-pigot-3edc9a71`, :src:`ordi-spare-030-1252-001`
   * - CPU-side Crosstown board
     - 030-1264-001, "PCA XBOX EXTENSION BOARD O200". Board record
       ``XBOX_EXT`` revision C. The upgrade guide's loopback example prints
       the record as ``2G_MPLN`` revision B.
     - *established* for the part number and name
       (:src:`108-0187-001 p. 1-2, Table 1-1`); *observed* for the board
       record (:src:`preterhuman-origin-200 hinv -vm`,
       :src:`usenet-pigot-3edc9a71`)
   * - Cabinet midplane
     - 030-1380-002, "PCA XBOX MIDPLANE O200"
     - *inferred* as a part identity: the number comes from seller listings
       that are not registered sources, and no capture shows its board
       record
   * - PCI backplane
     - Silkscreen "...0-1250-0?", with the leading digits and the last digit
       hidden. The PROM's ``030-1250-`` part prefix agrees.
     - *inferred* :src:`ntc-tech-origin-200-cmn-a019 image 02`
   * - Cabinet Bridge
     - Bridge widget revision 4, printed ``ASIC BRIDGE Rev 4, (widget 8)``.
       The letter this revision corresponds to is not printed anywhere.
     - *established* as printed in the upgrade guide's listing
       (:src:`108-0187-001 p. 3-8`); *observed* in the 2012 inventory
       (:src:`nekonomicon-16726704`)
   * - Upgrade kits
     - ``HU-CONNECT-XBOX`` (module hardware) and ``561-CHAS-XBOX`` (two
       ``018-0575-001`` XIO cables and a ``018-0649-001`` grounding cable)
     - *established* :src:`108-0187-001 Table 1-1`

The Crosstown and cabinet records are human-readable fields in ``hinv -vm``,
not raw one-wire bytes. That post adds ``Group ff Capability ffffffff
Variety ff`` for each record. *observed* in three systems across two
sources.

.. _sgi-gigachannel-composition:

Physical composition
====================

.. list-table::
   :header-rows: 1
   :widths: 24 46 30

   * - Assembly
     - What it carries
     - Rating
   * - Crosstown board in the CPU module
     - Replaces the XTalk jumper through flex cable 030-1273-001. Carries
       connectors XIO A and XIO B.
     - *established* :src:`007-3708-002 pp. 19-24`,
       :src:`108-0187-001 Figure 2-3`
   * - Interface board 030-1252-001 in the cabinet
     - Two external XIO connectors on one bracket, a card-edge connector to
       the midplane, two heatsinked ASICs with illegible markings, and link
       and data status LEDs for each of XIO A and XIO B. No Crossbow- or
       Bridge-sized package is visible on it.
     - *observed* :src:`ordi-spare-030-1252-001`. The maintenance guide
       covers the board's replacement, *established* for its presence
       :src:`007-3709-002 pp. 60-65`
   * - Midplane 030-1380-002
     - The XIO slots, the interface board and the PCI backplane plug into
       it. It carries a voltage regulator with a diagnostic LED.
     - *established* :src:`007-3709-002 pp. 107-123`
   * - Crossbow
     - In the cabinet. The Owner's Guide's block diagram places it there, and a
       builder reports the base I/O feed passing "through the
       Gigachannel's XBOW and back".
     - *established* for the block diagram
       (:src:`007-3708-002 Figure 1-5, p. 7`), *observed* for the builder's
       report (:src:`nekonomicon-16726704`). That it sits on the midplane is
       *inferred* from :src:`007-4222-002 p. 1`
   * - Cabinet Bridge
     - Drawn between the midplane and the PCI backplane.
     - *established* as present :src:`007-3708-002 Figure 1-5`,
       :src:`108-0187-001 p. 3-8`. That its board is the midplane is
       *inferred*: no photograph shows it on the interface board or on the
       visible part of the backplane
   * - PCI backplane
     - Four PCI slots, three full-length and one half-length (the one
       nearest the power switch). The rear panel labels them 0 to 3.
     - *established* :src:`007-3708-002 pp. 19-24`,
       :src:`007-3709-002 p. 23`; *observed* for the rear-panel labels
       :src:`ntc-tech-origin-200-cmn-a019 image 06`
   * - XIO slots
     - Five, physical numbers 11 to 15, customer-installable.
     - *established* :src:`007-3708-002 Table 1-1`,
       :src:`007-3709-002 pp. 33-41`, :src:`007-4222-002 Figure 4`
   * - Power supply, blower, LEDs
     - One supply (a redundant version needs module controller firmware
       3.3S or later), one constant-speed blower, three internal
       diagnostic LEDs and a front status LED.
     - *established* :src:`007-3708-002 pp. 30-32`,
       :src:`007-3709-002 pp. 107-123`

No source describes a register block specific to the cabinet apart from the
status word under :ref:`sgi-gigachannel-status`. The model is a composition
of existing devices with product-specific wiring and identity records.

.. _sgi-gigachannel-topology:

Cabling and link assignment
===========================

Two XIO cables join the module's XIO A to the cabinet's XIO A and its XIO B
to the cabinet's XIO B. Both are required. Loopback mode joins the module's
own XIO A to its XIO B with one cable and runs without a cabinet. The
cabinet may attach to the master or the slave module. One cabinet cannot be
shared by two modules and cabinets cannot be daisy-chained. XIO cables must
not be removed with either chassis powered. All *established*
:src:`007-3708-002 pp. 19-24, 63, 109-110`.

Each cable carries one complete :term:`Crosstalk` link, one joining the module's Hub
to the cabinet and the other its IP29 Bridge. This is *inferred* from the
Crosstown board replacing a jumper that joined Hub to Bridge. Which cable
carries which link is *unknown*.

.. list-table::
   :header-rows: 1
   :widths: 10 24 24 42

   * - Crossbow link
     - Endpoint
     - Software location
     - Rating
   * - 8
     - Cabinet Bridge, four PCI slots
     - ``io7``, board ``xbox_dualxtown``
     - *observed*: ``PCI_XIO ... Slot io7`` and ``ASIC BRIDGE Rev 4,
       (widget 8)`` :src:`nekonomicon-16726704`. The upgrade guide's
       listing prints the same, *established* :src:`108-0187-001 p. 3-8`
   * - 9
     - The module's IP29 Bridge (base I/O)
     - ``io1``, board ``MotherBoard``
     - *observed* in one capture: ``BASEIO ... Slot io1``, ``ASIC BRIDGE
       Rev 4, (widget 9)`` :src:`nekonomicon-16726704`. The upgrade guide's
       example listing disagrees: it shows ``BASEIO ... Slot MotherBoard``
       with ``ASIC BRIDGE Rev 3, (widget 8)`` beside the cabinet's own
       widget 8. Two Bridges cannot share widget 8 on one Crossbow, so the
       guide's example reads as a composed listing and the model follows
       the capture. The base I/O Bridge revision (3 or 4) is *unknown*
   * - A
     - The module's Hub
     - The node's own slot (``n1``)
     - *inferred*: no capture prints the Hub's port. The stock PROM names
       link A the node's own slot, as its Origin 2000 table does
       (*software expectation*, :src:`ip27-prom-6.156 widget-to-slot table`)
   * - B
     - Optional XIO board
     - ``io2``
     - *observed*: a Fibre Channel XIO board at ``io2`` reports widget 11
       :src:`nekonomicon-16726704`
   * - C, D, E, F
     - Optional XIO boards
     - ``io3``, ``io4``, ``io5``, ``io6``
     - *software expectation*: the stock PROM's table for an Origin 200
       names widgets 8 to F ``io7``, ``io1``, the node's own slot, ``io2``,
       ``io3``, ``io4``, ``io5`` and ``io6``. It is selected when the
       machine-type word at physical ``0x10000090`` is nonzero
       :src:`ip27-prom-6.156 widget-to-slot table`. Real boards sit at these
       names, *observed* :src:`preterhuman-origin-200`

Physical XIO slots 11 to 15 correspond, as a set, to links B to F and
positions ``io2`` to ``io6``. The one-to-one mapping is *unknown*.

The cabinet's four PCI slots are labelled 1 to 4 in the Owner's Guide's block
diagram :src:`007-3708-002 Figure 1-5` and 0 to 3 on the chassis, and IRIX
numbers PCI slots from 0 :src:`007-3708-002 p. 135`. That they are Bridge
devices 0 to 3 is *inferred* from hardware-graph paths of the form
``io7/xbox_dualxtown/pci/0`` :src:`nekonomicon-16731033`.

In a two-module system with one cabinet on module 1, module 1's base I/O
moves to ``io1`` behind the Crossbow and module 2's stays directly on its
Hub. This is *observed* for the module-1 case
(:src:`preterhuman-origin-200`, :src:`nekonomicon-16726704`). A system with
a cabinet on each module reports the same layout on each, *observed*
:src:`nekonomicon-16731033`. That capture prints no widget numbers, so it
does not settle the link rows. How IRIX treats a single cabinet on the slave
module alone is *unknown*.

.. _sgi-gigachannel-firmware-view:

What firmware and IRIX report
=============================

Each item is real-hardware output unless it names another source.

- **Board records.** ``hinv -vm`` lists the cabinet under
  ``/hw/module/1/slot/io7/xbox_dualxtown`` as ``XBOX_DUALXTOWN`` part
  030-1252-001 rev F, and lists ``XBOX_EXT`` part 030-1264-001 rev C beside
  the ``IP29`` record under ``/hw/module/1/slot/io1/MotherBoard``.
  *observed* in three systems across two sources
  (:src:`preterhuman-origin-200`, :src:`usenet-pigot-3edc9a71`). The wiring of the one-wire lines is
  *unknown*; see :ref:`sgi-gigachannel-record-wiring`.
- **Base I/O location.** With a cabinet the base I/O is
  ``/hw/module/1/slot/io1``. A plain Origin 200 reports
  ``slot/MotherBoard``. *observed* :src:`nekonomicon-16726704`,
  :src:`usenet-rasey-384c6090`. The upgrade guide's example disagrees (see
  the link table).
- **Bridge inventory.** The PROM's ``hinv -v`` prints ``PCI_XIO Origin 200
  IO Board, Module 1, Slot io7`` with ``ASIC BRIDGE Rev 4, (widget 8)``,
  and IRIX prints ``Origin PCI XIO board, module 1 slot 7: Revision 4``.
  The Crossbow does not appear as an inventory line in any capture.
  *observed* :src:`nekonomicon-16726704`. The upgrade guide's listing
  prints the PROM lines too, *established* :src:`108-0187-001 p. 3-8`.
- **I/O PROM load message.** On a GIGAchannel the PROM prints ``Loading
  BASEIO prom from XBOX Flashprom....`` followed by ``DONE``. Origin 2000
  systems print ``Loading BASEIO prom ....``. In the 2012 capture the
  BASEIO PROM that follows reports the build dated Nov 18, 2003, 11:26:28,
  the build that ships as the BaseIO card's flash image, not the build
  stored inside the node PROM (11:05:48). *observed*
  :src:`nekonomicon-16726704`, :src:`usenet-rasey-384c6090`.
- **Loopback inventory.** In loopback the PROM inventory matches the
  pre-upgrade one except for the added 030-1264-001 record, with the base
  I/O at ``Slot MotherBoard`` and ``ASIC BRIDGE Rev 3, (widget 8)``.
  *established* :src:`108-0187-001 pp. 2-9 to 2-11`.
- **IRIX releases.** IRIX 6.4 needs a patch installed before the hardware,
  and installing the hardware first prevents boot. IRIX 6.5 needs none.
  *established* by the release note for patch SG0003174 on SGI's
  *Origin200 GIGAchannel System Disk Patch Supplement* CD, part
  812-0788-001, which cites SGI document 007-3907-001. No public copy is
  registered. The patch payload was not examined.

What the stock IP27 PROM 6.156 does (each a *software expectation*,
:src:`ip27-prom-6.156`):

- **Machine type.** Every step below is taken only when the machine-type
  word at physical ``0x10000090`` is nonzero, as on an Origin 200. The model
  sets it to 1.
- **Cabinet detection.** On each boot the PROM reads the identification
  word of widget 0 through each node's Hub. It takes a part field of
  ``0x0000`` (a Crossbow) as the cabinet. It also accepts ``0xd000``, the part
  number of the :term:`XBridge` crossbar (widget 0, *established* on :doc:`xbridge`).
  Why an IP27 PROM accepts it is *unknown*. PROM 6.11 accepts ``0x0000`` only. No board
  identity record takes part in the decision.
- **Load address.** With a cabinet found, the PROM prints the XBOX load
  line and loads the BASEIO PROM from the cabinet Bridge's widget 8 at
  offset ``0xc0_0000``, the base chip select of the external-flash port.
  The loader's identify and copy sequence is the one it runs at an
  Origin 2000 BaseIO card (:doc:`baseio`): set the flash write-enable bit
  in the Bridge control register, probe the part with the AMD unlock
  sequence, check the header magic and the per-segment sums. On a bus error,
  an unrecognised part, bad magic or a bad sum it prints ``Retrying using
  the IP27 PROM internal copy of BASEIO PROM`` and loads the reduced copy
  it carries. A cabinet whose flash is missing or blank still boots.
- **Part-number recognition.** The PROM classifies a board whose identity
  part number starts ``030-1250-`` or ``030-1252-`` as the inventory type
  IRIX prints as ``PCI_XIO``. Linux names that board type
  ``KLTYPE_HAROLD``. The prefix ``030-1264-`` is in a separate list in the
  same routine and its use is unknown.
- **Two modules.** With a cabinet on each module the PROM keeps the node
  number of the last cabinet it finds. What a second module's node then
  fetches as its BASEIO PROM is *unknown*. The model builds one cabinet.

.. _sgi-gigachannel-status:

Register map
============

The cabinet has no registers of its own. Its Bridge answers the registers
in :doc:`bridge`. The one cabinet-specific location is on the Bridge's
external-flash port, whose two 2 MB chip selects sit at widget offsets
``0xc0_0000`` and ``0xe0_0000``.

.. list-table::
   :header-rows: 1
   :widths: 16 20 8 8 14 34

   * - Widget offset
     - Name
     - Width
     - Reset
     - Access
     - Notes
   * - ``0xc0_0000`` to ``0xdf_ffff``
     - BASEIO PROM flash
     - 16
     - image
     - read, command writes
     - Base chip select. A part answers here only when the machine supplies
       an image; see :ref:`sgi-gigachannel-flash`.
   * - ``0xe0_0000`` to ``0xff_ffff``
     - System-controller status
     - 16
     - 0
     - read
     - One word, repeated at every halfword of the chip select. Writes are
       dropped.

System-controller status word
-----------------------------

The cabinet holds its own power supply and blower, watched by a small
system controller. Software sees it as one read-only 16-bit word at the
start of the Bridge's second chip select, widget 8 offset ``0xe0_0000``. The
BASEIO PROM reads it to print the cabinet's redundant power supply in
``hinv``. The IRIX 6.5.30 kernel reads it at start-up and then polls it
about once a minute, if the clock tick is 100 Hz *(inferred)*. Neither
writes it. Every reader uses a 16-bit load. That software reads this word at
this address is a *software expectation*. That the hardware answers without
a bus error is *inferred*, since real systems boot IRIX and print ``hinv``.
No source describes the controller's hardware, so everything below the
address is known only from what software does with the bits. From a
processor the word is at the uncached Hub I/O window for widget 8, plus
``0xe0_0000``.

.. list-table::
   :header-rows: 1
   :widths: 8 10 52 30

   * - Bit
     - Mask
     - The kernel's reading
     - Other readers
   * - 0
     - ``0x01``
     - "Gigachannel blower failure". Starts an orderly shutdown.
     -
   * - 1
     - ``0x02``
     - "Gigachannel unknown system controller event". Printed only.
     -
   * - 2
     - ``0x04``
     - "Gigachannel unknown system controller event". Printed only.
     -
   * - 3
     - ``0x08``
     - "Power supply over temperature". Starts an orderly shutdown.
     -
   * - 4
     - ``0x10``
     - "Backup power supply kicked in - redundancy lost". Printed once, and
       the redundant-supply inventory entry changes to redundancy lost.
     - The BASEIO PROM prints ``Lost Redundancy`` when set and
       ``Enabled`` when clear.
   * - 5
     - ``0x20``
     - "Manufacturing mode". Printed once per boot.
     -
   * - 6
     - ``0x40``
     - Read once at start-up: a redundant power supply is fitted.
     - The BASEIO PROM prints ``Redundant Power Supply in Gigachannel unit
       on Module N`` when set and nothing for the supply when clear.
   * - 7-15
     -
     - Not examined by either program.
     -

Each kernel message is printed as ``Gigachannel System Controller[<thread>]:
<text>``. The labels in the table are the kernel's own, a *software
expectation*. That real hardware sets bit 0 on a blower failure, bit 3 on an
over-temperature supply, bit 4 when the backup supply takes over and bit 6
when a redundant supply is fitted is *inferred* from those labels. The
Owner's Guide describes a single constant-speed blower whose failure shuts
the server down and a redundant supply option, which matches the labels but
describes no interface :src:`007-3708-002 pp. 30-32, 134`. What bits 1, 2
and 5 report on hardware is *unknown*.

What the IRIX 6.5.30 kernel does with the word, a *software expectation*:

- It acts only when the machine-type word is nonzero, and only for a node
  whose widget 0 reports part ``0x0000``.
- At start-up it reads the word once for bit 6 and bit 4, then starts one
  polling thread per cabinet node. The first check comes one period after
  the thread starts.
- Each poll examines bits 0 to 5 and skips a clear bit. Bits 0 to 3 repeat
  their message on every poll while set.
- Only one orderly shutdown starts per boot. It signals init. If the system
  is still up 120 s later (at 100 Hz), the kernel prints ``Powering off the
  system.`` and asks the module system controller for a reset, the same
  request a normal reboot makes. If that fails it prints a message and
  requests a warm reset through its Hub.
- A set bit 4 is reported once. The redundancy-lost state never returns to
  good, because a clear bit 4 is never examined. If bit 4 is already set at
  start-up nothing is printed for it.
- A kernel tunable turns all of this off.

The tick rate behind the 60 s and 120 s figures is *inferred* to be 100 Hz.
The console prefix of the messages is *unknown*.

Decode, upper byte and writes
-----------------------------

How many address lines the part decodes, what the rest of the 2 MB returns,
what the upper byte holds, whether the word changes on its own and what a
write does are *unknown*. No reader touches any other offset. A readback at
other offsets, or a photograph or drawing of the part on that chip select,
would settle them. The model's choices are under
:ref:`sgi-gigachannel-limits`.

.. _sgi-gigachannel-record-wiring:

How the cabinet record reaches firmware
=======================================

How the PROM reaches the ``XBOX_DUALXTOWN`` record, and which one-wire line
carries it, is *unknown*. No source states the wiring, and the part
030-1252-001 is described only by photographs of a board with two
heatsinked ASICs whose markings are not legible.

The model presents the record without claiming a wiring. It rides the
cabinet Bridge's own one-wire line, the line the firmware names ``io7``
from. The ``XBOX_EXT`` record rides the IP29 Bridge's own line, because
inventories list it beside the IP29 record. These are each an *approximation*. The
barcodes and serials are placeholders marked as synthetic, since a real one
names a physical unit. A capture of the raw one-wire bytes from a real
cabinet would settle them.

.. _sgi-gigachannel-flash:

BASEIO PROM flash
=================

- The image is *observed*. A real cabinet's BASEIO PROM banner is the
  build shipped as the BaseIO card's flash image
  :src:`nekonomicon-16726704`. The match is by banner, not by comparing
  bytes.
- The loader's contract is a *software expectation*. It reads the part
  through the same probe and copy as at a BaseIO card and accepts the same
  identities. The probe writes the AMD unlock sequence at word addresses
  ``0x555`` and ``0x2aa`` and reads manufacturer and device as halfwords at
  offsets 0 and 2.
- Which part SGI fitted is *unknown*, as are its placement on the board
  and the strap behind the Bridge's flash-select pin. A photograph of the
  part, or a PROM capture that prints the part type, would settle them.

The model fits the BaseIO card's part (:doc:`baseio`): 1 MiB, 512K x16 top
boot, AMD manufacturer ``0x01`` and device ``0x22da``, an *approximation*.
The loader accepts that identity and the image fits in it. The flash-select
strap reads zero in the model, so the part answers the base chip select,
where the loader reads.

A machine fits the part when it is given an image, in either of two ways:

- ``-drive if=none,id=sgi-gigachannel-flash<N>,file=IMAGE,format=raw``
  (``N`` counts cabinets), or the machine's ``io<N>-flash`` store binding.
  Giving both a ``-drive`` and an ``io<N>-flash`` binding is refused.
- The machine's ``io-prom`` option, or the cabinet's ``firmware`` property,
  names an original I/O PROM file. The part then starts out holding that
  file followed by erased bytes. A drive's contents take precedence over
  ``firmware``.

With neither, nothing is fitted. The base chip select then fails as an
unpopulated address does, with a bus error and a ``LOG_GUEST_ERROR`` line.
The stock PROM retries with its own internal copy and says so on the
console, as it would for a cabinet whose part is missing.

Power, reset and link behaviour
===============================

- **Power.** The cabinet's rear switch arms it. It turns on only when the
  attached module turns on, and the module's front power switch also turns
  it off. The Crosstown LEDs by each connector light when the link is up.
  *established* :src:`108-0187-001 p. 3-7`, :src:`007-3709-002 p. 2`. That
  the XIO cables carry this control is *inferred*, since only they and a
  ground strap join the chassis.
- **Order.** SGI's power-on order is Origin Vault, cabinet, slave module,
  master module. Turning on a chassis with the XIO cables not properly
  connected "may not function correctly". *established*
  :src:`007-3708-002 p. 81`.
- **Blower failure.** A blower failure shuts down the whole server,
  cabinet and CPU modules, immediately. The cabinet's only blower status is
  its front LED. *established* :src:`007-3708-002 pp. 30-32, 134`. How the
  module learns of it is *unknown*.
- **Missing cable.** A module "cannot boot" without the cabinet or a
  loopback cable. What the PROM prints and where it stops are *unknown*.
- **Loose XIO board.** A loose board lever lets the server boot, but the
  board "may not be recognized". *established*
  :src:`007-3708-002 p. 79`.
- **Link training.** No cabinet-specific rule was found. The Crossbow's link
  reset, width negotiation and alive state apply (:doc:`xbow`). Whether the
  Crosstown adapters add latency, retiming or state is *unknown*.

In the model the cabinet's reset and power follow its attached module. A
module reset restarts the cabinet Bridge, including its flash write-enable
bit, and with the module off the cabinet answers nothing until power
returns. This is an *approximation* of the powered-together behaviour above.

Migration
=========

``sgi-gigachannel`` has no state of its own and registers an empty
migration description at version 1. The status word is a construction-time
property, and the Bridge and the flash part migrate their own state.
Migration works only between identical builds.

.. _sgi-gigachannel-unknowns:

Unknowns
========

.. list-table::
   :header-rows: 1

   * - Unknown
     - What would settle it
   * - Which XIO cable carries the Hub link and which the Bridge link
     - A cabinet-side wiring drawing, or a capture that prints the cabinet
       Crossbow's ports
   * - Physical XIO slots 11 to 15 against links B to F
     - An inventory from a cabinet that prints widget numbers for each
       slot, with known physical placement
   * - That the Hub is on link A
     - A cabinet Crossbow register dump, or a capture printing the Hub's
       port
   * - The cabinet Crossbow's revision
     - A cabinet PROM ``hinv -v`` or ``xbstat`` capture
   * - The flash part SGI fitted, its placement and the flash-select strap
     - A photograph of the part, or a PROM capture that prints the part
       type
   * - The status word's part, decode, upper byte and dynamic behaviour,
       and whether bit 4 clears when redundancy returns
     - A photograph or drawing of the part, a readback at other offsets, or
       a ``hinv`` from a cabinet with a redundant supply
   * - Raw one-wire line and bytes of each board record
     - A one-wire bus capture from a real cabinet
   * - Why PROM 6.156 treats an XBridge crossbar (part ``0xd000``) at
       widget 0 as a cabinet
     - A cabinet-side capture or a public description of the PROM's
       cabinet test
   * - PCI slot to Bridge device and interrupt pin
     - A PROM ``hinv -v`` with cards in several cabinet slots
   * - A single cabinet on the slave module alone, and a converted module
       with neither cabinet nor loopback
     - An inventory or boot capture of each
   * - The console prefix of the kernel's cabinet messages, and the
       ``hinv`` text for a cabinet with a redundant supply
     - A console capture of a cabinet event, or a ``hinv -vm`` from such a
       cabinet

.. _sgi-gigachannel-limits:

Model limits
============

- The cabinet Bridge is widget revision 4. Its PCI slots are Bridge devices
  0 to 3 on the PCI bus ``io7.0``, an *inferred* mapping. A card at device 4
  or above is refused with a "reserved" slot error.
- The Crossbow is the Origin 2000 module's selected revision (Crossbow 1.1,
  widget revision 2), an *approximation*, since no capture prints the
  cabinet's. Its widget-0 part is ``0x0000``, which is the PROM's whole
  cabinet test. Links 8, 9 and A (the Hub, on widget 10) are present and
  alive from reset. Links B to F, widgets 11 to 15 and positions ``io2`` to
  ``io6``, read presence clear until a board fitted at launch claims one.
- The IP29 Bridge sits on link 9 with the ``XBOX_EXT`` record beside its own.
  The loopback topology is the plain Origin 200, with the Bridge directly on
  the Hub at widget 8, no Crossbow and the ``XBOX_EXT`` record added. The
  model fits the revision C record in loopback, where the guide's example
  shows revision B named ``2G_MPLN``. The load line the PROM prints in
  loopback is *unknown*. The model prints the plain ``Loading BASEIO
  prom`` line.
- The flash part is the BaseIO card's, as above. The status word answers
  the construction-time property ``system-controller-status``, default 0,
  set with ``-global sgi-gigachannel.system-controller-status=N``. Zero
  reads as a healthy cabinet with no redundant supply, so IRIX prints
  nothing and starts no shutdown. ``0x40`` gives a redundant supply with
  redundancy intact and ``0x50`` one already lost. This is an
  *approximation*: the kernel's reading of zero is a software expectation,
  that a real healthy cabinet reads zero is *inferred*, and the upper byte
  is unknown.
- The word reads the same at every halfword of its chip select, because the
  Bridge's flash port repeats a part smaller than a chip select. The part
  accepts only 16-bit accesses. A write that reaches it logs ``LOG_UNIMP``
  and is dropped. The Bridge itself drops writes with ``LOG_GUEST_ERROR``
  while its flash write-enable bit is clear. These are each an *approximation*.
- The word cannot change while the guest runs, so the kernel's event
  messages, the orderly shutdown and the forced reset are unreachable in
  the model.
- A chip select with no part, such as the IP29 Bridge's second one or the
  cabinet's base one with no image, logs ``LOG_GUEST_ERROR`` and fails as
  an unpopulated address does.
- Both ``XBOX`` records ride Bridge one-wire lines chosen from where
  inventories list them, with placeholder barcodes and serials (see
  :ref:`sgi-gigachannel-record-wiring`).
- ``sgi-gigachannel`` is not creatable with ``-device``. The machine places
  it, and its id and QOM name are ``io7``.
- Systems with two modules and cabinets, a cabinet on the slave module and a
  converted module with neither cabinet nor loopback are not built.
- XIO boards in the cabinet are the existing board models. Their records
  are those models' own, not the part numbers of the published inventories'
  boards. The model does not enforce the Origin 2000 rule that keeps an
  MSCSI out of one slot (:doc:`mscsi`).

Tests
=====

``tests/qtest/sn0-gigachannel-test.c`` covers: widget 0 reading the
Crossbow's part with the cabinet and a Bridge's part on the plain and
loopback machines; the Crossbow's startup-alive links and the empty-link
pattern for B to F; the cabinet Bridge at revision 4 and the IP29 Bridge at
their links; the status word, its default of zero, the property value and
the dropped, logged write; unpopulated flash selects logging as such; the
flash part fitted from a drive, with image bytes, the autoselect identity
and the status word beside it; the loopback machine having no Crossbow; a
card enumerating in each of the four PCI slots and a fifth refused; and the
``io-prom`` and ``firmware`` ways of seeding the part.

``tests/qtest/sn0-bridge-identity-test.c`` checks the ``XBOX_DUALXTOWN`` and
``XBOX_EXT`` record bytes in the cabinet, IP29 and loopback cases.
``tests/qtest/sn0-module-domain-test.c`` checks that the cabinet resets and
loses power with its module. ``tests/qtest/sn0-mscsi-test.c`` and
``tests/qtest/sn0-menet-test.c`` place XIO boards in the cabinet's
positions.

``tests/functional/mips64/test_origin200_gigachannel_prom.py`` boots the
stock PROM 6.156 on the cabinet topology. With a flash image it checks the
XBOX load line, the flash build's banner and passing diagnostics. Without
one it checks the load line, the bus error, the retry from the internal copy
and passing diagnostics. On the loopback topology it checks the plain load
line and passing diagnostics. These tests need the PROM image supplied
through ``QEMU_TEST_SGI_ASSET_DIR``.
