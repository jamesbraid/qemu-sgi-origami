.. SPDX-License-Identifier: BSD-3-Clause

.. _sgi-lucent-ohci:

===============================================
AT&T/Lucent 11c1:5802: USB OHCI host controller
===============================================

Used in :ref:`sgi-ip35`, on the Origin 300 IO-8 board and the Fuel
system board, and in the Origin 3000 I-brick (:ref:`sgi-bricks-ibrick`),
whose published inventories show the same identity, *observed*. QEMU
builds the I-brick's with the ``io7`` base I/O preset on the Origin 3000
machines whose catalogue entries carry an I-brick (:doc:`baseio`).

.. seealso::

   Platform pages: :ref:`sgi-ip35`.

The part is a two-port USB host controller that enumerates as PCI
``11c1:5802``, revision ``0x10``. QEMU models it as the ``lucent-ohci`` type
(``hw/usb/lucent-ohci.c``), a subtype of the generic ``pci-ohci`` wrapper.
The transfer engine, the register file and most reset values are QEMU's own
OHCI core (``hw/usb/hcd-ohci.c``). This page covers only what the subtype
changes: the PCI identity, the two ports, a few reset words and the
root-hub power switching. The Origin 300, Fuel and I-brick base I/O models create one
at PCI device 5 (``hw/mips/sgi/baseio.c``). Facts are rated as described in
:ref:`sgi-ratings`.

.. _sgi-lucent-ohci-sources:

Sources
=======

- *OpenHCI: Open Host Controller Interface Specification for USB*, release
  1.0a, :src:`openhci-1.0a`. It defines the register file. Section numbers
  below refer to it.
- *USS-312 Two-Port PCI-to-USB OpenHCI Host Controller*, Lucent advance data
  sheet, revision 5, March 2000, :src:`lucent-uss312-ds`. It gives the
  part's PCI identity, root-hub reset values and register access types. Its
  PCI configuration summary names revision ID ``0x10`` as Revision C, the
  value both Origin 300 captures report.
- *Origin 300 User's Guide*, :src:`007-4365-002`, and *sMDK-based Field
  Diagnostics for SGI Origin 300 System and SGI 3000 Family*,
  :src:`108-0264-004`. The field diagnostics select USB as PCI device 5.
- Two published Origin 300 OpenBSD boot captures,
  :src:`marc-openbsd-sgi-125897882029839` and
  :src:`marc-openbsd-sgi-139223952727384`, a published Fuel IRIX console
  capture, :src:`tingo-fuel-irix-console`, a published Fuel OpenBSD boot,
  :src:`miod-openbsd-sgi-story`, and a published ``lspci`` listing of
  another system carrying an ``11c1:5802`` revision ``0x10`` function,
  :src:`lspci-saic-11c1-5802`.
- OpenBSD ``sys/dev/pci/pcidevs``, ``sys/dev/pci/ohci_pci.c`` and
  ``sys/dev/usb/ohci.c`` at ``2df91d80c104``, as software expectations.
- The *PCI Local Bus Specification*, revision 2.2, :src:`pci-local-bus-2.2`,
  for the Status word.

.. _sgi-lucent-ohci-identity:

Identity
========

.. list-table::
   :header-rows: 1
   :widths: 20 24 56

   * - Field
     - Model value
     - Basis
   * - Vendor and device
     - ``0x11c1:0x5802``
     - *established*. The USS-312 data sheet gives ``11C1h`` and ``5802h``
       :src:`lucent-uss312-ds Tables 3 and 8`. Both Origin 300 captures
       report the same, and OpenBSD ``pcidevs`` names it "AT&T/Lucent USB
       2-port".
   * - PCI revision
     - ``0x10``
     - *established* by the data sheet's Revision C row, and *observed*
       in both Origin 300 captures and a Fuel IRIX capture.
   * - Class
     - serial bus, USB, OHCI interface ``0x10``
     - *software expectation*: OpenHCI-matching drivers such as OpenBSD
       ``ohci_pci.c`` require it. The class comes from the generic
       ``pci-ohci`` wrapper.
   * - PCI function
     - device 5, function 0
     - *observed* on the IO-8 bus in both Origin 300 captures and on the
       Fuel (:src:`tingo-fuel-irix-console`). The field diagnostics also
       select USB as device 5 :src:`108-0264-004 printed pages 2-10 to 2-14`.
       The base I/O models place it there.
   * - Root-hub ports
     - 2
     - *established*. The product identity, both captures and the user's
       guide call it two-port :src:`007-4365-002 printed pages 9, 12 and 14`.
       The type sets the inherited ``num-ports`` property to 2.
   * - OHCI revision register
     - ``0x00000010`` (OHCI 1.0, Legacy bit clear)
     - Both captures report OHCI 1.0 with legacy support indicated,
       *observed*. The model returns a bare ``0x10`` from the core, so the
       Legacy bit differs from the captures (see Model limits). This is
       distinct from the PCI revision.
   * - Subsystem vendor and device
     - ``0xffff:0xffff``
     - *unknown* on SGI's boards. The data sheet gives ``0x11c1`` and
       ``0x5802`` as the chip's reset values, read-only unless firmware
       opens them :src:`lucent-uss312-ds Tables 3, 17 and 18`, so firmware
       may have rewritten them, and no published configuration dump shows
       the board's values. The model uses ``0xffff`` as a placeholder
       (*approximation*) because leaving both zero would make QEMU insert
       its own unrelated ``0x1af4:0x1100``. A raw configuration capture
       from an IO-8 or a Fuel would settle it.
   * - PCI Status after reset
     - ``0x0200``
     - *approximation*. Medium DEVSEL only, set after the wrapper realizes.
       The data sheet gives ``0x0200`` with PMIENABLE low and ``0x0210``
       (capability list present) with it high
       :src:`lucent-uss312-ds Table 3`. Whether SGI's board ties
       PMIENABLE high is *unknown*. The published ``lspci`` listing shows
       medium DEVSEL and no capability line, which fits low, but it has no
       raw bytes and was taken after initialisation on another system
       (:src:`lspci-saic-11c1-5802`). DEVSEL encoding:
       :src:`pci-local-bus-2.2 printed page 197`.

The marketing part name beyond the PCI product name is *unknown*.

.. _sgi-lucent-ohci-placement:

Placement and interrupts
========================

.. list-table::
   :header-rows: 1
   :widths: 50 14 36

   * - Fact
     - Rating
     - Source
   * - On the Origin 300 IO-8 the controller is ``00:05.0`` on :term:`XBridge`
       head F (widget ``0xf``), beside the ISP12160 at ``00:01.0`` and the
       :term:`IOC3` at ``00:04.0``. The bus runs at 33 MHz.
     - *observed*
     - :src:`marc-openbsd-sgi-139223952727384 lines 150-183`;
       :src:`007-4365-002 printed page 12`
   * - The controller asserts PCI INTA, wired to XBridge interrupt pin 5.
     - *observed*
     - both Origin 300 captures
   * - Head E (widget ``0xe``) carries the two option slots, empty on the
       base machine, at 66 MHz. A separate capture associates PCI device 1
       with XBridge interrupt number 1 and device 2 with number 2. The
       riser's source-to-pin swizzle is not known, so the model does not
       wire option-card interrupts.
     - *observed* / *unknown*
     - :src:`marc-openbsd-sgi-125897882029839 lines 55-90` (neither card's
       interrupt-pin field is reported)
   * - On the Fuel the controller is at PCI device 5 behind the :term:`Bedrock`'s
       XBridge, with two USB ports. The model wires its INTA to pin 5, the
       index OpenBSD registered its handler on.
     - *observed* (device, ports); *inferred* (pin)
     - :src:`tingo-fuel-irix-console lines 120-152`;
       :src:`007-4480-001 printed page 130`;
       :src:`miod-openbsd-sgi-story`; :ref:`sgi-sn-fuel-interrupts`

INTA is level-sensitive through the ordinary PCI path. Fixed wiring ends at
the XBridge pin. The XBridge status and enable registers then send a
crosstalk write to the target and pending-bit address the guest programmed.
The Bedrock interrupt number 59 in :src:`marc-openbsd-sgi-139223952727384` is OpenBSD's allocation
on that boot, not wiring, and the model assumes no fixed pending bit.

DMA goes through the endpoint's PCI bus address space, so XBridge direct-map
and translation entries stay in the path. This is an integration requirement
of the model: the controller reaches node memory only through the bridge.

.. _sgi-lucent-ohci-registers:

Register map
============

The register file is the OpenHCI 1.0a operational block at BAR 0 of the
generic core, with its transfer behaviour. Only the registers below differ
from the core. Every other register, reset value and access width is the
core's, which is QEMU's reading of OpenHCI and not a fact about this part.
The model overlays HcControl, HcCommandStatus, HcFmNumber, the root-hub
descriptors, HcRhStatus and HcRhPortStatus. Each overlay passes the access
to the core, after dropping power commands the switching mode does not
allow. The HcInterruptEnable and HcFmInterval values are set by the reset
hook.

.. list-table::
   :header-rows: 1
   :widths: 9 18 15 58

   * - Offset
     - Name
     - Reset
     - Subtype behaviour
   * - ``0x04``
     - HcControl
     - core
     - A write that takes HostControllerFunctionalState to USBRESET also
       returns the root hub to its power-on state (see Root-hub power), and
       HcRhDescriptorB keeps its value across it.
   * - ``0x08``
     - HcCommandStatus
     - core
     - A write with HostControllerReset set re-applies the two reset words
       below.
   * - ``0x10``
     - HcInterruptEnable
     - ``0``
     - Reads zero after reset, see below.
   * - ``0x34``
     - HcFmInterval
     - ``0x00002edf``
     - FSLargestDataPacket (bits 30:16) resets to zero.
   * - ``0x3c``
     - HcFmNumber
     - ``0``
     - A four-byte write is kept while the controller is not operational.
   * - ``0x48``
     - HcRhDescriptorA
     - ``0x10000902``
     - Bits ``0xff001b00`` are read/write. Power-on value below.
   * - ``0x4c``
     - HcRhDescriptorB
     - ``0x00060000``
     - DeviceRemovable and PortPowerControlMask are read/write for ports 1
       and 2 (bits 1-2 of each half).
   * - ``0x50``
     - HcRhStatus
     - core
     - SetGlobalPower and ClearGlobalPower reach only the ports the
       power-switching mode lets them reach.
   * - ``0x54``, ``0x58``
     - HcRhPortStatus 1, 2
     - ``0``
     - With NoPowerSwitching clear, PortPowerStatus reads the port's own
       bit. SetPortPower and ClearPortPower reach only the ports the
       power-switching mode lets them reach.

Interrupt enable after reset
----------------------------

OpenHCI gives every HcInterruptEnable field a reset value of ``0b``,
MasterInterruptEnable included
:src:`openhci-1.0a section 7.1.5, Figure 7-5, printed page 115`. The
HcInterruptDisable table, which reads the same register, repeats ``0b``
:src:`openhci-1.0a section 7.1.6, printed page 116`. One sentence of that
table says the master bit "is set after a hardware or software reset". The
specification is not self-consistent here. The core resets the register to
``0x80000000``. The subtype reads zero after the device's reset and after a
guest's HostControllerReset. The IO-8 board diagnostic writes
``0xc000007f``, resets the controller through HcCommandStatus and requires
zero when it reads the register back, *software expectation*. The zero is
*inferred*: two reset tables agree on it, the prose conflicts, and no read of
the real chip is available. A read-back from a real IO-8 after reset would
settle it.

FSLargestDataPacket
-------------------

OpenHCI leaves the reset value of FSLargestDataPacket to the part
(``TBD``, :src:`openhci-1.0a section 7.3.1, printed pages 120-121`). The
core resets it to ``0x2778``, the value Linux programs. The subtype resets it
to zero, so HcFmInterval reads ``0x00002edf`` after reset. The IO-8 board
diagnostic requires that word, a *software expectation*. The value for the
part is *inferred* from that alone.

Frame number
------------

OpenHCI makes FrameNumber read-only to the driver and written only by the
controller :src:`openhci-1.0a section 7.3.3, printed page 122`, and the
USS-312 data sheet repeats the access type
:src:`lucent-uss312-ds Table 42`. Neither says what a driver write does. The
IO-8 and IO-7 board diagnostics write HcFmNumber while the controller is not
operational and require the value back, *software expectation*. The subtype
latches the low 16 bits of a four-byte write whenever
HostControllerFunctionalState is not USBOPERATIONAL, *inferred* from the
diagnostics. A write while operational is discarded and logs ``LOG_UNIMP``.
What the part does then is *unknown*. A controller reset clears the field.
Writing ``0xffff`` with the controller not operational on a real IO-8 and
reading it back would settle both.

Real silicon advances HcFmNumber from its own clock. The core applies any
overdue frame boundary synchronously at the start of every register read, so
a read reflects elapsed virtual time instead of waiting for the timer. This
is an emulator timing choice for all OHCI types. The Origin 300 :term:`PROM` sets the
controller operational, waits about 1 ms and reads HcFmNumber, a *software
expectation* that it has advanced. The catch-up runs only on register reads.
A guest that polls the communication area by DMA without reading a register
depends on the core's timer alone.

Root-hub power
--------------

Power-on values, *established*: HcRhDescriptorA is ``0x10000902``
(NumberDownstreamPorts 2, PowerSwitchingMode 1, NoPowerSwitching 0,
OverCurrentProtectionMode 1, NoOverCurrentProtection 0,
PowerOnToPowerGoodTime ``0x10``), HcRhDescriptorB is ``0x00060000``
(DeviceRemovable 0, PortPowerControlMask ``0006h``), and every
HcRhPortStatus field resets to zero, PortPowerStatus included
:src:`lucent-uss312-ds Tables 45, 46 and 48`. Table 46 prints the mask's bit
range as 17:16, which cannot hold ``0006h``. The model puts it where OpenHCI
puts it, port *n* at bit 16 + *n* :src:`openhci-1.0a section 7.4.2`, so the
register reads ``0x00060000``. That placement is *inferred*. With a
``num-ports`` other than two, ports past the second are unmasked. The part
therefore powers on switching each port by its own commands, with both ports
unpowered. The core would instead power on with NoPowerSwitching set and
every port reading as powered.

The switching rules are OpenHCI's, *established*
:src:`openhci-1.0a sections 7.4.1 to 7.4.4`. With NoPowerSwitching set the
ports are always powered. With it clear and PowerSwitchingMode clear, only
SetGlobalPower and ClearGlobalPower, written through HcRhStatus, switch the
ports, all together. With PowerSwitchingMode set, a port whose
PortPowerControlMask bit is set answers only SetPortPower and ClearPortPower,
written through its own HcRhPortStatus, and a port whose bit is clear answers
only the global commands. A command the mode does not let reach a port is
dropped before the core sees it. The core otherwise acts on both kinds for
every port. That no power command reaches a port while NoPowerSwitching is
set is the model's reading, an *approximation*: the specification does not
say. What a port holds when switching is selected again is not specified.

A power-off clears PortPowerStatus, CurrentConnectStatus, PortSuspendStatus,
PortResetStatus and PortEnableStatus, and sets the change bits the core
defines. A power-on reconnects an attached device and sets
ConnectStatusChange. A device attached while its port is unpowered shows
as connected when the port is next powered.

Entering USBRESET returns the root hub's port registers to zero and
HcRhDescriptorA to the power-on value above, where the core would use its
own. HcRhDescriptorB keeps its value. OpenHCI leaves the root-hub registers
out of what USBRESET preserves and says the root hub "is being reset" there
:src:`openhci-1.0a section 5.1.2.1`, but gives the descriptor registers a
"Power-On Reset" reset column and says they are written during
initialisation (sections 7.4.1 and 7.4.2). The IO-8 board diagnostic writes
``0x3ffff`` to HcRhDescriptorB, commands USBRESET, resets the controller and
requires ``0x00020006`` back, a *software expectation*. Keeping the register
is *inferred* from that, and the specification's prose leans against it.
Returning HcRhDescriptorA to its power-on value on USBRESET is the core's
choice applied with the part's value, and is *unknown* on the part. A
HostControllerReset leaves both descriptors alone
:src:`openhci-1.0a section 7.1.3`.

The IO-8 board diagnostic's power check does the following, a *software
expectation*. It commands USBRESET, resets the controller and sets
USBOPERATIONAL, then requires both HcRhPortStatus registers to read zero. It
writes HcRhDescriptorB ``0x00060000`` and HcRhDescriptorA ``0x00000100``,
switches each port off and on through its own commands and requires the
other port to stay unpowered.

Transfers
=========

The core's packet and toggle handling is generic OHCI behaviour that any
``pci-ohci`` shares.

- An IN transaction is bounded by the endpoint's MaximumPacketSize and the
  buffer that remains. CurrentBufferPointer advances and DataToggle changes
  after each successful packet. A short final packet completes the
  descriptor only when BufferRounding is set
  :src:`openhci-1.0a printed pages 17, 20-22 and 102-103`. Without rounding
  the underrun error path applies.
- A payload read or write that fails through the PCI address space is fatal.
  The core sets UnrecoverableError, stops endpoint, list and frame
  processing and dispatches no further packet. An OUT or SETUP packet whose
  payload read failed is never sent. OpenHCI says that after this error the
  controller performs no further processing until the driver clears it after
  a reset :src:`openhci-1.0a printed pages 25 and 114`. These steps follow
  from those rules, *inferred*. Bytes of a transaction that partly completed
  are not rolled back.
- The subtype also sets Detected Parity Error in PCI Status on that stop and
  leaves the other Status bits alone. This reuses the core's error
  classification as a model policy, an *approximation*. It is not known how
  the part reports such an error.

Reset
=====

A PCI reset or a software reset of the device runs the core's reset and then
applies the subtype's words: HcInterruptEnable zero, FSLargestDataPacket
zero, and the root-hub power-on state. A guest's HostControllerReset
re-applies the first two and leaves the root hub alone.

The core still decodes the PXA27x HcHReset register at offset ``0x64``,
which the USS-312 does not have. A write of its reset bit takes the core's
hard reset with all its own values, including HcInterruptEnable
``0x80000000``, FSLargestDataPacket ``0x2778`` and NoPowerSwitching,
bypassing the subtype's.

Migration works only between identical builds. The core's state migrates
with the device, and the PCI core restores the INTx level.

Measurements
============

No read of this controller was taken on a physical machine for these pages.
The *observed* facts above come from the published captures cited in
:ref:`sgi-lucent-ohci-sources`.

.. _sgi-lucent-ohci-limits:

Model limits
============

- Over-current reporting is not modelled. OverCurrentProtectionMode and
  NoOverCurrentProtection are stored and have no effect.
- PowerOnToPowerGoodTime is stored and nothing waits on it.
- The USS-312's own configuration registers, power management and legacy
  registers and behaviour are *unknown* and not modelled. The HcRevision
  Legacy bit is clear, unlike the captures.
- Min_Gnt ``0x03`` and Max_Lat ``0x56`` are in the data sheet
  :src:`lucent-uss312-ds Table 3`. The model leaves both zero. The core's
  HcLSThreshold reset, ``0x628``, matches the data sheet's
  :src:`lucent-uss312-ds Table 44`.
- The core's reset values other than those listed are QEMU's, not hardware
  facts.
- A write to HcFmNumber while the controller is operational logs
  ``LOG_UNIMP`` and is dropped.
- The HcFmNumber latch and the HcRhDescriptor writes act only on full
  four-byte writes. Other widths reach the core unchanged.
- The model returns ``0xffffffff`` for unaligned reads, as the core does.

Tests
=====

``tests/qtest/usb-lucent-ohci-test.c`` covers the identity and parent
identity, the PCI Command and Status words, DMA and INTA reuse, both reset
paths, the interrupt-enable and FSLargestDataPacket resets, the writable
root-hub descriptors and their survival across USBRESET, the power-on
values, per-port and global power switching, power-off and reconnection of
attached devices, HcFmNumber tracking the clock and the latch with its
operational exception, and migration. It also drives a ``usb-wacom-tablet``
through the controller. It runs against an Origin 200 :term:`Bridge` host.

``tests/qtest/sn1-fuel-usb-test.c`` runs USB transfers through the Fuel's
controller and XBridge DMA. ``tests/qtest/sn1-fuel-test.c`` covers the Fuel
interrupt (``fuel/ohci-interrupt``) and HcFmRemaining
(``fuel/ohci-frame-remaining``). ``tests/qtest/usb-ohci-transfer-test.c``
checks the shared packet and toggle handling on a plain ``pci-ohci``.
