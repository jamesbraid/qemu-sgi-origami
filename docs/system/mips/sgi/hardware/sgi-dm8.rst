.. SPDX-License-Identifier: BSD-3-Clause

.. _sgi-dm8:

=====================================================
DM8: PCI audio option card (ICE1724 / Envy24HT)
=====================================================

The DMediaPro DM8 is a half-size PCI audio card SGI sold for the Origin 350,
Onyx 350, Onyx4 and Tezro. It is an off-the-shelf design: an IC Ensemble
(VIA) ICE1724 controller, also sold as Envy24HT, with AKM converters,
matching the M-Audio Revolution 7.1. QEMU models it as ``sgi-dm8``
(``hw/audio/sgi-dm8.c``), a PCI device that goes on any free PCI slot
of a machine with an :ref:`XBridge <sgi-xbridge>` or Bridge head. On the
:ref:`Fuel <sgi-ip35>` that is device 2 of head E, the first free slot.

.. seealso::

   Platform pages: :ref:`sgi-ip35`.

The register contract is ``docs/datasheets/dm8-ice1724.md`` in the Origami
repository. Facts below are rated *established*, *observed* (what the shipped
IRIX driver does), *expectation* (what the public Linux driver does),
*inferred*, *approximation* (a model choice, with its limit stated) or
*unknown*, as there.

Placement
=========

.. code-block:: shell

   qemu-system-mips64 -M fuel ... -audiodev pa,id=snd \
       -device sgi-dm8,bus=head-e.0,addr=2,audiodev=snd

``variant`` selects the board:

``dm8`` (default)
   The Revolution 7.1 wiring: AK4381 front and AK4355 surround converters on
   GPIO pins, a configuration EEPROM, PCI subsystem ID ``1412:3630``.
``ac97``
   The same controller on its AC'97 link, for boards that carry an AC'97
   codec, as the Tezro's onboard audio is inferred to. No EEPROM, no
   converter pins, the datasheet's default subsystem pair ``1412:1724``.
   The codec part and the board's own subsystem ID are *unknown*; see the
   approximations below.

Without an audio backend the device warns and runs without host samples;
the registers, DMA positions and interrupts behave the same.

What is modelled
================

- PCI configuration space: IDs, revision 1, class ``0401``, status and
  command masks, the 32-byte and 128-byte I/O BARs, the power-management
  capability at ``0x80`` (D3hot to D0 resets the chip including
  configuration space), and the subsystem ID write enable. The controller's
  own registers are *established*; that the DM8 carries ``1412:3630`` is
  *inferred*.
- The controller block (BAR0): control with soft reset, interrupt mask and
  status, system, AC-link, I2S and S/PDIF configuration bytes, the I2C
  port, the 23 GPIO pins with their data, write-mask and direction
  registers, and the power-down register. *Established* for the
  controller; which of its pins and bytes the DM8 uses is *inferred*.
- The multi-channel block (BAR1): sample rate and format, interrupt status
  and mask, the AC'97 port, start/stop, pause, the underrun and overrun
  flags, and seven DMA engines: the interleaved playback engine (eight, six,
  four or two slots by ``MT19``), three stereo playback engines, the S/PDIF
  playback engine and two record engines. *Established* for the
  controller; the DM8's use of them is *inferred*.
- DMA through ``pci_dma_read``/``pci_dma_write``, so the head's address
  translation and byte placement apply. An engine walks a circular buffer;
  its count register reads the remaining dwords, its address register the
  current address, and its terminal count raises its bit in ``MT00`` every
  interval. The macro bit in ``CCS02`` and ``INTA#`` follow the unmasked
  ``MT00`` bits. Positions and interrupts follow the virtual clock at the
  programmed rate, so they are exact under a stepped clock.
- Host audio: the interleaved engine and the S/PDIF engine play, and the
  first record engine captures, as 32-bit little-endian samples at the
  programmed rate. When the host supplies no input the record engines write
  silence, so positions still advance.
- The AKM write protocol on the GPIO pins (``dm8`` variant): a 16-bit
  frame, ``CCLK`` idle high, the front converter's select on GPIO 5 (chip
  address 1, five registers) and the surround converter's on GPIO 6 (chip
  address 3, eleven registers), clocked on rising ``CCLK`` edges and latched
  when the select rises. The frame is *established* (AKM datasheets); that
  the DM8 carries these parts on these selects is *inferred* (board fit).
  Each part's
  registers are readable as the read-only properties ``ak4381-reg0`` to
  ``ak4381-reg4`` and ``ak4355-reg0`` to ``ak4355-reg10``.
- The I2C port and a 256-byte EEPROM at device ``0x50`` (``dm8``). The port
  is *established*; a device answering at ``0xa0`` is *observed* in the IRIX
  driver, and the part and image are *unknown*.
- The AC'97 register file behind ``MT04`` to ``MT06`` (``ac97``): index,
  data, and the write and read cycles.

Properties for observation
--------------------------

``analog-frames``, ``spdif-frames``, ``capture-frames`` and
``dropped-frames`` count frames moved by the engines and frames the host
backend did not take. ``analog-last-sample`` is the last dword of the last
analog frame. ``ak4381-writes``, ``ak4355-writes`` and ``codec-rejects``
count converter frames taken and refused.

Approximations and limits
=========================

Each row is a place the datasheet is silent or the model departs from the
hardware. Unexpected accesses are logged (``-d unimp,guest_errors``), never
accepted silently.

.. list-table::
   :header-rows: 1
   :widths: 30 70

   * - Datasheet section
     - Model
   * - Access conventions, soft reset
     - ``CCS00`` bit 7 restores every register except ``CCS00`` and
       ``CCS04`` to ``CCS07``, as the VT1720 sibling datasheet says; the
       bit does not self-clear. The ICE1724 text does not describe the reset.
   * - EEPROM layout, unknowns
     - The DM8's EEPROM image was never captured. The image carries only
       what the datasheet constrains: the subsystem pair, a 49.152 MHz
       crystal, four stereo DACs, an I2S converter, the converter pins as
       outputs and ``CCLK`` and the selects idle high. Everything else is
       chosen to be inert.
   * - I2C port and EEPROM
     - A transfer completes at once. A transfer to an address nothing
       answers is logged and leaves ``CCS12`` alone; NAK handling is not
       described.
   * - GPIO block
     - An input pin reads its weak pull-up (1); the datasheet says the pin.
       Board straps are not modelled. The qtests expect these pull-up
       values, so those expectations come from the model, not the
       datasheet.
   * - Serial register write
     - Every complete frame is accepted; the parts ignore writes while
       ``PDN`` is low, and how the board wires ``PDN`` is unknown. A frame
       of other than 16 bits, a read command, a wrong chip address or an
       undefined register is refused and counted. The parts' registers
       reset to zero, except the attenuation registers, which reset to
       ``FF``; the other defaults are not in the datasheet. The converters'
       analog behaviour (soft mute, attenuation, the mute line on GPIO 22,
       sample rate fields) is not modelled: samples go to the host
       unchanged.
   * - CCS1C, power down
     - Reserved bits 6 and 1 read 0 and ignore writes (mask ``0xbd``); the
       datasheet lists the register as plain read/write. Bit 0, the
       multi-channel I2S serial clock, stops every DMA engine when set. The
       datasheet says only that it stops that clock, so the effect on the
       engines is *inferred*.
   * - AC'97 port
     - The index decodes as ``(MT04 & 0x7e) >> 1`` into a 64-entry register
       file; the datasheet gives only "bits 6:0 read/write".
   * - MT1A, underrun and overrun
     - The flags (and ``MT00`` bit 3) are set only by a failed bus transfer.
       The model's pacing never produces a FIFO underrun.
   * - RDMA1
     - Always records silence: no host source feeds the second record engine
       (typically S/PDIF input). Its buffer is still written and its
       position and interval run.
   * - MT3C, S/PDIF control
     - The datasheet says every field except the validity flag (bit 15) must
       be written only while the transmitter is disabled (``CCS07`` bit 7
       clear). The model enforces that: bit 15 stays writable, the other
       bits ignore writes and log. The register reads ``0000`` at reset,
       where the datasheet gives ``x000``.
   * - Converter registers and soft reset
     - The soft reset does not touch the converter registers or a frame in
       flight, nor does ``PRST#``: no converter reacts to it here.
   * - Crystal and ``CCS07``
     - The crystals are *unknown*. The EEPROM image selects 49.152 MHz
       because the stock driver writes that value without an EEPROM. Its
       ``CCS07`` is ``01``, the reset value, chosen to be inert: whether the
       DM8 sets "external S/PDIF out implemented" is *unknown*.
   * - MT00 and MT03
     - A masked source does not latch into ``MT00``. The datasheet wording
       suggests this; it does not say.
   * - Address, count and terminal-count registers
     - Terminal counts read as zero; they are write-only. A write to an
       address or count register restarts the buffer position, a write to
       a terminal count the interval, also while running. Counts reload
       and the interval repeats, as the drivers expect.
   * - Start, stop, pause, underrun
     - Stopping freezes the position. A failed bus transfer sets the flag in
       ``MT1A``, the master-abort bit in the PCI status and, if unmasked, bit 3
       of ``MT00``, and the engine plays or records silence. The datasheet
       does not say what a failed transfer does. A clock jump of more than a
       second (a paused guest or restored snapshot) is not replayed through
       memory.
   * - MT01
     - The external master clock (bit 4) is ignored: the rate code sets the
       pace. The reserved code ``1101`` keeps the previous rate. The
       128x/256x ratio and the crystal do not change timing; an impossible
       192 kHz setting is logged.
   * - MT05 and the AC'97 port
     - Bit 7 drives ``PRST#`` in I2S mode too (inferred from the sibling
       datasheet and both public drivers); no converter reacts. AC'97 cycles
       finish at once. The ``ac97`` variant's codec is a plain register file
       with every register zero at reset: its part, its reset values and
       its volume and rate registers are *unknown* and not modelled. Codec
       ID 0 only. In I2S mode, and on the ``dm8`` variant, there is no
       codec and cycles read zero.
   * - PCI configuration space, unknown registers
     - BARs 2 and 3, ``0x40``, ``0x41``, ``0x43`` and ``0x60`` are not
       described. They read 0, ignore writes and log the access.
   * - Routing, peak meters, MPU-401
     - ``MT2C`` and the MPU-401 UART registers store what is written and
       log it; the routing does not take effect and the UART does nothing.
       The peak meters read zero.
   * - PCI configuration space
     - The PCI core's power-management rules apply: D1 is refused, and the
       I/O BARs close outside D0. The datasheet does not say whether
       registers answer in D2 and D3hot. The ``CCS1C`` clock gate stops the
       engines. The interrupt line resets to ``FF`` as the datasheet says.
       The subsystem mask register is at ``0x42``; the datasheet also shows
       it at ``0x40``.
   * - Identity
     - The ``dm8`` variant's subsystem ID ``1412:3630`` is inferred from
       what the IRIX driver tests. The Envy24HT-S revision is *unknown* and
       not modelled.

The Fuel's head E interrupt is *observed* for an Intel 82550 at device 2
(XBridge pin 2) and is not yet wired in the Fuel model, so ``INTA#`` raised
by the card reaches the XBridge as a line with no board connection and is
logged. The qtests observe the line through the PCI status register.

Tests
=====

``tests/qtest/sgi-dm8-test.c`` places the card on the Fuel's head E and
drives it through the XBridge as the processor does. Cases cover the
configuration IDs and masks, register reset values and the soft reset,
system reset, power states, the EEPROM, the converter write protocol and its
refusals, GPIO masking, playback with the interrupt timeline and wrap,
pause, rate and clock gate, eight- and six-slot frames, a failed transfer,
record, the S/PDIF and stereo engines, the log of unknown accesses, placement,
the ``ac97`` variant, and migration of a running engine.
