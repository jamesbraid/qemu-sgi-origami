.. SPDX-License-Identifier: BSD-3-Clause

.. _sgi-ip30:

================================
SGI Octane and Octane2 (IP30)
================================

The ``octane`` and ``octane2`` machines (``hw/mips/sgi/octane.c``) each
build one IP30 board with one processor, a serial console and the
onboard I/O, booting a stock IP30 PROM that the user supplies. Graphics
are optional and outside this page. This page is the hardware contract
the machine models in ``hw/mips/sgi/models/`` cite for these two
products: what is fitted, how it is wired, what the emulator chose
where the hardware is not known, and which sources back each statement.

Every fact carries one of four ratings:

established
  stated by a cited specification or manual, or read from an identified
  real machine;
inferred
  follows from established facts by a stated step;
approximation
  an emulator value standing in for hardware that is not known, with its
  limitation stated;
unknown
  no available source settles it; the entry says what would.

The two machine names select two fitted populations, not two hardware
architectures. Register behaviour follows the fitted chip and board
revisions, not the product badge: SGI sold upgraded older boards under
the Octane2 name, and some Octane2 changes reached older systems
(:ref:`sgi-ip30-sources`, SGIStuff page).

.. _sgi-ip30-sources:

Sources
=======

SGI manuals, by document number and printed page:

- *OCTANE Technical Report*, SGI, 1997, no document number; printed
  pages 2-3 (Crossbow ports), 2-15 (HEART, section 2.3.2), 2-19 (the two
  Bridges, section 2.3.3) and 7-1 (standard configurations).
- *Octane Workstation Owner's Guide*, 007-3435-004, printed page 54
  (memory fitting).
- *Octane2 Workstation Owner's Guide*, 007-4207-002, printed page 47
  (memory fitting) and page 148, Table 6-5 (console serial port).

Public software:

- OpenBSD/sgi port, the-machine-hall/openbsd-sgi branch
  ``sgi-is-alive-at-7.9`` at ``2df91d80c104636deeef8124252ddd5951b55fbf``:
  ``sys/arch/sgi/sgi/ip30.h`` lines 19-55 (memory base, widget numbers,
  IOC3 slot, GPIO wiring, PROM apertures), ``sys/arch/sgi/sgi/ip30_machdep.c``
  lines 163-202 and 262-266 (early console, product classification,
  widget windows), ``sys/arch/sgi/xbow/xbowdevs`` (widget part and
  manufacturer codes), ``sys/dev/onewire/onewiredevs`` lines 29-33 and
  ``sys/arch/sgi/dev/owserial.c`` lines 175-243 (board record format).
- Linux at ``db2ddb87143519e20a95aa36c60b36107b736a58``:
  ``arch/mips/include/asm/mach-ip30/spaces.h`` (memory offset),
  ``arch/mips/include/asm/cpu.h`` lines 73 and 76 (R10000 and R12000
  implementation codes), ``arch/mips/sgi-ip30/ip30-xtalk.c`` and
  ``arch/mips/pci/pci-xtalk-bridge.c`` lines 488-492 (IOC3 at device 2).

Public captures and accounts:

- Miod Vallat, OpenBSD/sgi story part 4,
  http://miod.online.fr/software/openbsd/stories/sgi4.html, retained copy
  SHA-256 ``307c9d4678f52b8fb0c43d1690f552fd774ac1620bdf9aa1299c6f85ff40b91e``,
  lines 1308-1352: an OpenBSD 4.3-beta boot on a 128 MiB Octane with a
  175 MHz processor module.
- Niclas Steinel-Hansson, comp.sys.sgi.hardware, 2001-11-03,
  Message-ID ``<3BE33F04.848B3CEE@odense.kollegienet.dk>``: ``hinv -m -v``
  of a 195 MHz R10000 Octane.
- SGIStuff Octane system page,
  https://archive.irixnet.org/sgistuff/hardware/systems/octane.html,
  retained copy SHA-256
  ``3b785cc77b38ad3ac1ae58c9c6094c28810625d943547c17d0f4700d888dd0ea``.
- NEC, *VR10000, VR12000 64-bit Microprocessor User's Manual*, third
  edition, U10278EJ3V0UM00, May 1998, printed pages 184-185 (reset mode
  bits) and 366 (R12000 clock divider encodings).

The Octane2 population also rests on an inventory (``hinv -mvv``, a
hardware-graph listing and three PROM-monitor reads) of one physical
Octane2 taken by its owner for this work. No public copy of that
inventory exists; facts resting on it say so.

.. _sgi-ip30-products:

Products
========

.. list-table::
   :header-rows: 1

   * - Machine
     - Processor
     - Memory
     - IP30 board
     - Basis
   * - ``octane``
     - one R10000, 195 MHz, 1 MiB secondary cache
     - 128 MiB
     - ``030-0887-003`` revision F
     - published 195 MHz inventory and the Technical Report's standard
       configurations
   * - ``octane2``
     - one R12000, 360 MHz, 2 MiB secondary cache
     - 256 MiB
     - ``030-1467-001`` revision B
     - the unpublished owner inventory

``octane`` (established, with one model choice): the Technical Report's
standard configurations (page 7-1) include 128 MiB single-processor
Octanes at 195 MHz, and the published inventory reports a 195 MHz R10000
revision 2.7 on board ``030-0887-003`` revision F. That inventory
reports 256 MiB; pairing its board and processor with 128 MiB is a
model choice. A 175 MHz, 128 MiB Octane also exists (the OpenBSD
capture), so memory size does not fix processor speed.

``octane2`` (established for one unpublished machine): the inventory
reports one 360 MHz R12000 chip revision 3.5 with 2 MiB secondary cache,
256 MiB, HEART revision F, board ``030-1467-001`` revision B and Crossbow
ASIC revision 1.4. Its V12 graphics board is not part of the emulated
configuration.

.. _sgi-ip30-components:

Components and connections
==========================

.. list-table::
   :header-rows: 1

   * - Component
     - Attachment
     - Rating and basis
   * - Processor
     - SysAD bus to HEART
     - established: Technical Report section 2.3.2, printed page 2-15
   * - HEART
     - SysAD endpoint; Crossbow widget 8
     - established: OpenBSD ``ip30.h`` (``IP30_HEART_WIDGET``); OpenBSD
       ``xbowdevs`` gives HEART part ``0xc001``, manufacturer ``0x36``
   * - Crossbow
     - seven of eight ports used
     - established: Technical Report printed page 2-3
   * - Onboard Bridge
     - Crossbow widget 15
     - established: OpenBSD ``ip30.h`` (``IP30_BRIDGE_WIDGET``); the
       OpenBSD capture finds the Bridge at widget 15
   * - Two QLogic SCSI controllers
     - onboard PCI devices 0 and 1, function 0
     - established: the OpenBSD capture (ISP1020 revision 5); the Octane2
       inventory reports QL1040B parts and its hardware graph places them
       at devices 0 and 1
   * - IOC3
     - onboard PCI device 2, function 0
     - established: OpenBSD ``ip30.h`` (``IP30_IOC_SLOTNO``), Linux
       ``pci-xtalk-bridge.c``, and the OpenBSD capture (revision 1)
   * - RAD1 audio
     - onboard PCI device 3, function 0, identity ``10a9:0005``
     - established for the captured Octane (revision ``0xc0``); the
       Octane2's PCI revision byte is unknown (its inventory prints
       ``12.0``, which is not a PCI revision byte)
   * - DS1685-family clock
     - behind IOC3
     - established: the OpenBSD capture reports a DS1687
   * - Expansion Bridge for four PCI slots
     - not built
     - established that it exists (Technical Report printed page 2-19);
       its widget number is unknown

The machines model buses ``sysad``, ``xio`` (mastered by Crossbow) and
one onboard ``pci`` bus mastered by the Bridge, matching the table.

Revisions fitted by the model (``widget_revision`` in the module
layers):

- ``octane``: Crossbow 4, HEART 4, Bridge 3, all established from the
  OpenBSD capture of a 175 MHz Octane. The published 195 MHz inventory
  prints HEART revision D and Xbow ASIC revision 1.2; IRIX's lettered
  and dotted forms are not the raw register fields, and their mapping is
  inferred, not established.
- ``octane2``: HEART widget identification word ``0x6c00106d``
  (revision 6, part ``0xc001``, manufacturer ``0x36``), read on the
  owner's machine from the PROM monitor and consistent with its
  inventory's HEART revision F (established, unpublished). Crossbow
  revision 5, from a PROM-monitor read of its identification word
  ``0x50000001`` beside an inventory revision of 1.4 (established,
  unpublished). Bridge revision 3 is an approximation: the inventory
  does not report the Octane2 Bridge revision, and the value is carried
  over from the earlier Octane.

IOC3 general-purpose input 2 tells the two products apart in software:
OpenBSD reads it as 1 for Octane and 0 for Octane2 (``ip30.h``,
``IP30_GPIO_CLASSIC``; ``ip30_machdep.c`` lines 193-202). The model
drives it high on ``octane`` and low on ``octane2``. This is a public
driver expectation (inferred); the electrical wiring is unknown.

.. _sgi-ip30-address-map:

Processor address map
=====================

Established from OpenBSD ``ip30.h`` and ``ip30_machdep.c``, with Linux
``ip30-xtalk.c`` and ``mach-ip30/spaces.h`` agreeing on the widget
windows and the memory offset:

.. list-table::
   :header-rows: 1

   * - Physical address
     - Contents
   * - ``0x10000000 + (widget << 24)``
     - 16 MiB small window for each widget
   * - ``widget << 36``
     - 64 GiB large window for each widget
   * - ``0x1fc00000``
     - Bridge PROM aperture: widget 15's small window at offset
       ``0xc00000``, 2 MiB long; inferred to be the reset fetch address
       from that placement
   * - ``0x1fe00000``
     - alternate PROM aperture, widget 15 offset ``0xe00000``
   * - ``0x20000000``
     - first SDRAM address

.. _sgi-ip30-space-selection:

Address-space selection
-----------------------

The architecture layer records the selector as a two-bit processor
attribute in virtual-address bits 58:57, giving four spaces. The
R10000 family drives such an uncached attribute beside the address,
and the public SN0 header (Linux ``arch/mips/include/asm/mach-ip27/spaces.h``)
documents its use on Hub systems. That HEART decodes it the same way is
inferred from the shared processor family, not established; no public
HEART specification describes it.

.. _sgi-ip30-processors:

Processors
==========

``octane`` fits an R10000 at 195 MHz with 1 MiB secondary cache
(established, :ref:`sgi-ip30-products`). Its processor identification
word ``0x927`` is inferred: Linux ``cpu.h`` gives the R10000
implementation code ``0x09``, and the published inventory reports chip
revision 2.7. No raw PRId read was published.

``octane2`` fits an R12000 at 360 MHz with 2 MiB secondary cache
(established, unpublished inventory).

.. _sgi-ip30-r12000-defaults:

R12000 catalogue defaults
-------------------------

The ``octane2`` catalogue supplies every R12000 input, so ``-M octane2``
starts without CPU options. Each one can be overridden by the CPU
property named:

.. list-table::
   :header-rows: 1

   * - Value
     - Property
     - Rating
   * - PRId ``0x0e35``
     - ``r12000-prid``
     - inferred: the running IRIX kernel on the owner's machine recorded
       this word; Linux ``cpu.h`` gives the R12000 implementation code
       ``0x0e`` and the inventory reports revision 3.5. It is kernel
       inventory state, not a raw CP0 read.
   * - FPU identification ``0x0900``
     - ``r12000-fpu-id``
     - inferred: the same kernel record; not a raw CP1 read
   * - Reset mode word ``0x001aa683``
     - ``r12000-reset-mode``
     - approximation: big-endian, secondary-cache size code ``010``
       (2 MiB), 32-word secondary-cache blocks, Kseg0 algorithm 3, and
       other divisor and mode fields chosen only to be valid encodings.
       The NEC manual (pages 184-185) says cold reset loads these bits
       from SysAD, and page 366 gives the R12000 divider encodings, but
       no source gives the word this board drives.
   * - Secondary cache 2 MiB
     - ``r12000-scache-bytes``
     - established by the inventory; the model derives it from the reset
       word
   * - Secondary-cache block 32 words
     - ``r12000-scache-block-words``
     - approximation: derived from the chosen reset word; the fitted
       block size is unknown

.. _sgi-ip30-memory:

Memory kits and capacity
========================

Both owner's guides describe four DIMM banks of two sockets each. Bank 1
must be filled, banks fill in order without gaps, and each populated
bank holds two DIMMs (established: 007-3435-004 page 54, 007-4207-002
page 47). The Octane guide requires the two DIMMs of a bank to match in
capacity and type and allows 64 to 512 MiB per bank; the Octane2 guide
puts the highest-density DIMMs in bank 1 and allows up to 2 GiB per
bank.

The assembly layer records eight DIMM slots as four banks of two DIMMs,
one processor slot, and accepted bank sizes of 64, 128 and 256 MiB.
The bank-size list is a model limit, narrower than either guide.

.. list-table::
   :header-rows: 1

   * - Kit
     - DIMMs
     - Bank
     - Used by
   * - ``kit-128``
     - two 64 MiB
     - one 128 MiB bank
     - ``octane``
   * - ``kit-256``
     - two 128 MiB
     - one 256 MiB bank
     - ``octane2``

The kits are model choices (approximation). Neither inventory reports
which sockets are filled or the DIMM sizes, only the total. The machines
also accept other layouts that sum to the same total, through
``bank<N>-size``: 128+128 or 64+64+64+64 MiB on ``octane2``, and 64+64
on ``octane``. The guides number physical banks 1 to 4; public software
indexes HEART bank descriptors 0 to 3. Treating them as the same
positions is inferred, not a measured board wiring fact. HEART revision
F's descriptor reset values and density encoding are unknown.

.. _sgi-ip30-topology:

Topology and population
=======================

Each product has one node (the IP30 board), one processor and no
fabric. Dual-processor Octanes exist (Technical Report page 7-1) but are
not modelled; the
capacity notes restrict both machines to the single-processor profile.
This is a scope limit, not a hardware claim.

.. _sgi-ip30-board-identity:

Board identity records
======================

The model fits four one-wire board records per machine: the processor
module on HEART's one-wire master, the system board on the onboard
Bridge's one-wire master, and the frontplane and power supply on IOC3's
one-wire bus beside its Ethernet-address part.

.. list-table::
   :header-rows: 1

   * - Machine and role
     - Name
     - Part
     - Revision
     - Basis
   * - ``octane`` processor
     - ``PM10``
     - ``030-0888-004``
     - B
     - published 195 MHz inventory
   * - ``octane`` system board
     - ``IP30``
     - ``030-0887-003``
     - F
     - same
   * - ``octane`` frontplane
     - ``FP1``
     - ``030-0891-003``
     - A
     - same
   * - ``octane`` power supply
     - ``PWR.SPPLY.SR``
     - ``060-0028-003``
     - A
     - same, and the OpenBSD capture
   * - ``octane2`` processor
     - ``PM10360MHZ``
     - ``030-1590-002``
     - B
     - unpublished owner inventory
   * - ``octane2`` system board
     - ``IP30``
     - ``030-1467-001``
     - B
     - same
   * - ``octane2`` frontplane
     - ``FP1``
     - ``030-0891-003``
     - H
     - same
   * - ``octane2`` power supply
     - ``PWR.SPPLY.ER``
     - ``060-0035-002``
     - A
     - same

Names, parts and revisions are established from the inventories. The
rest of each record is not:

- Record format (inferred): the OpenBSD capture enumerates one 16 Kbit
  part on HEART and two 16 Kbit parts plus a 1 Kbit Ethernet-address
  part on IOC3; OpenBSD's ``onewiredevs`` assigns 16 Kbit to the DS2505
  family (``0x0b``). OpenBSD's ``owserial.c`` accepts a two-page record
  with a format byte ``0x01``, a split twelve-character part number,
  flag bytes and a name, which the model builds. This is an accepted
  software format, not a dump of factory programming (approximation).
- The system-board record on the Bridge's one-wire line is an inferred
  fitment and route: inventories show an IP30 system board, but the
  OpenBSD capture shows no Bridge one-wire responder. No physical route
  has been measured (unknown).
- Barcodes ``EMU<letter>00`` and ROM serials ``0x51454d55<letter>00``
  (``QEMU`` and a letter) are synthetic and marked so inventories show
  them as emulated. The IOC3 Ethernet-address part keeps the ``0x5e7``
  code drivers match on, then the ``EMU`` marker. None is taken from a
  real machine.
- Physical ROM serials, page checksums, padding and unused memory on real
  IP30 boards are unknown.

.. _sgi-ip30-flash-wiring:

Boot flash wiring
=================

This is an open question. What is known:

- The Bridge ASIC provides two 2 MiB flash regions, at widget offsets
  ``0xc00000`` and ``0xe00000``, and a strap input chooses which chip
  select appears at the lower region (established for the ASIC; the
  public Bridge specification is described in :doc:`sgi-bridge`).
- OpenBSD names the same two apertures (``ip30.h`` lines 49-55).
- Two IP30 board families exist, ``030-0887`` and ``030-1467`` (the
  inventories; the SGIStuff page describes the board change). Neither
  source identifies a flash part.
- On the owner's revision B Octane2, one 16-bit PROM-monitor read at
  ``0x1fe00000`` returned ``0xffff`` (established, unpublished; one
  word only).

Unknown: the fitted flash part and its capacity, which Bridge select it
sits on, the strap value, the upper address wiring and protection pins,
and what the rest of the alternate aperture returns. A public service
drawing or a readable board photograph identifying the flash package,
followed by that part's manual, would settle the part; the select and
strap need a board schematic or measurement.

The model's choices, all approximations:

- ``flash-select`` false and ``flash-strap`` false in both product
  layers (``boot_flash_wiring``): the boot flash on select 0, strapped
  to boot from it. This is the value every launch has used, not a
  measurement. The machine properties of the same names override it.
- A 1 MiB array (``firmware_bytes``), sized from a candidate part's
  datasheet, implemented with QEMU's Intel-command-set ``pflash_cfi01``
  and compatibility identifiers. The fitted part is unknown, so the
  size is not a hardware claim.
- The second select reads as a blank, all-ones region and discards
  writes, which reproduces the one measured word and nothing more.

.. _sgi-ip30-storage:

Storage items
=============

Each machine lists three writable stores in its catalogue offering,
bound through the machine property of the same name:

.. list-table::
   :header-rows: 1

   * - Item
     - Size
     - Starts from
   * - ``node0-flash``
     - 1 MiB
     - the supplied IP30 PROM at offset 0, every other byte erased; the
       IP30 flash carries no configuration block the machine formats
   * - ``nvram0``
     - 512 bytes
     - zero
   * - ``nvram0-clock``
     - 24 bytes
     - zero

.. _sgi-ip30-timekeeper:

Timekeeper
----------

The clock behind IOC3 is a DS1685-family part (the OpenBSD capture
reports a DS1687; established). The model keeps 512 bytes of its NVRAM
and a 24-byte clock record beside it, both starting zeroed; the part
treats an all-zero image as fresh. The store sizes and zero start are
emulator choices (approximation).

.. _sgi-ip30-launch:

Launch metadata
===============

These are emulator choices for launchers, not hardware facts.

- Console ``ioc3-serial-a``. The Octane2 guide assigns a console
  terminal to serial port 1 (page 148), OpenBSD's early console uses
  IOC3 UART A behind the onboard Bridge (``ip30_machdep.c`` lines
  163-182), and the OpenBSD capture shows ``com0`` at IOC3 offset
  ``0x20178`` as console. That the stock PROM selects that port with no
  graphics board fitted was observed only on the model.
- PROM dialogue ``none``: no launcher-driven PROM dialogue is defined.
- Disk bus ``scsi``: the boot disk attaches to the first onboard SCSI
  controller. The owner's hardware graph places its disk at PCI device
  0, target 1; the physical bay and channel wiring are unknown.
- Netboot adapter ``none``; supported guest ``irix``; default CPU
  model R10000 or R12000.

.. _sgi-ip30-unknowns:

Open questions
==============

- The flash part, select, strap and alternate-aperture contents
  (:ref:`sgi-ip30-flash-wiring`).
- R12000 reset mode word and cache block size
  (:ref:`sgi-ip30-r12000-defaults`).
- HEART revision F bank descriptors and density encoding, and the
  bank-number to descriptor-index mapping (:ref:`sgi-ip30-memory`).
- The Octane2 Bridge silicon revision and RAD PCI revision byte.
- The expansion Bridge's widget number.
- Interrupt wiring from the onboard PCI functions to Bridge inputs; no
  public source establishes it, and this page does not state it.
