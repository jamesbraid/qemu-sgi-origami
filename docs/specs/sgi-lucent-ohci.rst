.. SPDX-License-Identifier: BSD-3-Clause

.. _sgi-lucent-ohci:

===========================================================
AT&T/Lucent 11c1:5802 two-port OHCI controller (IO-8, Fuel)
===========================================================

The Origin 300 IO-8 board and the Fuel system board carry a two-port USB
host controller that enumerates as PCI ``11c1:5802``. QEMU models it as
the ``lucent-ohci`` type (``hw/usb/lucent-ohci.c``), a subtype of the
existing ``pci-ohci`` wrapper, so the transfer engine is QEMU's own OHCI
core and only the identity, ports and a few reset words are this
project's. Ratings follow :ref:`sgi-ip30`.

Sources: *OpenHCI: Open Host Controller Interface Specification for USB*,
release 1.0a (``web:openhci-1.0a``); *USS-312 Two-Port PCI-to-USB OpenHCI
Host Controller*, Lucent advance data sheet rev. 5, March 2000
(``web:lucent-uss312-ds``); *Origin 300 User's Guide* (007-4365-002);
*sMDK-based Field Diagnostics* (108-0264-004); two public Origin 300
OpenBSD captures (``web:marc-openbsd-sgi-125897882029839`` lines 55-90 and
``web:marc-openbsd-sgi-139223952727384`` lines 150-183); a Fuel IRIX
console capture (``web:tingo-fuel-irix-console`` lines 120-152); OpenBSD sgi
at ``openbsd-sgi:2df91d80c104636deeef8124252ddd5951b55fbf``.

.. _sgi-lucent-ohci-identity:

PCI identity
============

.. list-table::
   :header-rows: 1
   :widths: 22 18 60

   * - Field
     - Rating
     - Value and source
   * - vendor and device
     - established
     - ``0x11c1:0x5802``. The USS-312 data sheet's PCI configuration
       summary (Tables 3 and 8) gives vendor ``11C1h``, device ``5802h``
       and revision ID ``10h`` for Revision C; both Origin 300 captures
       report the same; OpenBSD ``sys/dev/pci/pcidevs`` lines 191 and
       7819-7851 name it "AT&T/Lucent USB 2-port".
   * - PCI revision
     - established
     - ``0x10``, observed in both Origin 300 captures and the Fuel console
       capture.
   * - PCI function
     - established
     - Device 5, function 0 on the Origin 300 IO-8 bus (both captures; the
       field diagnostics select USB as PCI device 5, 108-0264-004 printed
       pages 2-10 to 2-14) and on the Fuel (``web:tingo-fuel-irix-console``).
       The Fuel's widget 15 placement follows a separate Fuel OpenBSD
       capture whose board revision is unknown.
   * - class and interface
     - established
     - Serial bus, USB, OHCI interface ``0x10`` (OpenBSD ``ohci_pci.c``
       lines 74-160 match requires it).
   * - root-hub ports
     - established
     - Two: the product identity, both captures and the user's guide call
       it two-port (007-4365-002 printed pages 9, 12 and 14).
   * - OHCI revision register
     - established
     - 1.0 with legacy-support indication, observed in both captures;
       distinct from the PCI revision.
   * - subsystem vendor and device
     - unknown
     - No configuration-space dump or published source reports either
       field. The model presents ``0xffff:0xffff`` (emulator choice)
       because leaving them zero would make QEMU inject its unrelated
       ``0x1af4:0x1100`` defaults. A raw configuration capture from an IO-8
       or Fuel would settle it. The marketing part name beyond the PCI
       product name is unknown.
   * - PCI Status after reset
     - approximation
     - The model uses ``0x0200``, DEVSEL medium only, no capability list.
       The public comparison capture of a ``11c1:5802`` revision ``0x10``
       reports medium DEVSEL and no capability line; the capture has no raw
       bytes and was taken after initialisation on another system, so the
       exact reset word is unknown. DEVSEL timing encoding: PCI Local Bus
       Specification 2.2.

.. _sgi-lucent-ohci-placement:

Placement and interrupts
========================

.. list-table::
   :header-rows: 1
   :widths: 46 12 42

   * - Fact
     - Rating
     - Source
   * - On the Origin 300 IO-8 the controller is ``00:05.0`` on XBridge
       head F (widget ``0xf``), beside the ISP12160 at ``00:01.0`` and the
       IOC3 at ``00:04.0``; the bus runs at 33 MHz.
     - established
     - ``web:marc-openbsd-sgi-139223952727384`` lines 150-183;
       007-4365-002 printed page 12 (the IO-8 assembly).
   * - The controller asserts PCI INTA, wired to XBridge interrupt pin 5.
     - established
     - Both captures report XBridge interrupt pin 5.
   * - Head E (widget ``0xe``) carries the two option slots, empty on the
       base machine at 66 MHz. A separate capture associates PCI device 1
       with XBridge interrupt number 1 and device 2 with number 2. The
       riser's source-to-pin swizzle is not established, so the model does
       not synthesise option-card interrupt wiring.
     - established / unknown
     - ``web:marc-openbsd-sgi-125897882029839`` lines 55-90 (neither card's
       interrupt-pin field is reported).
   * - On the Fuel the controller is at PCI device 5 behind the Bedrock's
       XBridge, with two USB ports.
     - established
     - ``web:tingo-fuel-irix-console`` lines 120-152; 007-4480-001 printed
       page 130 (two USB ports).

.. _sgi-lucent-ohci-registers:

Register behaviour
==================

The OHCI register file and transfer behaviour follow the OpenHCI 1.0a
specification and QEMU's existing OHCI core (``hw/usb/hcd-ohci.c``);
DMA goes through the endpoint's PCI bus address space so the XBridge
direct-map and translation tables stay in the path (an integration
requirement of the model, not a hardware fact: the PCI device reaches node
memory only through the bridge). The model adds three things to the core:

- *Interrupt enable after reset.* OpenHCI gives every HcInterruptEnable
  field a reset value of ``0b``, MasterInterruptEnable included
  (section 7.1.5, Figure 7-5, printed page 115; HcInterruptDisable repeats
  ``0b`` on page 116), while one sentence of the HcInterruptDisable table
  says the master bit "is set after a hardware or software reset"
  (page 116) and section 7.1.3 (page 112) says HostControllerReset resets
  most operational registers. The specification is not self-consistent
  here. QEMU's core resets the register to ``0x80000000``; the model
  reads zero after the device's reset and after a guest's
  HostControllerReset, through an overlay on HcCommandStatus in the
  ``lucent-ohci`` subtype only, because the Origin 300 board diagnostic
  expects zero. Rating: inferred (the two reset tables agree on zero, the
  prose conflicts, and no real chip read is available; a real read-back of
  the register after reset would settle it). The USS-312 data sheet's
  Tables 42 and 45 to 48 give HcFmNumber and the root-hub register
  fields, access types and reset values; the model follows them for the
  root-hub port power fields and the frame number latch. The core's other
  reset values are QEMU's and are not hardware facts.
- *Frame number.* Real silicon advances HcFmNumber from its own clock. QEMU's
  shared core applies an overdue frame boundary synchronously on every
  register read so a read reflects the clock (an emulator timing choice;
  the Origin 300 PROM reads HcFmNumber about 1 ms after setting the
  controller operational and fails if it still reads zero).
- *Identity.* The vendor, device, revision, two ports and subsystem
  placeholders above.

Anything beyond the standard OHCI register contract (the USS-312's own
configuration registers, power management and legacy support behaviour) is
unknown here and not modelled.
