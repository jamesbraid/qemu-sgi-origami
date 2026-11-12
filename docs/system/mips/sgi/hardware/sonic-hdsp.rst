.. SPDX-License-Identifier: BSD-3-Clause

.. _sgi-sonic-hdsp:

=====================================
Sonic HDSP: PCI audio processor card
=====================================

Used in :ref:`sgi-ip30` (Octane and Octane2, in the PCI module or the
PCI-XTALK adapter). The Sonic HDSP is a PCI card from Sonic Solutions
that does real-time audio processing for a workstation. Discreet sold it
for the Octane family as its "Stream" audio hardware, and Sonic sold the
same card in Macintosh workstations. The card is built around an Intel
i960 Rx I/O processor that boots a flash image of its own, four Motorola
DSP56301 signal processors on that processor's private PCI bus, and an
external converter box on a serial link. It has no SCSI port and no
disk of its own.

.. seealso::

   Platform pages: :ref:`sgi-ip30`.

   Related parts: :doc:`pci-expansion` (the PCI module and the adapter
   that carry the card), :doc:`bridge` (the :term:`Bridge` between the
   card and the :term:`XIO` link), :doc:`at29c040a` (the flash programming
   model the card's flash resembles) and :doc:`rad1-audio` (the Octane's
   onboard audio, a different device).

QEMU models the card as one PCI device, ``sgi-sonic-hdsp``, with a second
device, ``sonic-ada8824``, for the converter box. The device runs the
card's own flash image on an embedded i960 interpreter, so the guest sees
what the firmware makes it see. It also carries four DSP56300-family
cores, but in the current model they do not execute code, and the
converter box and audio path described later are not modelled. The model
carries no firmware: the flash image is the user's, named by a property
(:ref:`sgi-sonic-hdsp-firmware`).

This page is the hardware reference for that device. Much of the card is
not documented anywhere public, so most of what follows is what the
shipped software expects of the board (*software expectation*), and
every gap is stated as an *approximation* with what would settle it. The
gaps are collected in :ref:`sgi-sonic-hdsp-approximations`. Facts are
rated as described in :ref:`sgi-ratings`.

.. _sgi-sonic-hdsp-sources:

Sources
=======

- The *i960 Rx I/O Microprocessor Developer's Manual*,
  :src:`intel-i960rx-272736-002`, for every on-chip unit of the I/O
  processor. Page numbers below are its printed pages. The board uses the
  80960RD part, which the flash image's boot code names; the manual covers
  the RD in its family. The unit layouts are *established* for the chip.
  That the card's firmware programs them as described is a *software
  expectation*.
- The *DSP56300 Family Manual*, :src:`nxp-dsp56300fm-5`, for the DSP
  core, its memory spaces and its instruction set, and the *DSP56301
  User's Manual*, :src:`nxp-dsp56301um-4`, for the part's host interface,
  serial ports, timers, bootstrap ROM, on-chip memory and register
  addresses. Page numbers are printed pages. Chip facts from these two are
  *established*; what the board does with the chip is not in them
  (:ref:`sgi-sonic-hdsp-dsp-limits`).
- Sonic's public *SonicStudio HD 1.8 Installation Guide*,
  :src:`sonicstudio-hd-1.8-installation-guide`, and *Configuration Guide*,
  :src:`sonicstudio-hd-1.8-configuration-guide`, for the card's connectors
  and the converter boxes it drives. They name no processor, memory size
  or firmware for the card. Page numbers are printed pages for the
  Installation Guide and PDF pages for the Configuration Guide.
- The IRIX Bridge driver's slot scan and the generic PCI-to-PCI bridge
  driver, read as software expectations. The scan routines are in the
  Bridge driver object of :src:`irix-pcibr-6.5.30`; the generic bridge
  driver and the connection registry are other shipped IRIX objects whose
  hashes the registry does not hold, so facts from them carry no address.
- The shipped software that drives the card, read as software
  expectations and cited by registry entry and address: the IRIX kernel
  driver for the card, :src:`sonic-hdsp-driver-9.2.5`; the user-space
  audio library, :src:`sonic-hdsp-library-5.0`; the board application
  module the library downloads to the card, :src:`sonic-hdsp-module-1.0`;
  the DSP program set, :src:`sonic-dsp-programs-5.0`; and the IRIX Bridge
  driver, :src:`irix-pcibr-6.5.30`. The Bridge facts are cross-checked
  against :doc:`bridge`.
- Two flash images of the card's I/O processor taken from the Macintosh
  product's installers, :src:`sonic-iop-flash-1.0.0` and
  :src:`sonic-iop-flash-1.1`. They are the card's boot code and resident
  operating system. The facts read from them are each a *software
  expectation*, and they are the only source for the board glue the I/O
  processor touches. The DSP monitor programs were read from the
  Macintosh product's copy of the resource file (sha256
  ``fb20b7c78239c2ff9f1901830b7f701f4b50694e16a09ba12fa2363c81174866``),
  not from the IRIX copy that the registry entry names. The two were not
  compared. That they hold the same monitor programs is *inferred*, and it
  carries over to every fact below that rests on those programs.

No card, board photograph or captured PCI configuration space was
available, and no measurement of one exists. Nothing on this page is a
hardware measurement.

Board overview
==============

.. code-block:: text

   host PCI (Bridge), INTx
        |
   +----+------------------------------------------------------------+
   | i960 Rx I/O processor (function 0: PCI bridge unit,             |
   |   function 1: primary ATU + messaging unit), flash, DRAM        |
   |        |                                                        |
   |        +-- board glue: reset bytes, interrupt request, UART     |
   |        |                                                        |
   |        +-- secondary PCI bus (private to the board)             |
   |               |        |        |        |                      |
   |            DSP 0     DSP 1    DSP 2    DSP 3   (DSP56301)       |
   |               |        |        |        |                      |
   |               +-- external memory, sync flags, sample ports      |
   |                                 |                               |
   |                              ESSI0 (DSP 2) -- serial link --> box|
   +-----------------------------------------------------------------+

.. list-table::
   :header-rows: 1
   :widths: 46 12 42

   * - Fact
     - Rating
     - Source
   * - The card carries an i960 Rx-class I/O processor with a PCI bridge
       unit, two address translation units, a messaging unit, DMA
       channels, a memory controller, a local bus arbiter, an interrupt
       controller, timers and an I2C unit. The boot code names the RD
       part.
     - *established*
     - :src:`intel-i960rx-272736-002 p. 1-2, C-8`. Boot code banner:
       :src:`sonic-iop-flash-1.1 0xf01`.
   * - The part is the 66 MHz RD, the only RD speed grade the manual
       lists.
     - *inferred*
     - :src:`intel-i960rx-272736-002 p. 1-9`. The board's clock source
       is not recorded anywhere read.
   * - Four DSP56301 processors sit behind the I/O processor's secondary
       PCI interface, one per DSP index 0 to 3.
     - *software expectation*
     - :src:`sonic-hdsp-module-1.0` (the four-way switch that builds each
       DSP's configuration address) and :src:`sonic-hdsp-library-5.0
       0x5ec2c0e0` (DSP index in the command word).
   * - The monitor programs are DSP56300 code, not DSP56000 code, so the
       DSPs are real DSP56301 parts running the 56300 instruction set.
     - *established*
     - :src:`sonic-dsp-programs-5.0 monitor images for DSPs 0 to 2`: the
       programs use the nested-loop (DOR), double-precision multiply-accumulate (DMAC) and
       bit-field insert (INSERT) instructions that only the 56300 core
       has (:src:`nxp-dsp56300fm-5`). That the IRIX copy matches the
       Macintosh copy read is *inferred*.
   * - The converter box hangs off DSP 2's first serial port and is not
       on the host bus.
     - *software expectation*
     - :src:`sonic-hdsp-library-5.0 0x5ecd6ed0` and
       :src:`sonic-dsp-programs-5.0 the converter-link routine of DSP 2`.
   * - The card has two external connectors: an audio link to the converter
       box and a serial connector for timecode and machine control.
     - *established*
     - :src:`sonicstudio-hd-1.8-installation-guide printed pp. 1-2, 1-13
       to 1-14`.

The model has the same three parts as one device: the processors, memories
and glue behind one PCI face, with the box as a separate device attached by
a link property. The two interpreter cores are written from the Intel and
NXP manuals, with the i960 core derived from MAME's BSD-3-Clause i960
core, and are libraries with no PCI or board knowledge
(``hw/coproc/i960`` and ``hw/coproc/dsp56300``). The processors
advance from a virtual-clock timer under the main loop lock in slices of
guest time and stop early when idle (:ref:`sgi-sonic-hdsp-timing`). The
model has no host thread of its own.

PCI identity
============

The card's PCI configuration is not hard-wired: the I/O processor's boot
code programs its configuration registers, and the host sees them only
after the boot code has run (:ref:`sgi-sonic-hdsp-boot`). The model has no
configuration space of its own beyond the units the Rx manual describes,
so the contents below are what the stock boot code leaves behind.

.. list-table::
   :header-rows: 1
   :widths: 40 12 48

   * - Fact
     - Rating
     - Source
   * - The i960 Rx is a multifunction PCI device. Function 0 is the
       PCI-to-PCI bridge unit and function 1 is the primary ATU, which
       carries the messaging unit. The ATU answers only type 0
       configuration cycles for function 1.
     - *established*
     - :src:`intel-i960rx-272736-002 p. 15-38, 16-2, 16-11`.
   * - The boot code programs the ATU's vendor and device IDs to
       ``1263:05dc`` and its subsystem IDs to the same pair. Vendor
       ``0x1263`` is Sonic Solutions.
     - *software expectation*
     - :src:`sonic-iop-flash-1.1 0x28-0x10df` (boot code, identical in both
       images). The IRIX driver registers for exactly this pair
       (:src:`sonic-hdsp-driver-9.2.5 0x9f8`).
   * - The boot code programs the bridge unit's vendor and device IDs to
       ``1263:060e`` and its subsystem IDs to the same pair.
     - *software expectation*
     - :src:`sonic-iop-flash-1.1 0x28-0x10df`.
   * - The bridge unit's header type register reads ``0x81``: a PCI-to-PCI
       bridge header with the multifunction bit set. The IRIX slot scan
       probes functions 1 to 7 only when bit 7 of function 0's header type
       is set, and reads the header type again for each function, so the
       model must present ``0x81`` on function 0.
     - *established* for the chip, *software expectation* for the scan
     - :src:`intel-i960rx-272736-002 p. 15-6, 15-49`;
       :src:`irix-pcibr-6.5.30 0xb768`.
   * - Before the boot code programs them, the chip's reset values are
       Intel's: the ATU class code reads ``0x058000`` (memory controller,
       other) and the interrupt pin ``1``.
     - *established*
     - :src:`intel-i960rx-272736-002 p. 16-32, 16-43`.
   * - The boot code programs the ATU class to ``0x050800`` (base class
       ``0x05``, subclass ``0x08``, programming interface ``0``). The
       resident operating system later writes the I2O class (base class
       ``0x0e``, subclass 0, programming interface 1) and reprograms the
       inbound window (:ref:`sgi-sonic-hdsp-windows`). The IRIX scan does
       not read the class of function 1.
     - *software expectation*
     - :src:`sonic-iop-flash-1.1 0x28-0x10df` for the boot value and
       :src:`sonic-iop-flash-1.1 0x2ce4 0x2cf0 0x2cf8` for the later write
       (1.0.0: ``0x32da4``, ``0x32db0``, ``0x32db8``),
       :src:`irix-pcibr-6.5.30 0xb768`.
   * - The boot code stores ``2`` to the ATU interrupt pin register, which
       selects INTB. The chip's reset value is INTA. No other store to
       that register was found in the boot ROM, the operating system or the
       board application module. The IRIX driver reads the pin byte at
       configuration offset ``0x3d`` and accepts 1 to 4 as INTA to INTD.
     - *software expectation*
     - :src:`sonic-iop-flash-1.1 0x28-0x10df`,
       :src:`sonic-hdsp-driver-9.2.5 0xf90-0xfb0`,
       :src:`intel-i960rx-272736-002 p. 16-43`.
   * - The driver's configuration read at ``0x3d`` is two bytes wide, so
       the byte after it, the ATU's maximum-grant register at ``0x3e``, is
       part of the value. The driver's switch treats a nonzero upper byte as
       no match and fails the attach, so the maximum grant must read 0.
     - *software expectation*
     - :src:`sonic-hdsp-driver-9.2.5 0xf90-0xfb0`.
   * - BAR0 is the ATU's primary inbound base address register: a memory
       BAR, 32-bit, prefetchable, whose size comes from the limit register.
       At reset of the boot code the limit is ``0xf8000000``, a 128 MiB
       window.
     - *inferred*
     - Type bits are fixed by the chip (:src:`intel-i960rx-272736-002
       p. 16-36`). The limit value is
       :src:`sonic-iop-flash-1.1 0x28-0x10df`. That the board's BAR0 is
       this register, and not a glue-logic BAR, follows from the driver
       needing 128 MiB or more (:ref:`sgi-sonic-hdsp-host-driver`).
   * - The card has no other memory or I/O BAR. The expansion ROM register
       has a 4 KiB limit and a translate value of ``0xa0008000``.
     - *software expectation*
     - :src:`sonic-iop-flash-1.1 0x28-0x10df`; the driver requests only
       BAR0 (:src:`sonic-hdsp-driver-9.2.5 0x17c-0x290`).
   * - The card uses one interrupt pin and no power-management capability.
     - *inferred*
     - The ATU has one pin register
       (:src:`intel-i960rx-272736-002 p. 16-43`). No capability list was
       read.
   * - The slot scan probes function 0, and when its header type has bit 7
       set, functions 1 to 7. Function 0 and function 1 become sibling
       hardware-graph vertices named with the slot and the letters ``a``
       and ``b``, not parent and child. A driver registered for ``1263:05dc``
       is attached to function 1, and on the boot-time scan the identity entry
       ``/hw/.id/pci/126305DC`` is created for function 1 whether or not a
       driver attached. That scan is the same on IP30, IP27 and IP35. On
       IP35 only, the hot-plug slot start-up path creates neither the
       identity entry nor an unknown-device entry for a function with no
       driver or a failed attach.
     - *software expectation*
     - :src:`irix-pcibr-6.5.30 0xb768 0xcfdc 0x904 0x580`. The registry
       code that makes the identity entry is another shipped object.
   * - The Bridge driver ignores the class code. A generic PCI-to-PCI
       bridge driver registered for every vendor and device reads the base
       class and subclass of every function, function 1 included, and
       returns without acting for a class that is not a bridge, so the ATU's
       boot-time class ``0x05`` and its later I2O class ``0x0e`` both fall
       through with no effect. For function 0's class ``0x0604`` it reserves a bus number (16 for the
       first bridge), writes the bridge's primary, secondary and
       subordinate bus numbers, then reads the vendor halfword of devices 0
       to 31 on the secondary bus with type 1 cycles, treating
       ``0xffff`` as empty, and assigns the bridge windows. An empty
       secondary bus is a normal outcome. Two or more functions with bus
       master enable set behind a bridge are refused unless the slot allows
       several masters, and the scan then initialises nothing behind it.
       Which answer an Octane slot gives is not recorded.
     - *software expectation*; *unknown* for the multiple-master answer
     - The generic bridge driver is a shipped IRIX object whose hash the
       registry does not hold. A read of the slot's multiple-master query
       in the Bridge driver would settle the last part.

The model implements the bridge unit and the ATU as two functions of one
PCI device, as the Rx does, with every identity register read back from
what the firmware wrote. While the configuration cycle retry bit of the
bridge unit's extended control register (EBCR bit 2) is set, the chip
answers every configuration cycle to either function with Retry
(:src:`intel-i960rx-272736-002 p. 11-3, 11-4, 15-64`, *established*). The
boot ROM sets the bit and the resident operating system clears it when its
PCI setup is finished (:ref:`sgi-sonic-hdsp-hiding`). The reset state of
the bit is a strap on the chip and the card's strap is *unknown*; the model
drives the strap so the bit is set from reset, an *approximation*.
QEMU's PCI core cannot express PCI Retry, so while the bit is set the model
answers configuration reads with all ones and drops configuration writes,
which a host sees as an empty slot, not as a wait: a host that scans before
the operating system has finished sees no card, and will not see one
later unless it scans again. The count of cycles answered this way is the
``config-retries`` property, which tests read. This is an *approximation*;
what would settle it is Retry support in the PCI core. The function numbers
are 0 and 1 as the chip defines them.

.. _sgi-sonic-hdsp-hiding:

Hiding the DSPs from the host scan
----------------------------------

The Rx gives the host no access to the secondary bus's private devices:
devices in private address space "are not part of the standard secondary
PCI address space" and the host cannot configure them. The five select
lines ``S_AD15`` to ``S_AD11`` are always private, and bit *n* of the
secondary IDSEL select register (SISR, local ``0x1042``) set means
``S_AD`` line 16 plus *n* is never asserted when the bridge converts a host
type 1 cycle to type 0 (bits 0 to 9 on this part)
(:src:`intel-i960rx-272736-002 p. 15-7, 15-8, 15-66`, *established*). The
module addresses DSP *n* on ``S_AD`` line 17 plus *n*, that is ``S_AD17``
to ``S_AD20``, through the ATU's secondary outbound configuration ports,
which is the private-device route of the manual
(:src:`sonic-hdsp-module-1.0`, *software expectation*). It makes no store to
any bridge register.

What each stage stores to the bridge, in order (*software expectation*,
:src:`sonic-iop-flash-1.1 0x28-0x10df` for the boot ROM, which both images
share, and the 1.1 offsets below, with the 1.0.0 offsets in brackets):

- The boot ROM sets EBCR bit 2, so every host configuration cycle gets
  Retry, and stores SISR = ``0x03c1`` (bits 0 and 6 to 9: ``S_AD16`` and
  ``S_AD22`` to ``S_AD25``). That leaves ``S_AD17`` to ``S_AD20``, the DSPs,
  reachable; hiding them needs bits 1 to 4 (mask ``0x1e``).
- The resident operating system, when its PCI setup finishes, writes SISR
  from a word in the board's non-volatile storage (1.1:
  :src:`sonic-iop-flash-1.1 0x2da0 0x2db0`; the read is at ``0x4950``),
  stores ``0x22`` to the bridge control register at ``0x103e`` (1.1
  ``0x2dbc``), sets EBCR bit 3, upstream prefetch (``0x2dcc``), and
  clears EBCR bit 2, retry off (``0x2de0``). The word is valid only when
  its upper half is ``0x9876``; otherwise the operating system writes
  ``0x98760000`` back as the default (1.1 ``0x52e0``), so a blank store
  gives SISR = 0, which hides nothing. The two images differ here: 1.0.0
  uses the low 16 bits of the word unmasked (read at ``0x3f20``, SISR
  store ``0x32e20``, bridge control ``0x32e2c``, EBCR ``0x32e3c`` and
  ``0x32e50``), and 1.1 masks it with ``0x3ff``, a 10-bit SISR, and also
  stores 1 to the secondary bus number register (``0x2d80``), which
  1.0.0 never writes. The default ``nv-word`` of ``0x987603df`` gives
  ``0x3df`` under either, which fits the mask.
- It only reads the bridge's other bus-number and window registers.

The word is the first four bytes, little-endian, of a 1 KiB serial EEPROM
on the Rx's I2C unit: the operating system reads it through its
non-volatile-storage read routine (1.1 ``0x5260``, body ``0x5370``;
1.0.0 ``0x2a60``), which is bounded to offset plus length of ``0x400`` and
sends a START with ``0xa0`` or'd with ``(offset & 0x300) >> 7``, the
offset's low byte, a repeated START with ``0xa1`` or'd with the same page
bits, and then reads with an acknowledge on every byte but the last, which
gets a not-acknowledge and STOP. That is the 24C08-class sequence: 7-bit
device address ``0x50`` and four 256-byte pages selected by address bits 1
and 2 (*software expectation*, :src:`sonic-iop-flash-1.1 0x5260 0x5370
0x5660`; the device class is *inferred*). A blank part, with no ``0x9876``
magic, gets ``0x98760000`` written back by the write routine (1.1
``0x5470``), which must be acknowledged or the operating system reports an
error, and SISR = 0. If nothing answers the first address byte the read
fails with status -1 and SISR also takes the default 0. The model has that
EEPROM at address ``0x50`` on the I2C unit (:ref:`sgi-sonic-hdsp-units`) and
its contents are the ``nv-word`` property, default ``0x987603df``, an
*approximation*: the rest of the part reads ``0xff``, and its SISR value
``0x03df`` is the boot ROM's ``0x03c1`` plus bits 1 to 4, so the firmware
itself hides all four DSPs. The model implements SISR and the retry bit as
the Rx manual describes and the firmware writes them. The shipped content
is not in the held images (*unknown*); a real board's EEPROM dump would
settle it. The operating system's secondary-bus scan builds its
candidate private lines from SISR shifted left five places with the five
reserved lines always set, which suggests the stored value hides the
DSPs' lines (*inferred* from the loop shape, 1.1 ``0x32f0`` and 1.0.0
``0x33360``, :src:`sonic-iop-flash-1.1 0x32f0`).

What a host type 1 scan sees (the model's behaviour, following the Rx
rules above):

#. From reset to the end of the operating system's PCI setup: the chip
   answers Retry to every configuration cycle, so nothing is readable. The
   model answers all ones, an empty slot (see above).
#. After that, the bridge unit (``1263:060e``), the ATU (``1263:05dc``) and
   whatever SISR leaves exposed. With the default ``nv-word`` the DSPs are
   hidden, so a type 1 scan behind function 0 finds no device. With a blank
   word (SISR = 0) all four DSPs are exposed. If a DSP is held in reset it
   does not answer a configuration cycle (the DSP reset lines' default
   state is *unknown*; the model holds them in reset after a board
   reset, an *approximation*). A DSP out of reset but not yet in PCI mode
   (DCTR HM = 1) answers every memory and configuration cycle with Retry,
   not as an empty slot (*established*, :src:`nxp-dsp56301um-4 pp. 6-13,
   6-69`).
#. After the module attaches, the module has set each DSP's command
   register to ``0xf8000006`` (memory space and bus master) and its base
   address register. An exposed DSP answers with the DSP56301's hardwired
   vendor and device, ``1057:1801`` (*established*,
   :src:`nxp-dsp56301um-4 pp. 6-63`). The module's adapter handler does
   not read those. It reads the subsystem register (CSID) at configuration
   offset ``0x2c`` and accepts a DSP whose subsystem vendor is ``0x1263``
   and subsystem ID is ``0x0618``, ``0x0622``, ``0x062c`` or ``0x0636``,
   and it also accepts an all-zero CSID, which is the HI32's default, by
   falling back to ``1057:1801`` (*software expectation*,
   :src:`sonic-hdsp-module-1.0 0x4c0-0x65c`). Only the DSP core can write
   CSID, in self-configuration mode before it enters PCI mode
   (*established*, :src:`nxp-dsp56301um-4 pp. 6-5, 6-69`). The monitor
   programs and the module do not write it, so the writer, a board-side
   bootstrap program the held files do not contain, is *inferred*, and the
   mapping of the four IDs to DSP indices was not recovered. The model
   leaves CSID at 0, an *approximation*.

Multiple masters. Each DSP is set up as a secondary-bus master: the module
writes ``0xf8000006`` to its command register, and the monitor
programs write the host interface's PCI master registers
(DPMC ``0x400000``, DPAR ``0x060000``). The Rx's secondary ATU is a fifth
master, and the secondary inbound window the boot ROM programs is the path
by which a secondary-bus master reaches the card's DRAM
(*software expectation*, :src:`sonic-hdsp-module-1.0`,
:src:`sonic-dsp-programs-5.0 the start-up code`). Whether a DSP actually
masters the secondary bus in normal audio operation was not traced. No
firmware code bears on IRIX's multiple-master policy, so hiding the DSPs is
the only mechanism the card has that keeps IRIX from counting them; the
policy itself is IRIX's (:ref:`sgi-sonic-hdsp-secondary`).

.. _sgi-sonic-hdsp-large-bar:

Placement
---------

The card is a normal PCI function on the Bridge of the Octane PCI module
(devices 1 to 3) or the PCI-XTALK adapter (device 1)
(:ref:`sgi-pci-expansion-octane`):

.. code-block:: text

   -device sgi-octane-pci-cage,slot=widget-13
   -device sgi-sonic-hdsp,bus=widget-13.0,addr=1,flash-image=<path>,box=ada0
   -device sonic-ada8824,id=ada0,audiodev=snd0

.. list-table::
   :header-rows: 1
   :widths: 46 12 42

   * - Fact
     - Rating
     - Source
   * - The card maps only a memory BAR, so a Bridge of revision 3 or 4
       serves it. The IRIX PCI I/O window needs revision 4 or later and
       the card does not use it.
     - *software expectation*
     - :src:`irix-pcibr-6.5.30 0x14a8-0x14b4`, :src:`sonic-hdsp-driver-9.2.5
       0x17c-0x290`.
   * - A 128 MiB BAR is reached through the Bridge's 32-bit memory PIO
       window: a BAR too large for a Bridge device window is mapped at widget
       offset ``0x40000000`` plus the PCI address, for PCI addresses below
       ``0x40000000``, so the 128 MiB BAR must lie below 1 GiB.
     - *software expectation*
     - :src:`irix-pcibr-6.5.30 0xf6c`; :doc:`bridge`
       (:ref:`sgi-bridge-pio`).
   * - The IRIX driver is the same code for IP27, IP30 and IP35, so the
       card works in any Bridge-backed slot.
     - *inferred*
     - The three driver builds' code and data match
       (:src:`sonic-hdsp-driver-9.2.5 0x0-0x4114`). That a card sold for
       other machines runs on an Octane is *unknown*.
   * - The card's slot and device number in a real PCI module.
     - *unknown*
     - No listing of an Octane with this card is held.

The Octane PCI module's Bridge (revision 3) and the PCI-XTALK adapter's
(revision 4) are expected to decode a 128 MiB BAR in that window and to
place it, since :doc:`bridge` describes the ``0x40000000`` range as reaching
PCI memory from address zero without a device window. Whether the existing
board models do so for a BAR of that size is a test item, not a claim: a
qtest places a 128 MiB BAR0 behind each board and reads and writes its first
and last words.

BAR0 map
========

IRIX maps ``0x40f8000`` bytes (68,059,136, which is 0xf8000 bytes over
64 MiB) of BAR0 at driver file offset ``0xa2000000``, from BAR0 plus 0
(*software expectation*, :src:`sonic-hdsp-driver-9.2.5 0x3c4c-0x3d4c`).
The library takes the mapping's base as the message port's base and
adds ``0x4000000`` for the glue logic's pointer
(:src:`sonic-hdsp-library-5.0 0x5ec26640 0x5ecd2b60`, *software
expectation*). The firmware agrees with it: the Rx's translate rule puts
the messaging unit and the DRAM at the bottom of BAR0 and the glue logic
at BAR0 plus ``0x4000000`` (:ref:`sgi-sonic-hdsp-windows`).

.. list-table::
   :header-rows: 1
   :widths: 18 41 41

   * - BAR0 offset
     - After the operating system (limit ``0xf8000000``, translate
       ``0xa2000000`` for 64 MiB of DRAM)
     - At reset, before it (boot ROM: limit ``0xf8000000``, translate
       ``0xa0000000``)
   * - ``0x0000000`` to ``0x0000fff``
     - Messaging unit registers
       (:ref:`sgi-sonic-hdsp-mu-window`)
     - Messaging unit registers
   * - ``0x0001000`` to ``0x1ffffff``
     - Top half of the DRAM, local ``0xa2001000`` to ``0xa3ffffff``:
       message frames and shared buffers
     - DRAM from the bottom, local ``0xa0001000`` and up
   * - ``0x2000000`` to ``0x3ffffff``
     - An alias of the range above: bit 25 of the translate value is already
       set, so local ``0xa2000000`` to ``0xa3ffffff`` again
     - The second half of a 64 MiB DRAM, local ``0xa2000000`` to
       ``0xa3ffffff``
   * - ``0x4000000`` to ``0x40f7fff`` (decoded to ``0x7ffffff``)
     - The glue logic, memory controller bank 1: local ``0xa6000000`` for
       64 MiB of DRAM (:ref:`sgi-sonic-hdsp-pld`)
     - Local ``0xa4000000`` to ``0xa5ffffff``, past the end of a 64 MiB
       DRAM and not mapped; the boot ROM sets bank 1 to ``0xa6000000`` and
       the window then reaches it at BAR0 plus ``0x6000000``
   * - ``0x4100000`` to ``0x7ffffff``
     - Not used by the library; further bank 1 addresses
     - Same

All the layout rows are *software expectation* for the library side. The
Rx's rule is that the local address equals the PCI address with the limit's
bits cleared, ORed with the translate value (*established*,
:src:`intel-i960rx-272736-002 p. 16-7`). That the board's decode applies it
to the programmed values as written (:ref:`sgi-sonic-hdsp-windows`), and
so the rows above, is *inferred*. The PLD at BAR0 plus
``0x6000000`` before the operating system runs is *inferred* by the same
arithmetic. The library runs only after the operating system has set the
window. The use of bank 1 beyond the library's offsets is *unknown*; the
model reads zero there and logs ``LOG_UNIMP``, an *approximation*.

Whether BAR0 is one 128 MiB BAR or a smaller BAR with the board decoding
more is *unknown*; a driver that maps a window longer than the BAR fails to
attach, so 128 MiB is the smallest that works
(:src:`sonic-hdsp-driver-9.2.5 0x17c-0x290`, *software expectation*). The
model presents one 128 MiB BAR, an *approximation*: the Rx's window size is
a power of two in the limit register
(:src:`intel-i960rx-272736-002 p. 16-46`). A configuration dump of a real
board would settle it.

.. _sgi-sonic-hdsp-byte-order:

Byte order
----------

The card is little-endian: the i960 stores words little-endian and the
host library byte-swaps window accesses and frame words itself
(:src:`sonic-hdsp-library-5.0 0x5ed9a180 0x5ed99a50`, *software
expectation*). The IRIX driver declares a little-endian device on a
big-endian host, which turns on the Bridge's byte swap for DMA only
(Device register bits 19 and 20) and leaves programmed I/O unswapped
(:src:`sonic-hdsp-driver-9.2.5 0x11dc-0x1200`,
:src:`irix-pcibr-6.5.30 0x3904`, *software expectation*).

The model presents BAR0 as little-endian memory and moves DMA data
without changing its bytes, so :doc:`bridge` applies the swap
(:ref:`sgi-bridge-dma-byte-order`). A byte store the library makes at BAR0
offset *N* reaches PCI byte *N* XOR 3 of its aligned word through the
Bridge's ``0x40000000`` range with swap bit clear (:ref:`sgi-bridge-pio`),
so the glue register bytes sit at byte position 3 of a 32-bit word on the
device side. That agrees with the i960 side, where the reset byte is at
the glue logic's base plus ``0xa0003``
(:src:`sonic-hdsp-module-1.0 0x9888`), and is *inferred*
(:ref:`sgi-sonic-hdsp-glue`).

.. _sgi-sonic-hdsp-mu-window:

Messaging unit and DRAM window
------------------------------

The bottom of BAR0 is the Rx's primary inbound window. Its first 4 KiB is
the messaging unit, at the offsets the manual gives, and the rest is local
memory through the window's translate value
(:src:`intel-i960rx-272736-002 p. 16-36, 16-47, 17-13`, *established* for
the chip).

.. list-table::
   :header-rows: 1
   :widths: 14 22 36 28

   * - Offset
     - Register
     - What the library does
     - Rating and source
   * - ``0x10``
     - Inbound message 0 (IMR0)
     - Host writes a message word. The firmware's interrupt handler reads it.
     - *software expectation*,
       :src:`sonic-hdsp-library-5.0 0x5ec2c300`,
       :src:`sonic-hdsp-module-1.0 0x83d0 0x8610`
   * - ``0x18``
     - Outbound message 0 (OMR0)
     - Host reads the firmware's reply word.
     - same
   * - ``0x30``
     - Outbound interrupt status (OISR)
     - Host reads it to see pending outbound events.
     - *software expectation*,
       :src:`sonic-hdsp-library-5.0 0x5ed89370`
   * - ``0x34``
     - Outbound interrupt mask (OIMR)
     - Host writes ``0xffffffff`` first, which masks every source.
     - same
   * - ``0x40``
     - Inbound queue port
     - Read: the free request frame address, or ``0xffffffff`` when the queue
       is empty. Write: post a request.
     - *established* for the chip, *software expectation* for the use,
       :src:`intel-i960rx-272736-002 p. 17-8`,
       :src:`sonic-hdsp-library-5.0 0x5ed89370`
   * - ``0x44``
     - Outbound queue port
     - Read: the next reply frame address, or ``0xffffffff`` when empty.
       Write: return the frame.
     - same

The registers at ``0x14``, ``0x1c``, ``0x20``, ``0x24``, ``0x28`` and
``0x2c`` (inbound message 1, outbound message 1, the two doorbells and the
inbound status and mask) are the chip's as the manual lays them out, and
the library does not touch them (*established* for the chip,
:src:`intel-i960rx-272736-002 p. 17-13`).

The library derives its register pointers from the mapping's base: the
outbound mask at +0x34, the status at +0x30 and the queue ports at +0x40
and +0x44, and it converts a frame's local address to a window pointer
by subtracting ``0xa2000000`` and adding the base
(:src:`sonic-hdsp-library-5.0 0x5ec26640 0x5ed89370`, *software
expectation*). It reads and writes every request and reply frame there
during bring-up steps 1 to 7, before the handshake word ``0x699`` is ever
written, so the local memory behind the messaging unit must be reachable
at the bottom of BAR0. The frame words follow I2O: the library reads reply
words as host-order 32-bit values with the context word byte-swapped. The
bias ``0xa2000000`` is the resident operating system's translate value for
64 MiB of DRAM (:ref:`sgi-sonic-hdsp-windows`).

The model implements the Rx's primary inbound window and its translation
as the firmware programs them, with no decode of its own in front of it. A
discriminating test is the library's first bring-up traffic: a read of the
inbound queue port at BAR0 plus ``0x40``, then a write of a request frame
at BAR0 plus the address it returned, must reach the firmware's queue and
local memory.

.. _sgi-sonic-hdsp-windows:

Inbound window: boot-time and operating-system views
----------------------------------------------------

The Rx's primary ATU translates a PCI address inside its window, outside
the messaging unit's first 4 KiB, to a local address equal to the PCI
address with the limit's bits cleared, ORed with the translate value
(:src:`intel-i960rx-272736-002 p. 16-7, 16-47`, *established*). That the
board applies the rule to the values the firmware writes is *inferred*. The
window has
two states on this card (*software expectation*). Offsets are file
offsets into :src:`sonic-iop-flash-1.1` (first) and
:src:`sonic-iop-flash-1.0.0` (second) where the two differ:

.. list-table::
   :header-rows: 1
   :widths: 20 40 40

   * - Item
     - After the boot ROM
     - After the operating system
   * - ATU class
     - ``0x050800``
     - I2O class: base ``0x0e``, subclass 0, programming interface 1
       (1.1: ``0x2ce4``, ``0x2cf0``, ``0x2cf8``; 1.0.0: ``0x32da4``,
       ``0x32db0``, ``0x32db8``)
   * - Inbound limit
     - ``0xf8000000``, a 128 MiB window
     - First half the DRAM (32 MiB for 64 MiB of DRAM; 1.1: ``0x2d10``,
       1.0.0: ``0x32dd0``), then rewritten to ``0xf8000000`` again by the
       kernel start code, so the BAR stays 128 MiB (1.1: ``0x13ac``,
       unconditional; 1.0.0: ``0x1318``, only when the PCI setup ran)
   * - Inbound translate
     - ``0xa0000000``
     - DRAM base plus DRAM size minus the half window: ``0xa2000000`` for
       64 MiB (1.1: ``0x2d30``; 1.0.0: ``0x32df0``)
   * - ATU configuration register
     - ``0x756``
     - ``2`` from the operating system (1.1: ``0x2d38``; 1.0.0:
       ``0x32dfc``), then ``6`` (both outbound enables) from the module
   * - Secondary inbound
     - base ``0x60000000``, limit ``0xfc000000``, translate ``0xa0000000``
     - not rewritten there

The DRAM size the operating system uses is the word the boot ROM stores at
local address ``0x40`` after its pattern tests: 64, 32 or 16 MiB, with
three odd sizes giving a 4 MiB half window (the halving is at 1.1
``0x59d0``, 1.0.0 ``0x4130``). The model keeps the boot-time view until the
operating system writes the I2O class, and the window then follows
``dram-size``, so a different ``dram-size`` moves the translate, the library's
bias and the glue logic's base with it. The mapped region stays 128 MiB. The
size a host reads back when sizing BAR0 follows the inbound limit register
while the firmware has it narrowed (32 MiB at 0xfe000000, for example). The
firmware narrows the limit inside its PCI setup, clears the configuration
Retry inside the same routine, and restores 0xf8000000 only afterwards, so a
host that sized BAR0 in that short window would read 32 MiB. IRIX sizes the
BAR during its boot-time slot scan; that it never sizes it again is
*unverified*.

The flash is reachable from the host only if the translation reaches
``0xfef80000``, which neither translate value does. No host window onto
flash is recorded, so the model has none.

.. _sgi-sonic-hdsp-pld:

Glue logic
----------

The glue logic ("PLD") is memory controller bank 1. The host reaches it at
BAR0 plus ``0x4000000`` after the operating system has set the window and
at BAR0 plus ``0x6000000`` before it. The i960 reaches it at the bank 1
base, which the boot ROM computes as the DRAM base plus half the DRAM plus
64 MiB, ``0xa6000000`` for 64 MiB of DRAM; the module reads that base from
the bank 1 base register (local ``0x1510``) and finds the reset byte at
the base plus ``0xa0003`` (*software expectation*,
:src:`sonic-iop-flash-1.1 0x28-0x10df`, :src:`sonic-hdsp-module-1.0
0x9888`).

Byte accesses at these offsets from the glue logic's base, with a shadow
copy the library keeps (*software expectation*,
:src:`sonic-hdsp-library-5.0 0x5ec167c0`):

.. list-table::
   :header-rows: 1
   :widths: 14 38 12 36

   * - Offset
     - Use
     - Rating
     - Source
   * - ``0x0a0000``
     - Reset lines, bits 0 to 3. "Enter reset *n*" sets bit *n* and "exit
       reset *n*" clears it. Each bit holds DSP *n* in reset.
     - *software expectation*
     - :src:`sonic-hdsp-library-5.0 0x5ec167c0`. The module drives the
       same byte from the i960 side (:src:`sonic-hdsp-module-1.0 0x9888`).
   * - ``0x030000``
     - Bit 0 is "command mode". Its effect is not recorded.
     - *software expectation*
     - :src:`sonic-hdsp-library-5.0 0x5ec167c0`.
   * - ``0x028000``
     - Interrupt request. The low five bits read nonzero when an interrupt
       is pending.
     - *software expectation*
     - :src:`sonic-hdsp-library-5.0 0x5ec167c0`.
   * - ``0x048000``, ``0x050000``
     - Interrupt enable. The library writes both bytes 1 to enable and 0 to
       disable.
     - *software expectation*
     - :src:`sonic-hdsp-library-5.0 0x5ec167c0`.
   * - ``0x018003``
     - A byte the boot ROM reads after its banner (at ``0x6f0``) and tests
       bit 7 of. With bit 7 set the boot ROM starts the operating system
       (the "starting" path, ``0x7f8``); with it clear it prints a message
       and waits for bit 0 of the local word at ``0x14``, an interactive
       debug monitor on the console. The model returns a value with bit 7
       set, an *approximation* (the byte is a boot-mode switch or jumper,
       *inferred*).
     - *software expectation*
     - :src:`sonic-iop-flash-1.1 0x6f0 0x7f8`.
   * - ``0x000003``
     - The boot ROM writes progress codes 1, 2, 3 and 5 here (at ``0x21c``,
       ``0x480``, ``0x5cc``, ``0x6c4``). The model accepts and logs them.
     - *software expectation*
     - :src:`sonic-iop-flash-1.1 0x21c 0x480 0x5cc 0x6c4`.
   * - ``0x080000``, ``0x038000``
     - The library stores these two addresses. Their use is not recorded.
     - *unknown*
     - :src:`sonic-hdsp-library-5.0 0x5ec167c0`. A trace of the library's
       accesses would settle it.

The host's offsets and the i960's offsets from the bank 1 base are the same
offsets into the glue logic (the library and the module both drive the DSP
reset lines through them), so the model has one glue register file, seen
from the host at the glue base in BAR0 plus the offsets above and from
the i960 at the bank 1 base plus the same offsets, with the byte at
position 3 of the word, an *approximation* for the sharing; a trace of the
library's and the module's accesses together would settle it. The glue
block is not the byte-register block at ``0xd0000000``
(:ref:`sgi-sonic-hdsp-glue`).

Behaviour of the reset byte. The module holds a DSP in reset for 8 ms,
releases it, waits 1 ms and then configures it, so a DSP's reset byte
must hold its value and release with no side effect on the other three
bits (:src:`sonic-hdsp-module-1.0`, the DSP boot routine,
*software expectation*). Writing bit *n* resets DSP *n* only. The model
resets the DSP core, its on-chip memories and its host port when bit *n*
rises and runs it when bit *n* falls, an *inferred* reading of the module's
use. The glue logic's real reset duration and what it resets are
*unknown*.

Interrupt request byte. The model holds one bit per DSP in the low four
bits and one spare bit, the fifth, always zero. A DSP's bit follows its
interrupt-out line, which the monitor programs drive through a bit of
timer 0's control register (:ref:`sgi-sonic-hdsp-dsp-int`). That the five
bits are four DSPs and a spare is an *approximation*. What would settle it
is a trace of which bit sets when each DSP raises its line.

Interrupts
==========

.. list-table::
   :header-rows: 1
   :widths: 46 12 42

   * - Fact
     - Rating
     - Source
   * - The i960's messaging unit raises the PCI interrupt pin named in the
       ATU interrupt pin register when an outbound message register is
       written, an outbound doorbell is set or the outbound post queue is
       not empty, and the matching mask bit is clear. The pin is INTB with
       the stock boot code.
     - *established* for the chip, *software expectation* for the pin
     - :src:`intel-i960rx-272736-002 p. 16-43, 17-2, 17-23, 17-24`,
       :src:`sonic-iop-flash-1.1 0x28-0x10df`.
   * - The library masks every outbound source (``0xffffffff`` to the mask)
       before it posts its first message. Its synchronous post loops run
       the reply handler and sleep 1 ms between tries, so it polls for
       replies.
     - *software expectation*
     - :src:`sonic-hdsp-library-5.0 0x5ed89370`.
   * - The IRIX interrupt handler for the card takes and drops the board
       mutex and touches no register, so it neither acknowledges nor
       masks the source. The Bridge pin stays asserted for as long as the
       card holds it.
     - *software expectation*
     - :src:`sonic-hdsp-driver-9.2.5 0x3f64`.
   * - The glue logic has an interrupt-request byte and two enable bytes, which
       the library sets and reads. Which of them drives the PCI pin, and
       whether the card has any interrupt source the library enables, are
       not recorded.
     - *unknown*
     - :src:`sonic-hdsp-library-5.0 0x5ec167c0`.

The model raises the PCI pin the ATU interrupt pin register names while a
messaging-unit outbound source is unmasked and pending. It also raises it
while the interrupt-request byte is nonzero and both enable bytes are
nonzero, an *approximation*; with the stock library, which keeps both
disabled, no interrupt is raised. A trace of an enabled run would settle
which sources reach the pin. The card's INTB wires to Bridge pin *device*
XOR 4 (:doc:`bridge`, *software expectation*). INTA is the chip's reset
value, which the stock boot code replaces, so a model that wires INTA by
hand disagrees with what the boot code programs.

.. _sgi-sonic-hdsp-iop:

The i960 I/O processor
======================

.. _sgi-sonic-hdsp-boot:

Boot
----

The processor reads the initial boot record at ``0xfeffff30`` at reset,
takes its process control block and first instruction from it, and runs
(:src:`intel-i960rx-272736-002 p. 3-11, 11-13`, *established*). Both flash
images carry the same record and process control block:

.. list-table::
   :header-rows: 1
   :widths: 46 12 42

   * - Fact
     - Rating
     - Source
   * - The flash is 512 KiB, mapped at ``0xfef80000`` to ``0xfeffffff``, so
       flash offset *N* is address ``0xfef80000`` plus *N*. The boot record
       is at offset ``0x7ff30``.
     - *software expectation*
     - :src:`sonic-iop-flash-1.1 0x7ff30`,
       :src:`sonic-iop-flash-1.0.0 0x7ff30`.
   * - The record holds bus-configuration byte ``0x40`` in its fourth word,
       first instruction pointer ``0xfef80028``, process control block
       pointer ``0xfef80000`` and six check words.
     - *software expectation*
     - :src:`sonic-iop-flash-1.1 0x7ff30`.
   * - The process control block is the first ``0x28`` bytes: control
       table ``0xfef80e80``, arithmetic-controls image ``0x9000``, fault
       configuration ``0x40000000``, interrupt table ``0xa0000000``,
       interrupt stack ``0xa007fe00``, fault table ``0xf0040000``,
       system procedure table ``0xf00c0000``, instruction cache word
       ``0x10000`` and register cache word ``0x1400``.
     - *software expectation*
     - :src:`sonic-iop-flash-1.1 0x0-0x27`.
   * - Reset runs a boot ROM from ``0x28`` to about ``0x10e0``. It programs
       the on-chip units, writes the interrupt table at ``0xa0000000``,
       sizes DRAM with pattern tests at ``0xa0000000`` (messages for 16, 32
       and 64 MB), runs a DMA read and write self-test, prints progress on a
       debug UART at 38400 baud and polls the same UART for a one-letter
       command.
     - *software expectation*
     - :src:`sonic-iop-flash-1.1 0x28-0x10df`; the command letters (d, s,
       r, w, z) are *inferred* from the compare chain.
   * - Without a command the boot ROM starts the operating system. It
       copies ``0x7e000`` bytes from the flash to DRAM at ``0xa0080000``
       (the copy, at ``0x10b0``, is in the shared boot ROM) and the
       operating system's pointers are linked for ``0xa0080000`` plus the
       file offset. What follows differs: 1.1 clears a bss span at
       ``0xa00ded44`` to ``0xa010f454`` and continues at flash offset
       ``0x1560``, and 1.0.0 clears ``0xa00d60d4`` to ``0xa00dbdf4`` and
       continues at ``0x1490``.
     - *software expectation*
     - :src:`sonic-iop-flash-1.1 0x10b0 0x1560`, :src:`sonic-iop-flash-1.0.0
       0x10b0 0x1490`.
   * - The two images are identical up to ``0x10df`` (boot ROM, boot
       record, process control block, copy loop) and differ after it.
     - *software expectation*
     - :src:`sonic-iop-flash-1.0.0` and :src:`sonic-iop-flash-1.1`.
   * - The library derives a version number from the status block's
       product-id string and needs 110 or more. A string that begins with
       the third-party evaluation-board family name gives 106 and fails
       bring-up. The 1.0.0 image carries that family name and the 1.1 image
       does not, so the library expects the 1.1 image.
     - *software expectation* for the rule, *inferred* for the image
     - :src:`sonic-hdsp-library-5.0 0x5ed8b580 0x5ecd2b84`,
       :src:`sonic-iop-flash-1.0.0 0x3c60`, :src:`sonic-iop-flash-1.1
       0x5950`. How each image assembles the string at run time was not
       run.

Both images start the I2O operating system that the host talks to through
the messaging unit: it answers the host's executive messages, loads the
downloaded board application module and gives it its services
(:src:`sonic-iop-flash-1.1 0x53388`, *software expectation*). Every service
the module imports is in the operating system's own service table, 134
entries in 1.1 and 117 in 1.0.0, in both
(:src:`sonic-hdsp-module-1.0`, :src:`sonic-iop-flash-1.1 0x53388`,
:src:`sonic-iop-flash-1.0.0 0x4c520`), so the
model re-creates none of it: it runs the image.

The processor core is an i960 Rx and Jx instruction-set interpreter that
the device embeds. It is written from the Intel Rx and Jx developer's
manuals, derived from MAME's BSD-3-Clause i960 core for the register set,
the local register cache and the interrupt-table walk, and is a library
with no PCI or board knowledge (``hw/coproc/i960``). It implements the instruction formats the boot ROM and operating
system use, the call and fault machinery, the system procedure table and
the interrupt table walk, and the supervisor and atomic instructions. It
models no caches, no pipeline timing, no breakpoints and no parallel
faults; the on-chip units are the device's.

.. _sgi-sonic-hdsp-local-map:

Local address map
-----------------

.. list-table::
   :header-rows: 1
   :widths: 24 34 14 28

   * - Local address
     - Contents
     - Rating
     - Source
   * - ``0x00000000``-``0x000003ff``
     - Internal data RAM. Word 0 holds the NMI vector.
     - *established*
     - :src:`intel-i960rx-272736-002 p. 3-11, 11-13`.
   * - ``0x00001000``-``0x000017ff``
     - On-chip peripheral registers: bridge unit ``0x1000``, ATU
       ``0x1200``, messaging unit ``0x1310``, DMA ``0x1400``, memory
       controller ``0x1500``, bus arbiter ``0x1600``, I2C ``0x1680``.
     - *established*
     - :src:`intel-i960rx-272736-002 pp. C-6 to C-12`.
   * - ``0x88000000`` and up
     - Secondary outbound window onto the DSPs' PCI memory space.
     - *software expectation*
     - :src:`sonic-hdsp-module-1.0` (the DSP register addresses).
   * - ``0xa0000000``
     - DRAM base. The interrupt table is at the start. The operating
       system image runs at ``0xa0080000`` and up.
     - *software expectation*
     - :src:`sonic-iop-flash-1.1 0x10b0`.
   * - Memory controller bank 1: DRAM base plus half the DRAM plus
       64 MiB, ``0xa6000000`` for 64 MiB of DRAM
     - The glue logic ("PLD"). The module reads the bank 1 base register
       (local ``0x1510``) for its glue base and finds the DSP reset byte at
       that base plus ``0xa0003``. The boot ROM first writes
       ``0xa4000000`` there and then recomputes it.
     - *software expectation*
     - :src:`sonic-iop-flash-1.1 0x28-0x10df`,
       :src:`sonic-hdsp-module-1.0 0x9888`.
   * - ``0xd0000000`` plus *n* x ``0x10000`` plus 3
     - A byte-wide register block, for *n* up to ``0xf``: the debug UART
       and board registers, not the glue logic
       (:ref:`sgi-sonic-hdsp-glue`).
     - *software expectation*
     - :src:`sonic-iop-flash-1.1 0x28-0x10df`.
   * - ``0xe0040000``
     - One byte register. The 1.0.0 operating system reads it in a
       board-identification routine: an upper nibble of 1 gives board
       type 7, anything else 13. The 1.1 image has no read and writes the
       byte once. Both write it elsewhere. Nothing else of ``0xe0000000`` is
       hardware: the run of ``0xe000`` to ``0xe0ff`` in the image is a table
       of 16-bit constants.
     - *software expectation*
     - :src:`sonic-iop-flash-1.0.0 0x3c24 0x2bb0 0x39d0`,
       :src:`sonic-iop-flash-1.1 0x5ea0`.
   * - ``0xf0000000``-``0xf00fffff``
     - Nothing backs it. The process control block names the fault table at
       ``0xf0040000`` (word 0) and the system procedure table at
       ``0xf00c0000`` (word 5) there, but neither image holds such tables:
       the operating system builds its own in RAM and installs them with
       the reinitialising ``sysctl`` long after the boot ROM. The reset
       flow reads the system procedure table's word at +0xc
       (``0xf00c000c``) for the supervisor stack pointer and trace bit,
       which nothing uses before that reinitialisation.
     - *software expectation* for the pointers and the images' content;
       *inferred* for the bus behaviour
     - :src:`sonic-iop-flash-1.1 0x0-0x27 0x15a0`, :src:`sonic-iop-flash-1.0.0
       0x14d0`, :src:`intel-i960rx-272736-002 pp. 7-16, 7-17, 11-12`. The reinitialising
       ``sysctl`` is that one; the other ``sysctl`` instructions (1.1
       ``0x7c68``, ``0x7c8c``, ``0x7dec``; 1.0.0 ``0x60a8``, ``0x60cc``,
       ``0x622c``) are type-1, pass-through and type-5 messages.
   * - ``0xfe000000``-``0xfeffffff``
     - Memory controller bank 0, enabled at reset as a 16 MiB bank with the
       512 KiB flash repeating inside it: flash offset is the address modulo
       ``0x80000``, so ``0xfeffff30`` is image offset ``0x7ff30``.
     - *established* for the bank, *inferred* for the flash repeating
       inside it
     - :src:`intel-i960rx-272736-002 p. 14-10`,
       :src:`sonic-iop-flash-1.1 0x7ff30`.
   * - ``0xfef80000``-``0xffffffff``
     - Flash, then the boot record, then the core's memory-mapped
       registers from ``0xff000000``: timers at ``0xff000300``, the
       interrupt controller at ``0xff008500``.
     - *established* for the layout, *software expectation* for the flash
     - :src:`intel-i960rx-272736-002 p. 3-6, C-2 to C-4`.

Addresses from ``0xf0000000`` to ``0xfdffffff`` are outside every memory
bank: bank 1 and the DRAM bank are disabled at reset and the boot ROM
programs them later. The Rx treats an unclaimed local-bus access as a bus
monitor event; the boot ROM leaves the bus monitor disabled (it writes the
bus monitor enable register, ``0x1534``, to 0) and clears bits 5 and 6 of
the local processor interrupt status register (``0x153c``, written ``0x60``
at ``0x7c`` and ``0x86c``) early in start-up, so an unclaimed cycle
completes with undriven data and no fault, and the local-bus-fault status
bit (bit 5) may latch (*inferred*, from the manual and the boot ROM's own
clearing: :src:`intel-i960rx-272736-002 p. 14-36, 14-38`,
:src:`sonic-iop-flash-1.1 0x7c 0x86c`).

The model gives a read of an unclaimed local address a fixed value, all
ones, and never faults; it may set bit 5 of the status register. A write
is dropped. Each access is logged ``LOG_UNIMP`` with the address and the
program counter. Each is an *approximation*; what the real board's data
bus returns is *unknown*. A read of the fault table on a normal boot
means the model's core took a fault the hardware does not: the table has 8
bytes per fault type, so a read of ``0xf0040050`` is type ``0xa`` (type
mismatch, such as a supervisor-only instruction in user mode) and
``0xf0040010`` is type 2 (operation). With the table reading as zeros a
type-0 entry is a local call to address 0, which re-faults, which is the
repeated read. Check first the core's reset state (supervisor mode, reset
PC ``0x001f2002``) and the ``modpc`` operand order of the one privileged
instruction between the first bank 1 write and the first console
character (``0x4b8``) (*inferred*, :src:`sonic-iop-flash-1.1 0x4b8`). The
model returns a constant with upper nibble 1 from ``0xe0040000`` for
1.0.0, which is the only image that reads it (``0x10``), and ignores
writes, an *approximation*: the board-identification value of a real board
is *unknown*.

.. _sgi-sonic-hdsp-units:

On-chip units
-------------

Register addresses are the chip's (*established*,
:src:`intel-i960rx-272736-002 pp. C-6 to C-12`). What the firmware does to
each unit is a *software expectation* from the flash images and the board
application module. The model implements each unit as the manual describes
it; the contract below is what the firmware needs to run.

.. list-table::
   :header-rows: 1
   :widths: 18 26 36 20

   * - Unit and address
     - Firmware use
     - Model
     - Rating
   * - Bridge unit ``0x1000``-``0x105f``
     - Boot ROM programs ``1263:060e`` identity, secondary IDSEL select
       ``0x1042`` = ``0x03c1``, secondary decode enable ``0x105c`` =
       ``0xffc0``, PCI interrupt routing select ``0x1050`` = ``0xf``, and
       sets bit 2 of the extended bridge control ``0x1040`` (the
       configuration cycle retry bit).
     - Registers as the manual describes. Retry gates the host's
       configuration cycles. The secondary IDSEL select hides ``S_AD16``
       and ``S_AD22`` to ``S_AD25`` and not the DSPs' lines
       (:ref:`sgi-sonic-hdsp-secondary`).
     - *software expectation*
   * - ATU ``0x1200``-``0x12b0``
     - Identity, interrupt pin, primary inbound window, secondary inbound
       window, expansion ROM window, ATU configuration (``0x1288``,
       boot code ``0x756``, operating system ``2``, module ``6``), secondary ATU command
       (``0x1298``, boot code ``0x16``, module ``0x316``), secondary
       outbound memory window (``0x1268``) and the secondary outbound
       configuration address and data ports (``0x12a8``, ``0x12b0``).
     - Registers as the manual describes. The ATU class and the inbound
       window follow the firmware's writes as in
       :ref:`sgi-sonic-hdsp-windows`. Outbound windows reach the secondary
       bus (:ref:`sgi-sonic-hdsp-secondary`).
     - *software expectation*
   * - Messaging unit ``0x1310``-``0x1380``
     - IMR0, IMR1, OMR0, OMR1, IISR, IIMR and OIMR for the handshake
       (:ref:`sgi-sonic-hdsp-host-library`). The operating system drives
       the four circular queues.
     - Message registers, doorbells, interrupt status and mask, and the
       four circular queues with the head and tail pointers and the
       queue-base register as the manual gives them. The queue ports at
       ``0x40`` and ``0x44`` of the host window read and advance the tail
       pointers.
     - *software expectation*
   * - DMA ``0x1400``-``0x14a4``
     - The operating system drives the DMA service. The boot ROM runs a
       self-test with descriptors at ``0xa0002000`` and polls bit 10 of
       the channel status ``0x1404``.
     - Three channels (channel 0 at ``0x1400``, channel 1 at ``0x1440``,
       channel 2 at ``0x1480``, :src:`intel-i960rx-272736-002 p. C-10`).
       Descriptor chains in local memory, PCI addresses on
       the primary or secondary bus. DMA to the primary bus uses the
       function's PCI address space, so the Bridge applies its swap.
     - *established* for the layout, *software expectation* for the use
   * - Memory controller ``0x1500``-``0x153c``
     - ``0x1504`` = ``0xfef80000`` (bank 0 base is the flash), ``0x1508``
       and ``0x150c`` wait states ``0x30301``, ``0x1510`` = ``0xa4000000``
       then recomputed as the bank 1 glue base, ``0x1500`` =
       ``0x530033``, DRAM registers ``0x151c`` to ``0x1530`` (base
       ``0xa0000000``, wait states, refresh ``0x204``). The module reads
       ``0x1510`` for its glue base.
     - Registers as the manual describes. DRAM size is the model's
       ``dram-size`` property. Bank 1 decodes the glue logic at the base the
       register holds. The refresh and wait-state fields have no timing
       effect.
     - *software expectation*
   * - Bus arbiter ``0x1600``, ``0x1604``
     - ``0x2056`` and ``8``.
     - Registers hold their value. No arbitration.
     - *software expectation*
   * - ``0x1610``
     - The module writes ``1`` then ``0x10``. The manual lists
       ``0x1608``-``0x167f`` as reserved.
     - A stored register, logged ``LOG_UNIMP`` on each write.
     - *unknown*
   * - Interrupt controller ``0xff008500``, ``0xff008504``,
       ``0xff008510``, ``0xff008520``-``0xff008528``
     - Boot ROM clears the pending register and sets the mask to ``0x48``
       with atomic modify instructions, ``ICON`` ``0x4000``, ``IMAP1`` set.
       The module's periodic and external interrupt handlers read pending
       and mask. The module hooks the messaging unit's vector (XINT7).
     - Pending, mask, control and map registers as the manual describes.
       The interrupt table at the address in the process control block
       stays live: the module reads and rewrites vector entries.
     - *software expectation*
   * - Timers ``0xff000300``-``0xff000318``
     - Not recorded by the boot ROM. The operating system needs a tick, and
       the module's microsecond delay and timeouts read an elapsed-time
       service.
     - Two 32-bit timers as the manual describes, counting at the core
       clock, interrupt on reload.
     - *inferred*
   * - I2C ``0x1680``-``0x1690``: ICR, ISR, ISAR, IDBR, ICCR
     - The operating system's driver table points at these five registers.
       It writes ICR ``0x4000`` (unit reset), ISR ``0x7f0`` (clear the
       status bits), ICR 0 and then ``0x60`` (unit enable and SCL enable),
       and ICCR ``0xff``. Each byte goes through IDBR, then ICR bit 3
       (transfer byte) with START, STOP and not-acknowledge bits as needed,
       and the driver waits for ISR bit 7 (IDBR receive full) or for ICR bit
       3 to clear. The reads and writes are the 1 KiB EEPROM's
       (:ref:`sgi-sonic-hdsp-hiding`).
     - The register set as the manual lays it out: ICR bit 0 START, 1 STOP,
       2 acknowledge control, 3 transfer byte, 4 master abort, 5 SCL
       enable, 6 unit enable and 14 unit reset; ISR bit 7 IDBR receive
       full, 6 IDBR transmit empty, 3 bus busy, 2 unit busy, 1 not-ack
       received. Master mode only, at one slave, a 24C08-class EEPROM at
       7-bit address ``0x50`` holding 1 KiB, initial content from
       ``nv-word`` and ``0xff`` elsewhere. Every other address phase is not
       acknowledged. The EEPROM acknowledges its write sequence.
     - *established* for the register layout
       (:src:`intel-i960rx-272736-002 pp. 21-19 to 21-27`), *software
       expectation* for the use (:src:`sonic-iop-flash-1.1 0x57d0`), and
       *approximation* for the EEPROM's presence and content

The bridge unit's configuration-cycle retry bit holds the host's
configuration cycles in retry from reset until the resident operating
system clears it at the end of its PCI setup; the boot ROM sets it
(:ref:`sgi-sonic-hdsp-hiding`, :src:`intel-i960rx-272736-002 p. 15-64`,
*established* for the bit, *software expectation* for the sequence).

.. _sgi-sonic-hdsp-unit-model:

On-chip unit behaviours the model fixes
---------------------------------------

The manual leaves some of the units' behaviour to the implementation, and
the model has to choose. Chip facts are from the Rx manual; each model
choice is an *approximation* with what would settle it.

.. list-table::
   :header-rows: 1
   :widths: 46 12 42

   * - Fact
     - Rating
     - Source
   * - The primary outbound memory window is local ``0x80000000`` to
       ``0x83ffffff``, a fixed 64 MiB, and the primary outbound memory
       window value register (POMWVR, ``0x1254``) supplies the PCI address
       it translates to. A local access in it becomes a cycle on the primary
       PCI bus, which the model issues as the function's PCI DMA, so the
       Bridge's byte swap applies. The secondary outbound memory window
       starts at ``0x88000000`` and its register (SOMWVR) is ``0x1268``.
     - *established* for the windows and registers, *inferred* for the DMA
       and swap consequence
     - :src:`intel-i960rx-272736-002 pp. 16-12, 16-51, 16-55`.
   * - The APIC register select register (``0x1300``) and window register
       (``0x1308``) are stored. A host write to messaging-unit offset ``0x00``
       or ``0x08`` sets IISR bit 7 or 8. The model has no APIC bus.
     - *established* for the interrupt bits, *approximation* for the
       missing bus
     - :src:`intel-i960rx-272736-002 pp. 17-15, 17-16, 17-19`. A
       firmware use of the APIC bus would settle it.
   * - A host write to a messaging-unit index register (offset ``0x50`` and
       up, to ``0xffc``) sets IISR bit 6. The model does not store the
       value: the chip keeps index registers in local memory.
     - *established* for the bit, *approximation* for the value
     - :src:`intel-i960rx-272736-002 pp. 17-19, 17-36`. A firmware read of
       those locations would settle it.
   * - NISR (``0x1700``), X7ISR (``0x1704``) and X6ISR (``0x1708``) are
       computed from unit state, not stored: NISR bit 8 from the messaging
       unit's NMI sources, X7ISR bit 2 from its XINT7 sources and X6ISR
       bits 0 to 2 set when DMA channel 0 to 2 completes with its interrupt
       enabled (the manual's "DMA channel n interrupt pending"). The other
       bits (the external pins, BIST, I2C and APIC) read 0, an
       *approximation*.
     - *established* for the bit meanings
     - :src:`intel-i960rx-272736-002 pp. 8-39, 8-41, 8-43`.
   * - The outbound free queue overflowing sets IISR bit 5 and raises NMI,
       and so does IDR bit 31 (the NMI doorbell bit).
     - *established*
     - :src:`intel-i960rx-272736-002 pp. 17-18, 17-19`.
   * - PDIDR (``0x1710``) reads ``0x13``: the manufacturer field for Intel
       and the constant bit. On the RD, bit 27 (VCC, 1 for 3.3 V) is set,
       since the RD is the 3.3 V part, so the model's value differs from
       hardware there. The version, product, generation and model fields are
       unknown here, and the model's zeros in them are unverified.
     - *established* for the bit meanings, *approximation* for the value
     - :src:`intel-i960rx-272736-002 p. 11-23` (VCC bit, Table 11-9),
       :src:`intel-i960rx-272736-002 p. 1-9` (3.3 V part). The RD's value
       from the Specification Update (order number 272918) or a read of the
       chip would settle it.
   * - The core memory-control registers at ``0xff008600`` to ``0xff0086ff``
       and the logical memory registers DLMCON, LMADR and LMMR at
       ``0xff008100`` to ``0xff00813f`` are stored and have no effect on
       bus behaviour.
     - *established* for the addresses, *approximation* for the effect
     - :src:`intel-i960rx-272736-002 pp. 12-4 to 12-9, C-2, C-3`. A
       firmware dependence on their effect would settle it.
   * - The EEPROM's word address wraps within each 256-byte page.
     - *approximation*
     - A real 24C08's counter behaviour at a page end is not in a held
       source; the part's data sheet would settle it.
   * - The I2C unit completes every transfer at once, with no SCL timing.
       ICR bit 14 resets the unit, and ISR bits 4 to 10 clear when a 1 is
       written to them.
     - *approximation* for the timing, *established* for the bits
     - :src:`intel-i960rx-272736-002 pp. 21-19, 21-22, 21-23`.
   * - The ATU's expansion ROM window is not decoded and the expansion ROM
       base register reads 0, though the boot code programs its limit and
       translate value.
     - *approximation*
     - :src:`intel-i960rx-272736-002 p. 16-41`. A host read of the ROM
       BAR would settle it.
   * - Configuration space past the ATU header is writable from the local
       bus only (the host can still read those bytes), and the bridge unit's
       registers at ``0x40`` to ``0x5f`` are read-only to the host.
     - *approximation*
     - The manual's per-register PCI access rows, which settle it register
       by register (:src:`intel-i960rx-272736-002 pp. 15-38 to 15-73`).
   * - Registers that clear on write clear per written byte. Sub-word writes
       to the two outbound configuration data ports (``0x12ac``, ``0x12b0``),
       non-word writes to messaging unit offsets below ``0x50``, and non-word
       reads of the two queue ports (``0x40``, ``0x44``) are ignored and
       logged. Other sub-word reads of the messaging unit return the register
       shifted by the byte offset.
     - *approximation*
     - The registers are 32 bits wide in the manual
       (:src:`intel-i960rx-272736-002 p. C-8`); the byte-granular clear and
       the ignored accesses are the model's choice. The manual
       (:src:`intel-i960rx-272736-002 pp. 17-5, 17-8`) only requires one word
       per queue access.
   * - The DSP host registers are 32-bit only. The data port at ``0x1c``
       and above aliases the host FIFO. HCVR bit 0 (HC) is cleared by the DSP
       core taking the command, so with no DSP core running the vector it
       stays set.
     - *established* for the layout and HC, *approximation* for the
       missing core
     - :src:`nxp-dsp56301um-4 pp. 6-46, 6-59`.
   * - A DSP's PCI mode (DCTR HM = 1) is set on release from reset, which
       stands in for the bootstrap ROM's own write.
     - *approximation*
     - Running the bootstrap ROM on the DSP core would settle it
       (:src:`nxp-dsp56301um-4 pp. 4-4, 6-69`).

.. _sgi-sonic-hdsp-memory:

DRAM and flash
--------------

The boot ROM probes DRAM at ``0xa0000000`` with pattern tests and has
messages for 16, 32 and 64 MB (*software expectation*,
:src:`sonic-iop-flash-1.1 0x28-0x10df`). The DRAM fitted to the card is
*unknown*. The model's ``dram-size`` property defaults to 64 MiB, the
largest size the boot ROM tests, an *approximation* that cannot starve the
operating system (the disk-channel setup allocates several hundred
kilobytes per channel, :src:`sonic-hdsp-module-1.0 0x9b90`). The sizing
tests read back what they write, so DRAM must be real RAM at
``0xa0000000``, and the boot ROM probes ``0x4000000``, ``0x2000000`` and
``0x1000000`` above the base: a model that returns the same data for
aliased addresses reports a smaller size. A photograph of a real board's
memory chips would settle the size.

The flash is 512 KiB at ``0xfef80000``. The board application's upgrade
module reprograms it with the byte-program command sequence at offsets
``0x5555`` and ``0x2aaa`` of the part and a bulk erase, touching the boot
record area as well (:src:`sonic-iop-flash-1.1 0x7ff30`,
*software expectation*). That sequence is the AT29C040A's
(:doc:`at29c040a`); the part fitted to the card is *unknown*. The model
uses the AT29C040A programming model for the flash, an *approximation*
whose limit is that a part with other sector or protection rules would
behave differently; a board photograph would settle it. The flash is
writable only when the ``flash-writable`` property is on, and writes
persist only to the backing file when ``flash-persist`` is on
(:ref:`sgi-sonic-hdsp-firmware`).

.. _sgi-sonic-hdsp-glue:

Board glue the boot ROM assumes
-------------------------------

The boot ROM and the operating system touch a block of byte-wide
registers at ``0xd0000000`` plus *n* x ``0x10000`` plus 3. They behave as
a dual UART in the MC68681 family (*inferred* from the register behaviour,
not from a part number): channel A at *n* = 0 to 3 and channel B at *n* =
8 to 11 (mode, status and clock select, command, data), *n* = 4 auxiliary
control and *n* = 5 interrupt status and mask. The boot ROM
touches *n* = 4, 5, 8, 9, 10 and 11 at the shared offsets ``0x4e4`` to
``0xdb8``, and the operating system touches 0 to 15 in 17 references
(*software expectation*, :src:`sonic-iop-flash-1.1 0x28-0x10df
0x4afc-0x4bec`; 1.0.0 ``0x17ec-0x18dc``). The only use established is a console: *n* = 9 is a
status register and *n* = 11 the data register. The block is not the glue
logic that holds the DSP reset byte, which is at memory controller bank 1
(:ref:`sgi-sonic-hdsp-local-map`). The pattern of the start-up writes
looks like a UART's divisor, mode and control registers, but the part is
*unknown*.

.. list-table::
   :header-rows: 1
   :widths: 46 12 42

   * - Fact
     - Rating
     - Source
   * - Register *n* = 9 is a status register. Bit 2 is polled for
       transmit ready.
     - *software expectation*
     - :src:`sonic-iop-flash-1.1 0x28-0x10df`.
   * - Register *n* = ``0xb`` is the transmit data register.
     - *software expectation*
     - same.
   * - The UART runs at 38400 baud and the boot ROM polls it for a command
       letter.
     - *software expectation*
     - same. The baud rate is the boot ROM's message, not a register
       reading.
   * - Start-up writes: *n* = 10 (command B) gets ``0x3a``, ``0x2a``,
       ``0x1a`` (reset transmitter, receiver and mode pointer) and then
       ``5`` (enable both); *n* = 8 (mode B) gets ``0x13`` and then
       ``0x0f``; *n* = 4 gets 0; *n* = 9 (clock select B) gets ``0xcc``,
       38400 baud; *n* = 5 gets ``0x20``.
     - *software expectation* for the writes, *inferred* for the meanings
     - same.
   * - The boot ROM reads only status B (*n* = 9), polling bit 2
       (transmitter ready) before every character until ``(status & 4) ==
       4``. Nothing else in the block is read before or during the banner.
       The operating system's console driver also reads status and the
       receive data.
     - *software expectation*
     - :src:`sonic-iop-flash-1.1 0xc4c 0xc78 0xccc 0xd18`.
   * - The glue logic's base and its LED register are set by the boot path
       or the operating system, and the LED register's use was not traced.
     - *unknown*
     - :src:`sonic-hdsp-module-1.0`.

The model implements the block as a bank of 16 byte registers. Register
9 reads ``0x04`` or ``0x0c`` (transmitter ready always) and register
``0xb`` forwards each byte written to the ``iop-console`` character
device, which the user connects to a chardev or leaves unconnected, an
*approximation* that makes the boot ROM's transmit-ready poll always
succeed. It reports receiver ready only when the character device has
input, so with none the boot ROM's command poll sees nothing and falls
through to start the operating system, the behaviour the boot ROM has with
no console command (*inferred*). The other registers hold what is written
and log ``LOG_UNIMP``. What would settle the register meanings is the board's
schematic or a longer boot ROM and operating-system trace.

.. _sgi-sonic-hdsp-timing:

Timing
------

The i960 runs at ``iop-clock`` (66 MHz by default, *inferred* from the RD
grade, :src:`intel-i960rx-272736-002 p. 1-9`). The model charges the
manual's per-instruction cycle counts with no pipeline, cache or bus-wait
model, so the i960's simulated speed is an *approximation* in the sense
that a real core is slower on cache misses and bus waits.

The host library imposes hard limits in guest time on how fast the
firmware must answer, so the processors run in slices of at most 100
microseconds of virtual time whenever the host has an outstanding wait
(*software expectation*,
:src:`sonic-hdsp-library-5.0 0x5ec2c300 0x5ed89370 0x5ec2c0e0`):

- the first status request is polled up to 26 times at 100 ms (2.6 s);
- the outbound-init request is polled up to 25 times at 100 ms;
- the handshake polls up to 100 times at 100 microseconds for the
  inbound message word to read 0 and up to 100 times for the outbound
  word to read ``0x700``, so the firmware must consume a message within
  10 ms of virtual time;
- a ring command's transmit-space wait retries at 100 microseconds, up to
  1000 times.

An idle i960 (``halt``, or a polling loop on a register the model knows)
gives its slice back at once. Idle detection must not change the firmware's
view of time: the timers and the host-visible clock continue.

.. _sgi-sonic-hdsp-dsps:

The four DSP56301 processors
============================

.. _sgi-sonic-hdsp-secondary:

Secondary PCI bus and the host port
-----------------------------------

The DSPs sit on the i960's secondary PCI bus, which the host cannot see.
The module reaches them through the ATU's secondary outbound window and
configuration ports. The model builds a real QEMU PCI bus for it, hung
off the secondary side of the bridge unit and invisible to the host's
bus; the DSPs' host ports sit on it as PCI functions, so the secondary
ATU's configuration and memory cycles reach them as they do on the card.
That the model's DSP functions are PCI devices on the bus is the model's
own structure, not a measured fact.

.. list-table::
   :header-rows: 1
   :widths: 46 12 42

   * - Fact
     - Rating
     - Source
   * - Each DSP has a configuration IDSEL on ``S_AD`` line 17 plus its
       index: ``1 << (17 + n)`` for DSP *n*.
     - *software expectation*
     - :src:`sonic-hdsp-module-1.0` (the four-way switch in the DSP PCI
       configuration routine).
   * - The module programs each DSP's configuration with the command
       register (offset 4) = ``0xf8000006`` (memory space and bus master
       enable, status bits cleared), offset ``0xc`` = ``0x2008`` (latency
       timer ``0x20``, cache line size 8) and base address register 0
       (offset ``0x10``) = the DSP's window base, using the secondary
       outbound configuration address port ``0x12a8`` and data port
       ``0x12b0``.
     - *software expectation*
     - :src:`sonic-hdsp-module-1.0` (the DSP PCI configuration routine).
   * - A DSP's host-port registers are four 32-bit registers at the DSP's
       window base plus ``0x10``, ``0x14``, ``0x18`` and ``0x1c``, reached
       at local address ``0x88000000`` plus the base masked with
       ``0x03ff0000`` plus the offset.
     - *software expectation*
     - :src:`sonic-hdsp-module-1.0` (the DSP register address setup).
   * - On the DSP56301 those are the HI32 host-side registers: control
       (HCTR) at ``0x10``, status (HSTR) at ``0x14``, command vector
       (HCVR) at ``0x18`` and the transmit and slave-receive data port
       (HTXR and HRXS) from ``0x1c``. The host-side configuration space has
       the device and vendor IDs at ``0x00`` and a single memory base
       address register (CBMA) at ``0x10``.
     - *established*
     - :src:`nxp-dsp56301um-4 pp. 6-46, 6-47`.
   * - HSTR bit 0 is TRDY (both HTXR and the DSP's receive FIFO empty),
       bit 1 is HTRQ (transmit request) and bit 2 is HRRQ (receive
       request). The module polls them with bounded waits whose failure it
       reports as -537 (transmit side) or -536 (receive).
     - *established* for the bits, *software expectation* for the waits
     - :src:`nxp-dsp56301um-4 p. 6-57`,
       :src:`sonic-hdsp-module-1.0` (the three wait routines).
   * - HCTR bits 9:8 are HTF, the transmit data transfer format, and bits
       12:11 are HRF, the receive format. The module writes ``0x900`` after
       a reset, which is HTF = 1 and HRF = 1: 24-bit words in both
       directions, through six-word-deep FIFOs.
     - *established* for the bit fields and FIFO depth, *software
       expectation* for the value
     - :src:`nxp-dsp56301um-4 pp. 6-6, 6-7, 6-50, 6-51`,
       :src:`sonic-hdsp-module-1.0` (the DSP boot routine).
   * - HCVR bit 0 is HC (host command), bits 7:1 are HV (the vector is 2 x
       HV) and bit 15 is HNMI (non-maskable). The module writes ``0xf5`` for
       an update, which is HC with vector ``0xf4``, and ``0x80fd`` to clear
       the DSP's interrupt, which is HNMI with HC and vector ``0xfc``. It
       writes the data register first for the update, then polls bit 0 of
       the command register for the DSP to clear HC, up to 20000 polls,
       error -535. The DSP monitor's vector table has handlers at those
       two vectors.
     - *established* for the encoding, *software expectation* for the
       values
     - :src:`nxp-dsp56301um-4 p. 6-59`,
       :src:`sonic-hdsp-module-1.0` (the update and interrupt-clear
       routines), :src:`sonic-dsp-programs-5.0 the DSP 0 monitor image,
       vector table at P:0xe0 to P:0xfc`.
   * - Each DSP's PCI base address register 0 is programmed by the
       module to the window base. The config command ``0xf8000006``
       enables memory space and bus mastering.
     - *software expectation*
     - :src:`sonic-hdsp-module-1.0` (the DSP PCI configuration routine).

The model's host port follows the manual's HI32 as above: HTXR becomes
receivable by the DSP through its receive FIFO, a word the DSP writes to
its slave transmit register is readable from the data port, HSTR follows
the FIFOs' state, and an HCVR write latches a host command the DSP takes at
the vector in HV, ahead of everything else when HNMI is set. FIFO depths
are the manual's six words for the 24-bit formats the module selects.
Other HCTR formats, the PCI master mode and the universal bus modes are
not modelled, because nothing on the card uses them. That the board wires
the HI32 as a PCI target on the private bus is *inferred* from the
module's configuration cycles.

The DSP is held in reset by the glue's reset byte
(:ref:`sgi-sonic-hdsp-pld`). After release the module boots it over the
host port. The DSP56301's bootstrap ROM is at ``$FF0000`` in program space
and, in host bootstrap mode C, loads a program through the HI32 as a
PCI target: it expects a word count, then a start address, then one 24-bit
word per program word, stored contiguously from that address, and starts
the program at the address where it began loading
(*established*, :src:`nxp-dsp56301um-4 pp. 4-2, 4-4, 4-5, A-3`). That the
board selects mode C is *inferred*: nothing records the mode pins, but the
module pushes a block over the host port after reset, which only a host
bootstrap accepts, and a PCI target is what the module's configuration
cycles set up (*software expectation*,
:src:`sonic-hdsp-module-1.0` (the DSP boot routine)). The model implements
this sequence as the manual lists it, so the first program arrives as the
bootstrap would load it. The mode pins are the *approximation* and the
bootstrap ROM's code is the model's own, behaving as the listing does; a
read of the board's mode strap or a scan of the module's first block
against the listing would settle it.

DSP memory and wiring
---------------------

.. list-table::
   :header-rows: 1
   :widths: 46 12 42

   * - Fact
     - Rating
     - Source
   * - Program segments load at P:0x100 to P:0xc9b on-chip for DSP 0, a
       block at P:0x408000 (start-up code and the vector template),
       X and Y data at low addresses and a loader at P:0x41fbc8. DSP 2
       places its loader and command handlers near P:0x41e800.
     - *software expectation*
     - :src:`sonic-dsp-programs-5.0 monitor image for DSP 0, P segments`.
   * - External buffers are in the address range from ``0x400000`` and
       up: Y:0x400000 for 0x8000 words (cleared by start-up code), Y:0x41f000
       (a pattern fill), Y:0x418000 (3840 words) and P:0x408000 to about
       P:0x41ffff.
     - *software expectation*
     - :src:`sonic-dsp-programs-5.0 the start-up code at P:0x408000`.
   * - The start-up code sets the status register to ``0x300``, zeroes
       Y:0xfff000 for 0x800 words, copies a 256-word vector template from
       P:0x4080c1 to the vector area, configures two serial ports, six DMA
       channels, the host interface and the SCI, and parks in a loop until
       the host sets run-mode bit 0.
     - *software expectation*
     - :src:`sonic-dsp-programs-5.0 the start-up code at P:0x408000`.
   * - Each DSP also has a memory-mapped external device in Y space: a block
       of nine locations at Y:0xfffff7 to Y:0xffffff that the four DSPs use
       as synchronisation flags, with two ports at Y:0xfffffb and
       Y:0xfffffc. Y:0xfffffb is read eight times a frame on DSPs 0 and 1,
       a high and a low word for each of four channels. Y:0xfffffc is
       written once a frame by DSP 0 and polled by DSPs 1 to 3, and DSP 3
       writes Y:0xffffff. These ports are inter-DSP plumbing, not the
       converter's audio. Two Y blocks of the images sit on that
       range: Y:0xfff000 (80 words, eight of them ``0x080000``) and
       Y:0xffff80 (128 words, all zero).
     - *software expectation* for the addresses, *inferred* for the roles
     - :src:`sonic-dsp-programs-5.0 the sync-wait loops and port accesses
       of the monitor images`. Whether Y:0xfffffb is a FIFO fed by another
       device or a bus shared with another DSP's output is *unknown*.
   * - A wait loop of the form "jump while bit 0 of Y:0xfffff8 is clear" is
       how one DSP waits for another, so the four DSPs run in lock step.
       DSP 2 is the converter-link and metering DSP.
     - *inferred*
     - :src:`sonic-dsp-programs-5.0 monitor images for DSPs 0 to 2`.

Memory model. The DSP56301 has 4096, 2048 and 2048 words of on-chip
program, X and Y memory in its default configuration. The memory switch
(OMR bit MS) and the instruction cache enable (SR bit CE) trade
program memory for cache and data memory for program memory: with MS
set, X and Y are 3072 words each and program memory is 2048 (1024 with
the cache on) (*established*, :src:`nxp-dsp56301um-4 p. 1-10`). The
bootstrap ROM is 3 K words at ``$FF0000`` in program space on recent
revisions and 192 words on early ones
(:src:`nxp-dsp56301um-4 p. 3-2`, *established*). Which revision the card
carries is *unknown*; the model uses the 3 K ROM, an *approximation*. The
external memory the board fits is not in any source. The model gives each
DSP 128K words (``0x20000``) of external P and Y memory at address
``0x400000``, which covers every address the images use, an
*approximation*; the external bus and wait states are not modelled. The
32768 words the start-up code clears at Y:0x400000 may be storage the
four DSPs share, and may be four separate banks: no source says, so the
model gives each DSP its own copy, an *approximation*; a trace of one
DSP's writes being read by another would settle it. The nine locations at
Y:0xfffff7 to Y:0xffffff are shared storage across the four DSPs,
readable and writable, that read zero after reset, an *approximation*.
The ports at Y:0xfffffb and Y:0xfffffc are the only addresses in that
block the model treats separately: it records each access in the device
trace and returns the last value written. What feeds Y:0xfffffb is
*unknown*. What would settle it is a trace of which DSP accesses it at
which frame of a running monitor image.

The DSP core is the interpreter in ``hw/coproc/dsp56300`` (DSP56300
family, written from the family manual).

.. _sgi-sonic-hdsp-dsp-int:

Serial ports, timers and interrupts
-----------------------------------

The monitor programs configure two enhanced synchronous serial interfaces
(ESSI0 and ESSI1), a serial communication interface, three timers and six
DMA channels at start-up, by writing X-space peripheral registers
(*software expectation*, :src:`sonic-dsp-programs-5.0 the start-up code at
P:0x408000`). The names and addresses of those registers are the part's
(*established*, :src:`nxp-dsp56301um-4 pp. B-2 to B-8`). The values are in
the table.

.. list-table::
   :header-rows: 1
   :widths: 30 36 34

   * - Register (X address)
     - Start-up value
     - Role
   * - ESSI0 control A, B and port C control (``0xffffb5``, ``0xffffb6``,
       ``0xffffbf``)
     - ``0x180000``; ``0x033110``, or ``0x031100`` when the not-first-board
       flag Y:0x40 is set; ``0x3c``
     - First serial port, the converter link and the audio path.
   * - ESSI1 control A, B and port D control (``0xffffa5``, ``0xffffa6``,
       ``0xffffaf``)
     - ``0x180000``; ``0x031100``; ``0x3c``
     - Second serial port. Its wiring is not recorded.
   * - Timer 2 (``0xffff87``, ``0xffff85``, ``0xffff86``)
     - control ``0x205``, compare ``0x02dc60``, load ``0``
     - Frame or sample timer, *inferred*.
   * - Timer 0 and timer 1 control (``0xffff8f``, ``0xffff8b``)
     - ``0x800``
     - Timer 0's bit 13 is driven by the monitor as a data output
       (below).
   * - SCI clock and control, port E control (``0xffff9b``, ``0xffff9c``,
       ``0xffff9f``)
     - ``0x20``; ``0x010b05``; ``3``
     - Serial communication interface; the card's second external
       connector is a serial one (:ref:`sgi-sonic-hdsp-limits`).
   * - Host control, PCI master control, PCI address (``0xffffc5``,
       ``0xffffc7``, ``0xffffc8``)
     - ``0x100000`` then ``0x100001``; ``0x400000``; ``0x060000``
     - Host interface (DCTR, DPMC, DPAR).
   * - Interrupt priority C and P (``0xffffff``, ``0xfffffe``)
     - ``0x1ff000``; ``0x241``
     - Interrupt priority configuration.

ESSI0's control A of ``0x180000`` selects 24-bit words with a frame rate
divider of 0 (*inferred* from the field decode of
:src:`nxp-dsp56301um-4 pp. 7-13 to 7-17`). Control B ``0x033110`` on the first
board enables the receiver and transmitter 0, selects network mode,
synchronous operation, an external bit clock and a frame sync the DSP
generates. ``0x031100`` on later boards selects normal mode with both
external. ESSI1 uses ``0x180000`` and ``0x031100``. These decodes are
*inferred*: they apply the manual's field tables to the stored values
and no run confirms them. They put the audio sample timing on the
board's external bit clock, whose supplier is *unknown*.

Six DMA channels move the samples. Channel 0 moves words from a Y buffer at
Y:0xfff050 to ESSI0's transmit register (``0xffffbc``) on the transmit
request, and channel 1 moves ESSI0's receive register (``0xffffb8``) into
a buffer at Y:0xfff050, with control values ``0x4e5a51`` and ``0x6e52c4``.
Channels 2 and 3 do the same for ESSI1. Each frame the code switches
between two buffer sets, Y:0xfff050 and Y:0xfff258, by rewriting the source
registers (*software expectation*, :src:`sonic-dsp-programs-5.0 the
start-up code and frame loop`).

The monitor programs do not write the PLL control register
(*software expectation*, :src:`sonic-dsp-programs-5.0 the start-up
code`), so the DSP clock is whatever the board's external clock input and
the PLL's reset state give. The PLL
control register's reset state follows the PINIT pin
(:src:`nxp-dsp56301um-4 p. 4-20`, *established* for the chip), and the
board's PINIT strap and input clock are *unknown*. The model clocks each DSP
at ``dsp-clock`` (100 MHz by default), an *approximation* chosen high
enough that code paced by the sample clock never starves: a static count
puts DSP 0's frame loop at 550 to 650 cycles per 48 kHz frame, about 26 to
31 million cycles a second, and the images for DSPs 1 to 3 are within a
factor of 1.4 of that (*inferred*, :src:`sonic-dsp-programs-5.0 monitor
images, cycle count by instruction words`; not measured). A board
photograph of the crystal, or the PLL control register of a running board,
would settle the clock.

The interrupt-out line. The monitors set a bit in an interrupt word when a
timer compare expires (bit 10) and every 32 click counts (bit 14), and a
nonzero AND of that word with a mask sets bit 13 of timer 0's control
register. With the timer in general-purpose I/O mode and the direction bit
(bit 11) set, bit 13 is the data output to the timer's pin
(*established*, :src:`nxp-dsp56301um-4 p. 9-28`). Separately, the HI32 has
its own interrupt output: bit 6 of DCTR drives the HINTA pin and is
mirrored in bit 6 of HSTR, and the host-command vector ``0xfc`` clears it
(*established* for the bit, :src:`nxp-dsp56301um-4 pp. 6-24, 6-57`;
*software expectation* for the vector,
:src:`sonic-dsp-programs-5.0 the host-interrupt routine`). Which of the two
reaches the i960 and the interrupt-request byte is *inferred* to be the
timer pin from the monitors' use, and could be the HI32 pin. The model
drives DSP *n*'s interrupt-out line from the timer's data output and
presents it to the i960's external interrupt input and to the
interrupt-request byte (:ref:`sgi-sonic-hdsp-pld`), an
*approximation*. A DSP-side trace of the two against the i960's interrupt
controller would settle it.

.. _sgi-sonic-hdsp-dsp-limits:

What the DSP model lacks
------------------------

The chip is documented, so the DSP model follows the manual wherever the
card uses a feature. What no source gives, and the model therefore
chooses, is:

- the board's input clock and the PLL's PINIT strap (above);
- the mode pins the card straps (the bootstrap, above);
- the external memory fitted and the external bus wait states;
- the board's wiring of both ESSI pins, the SCI pins and the timer pins.

The DSP model executes the 56300 instruction set from
:src:`nxp-dsp56300fm-5` and implements the HI32 target mode, ESSI,
SCI, timer, DMA and interrupt registers of the DSP56301 as the manual
documents them, at least as far as the monitors and the module exercise
them. Features the card does not use (the HI32's master and universal-bus
modes, the SCI's other modes) are not modelled and an access that needs one
logs ``LOG_UNIMP``. An unclaimed X or Y peripheral access logs
``LOG_UNIMP`` and reads zero.

The converter box
=================

The box is a separate device on ESSI0 of DSP 2. The model attaches it with
the ``box`` link property and ``sonic-ada8824``'s own properties.

.. list-table::
   :header-rows: 1
   :widths: 46 12 42

   * - Fact
     - Rating
     - Source
   * - The host talks to the box through DSP 2: it writes a command
       shifted left by 8 bits into a DSP variable, waits 200
       microseconds, reads the reply from another variable shifted right
       by 8, and takes ``0x80000000`` as an error.
     - *software expectation*
     - :src:`sonic-hdsp-library-5.0 0x5ecd6ed0`.
   * - The command word is ``address | (load << 8) | (command << 11)``,
       with a 5-bit address and a 5-bit command.
     - *software expectation*
     - :src:`sonic-hdsp-library-5.0 0x5ec67f50`.
   * - A scan sends command 7 to the board, then for addresses 1 to 31
       sends command 1 and command 2. A box that does not answer leaves
       its slot empty.
     - *software expectation*
     - :src:`sonic-hdsp-library-5.0 0x5ec67f50`.
   * - A box that answers command 1 with a value of 288 or 289 (that is,
       the value shifted right by 1 is 144) is built as an ADA8 box.
       Other values pick other box types.
     - *software expectation*
     - :src:`sonic-hdsp-library-5.0 0x5ec67f50`.
   * - The audio library fails with "no I/O box" unless at least one
       address answers command 1 with 288 or 289.
     - *software expectation*
     - :src:`sonic-hdsp-library-5.0 0x5eb46010`.
   * - The host's command 29 stops DSP 2's DMA channels, installs a fast
       receive handler on the ESSI0 vector, sets ESSI0 control A to
       ``0x100000``, control B to ``0x032130`` and port control to
       ``0x3f``, waits up to 2000 loop iterations twice for the
       receive-full flag, writes the command word to the ESSI0 transmit
       register and stores the received word. A residual wait count of zero
       means the box did not answer.
     - *software expectation*
     - :src:`sonic-dsp-programs-5.0 the DSP 2 converter-link routine`.
   * - Control A ``0x100000`` gives 16-bit words with no prescale. Control B
       ``0x032130`` enables the receiver and transmitter 0 in network mode
       with a bit-length frame sync and an internal clock and frame sync. So
       during a command the DSP is the master: it drives the bit clock and
       frame sync, sends one 16-bit word most significant bit first and
       receives one 16-bit word, then returns to the audio configuration.
       The clock rate is the core clock over the prescaler's slowest fixed
       divide, and the core clock is *unknown*. The wire is one 16-bit
       word out and one in per command, the 24-bit register carrying
       the value left-aligned, which matches the library's shift by 8.
     - *inferred* (field decodes of the manual's tables)
     - :src:`nxp-dsp56301um-4 pp. 7-13 to 7-17`, same routine, and the
       library's shift above.
   * - The converter is the Sonic ADA8 class the library builds for a
       288 or 289 answer. That this is Lucid Technology's 8824 or a sibling
       is a reading of the vendor guides, which list the Lucid 8824 and the
       Sonic 888 among the compatible boxes.
     - *inferred*
     - :src:`sonicstudio-hd-1.8-configuration-guide PDF pp. 18-20`,
       :src:`sonicstudio-hd-1.8-installation-guide printed p. 1-5`.
   * - The box carries up to eight channels at 44.1 to 96 kHz or two at
       192 kHz. Two boxes cascade on one card for 16 channels at
       44.1 or 48 kHz. Its front panel shows the clock source (internal,
       word sync, digital sync) and lock lamps.
     - *established* for the Sonic HD3, *inferred* for the ADA8
     - :src:`sonicstudio-hd-1.8-installation-guide printed pp. 3-2,
       3-6`.
   * - The card's audio link carries the Sonic I/O bus to the box on a
       cable with a small connector on the card.
     - *established*
     - :src:`sonicstudio-hd-1.8-installation-guide printed pp. 1-13 to
       1-14`.

What the virtual box answers
----------------------------

The box device answers the 16-bit words DSP 2 sends on ESSI0:

- It latches each word and decodes address (bits 4:0), load-settings
  (bit 8) and command (bits 15:11).
- A word whose address is not the box's address is ignored and the
  box drives no reply.
- Command 1 at the box's address returns 288, an *approximation*: the
  library accepts 288 or 289, and no source says which bit 0 means.
- Command 2 returns a nonzero value, ``cmd2-reply``, defaulting to 0x0100, an
  *approximation*: the library uses the field for a version or rate test
  whose rule is *unknown*. The property exists so a run can try other
  values.
- Command 7 is accepted and answered with zero.
- Every other command is logged ``LOG_UNIMP`` and answered with zero.
- A box with no device attached drives no reply, so the scan finds no box
  and the library reports "no I/O box", as the library does with real
  hardware.

During a command the DSP is the clock master, so the model's box
samples the DSP's bit clock and frame sync for the command word and
shifts its reply out on the same clock. The reply is available in the
DSP's receive register by the end of the frame in which the DSP sent the
command, so the DSP's second receive-full wait completes. Whether a real
box replies in the same frame, one frame later or on a separate return path
is *unknown*, an *approximation*: the DSP code waits twice around the
transmit, which fits a reply in the same or the following frame. In audio
mode the bit clock is external (control B selects it), and who supplies it
is *unknown*. The model supplies it from the board's sample clock, an
*approximation*. A capture of a real exchange and of the audio-mode clock
would settle both.

The box's address is a property, ``address``, default 1. The box's own
word-sync, lock and sample-rate state is not modelled beyond the rate the
card is configured for, so the box always reports the card's rate and
reports itself locked, a model choice that follows from the box having no
clock of its own to lose.

.. _sgi-sonic-hdsp-audio:

Audio output
============

The box's output goes to a QEMU ``audiodev``. The model opens one output
stream at the board's sample rate, 48 kHz by default
(``sample-rate``, 44.1 kHz selectable), 32-bit signed samples carrying 24
significant bits. The shipped guides give 44.1, 48, 88.2, 96, 176.4 and
192 kHz as rates the Sonic converters handle
(:src:`sonicstudio-hd-1.8-configuration-guide PDF pp. 18-20`,
*established*). How the library selects the board's rate is not recorded
(its clock-source words are serial-shifted into a register file that was
not decoded), so the rate is a property fixed at realize, an
*approximation* that a trace of the clock-selection words would settle.

The audio path is ESSI0, not the inter-DSP ports. In audio mode DMA
channel 0 feeds ESSI0's transmit register from the Y buffer, so the words
the DSP transmits on ESSI0 are the box's output, and the words the
model's box shifts in are the DSP's captured input
(:ref:`sgi-sonic-hdsp-dsp-int`, *software expectation*). The model carries
the 24-bit words ESSI0 transmits to the ``audiodev``: each transmitted
word is one channel sample, assigned to channels in transmit order and
wrapping at the configured channel count. The stream carries the first two
channels by default and up to eight when the ``audio-channels`` property
is raised and the backend takes them, an *approximation*: the number of
time slots per frame and the channel order are not recorded (control A's
frame rate divider reads 0, which is one slot per frame, so the card's
eight channels may arrive on more than one frame sync), and QEMU's
backends differ in channel support. A trace of the ESSI0 transmit
sequence against the box's channel numbering would settle both.

The sample clock is virtual-time: one frame every period at the selected
rate, driving the ESSI frame syncs and the ``audiodev`` stream, so the DSP
programs' click counter and periodic interrupt come from the programs
counting frames and not from the model. The box's input side delivers
silence: the model has no capture from the host's input device, an
*approximation*.

.. _sgi-sonic-hdsp-host:

The host-side contract
======================

The shipped software expects the following of the card. It is a *software
expectation*, not a hardware fact, and it tells a model tester what a
guest does with the device.

.. _sgi-sonic-hdsp-host-driver:

Kernel driver
-------------

.. list-table::
   :header-rows: 1
   :widths: 46 12 42

   * - Fact
     - Rating
     - Source
   * - The driver matches ``1263:05dc``. At attach it reads the interrupt-pin
       byte at configuration offset ``0x3d`` and maps pins 1 to 4 to
       interrupt line masks 1, 2, 4 and 8. Any other value fails the attach.
     - *software expectation*
     - :src:`sonic-hdsp-driver-9.2.5 0xf90-0xfb0`.
   * - It maps BAR0 as PCI space 8 from offset 0 for ``0x40f8000`` bytes and
       the Bridge fails the map if the BAR is smaller than offset plus
       length, so attach fails with a BAR below 128 MiB.
     - *software expectation*
     - :src:`sonic-hdsp-driver-9.2.5 0x17c-0x290`,
       :src:`irix-pcibr-6.5.30 0x1024-0x1090`.
   * - It declares a little-endian device on a big-endian host, which sets
       Bridge Device register bits 19 and 20 (DMA swap) and leaves
       programmed I/O alone.
     - *software expectation*
     - :src:`sonic-hdsp-driver-9.2.5 0x11dc-0x1200`.
   * - It creates a bus-master DMA map of ``0x7d000`` bytes (512,000)
       whatever length the caller gives, with byte-stream and data flags
       (``0x1020``), and a zeroed 16 KiB shared table.
     - *software expectation*
     - :src:`sonic-hdsp-driver-9.2.5 0x0 0x133c-0x141c`.
   * - Memory-map file offset ``0xa2000000`` maps the BAR0 window; offset
       ``0x08000000`` maps the 16 KiB table; offset ``0x01000000`` is
       recognised and maps nothing.
     - *software expectation*
     - :src:`sonic-hdsp-driver-9.2.5 0x3b14-0x3e44`.
   * - The table holds 20 slots of 12 bytes: user virtual address, bus
       address and the first extent's length. A slot is free unless bit 31
       of its first word is set. The add-buffer request fills a slot, the
       remove request always fails with -1.
     - *software expectation*
     - :src:`sonic-hdsp-driver-9.2.5 0x25c0 0x2298 0x2f0c`.
   * - Reads and writes on the device return error 12. The interrupt
       handler takes and drops the mutex. The driver never touches a
       messaging-unit register.
     - *software expectation*
     - :src:`sonic-hdsp-driver-9.2.5 0x1b40 0x1ba4 0x3f64`.
   * - The library tests for the hardware-graph node
       ``/hw/.id/pci/126305DC`` to find the card. On the boot-time scan
       that node exists for any card whose function 1 reads that identity,
       with or without the driver. On IP35 only, hot-plug slot start-up
       does not create it for a function with no driver or a failed
       attach.
     - *software expectation*
     - :src:`irix-pcibr-6.5.30 0xb768` and the library's probe string. The
       registry code is another shipped object.

The driver's DMA addresses are Bridge bus addresses (a translated window
of 16 KiB pages from PCI ``0x40000000``), so the i960's DMA to host memory
uses them as the PCI address with no translation of its own
(:doc:`bridge`, :ref:`sgi-bridge-dma`).

.. _sgi-sonic-hdsp-host-library:

User library bring-up
---------------------

The library does all the messaging in user space. It posts standard I2O
executive messages through the inbound queue, then does a mailbox
handshake, then drives the DSPs through a pair of word rings in the card's
memory (*software expectation*,
:src:`sonic-hdsp-library-5.0 0x5ed89370 0x5ec2c300 0x5ec2c0e0`):

.. list-table::
   :header-rows: 1
   :widths: 8 50 14 28

   * - Step
     - What the library does and waits for
     - Rating
     - Source
   * - 1
     - Write ``0xffffffff`` to the outbound interrupt mask.
     - *software expectation*
     - :src:`sonic-hdsp-library-5.0 0x5ed89370`
   * - 2
     - Post the executive status request. Poll up to 26 times at 100 ms
       for the sync byte, the top byte of the last status word, to read
       ``0xff``.
     - *software expectation*
     - same
   * - 3
     - Parse an 8-word status block that includes a product-id string and
       compare a version derived from it with the version the library
       needs. An older firmware fails with library error ``0xffff7f01``.
     - *software expectation*
     - same. The string grammar is *unknown*.
   * - 4
     - Post the outbound-init request and poll up to 25 times at 100 ms
       for the low status byte to read 4 (complete). Then post the free
       outbound frames and wait one second.
     - *software expectation*
     - same
   * - 5
     - Post the hardware resource table request, then the system table
       set, each synchronous with a 5 s timeout.
     - *software expectation*
     - same
   * - 6
     - Download one device-driver module through the software download
       message (a DMA map of 4096-byte pages), posted asynchronously, then
       wait 5 s. The module is the board application.
     - *software expectation*
     - same
   * - 7
     - If the board state is not 8, post the system enable request
       (1 s). Then post the logical configuration table request for all
       classes (10 s). The library then looks for the device class
       ``0xb3`` in that table and fails with ``0xffffe8be`` if it is missing.
     - *software expectation*
     - same, and :src:`sonic-hdsp-library-5.0 0x5ecd2b60`
   * - 8
     - Handshake: write ``0x699`` to BAR0 plus ``0x10`` and poll for it to
       read 0, poll for BAR0 plus ``0x18`` to read ``0x700``, write
       ``0x601`` and wait for the same.
     - *software expectation*
     - :src:`sonic-hdsp-library-5.0 0x5ec2c300`
   * - 9
     - Read the board address the firmware returns in BAR0 plus ``0x18``,
       convert it to a window pointer, read a version word at +44 and
       a failure word per DSP from +60 on.
     - *software expectation* for the sequence, *inferred* for the
       contents
     - same

The firmware's messaging-unit handler acts on the handshake words: ``0x699``
resets the command queue and loads ``0x700`` into the outbound message
register, ``0x601`` loads the address of the queue control block, ``0x604``
is the command doorbell (the host writes it after publishing a ring
index) and ``0x606`` reads a local word and returns it (*software
expectation*, :src:`sonic-hdsp-module-1.0 0x83d0 0x8610`). The ring
commands the library sends afterwards (read and write DSP memory in words,
longs and blocks, load a program, send a host command, read the click
counter and set up the audio disk channels) are the module's own protocol
and the model implements none of them: the firmware does
(:src:`sonic-hdsp-module-1.0 0x89f0`).

DSP addressing. Every ring command's first word is the DSP index in its
upper half and an opcode in its lower half; the module rejects an index
above 4 (*software expectation*,
:src:`sonic-hdsp-module-1.0 0x89f0`). The host reads 24-bit words and
sign-extends with ``0xff000000``.

.. _sgi-sonic-hdsp-firmware:

Firmware image
==============

The device takes the card's flash image from the user and bundles none.
The image is the Macintosh product's I/O processor flash image, 512 KiB:
the model has not been run against a flash image that is not one of the
two held.

.. list-table::
   :header-rows: 1
   :widths: 26 74

   * - Property
     - Meaning
   * - ``flash-image``
     - Path of a 512 KiB flash image. Required: the device refuses to
       realize without it, so no synthetic firmware stands in. A file
       that is not exactly 512 KiB is refused.
   * - ``flash-writable``
     - Boolean, default off. With it off a write to the flash window is
       dropped and logged, so the upgrade module's reflash fails visibly.
   * - ``flash-persist``
     - Boolean, default off. With it on, programmed bytes are written back
       to the image file. With it off the file is read once and writes
       change only the in-memory copy.
   * - ``dram-size``
     - Size of the local DRAM, default 64 MiB. The boot ROM tests 16, 32
       and 64 MB.
   * - ``iop-console``
     - Character device for the debug UART's transmit side. Optional.
   * - ``nv-word``
     - The first four bytes, little-endian, of the I2C EEPROM the resident
       operating system loads SISR from. Default ``0x987603df``. Valid only
       with ``0x9876`` in the upper half.
   * - ``iop-clock``, ``dsp-clock``
     - Core clocks in Hz, defaults 66 MHz and 100 MHz.
   * - ``box``
     - Link to a ``sonic-ada8824``. Optional. Without one the scan finds
       no box.
   * - ``audiodev``
     - On ``sonic-ada8824``: the QEMU audio backend (:ref:`sgi-sonic-hdsp-audio`).

Which image the card shipped with is *unknown*. The host library needs a
version derived from the status block's product-id string of at least 110,
and the 1.0.0 image's string begins with the name of a third-party Rx
evaluation-board family, which the library reads as version 106 and
rejects (:ref:`sgi-sonic-hdsp-boot`, *software expectation*). The 1.1 image
has no such string and adds peer-to-peer data movement while dropping the
EEPROM erase and write messages
(:src:`sonic-iop-flash-1.0.0 0x3c60`, :src:`sonic-iop-flash-1.1 0x5950`).
So the expected image is 1.1, *inferred*, and it is the documented
default: the examples and tests name a 1.1 image. The model takes either;
the product-id string and the version the host reads come from the image
itself, not from the model, and a model that overrides the reported
version is not permitted. The 1.0.0 image is accepted by the device and
fails in the library, as the library's check would on hardware. Running
both against the library would settle which text each image builds.

Reset
=====

A PCI reset holds the i960 in reset until the model's reset-mode strap
releases it (:ref:`sgi-sonic-hdsp-boot`) and clears every on-chip unit to
the Rx manual's reset values (:src:`intel-i960rx-272736-002 p. 11-3`,
*established*): the model drives the strap so the processor boots at once,
an *approximation*. The DRAM, the DSPs' memories and the glue registers
clear. The four DSPs reset with the card: each starts in the held-in-reset
state the glue's reset byte gives after reset, a model choice because the
reset value of the glue's reset byte is *unknown*. A DSP's own reset on a
byte write is described under the glue logic. The flash keeps its contents.

Migration
=========

The device holds its processor and unit state in one device and migrates
it as a unit. The i960 and DSP56300 cores have their own migration
descriptions, embedded in the device's (``VMSTATE_I960`` and
``VMSTATE_DSP56300``), and the on-chip units, glue, EEPROM and flash
contents are in the stream. The flash backing file is not: a destination
must give the same ``flash-image``. That the virtual clock slice timer
restarts at the same guest time is the model's own design.

.. _sgi-sonic-hdsp-limits:

Model limits
============

- Only the HDSP exists. The earlier Discreet Stream boards (the USP and the
  SSP, a different PCI device and a VME board) are different hardware
  and are not modelled; the HDSP's PCI identity is the only one the
  device presents.
- The video modules the card's library and installers mention (a video
  decoder, an MPEG-2 encoder and a video capture module) are not
  modelled. The host's private message that would use them is not sent on
  the audio path (*software expectation*,
  :src:`sonic-hdsp-library-5.0 0x5ed9a180`).
- The serial connector on the card for timecode and machine control is not
  modelled: no character device is attached to the DSP's serial interface
  (:src:`sonicstudio-hd-1.8-installation-guide printed pp. 1-13 to 1-14`,
  *established* for the connector).
- Only one box and one card are modelled: the second box that cascades for
  16 channels, the not-first-board mode for several cards and the three-card
  24-channel configuration are not (:src:`sonicstudio-hd-1.8-installation-guide
  printed p. 3-2`, *established*).
- Word sync, digital sync and the box's lock lamps have no input, so no
  loss-of-sync fallback is modelled.
- DSP56301 behaviour beyond what the monitors and the module exercise is
  not modelled (:ref:`sgi-sonic-hdsp-dsp-limits`).
- The flash reprogramming path is modelled at the part's command
  interface; the upgrade module's other effects are the firmware's.
- DMA to the secondary bus (channel 2, and 64-bit transfers) is not
  modelled: it is logged and refused.
- If the firmware narrows the inbound limit, the mapped region stays 128 MiB,
  and the size a host reads back when sizing BAR0 follows the inbound limit
  register while it is narrowed (32 MiB at 0xfe000000, for example). The
  narrowing is logged. The firmware restores 0xf8000000 shortly after it
  clears the configuration Retry, so a host sizing BAR0 between the two
  would read 32 MiB. IRIX sizes the BAR during its boot-time slot scan;
  that it never sizes it again is *unverified*.
- The DSP cores are instantiated and their host ports answer, but nothing
  runs DSP code, so the bootstrap, the serial ports, the converter box and
  the audio output described above are absent from the model.

.. _sgi-sonic-hdsp-approximations:

Approximations and unknowns
===========================

Every model choice above that stands in for hardware nobody has documented,
with what would settle it:

.. list-table::
   :header-rows: 1
   :widths: 34 38 28

   * - Choice
     - Model
     - What would settle it
   * - BAR0 type and size
     - One 128 MiB memory BAR, 32-bit, prefetchable, from the ATU limit
       register
     - A configuration dump of a real board
   * - Bank 1 beyond the library's offsets
     - Reads zero and logs
     - A trace of the library's accesses, or a board schematic
   * - Glue logic and the i960's bank 1
     - One glue register file, seen at the glue base in BAR0 + offset and at
       the bank 1 base + offset, byte position 3 of each word
     - A trace of host and firmware accesses together
   * - Interrupt sources reaching the pin
     - Messaging unit outbound sources, plus the request byte with both
       enables set
     - An enabled run's trace
   * - Interrupt request byte bits
     - Four DSP lines and a spare bit
     - A DSP-line trace
   * - Reset byte behaviour
     - Bit *n* resets DSP *n* only
     - The glue's real reset and its duration
   * - Configuration retry strap
     - Set from reset until the operating system clears it
     - The board's strap wiring
   * - PCI Retry
     - Configuration reads return all ones and writes are dropped while the
       bit is set, an empty slot to the host; ``config-retries`` counts them
     - Retry support in QEMU's PCI core
   * - PCI function layout
     - Functions 0 and 1 of one device
     - A real card's configuration dump
   * - The word the operating system loads into SISR
     - ``nv-word`` property, default ``0x987603df``, so SISR = ``0x03df``
       and the firmware hides all four DSPs; where the word lives is
       unknown
     - A real board's EEPROM dump
   * - DSP reset lines after a board reset
     - Held in reset until the reset byte releases them
     - The glue's reset default
   * - Local bus behaviour for unclaimed addresses
     - Reads all ones, no fault, may set LPISR bit 5; writes dropped; each
       logged
     - A real board's data bus, or the boot ROM's own accesses
   * - ``0xf0000000`` and ``0xe0040000``
     - Tables read as zero; the board-identification byte is a constant with
       upper nibble 1
     - A board schematic, or a real card's boot trace
   * - Debug UART block at ``0xd0000000``
     - Transmit ready always set, transmit forwarded, receive never ready
     - The schematic, or a longer operating system trace
   * - NV word and the I2C EEPROM
     - A 1 KiB 24C08-class EEPROM at ``0x50``; first four bytes are
       ``nv-word``, default ``0x987603df``; the rest ``0xff``
     - A real board's EEPROM dump
   * - DSP subsystem ID
     - CSID left at 0
     - The board-side bootstrap program that writes it
   * - Boot-mode byte at glue ``0x18003``
     - Bit 7 set, so the boot ROM starts the operating system
     - The board's switch or jumper
   * - DRAM size
     - 64 MiB
     - A board photograph
   * - Flash part
     - AT29C040A programming model
     - A board photograph
   * - Clocks
     - 66 MHz i960, 100 MHz DSPs
     - A board photograph, or PLL control read from a running board
   * - Secondary bus
     - A real QEMU PCI bus behind the bridge unit, not visible to the host;
       the DSP host ports are PCI functions on it
     - Nothing the host can see depends on it
   * - DSP bootstrap mode
     - Mode C, the PCI target host bootstrap, with the ROM's listed protocol.
       The manual has a boot program (from an EPROM or the SCI) write CSID
       in self-configuration mode before PCI mode, and mode C has no such
       step, so the board may not use mode C
       (:src:`nxp-dsp56301um-4 pp. 6-16, 6-69`)
     - The board's mode strap
   * - DSP bootstrap ROM size
     - 3 K words
     - The DSP revision on the card
   * - DSP external memory
     - 128K words of P and Y at ``0x400000``, a private copy of the
       32768 words at Y:0x400000 for each DSP
     - A board photograph, and a cross-DSP trace
   * - Sync flags and the Y:0xfffffb port
     - Nine shared words at Y:0xfffff7; Y:0xfffffb returns the last value
       written
     - A trace of a running monitor image
   * - Interrupt-out line
     - Timer 0's data output drives the DSP's line
     - A DSP-line trace against the i960's interrupt controller
   * - Box reply timing and clocks
     - Reply in the same frame; the card supplies the audio-mode bit clock
     - A capture of a real exchange and of the audio-mode clock
   * - Box answers
     - Command 1 returns 288, command 2 returns ``cmd2-reply``, command 7
       returns zero
     - Running the library against the box and reading its checks
   * - Audio channels and input
     - ESSI0 transmit words in order, wrapping at the channel count; two
       channels by default; silence in
     - The ESSI0 transmit sequence against the box's channel numbering
   * - Unit behaviours the manual leaves open
     - The choices in :ref:`sgi-sonic-hdsp-unit-model`
     - The routes listed there
   * - Flash backing file in migration
     - Not in the stream; the destination names the same image
     - Nothing on the card's side

Tests
=====

The register-contract tests start the device with a flash image they build
themselves from an i960 boot record and a few instructions, so the tests
need no vendor firmware and the page stays the only source of the
expected values. They check that:

- a configuration read of the card returns all ones, and ``config-retries``
  counts it, until the test image clears the bridge unit's retry bit, then
  reads back the identity the test image programmed;
- BAR0 sizes from the ATU limit register and decodes the 128 MiB
  window;
- function 0 reads header type ``0x81``; a host type 1 scan of devices 0
  to 31 behind function 0 after the operating system's PCI setup, with the
  default ``nv-word``, finds no devices, and with ``nv-word`` set to
  ``0x98760000`` finds the DSPs the test image has released from reset;
- the messaging unit answers at BAR0 plus ``0`` to ``0xfff``; a read of
  the inbound queue port at BAR0 plus ``0x40`` returns the free frame
  address and a write at BAR0 plus that address reaches local memory; a
  write of a message word to the inbound message register reaches the
  test image's interrupt handler and a reply word written by it reads
  back from the outbound message register;
- the inbound window follows the Rx's translate rule: with limit
  ``0xf8000000`` and translate ``0xa0000000`` BAR0 plus ``0x6000000``
  reaches bank 1, and with translate ``0xa2000000`` BAR0 plus
  ``0x4000000`` does and BAR0 plus ``0x2000000`` aliases BAR0 plus
  ``0x0``'s DRAM; the translate follows ``dram-size`` once the test image
  writes the I2O class;
- the ATU's maximum-grant byte at configuration ``0x3e`` reads 0;
- the inbound and outbound queue ports return ``0xffffffff`` when empty
  and advance the pointers as the Rx manual describes;
- the glue control bytes read back what is written at byte position 3
  of the word, the i960 sees the same byte at the bank 1 base plus
  ``0xa0003``, and reset byte writes hold DSP *n* in reset;
- a DSP's host port accepts a word count, a start address and program
  words and starts the program at that address, and an HCVR write of
  ``0xf5`` runs the handler at vector ``0xf4``;
- the debug UART's transmit data reaches the ``iop-console`` character
  device and its status register reads transmit ready;
- an access outside every unit logs and returns all ones;
- an unclaimed local read returns all ones without a fault and may set
  bit 5 of LPISR;
- the boot-mode byte at glue ``0x18003`` has bit 7 set and writes to glue
  ``0x3`` are logged;
- the I2C unit reads the first four bytes of the EEPROM at ``0x50`` as
  ``nv-word`` little-endian, acknowledges the write-back of a blank part and
  not-acknowledges any other address;
- a BAR0 of 128 MiB is placed and reached through the Bridge's 32-bit
  memory PIO window (the cage and the adapter both); see
  :ref:`sgi-sonic-hdsp-large-bar`;
- ``sonic-ada8824`` answers command 1 with 288 at its address and does not
  answer at any other address, and a device with no box attached drives no
  reply;
- a missing or wrongly sized ``flash-image`` is refused at realize.

The tests that run the real firmware and the host library need vendor
files. They skip when the files are absent.
