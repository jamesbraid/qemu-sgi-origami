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

- Miod Vallat, OpenBSD/sgi story part 4
  (``web:miod-openbsd-sgi-story-4``), lines 1308-1352: an OpenBSD 4.3-beta boot on a 128 MiB Octane with a
  175 MHz processor module.
- Niclas Steinel-Hansson, comp.sys.sgi.hardware, 2001-11-03
  (``web:usenet-3be33f04``): ``hinv -m -v`` of a 195 MHz R10000 Octane.
- SGIStuff Octane system page (``web:sgistuff-octane``): lists the 195 MHz
  R10000 with 1 MB of onboard secondary cache.
- NEC, *VR10000, VR12000 64-bit Microprocessor User's Manual*, third
  edition, U10278EJ3V0UM00, May 1998, printed pages 184-185 (reset mode
  bits) and 366 (R12000 clock divider encodings).

The Octane2 population also rests on measurements the owner of one
physical Octane2 took for this work, recorded in
:ref:`sgi-ip30-octane2-inventory`. Facts resting on them cite the
sub-section that holds the read.

.. _sgi-ip30-octane2-inventory:

Octane2 owner measurements
==========================

These are the measurements the owner of one physical Octane2 took for
this project. The machine ran IRIX 6.5.30m, reached over the network
shell for the IRIX commands, and its PROM Command Monitor for the
``dump`` reads; the V12 graphics board was fitted. Commands and returned
values are reproduced exactly. Board barcodes, Laser IDs, serial numbers
and MAC addresses are left out. The owner ran the PROM-monitor sessions
and supplied their full output. No read is a reset value: each shows what
this one machine answered at the stated moment.

.. _sgi-ip30-octane2-hinv:

Inventory (``hinv -mvv``, ``uname -R``), 2026-09-26
---------------------------------------------------

``uname -R`` printed ``6.5 6.5.30m``. A repeated ``hinv -mvv`` agreed with
the first. The inventory reports:

.. list-table::
   :header-rows: 1

   * - Component
     - Reported value
   * - Processor
     - one 360 MHz IP30 R12000, chip revision 3.5, 2 MiB secondary cache
   * - HEART
     - revision F
   * - IP30 board
     - ``030-1467-001``, revision B
   * - Frontplane
     - ``FP1``, ``030-0891-003``, revision H
   * - Power supply
     - ``PWR.SPPLY.ER``, ``060-0035-002``, revision A, under
       ``/hw/node/xtalk/15/pci/2``
   * - Memory
     - 256 MiB total (one 256 MiB bank was later seen configured)
   * - Crossbow
     - ASIC revision 1.4
   * - Processor module
     - ``PM10360MHZ``, ``030-1590-002``, revision B
   * - SCSI
     - two integral QL1040B controllers, revision 2; the disk is unit 1
       on controller 0
   * - Onboard I/O
     - IOC3 serial ports ``tty1`` and ``tty2``, parallel ``plp1``,
       Ethernet ``ef0``, RAD audio revision 12.0
   * - Graphics
     - V12 board at widget 11

The inventory shows neither which bay holds the disk nor how controller 0
maps to a PCI device number, and it reports no register values.

.. _sgi-ip30-octane2-graph:

Hardware graph, 2026-09-26
--------------------------

A read-only ``ls -l`` of the running hardware graph showed SCSI links under
``/hw/node/xtalk/15/pci/0`` and ``/hw/node/xtalk/15/pci/1``. The first
resolves to ``/hw/node/xtalk/15/pci/0/scsi_ctlr/0``, whose ``target``
directory holds ``1``, whose ``lun`` directory holds ``0``; that LUN holds
a ``disk`` directory and a ``scsi`` device node. The installed disk is
therefore PCI device 0, SCSI target 1, LUN 0 as the kernel names it. This
is kernel bookkeeping, not an electrical measurement. No device node was
opened.

.. _sgi-ip30-octane2-prom-header:

PROM headers, 2026-09-27
------------------------

``/usr/sbin/flash -V`` exited zero and printed the loaded primary PROM
header as version ``4.17``, length ``682448`` bytes, programmed
``2026-08-16 23:59:56 GMT``, ``7`` reloads; the recovery header reads
version ``4.9``, length ``147198`` bytes, same time and count.
``/usr/sbin/flash -d`` exited zero with no output. The flash contents
were not dumped or hashed.

.. _sgi-ip30-octane2-xbow-id:

Crossbow widget identification, 2026-09-28
------------------------------------------

.. code-block:: text

   >> dump -w -x 0x9000000010000004#1
   0x9000000010000004:   50000001

One 32-bit read of the Crossbow widget-zero identification word. Bits
31:28 give raw revision code 5 (established for this board at PROM
time).

.. _sgi-ip30-octane2-heart-id:

HEART widget identification and low memory, 2026-09-29
------------------------------------------------------

.. code-block:: text

   >> dump -d -x 0x9000000018000000#1
   0x9000000018000000:           6c00106d
   >> dump -d -x 0x9000000000002000#1
   0x9000000000002000:                  0

The first is one 64-bit read of the HEART widget identification word,
``0x000000006c00106d`` (revision 6, part ``0xc001``, manufacturer
``0x36``) on a HEART revision F machine. The second is one 64-bit read at
physical ``0x2000`` and returned zero. Both returned without an
exception.

.. _sgi-ip30-octane2-heart-status:

HEART widget status, 2026-09-29
-------------------------------

.. code-block:: text

   >> dump -d -x 0x9000000018000008#1
   0x9000000018000008:                 60

One 64-bit read of physical ``0x18000008``, the HEART widget status slot:
``0x0000000000000060``. It is not a power-on value, and a single read
shows neither read side effects nor what the status bits mean.

.. _sgi-ip30-octane2-heart-bank:

HEART bank, 2026-09-27
----------------------

.. code-block:: text

   >> dump -w -x 0xffffffffaff00020#4
   0xffffffffaff00020:   81470000          0          0          0

Four 32-bit words from the HEART memory bank descriptor area. The first
decodes as valid, size code 7 (256 MiB), density code 5, base code 0
(physical ``0x20000000``) under the public HEART field definitions; the
other three are zero. This agrees with the 256 MiB inventory total and
shows one configured bank at monitor time. It does not identify DIMM
sockets or a reset value.

.. _sgi-ip30-octane2-alt-flash:

Alternate flash aperture, 2026-09-27
------------------------------------

.. code-block:: text

   >> dump -h -x 0xffffffffbfe00000#1
   0xffffffffbfe00000:   ffff

One 16-bit read at physical ``0x1fe00000``, Bridge widget 15's alternate
flash aperture at offset ``0xe00000``, on IP30 revision B with HEART
revision F during the PROM monitor. It names no fitted chip and says
nothing about other bytes of the aperture.

.. _sgi-ip30-octane2-heart-widget:

HEART widget registers, 2026-09-27
----------------------------------

.. code-block:: text

   >> dump -d -x 0x9000000018000060#2
   0x9000000018000060:                  0           3ff7fc0f

Two consecutive 64-bit words in the HEART widget window: offset ``0x60``
returned zero and ``0x68`` returned ``0x3ff7fc0f``, during the PROM
session, with no register writes.

.. _sgi-ip30-octane2-heart-timeout:

HEART request timeout, 2026-09-27
---------------------------------

.. code-block:: text

   >>dump -d -x 0x9000000018000028#1
   0x9000000018000028:                 28

One 64-bit read of the widget request-timeout slot at offset ``0x28``:
``0x28`` after PROM startup. It is not a cold-reset value.

.. _sgi-ip30-octane2-rad:

Onboard RAD class and revision, 2026-09-30
------------------------------------------

.. code-block:: text

   >> dump -w -x 0x900000001f023008#1
   0x900000001f023008:         c0

An aligned 32-bit read of Bridge device 3, function 0, configuration
register ``0x08`` (not BAR0 memory). The word ``0x000000c0`` gives PCI
class ``0x000000`` and revision ``0xc0``, at PROM time on IP30 revision B
with HEART revision F. It does not show cold reset or write masks.

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
     - 128 MiB by default; 64 MiB to 2 GiB (:ref:`sgi-ip30-memory`)
     - ``030-0887-003`` revision F
     - published 195 MHz inventory, the Technical Report's standard
       configurations, and the SGIStuff page for the 1 MiB cache
   * - ``octane2``
     - one R12000, 360 MHz, 2 MiB secondary cache
     - 256 MiB by default; 64 MiB to 8 GiB (:ref:`sgi-ip30-memory`)
     - ``030-1467-001`` revision B
     - the owner's ``hinv -mvv`` (:ref:`sgi-ip30-octane2-hinv`)

``octane`` (established, with one model choice): the Technical Report's
standard configurations (page 7-1) include 128 MiB single-processor
Octanes at 195 MHz, the published inventory reports a 195 MHz R10000
revision 2.7 on board ``030-0887-003`` revision F, and the SGIStuff page
gives the 195 MHz part 1 MB of secondary cache (neither the inventory nor
the Technical Report states the cache size). That inventory
reports 256 MiB; pairing its board and processor with 128 MiB is a
model choice. A 175 MHz, 128 MiB Octane also exists (the OpenBSD
capture), so memory size does not fix processor speed.

``octane2`` (established for one machine, :ref:`sgi-ip30-octane2-hinv`):
the inventory reports one 360 MHz R12000 chip revision 3.5 with 2 MiB secondary cache,
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
       at devices 0 and 1 (:ref:`sgi-ip30-octane2-hinv`,
       :ref:`sgi-ip30-octane2-graph`)
   * - IOC3
     - onboard PCI device 2, function 0
     - established: OpenBSD ``ip30.h`` (``IP30_IOC_SLOTNO``), Linux
       ``pci-xtalk-bridge.c``, and the OpenBSD capture (revision 1)
   * - RAD1 audio
     - onboard PCI device 3, function 0, identity ``10a9:0005``
     - established for the captured Octane (revision ``0xc0``) and for
       the owner's Octane2, whose PROM-monitor read of configuration
       register ``0x08`` returned class ``0x000000`` and revision ``0xc0``
       (:ref:`sgi-ip30-octane2-rad`); its inventory's ``12.0`` is not a
       PCI revision byte
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
  owner's machine from the PROM monitor
  (:ref:`sgi-ip30-octane2-heart-id`) and consistent with its
  inventory's HEART revision F (established). Crossbow
  revision 5, from a PROM-monitor read of its identification word
  ``0x50000001`` (:ref:`sgi-ip30-octane2-xbow-id`) beside an inventory
  revision of 1.4 (established). Bridge revision 3 is an approximation: the inventory
  does not report the Octane2 Bridge revision, and the value is carried
  over from the earlier Octane.

.. _sgi-ip30-heart-status:

The model answers the HEART widget status word (widget register offset
``0x8``) with ``0x60`` on the ``octane2`` only, where the HEART revision
is 6; the ``octane`` returns no such word. A PROM-monitor read on the
owner's HEART revision F Octane2 returned exactly ``0x60``
(:ref:`sgi-ip30-octane2-heart-status`), so the word is established for
that machine at PROM time. It is not a reset value, and the Octane's
HEART is unmeasured.

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
(established, :ref:`sgi-ip30-octane2-hinv`).

.. _sgi-ip30-scheduler-clock:

Scheduler clock
---------------

CP0 Count advances every other processor clock (R10000 manual, version
2.0, section 14.8): 97.5 MHz on ``octane`` and 180 MHz on ``octane2``.
A 32-bit wrap therefore takes 44.05 s and 23.86 s.

The IRIX 6.5.30 IP30 kernel drives its scheduler tick from CP0 Count and
Compare and arms it in two places, both with interrupts masked. The
rearm routine reads Compare and Count and then takes one of two
branches. When the deadline has passed, it writes Count zero and, up to
eight register instructions later, Compare with a target of at least
1000 ticks (5.6 us at 180 MHz). When it has not, it writes the Count it
read back and either leaves Compare alone or writes Compare to the
larger of the interval and 1000 as an absolute value, which need not be
1000 ticks past that Count. A second routine that pulls the deadline in
reads Count and, at most seventeen instructions later (a Compare read
and register arithmetic), writes Compare at least 1000 ticks ahead of
the value read. These are from our disassembly of the shipped
``kernel.o``, whose only Compare writes are these and two adjacent
Count/Compare pairs; they are not hardware measurements. On the
processor those instructions take a few cycles. Without instruction
counting, QEMU's virtual clock follows the host, and a vCPU held between
the sample and the write leaves the target behind Count. A strict
equality comparator then defers the tick a full wrap, and IRIX reports
that CPU 0 "hasn't seen a scheduler clock interrupt". A traced
wall-clock Octane2 boot rearmed 16,114 times in 549 virtual seconds;
twelve rearms crossed their target, each costing 23.9 s, and the desktop
never appeared. Recovering only the Count-write form reached the greeter
but still lost six ticks in three minutes to the Count-read form.

Both machines therefore set the CPU's ``count-sample-window`` property.
Every guest Count read or write records the value and the virtual
clock. The next Compare write raises the timer interrupt when its target
lies less than half the counter range ahead of the latest recorded
value and Count has already reached it. A target behind that value is a
disarm or a deliberate long wait and keeps the ordinary rule: the
read-Count-minus-one disarm public drivers use, and the kernel's
Count=1/Compare=0 pair, and a target equal to it. In the rearm
routine's not-expired branch the sample is the Count written back, so an
absolute target behind it keeps the ordinary rule and one ahead of it is
recovered only once Count has really passed it. Exception or interrupt
entry and reset close the window, and a later Count read or write starts
a new one. Nothing else ends it: the window has no limit on how many
instructions separate the sample from the Compare write, only the limit of half the counter range on elapsed ticks. (The
condition that the target lies less than half the range ahead follows
from that limit and is not a separate bound.) This is an approximation of a host artefact, not processor
behaviour: after a delayed arm the model raises IP7 at the Compare
write, where silicon would not have been late. A window open when the
machine is migrated is dropped, and the destination applies the
ordinary rule to that one write. This is deliberate, unlike the adjacent
rule, which migrates its pending state: the virtual clock is frozen
while the machine is stopped, so a window open across a migration or
snapshot loses nothing unless a host stall after the resume crosses the
target, at a cost of one tick. The SN machines do not set the
property and keep the adjacent-write rule alone.
``tests/qtest/octane-timer-test.c`` pins the recovery and its controls.

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
     - inferred: Linux ``cpu.h`` gives the R12000 implementation code
       ``0x0e`` and the owner's inventory reports chip revision 3.5
       (:ref:`sgi-ip30-octane2-hinv`). No raw PRId read of this word is
       recorded.
   * - FPU identification ``0x0900``
     - ``r12000-fpu-id``
     - approximation: no recorded measurement gives this word; the
       inventory prints no FPU revision for the Octane2
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
     - established by the inventory (:ref:`sgi-ip30-octane2-hinv`); the
       model derives it from the reset word
   * - Secondary-cache block 32 words
     - ``r12000-scache-block-words``
     - approximation: derived from the chosen reset word; the fitted
       block size is unknown

.. _sgi-ip30-memory:

Memory kits and capacity
========================

Both owner's guides describe four DIMM banks of two sockets each. Bank 1
must be filled, banks fill in order without gaps, and each populated
bank holds two DIMMs of the same capacity and type (established:
007-3435-004 page 54, 007-4207-002 page 47).

- Octane (007-3435-004 pages 54-55): 32, 64, 128 and 256 MB DIMMs, so
  64 to 512 MiB a bank and at most 2 GiB. The guide sets no order of
  bank sizes.
- Octane2 (007-4207-002 page 47): DIMMs from 32 MB to 1 GB, so 64 MiB
  to 2 GiB a bank and at most 8 GiB, highest-density DIMMs in bank 1
  and the rest in descending order.

Public posts agree with the split by board: an SGI engineer wrote in
2000 that the older IP30 board stops at 2 GB and that board
``030-1467-001``, which the ``octane2`` profile carries, takes 2 GB
memory kits (Library mail ``39B750E0.CA1E23C4@brussels.sgi.com``).
Published ``hinv`` captures report totals, among them 320 and 640 MiB
on R10000 Octanes, which need more than one bank size, and 2048 MiB on
a dual 250 MHz R10000 Octane. They report totals, never the fitted
sockets.

The catalogue carries each product's rule as ``accepted_bank_sizes_mib``
(64 to 512 on ``octane``, 64 to 2048 on ``octane2``) and, for
``octane2``, ``bank_order: descending``. The generator emits the bank
sizes, the order rule and every total one to four banks make (27 totals
on ``octane``, 79 on ``octane2``). ``-m`` alone fills the fewest banks,
largest first: ``-m 3072`` on ``octane2`` is 2048+1024. ``bank<N>-size``
selects a layout explicitly; setting any bank leaves the unset ones
empty, and the banks must hold exactly ``-m``. Anything else is
refused with the rule it breaks.

.. list-table::
   :header-rows: 1

   * - Kit
     - DIMMs
     - Bank
     - Default of
   * - ``kit-128``
     - two 64 MiB
     - one 128 MiB bank
     - ``octane``
   * - ``kit-256``
     - two 128 MiB
     - one 256 MiB bank
     - ``octane2``

The default kits are model choices (approximation): the profiles'
inventories report only their totals. The guides number physical banks
1 to 4; public software indexes HEART bank descriptors 0 to 3. Treating
them as the same positions is inferred, not a measured board wiring fact.

The stock 6.5.30 PROM sizes each bank itself under a temporary 2 GiB
descriptor and writes the final descriptors; the IRIX 6.5.30 kernel
only reads them (the HEART memory configuration in the Origami HEART
datasheet). The model's response to that probe is a functional
projection chosen so the PROM's own table names the fitted capacity on
both of its HEART revision paths; it is not a measured HEART address
decoder. With it, the PROM writes density code 4 for 64 and 128 MiB
banks, 5 for 256 and 512 MiB and 6 for 1 and 2 GiB, the code an
Octane2 PROM was seen writing for a 256 MiB bank
(:ref:`sgi-ip30-octane2-heart-bank`). What the density code does to
HEART's address decoding is unknown.

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
     - owner inventory (:ref:`sgi-ip30-octane2-hinv`)
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

Names, parts and revisions are established from the inventories
(:ref:`sgi-ip30-octane2-hinv` for the Octane2). The
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
  ``0x1fe00000`` returned ``0xffff`` (established,
  :ref:`sgi-ip30-octane2-alt-flash`; one word only).

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
  0, target 1 (:ref:`sgi-ip30-octane2-graph`); the physical bay and channel wiring are unknown.
- Netboot adapter ``ioc3-ethernet``, the onboard IOC3's Ethernet; install
  media ``disk-image`` and ``netboot``. The stock PROM's "Install System
  Software" netbooted the IRIX 6.5.30 miniroot through it on the model,
  and complete installs on an Octane with SI and an Octane2 with V12 on
  2026-10-24 reached the installed system's login prompt
  (:ref:`sgi-sn-launch-disk-bus`). The physical PROM's network install
  rests on the IRIX 6.5.30 ``prom(1M)`` page, not on a measurement of
  this board. Supported guest ``irix``; default CPU model R10000 or
  R12000.

.. _sgi-ip30-unknowns:

Open questions
==============

- The flash part, select, strap and alternate-aperture contents
  (:ref:`sgi-ip30-flash-wiring`).
- R12000 reset mode word and cache block size
  (:ref:`sgi-ip30-r12000-defaults`).
- HEART's density decoding and its response to the PROM's probe of a
  smaller bank, the reset values of the bank descriptors, and the
  bank-number to descriptor-index mapping (:ref:`sgi-ip30-memory`).
- The Octane2 Bridge silicon revision and RAD PCI revision byte.
- The expansion Bridge's widget number.
- Interrupt wiring from the onboard PCI functions to Bridge inputs; no
  public source establishes it, and this page does not state it.
