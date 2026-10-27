.. SPDX-License-Identifier: BSD-3-Clause

.. _sgi-heart:

=================================================
HEART processor and memory controller (Octane)
=================================================

HEART is the processor, memory and I/O controller of the Octane and
Octane2 IP30 system board. It sits between the processor, the SDRAM
banks and the Crossbow crossbar (:src:`octane-technical-report p. 2-15`),
where it is widget 8 (:src:`miod-openbsd-sgi-story-4 lines 1308-1352`).
QEMU models it as
``sgi-heart`` in ``hw/misc/sgi-heart.c``. The ``octane`` machine fits
HEART revision 4 and the ``octane2`` machine HEART revision 6, which
IRIX reports as revision F (:ref:`sgi-ip30`). The model is a functional
profile for one processor: it gives the stock IP30 PROM and IRIX 6.5.30
the registers they use, and it is not a revision-matched description
of the chip.

Facts below are rated *established* (a cited manual or a measurement of
an identified machine), *observed* (a published capture), *software
expectation* (what a public driver or a shipped IRIX 6.5.30 binary
reads, writes or requires; it shows what software needs, not what the
chip does), *inferred*, *approximation* (a model choice where the
hardware is not known) or *unknown*. No HEART specification is public,
so most register behaviour here is approximation.

.. _sgi-heart-sources:

Sources
=======

- SGI's *OCTANE Technical Report*,
  :src:`octane-technical-report p. 2-15-2-19`: HEART's processor,
  memory and I/O units, its place between processor, memory and
  Crossbow, and its monitoring of I/O writes for cache coherence.
  Printed pages 2-21 to 2-23 describe memory banks. It gives no register
  values.
- The owner's guides :src:`007-3435-004 p. 54` (Octane) and
  :src:`007-4207-002 p. 47` (Octane2): DIMM pairs, bank order and bank
  capacities.
- OpenBSD ``sys/arch/sgi/xbow/xheartreg.h``, ``xheart.c``,
  ``xbowdevs.h``, ``widget.h`` and ``sys/arch/sgi/sgi/ip30.h``,
  ``ip30_machdep.c`` at ``2df91d80c104``, as software expectations:
  register offsets, the bank descriptor fields, the interrupt levels,
  the counter rate, the MicroLAN command and the widget identity.
- Linux ``arch/mips/include/asm/sgi/heart.h``,
  ``arch/mips/include/asm/xtalk/xwidget.h`` and ``arch/mips/sgi-ip30/``
  at ``db2ddb871435``, as software expectations: the register layout,
  mode-register bit names, interrupt level to CPU line assignment and
  common widget register names. The header credits some definitions to
  an IRIX header, so it does not corroborate OpenBSD independently.
- Miod Vallat's OpenBSD boot capture of a 128 MiB Octane,
  :src:`miod-openbsd-sgi-story-4 lines 1308-1352`: HEART at widget 8,
  revision 4.
- The *Crossbow ASIC Specification*, revision 12.0,
  :src:`crossbow-asic-spec-12.0 p. 64-65`, for the interrupt packet
  fields.
- The DS2505 one-wire EEPROM datasheet, :src:`ds2505-datasheet p. 15`,
  for the meaning of a one-wire reset slot.
- The IP30 PROM and the IP30 kernel and stock diagnostic shipped with
  IRIX 6.5.30, read as binaries, as software expectations.
- Reads taken on the owner's Octane2 (:ref:`sgi-heart-measurements`).

.. _sgi-heart-identity:

Identity and placement
======================

HEART is Crossbow widget 8, *observed* in a published Octane capture
(:src:`miod-openbsd-sgi-story-4 lines 1308-1352`) and agreed by OpenBSD
``ip30.h`` and Linux. Its processor-interface block starts at physical
``0x0ff00000`` in OpenBSD ``xheartreg.h`` and Linux ``heart.h``, a
*software expectation*; the owner's Octane2 answered descriptor reads
there (:ref:`sgi-heart-octane2-bank`), *established* for that board.
RAM starts at physical ``0x20000000`` in OpenBSD ``ip30.h``, a *software
expectation*. OpenBSD ``xbowdevs.h`` gives the widget manufacturer
``0x036`` and part ``0xc001``, and OpenBSD and Linux put revision in
bits 31:28, part in bits 27:12 and manufacturer in bits 11:1 of the
identification word, read as 32 bits at widget offset ``0x04`` or as 64
bits at ``0x00``, a *software expectation*. The Octane2 read below
decodes consistently with that layout.

.. list-table::
   :header-rows: 1

   * - Machine
     - Revision field
     - Identification word
     - Basis
   * - ``octane``
     - 4
     - ``0x4c00106d``
     - Revision 4 *observed* at widget 8 in the OpenBSD capture
       (:src:`miod-openbsd-sgi-story-4 lines 1308-1352`). A published
       195 MHz inventory prints HEART revision D
       (:src:`usenet-3be33f04`). That D is revision 4 is *inferred*:
       counting A as 1 makes D 4 and F 6, and the Octane2's revision F
       reads widget revision 6 (:ref:`sgi-heart-octane2-id`). Bit 0 set is
       an *approximation*.
   * - ``octane2``
     - 6
     - ``0x6c00106d``
     - *Established*: read on the owner's revision F Octane2
       (:ref:`sgi-heart-octane2-id`).

The stock IRIX 6.5.30 IP30 diagnostic reads the word as 64 bits and
requires bit 0 set and a nonzero revision in bits 31:28, a *software
expectation*. The meaning of bit 0 is *unknown*. Both machines return
the same word for an aligned 64-bit read at ``0x00`` and an aligned
32-bit read at ``0x04``. The 64-bit upper half reads zero, an
*approximation* that matches the Octane2 read. Every other width or
offset within the identification slot, and any write to it, fails the
transaction. A standalone ``sgi-heart`` without the ``widget-id-64``
property builds the word from ``widget-revision`` with bit 0 clear.

The machines map HEART's widget registers at physical ``0x18000000``,
widget 8's small window, directly from the processor rather than
through a Crossbow loopback.

The model fits one processor. Processor-interface status at ``0x80``
reads ``0x18``: bit 4 marks processor 0 active and bits 3:0 give widget
8 (Linux names the low nibble the widget ID). The processor ID register
at ``0x50000`` reads zero. These are *approximations* for the
one-processor profile. The full status word of either revision is
*unknown*. The IRIX 6.5.30 kernel takes the low nibble of status as the
destination widget for PCI DMA, a *software expectation*; with a zero
nibble it would send SCSI DMA to widget 0.

.. _sgi-heart-registers:

Register map
============

The processor-interface block is 64-bit registers at physical
``0x0ff00000``, big-endian. The model takes 32- and 64-bit accesses;
narrower or unaligned accesses are refused by QEMU's memory core. Names
follow Linux ``heart.h``. Every reset value is the model's choice
(*approximation*); no HEART reset value is published or measured.

.. list-table::
   :header-rows: 1

   * - Offset
     - Name
     - Width
     - Reset
     - Access
     - Notes
   * - ``0x00``
     - Mode
     - 64
     - ``0``
     - read/write
     - Stored whole. Only bits 0, 1, 13, 15, 19, 20, 21 and 25 change
       behaviour (:ref:`sgi-heart-interrupts`, :ref:`sgi-heart-ecc`).
   * - ``0x08``, ``0x10``, ``0x18``
     - SDRAM mode, memory refresh, request arbitration
     - 64
     - ``0``
     - read/write
     - Stored with no effect.
   * - ``0x20``-``0x3c``
     - Memory bank descriptors
     - 32 or 64
     - ``0``
     - read/write
     - Eight 32-bit words; words 0-3 map banks 0-3
       (:ref:`sgi-heart-memory`).
   * - ``0x50``, ``0x58``
     - Flow-control addresses
     - 64
     - ``0``
     - read/write
     - Stored independently with no effect.
   * - ``0x80``
     - Status
     - 64
     - ``0x18``
     - read-only
     - Constant.
   * - ``0x98``, ``0xa0``
     - Memory error address and data
     - 64
     - ``0``
     - read-only
     - Saved ECC record (:ref:`sgi-heart-ecc`).
   * - ``0xb0``
     - MicroLAN clock divider
     - 64
     - ``0``
     - read/write
     - Stored with no effect on slot timing.
   * - ``0xb8``
     - MicroLAN control
     - 64, or 32 at ``0xbc``
     - ``0``
     - read/write
     - A write starts a one-wire slot (:ref:`sgi-heart-onewire`).
   * - ``0x10000``-``0x10018``
     - Interrupt masks, one per processor
     - 64
     - ``0``
     - read/write
     - Only mask 0 drives a processor.
   * - ``0x10020``
     - Set pending
     - 64
     - none
     - write-one-to-set
     -
   * - ``0x10028``
     - Clear pending
     - 64
     - none
     - write-one-to-clear
     -
   * - ``0x10030``
     - Pending
     - 64
     - ``0``
     - read-only
     - Raw pending bits.
   * - ``0x10038``
     - Masked pending
     - 64
     - ``0``
     - read-only
     - Pending AND mask 0.
   * - ``0x10040``
     - Cause
     - 64
     - ``0``
     - write-one-to-clear
     - Bit 0 corrected and bit 1 uncorrectable memory error.
   * - ``0x20000``
     - Count
     - 64
     - ``0``
     - read-only
     - 52-bit counter (:ref:`sgi-heart-timer`).
   * - ``0x30000``
     - Compare
     - 64
     - ``0``
     - read/write
     - Low 24 bits stored with no effect.
   * - ``0x50000``
     - Processor ID
     - 64
     - ``0``
     - read-only
     -

The widget register window is HEART's face on Crossbow, at physical
``0x18000000``. Offsets follow Linux ``xwidget.h`` and OpenBSD
``widget.h``, which put the common widget registers in eight-byte
slots with the 32-bit register in the low (higher-addressed) half. The
model serves only the widths listed; any other access in the window
fails the transaction and logs ``LOG_UNIMP``.

.. list-table::
   :header-rows: 1

   * - Offset
     - Name
     - Width
     - Reset
     - Access
     - Notes
   * - ``0x00`` / ``0x04``
     - Identification
     - 64 / 32
     - per machine
     - read-only
     - :ref:`sgi-heart-identity`
   * - ``0x08``
     - Status
     - 64
     - ``0x60`` on ``octane2``
     - read-only
     - Absent on ``octane``.
   * - ``0x14``, ``0x1c``
     - Error address upper and lower
     - 32
     - ``0``
     - read-only
     - Constant zero.
   * - ``0x24``
     - Control
     - 32
     - ``0``
     - read/write
     - Stored whole with no effect.
   * - ``0x28`` / ``0x2c``
     - Request timeout
     - 64 / 32
     - ``0``
     - read/write
     - One 32-bit word in both views.
   * - ``0x44``
     - Error command word
     - 32
     - ``0``
     - read; write zero only
     - A nonzero write fails.
   * - ``0x60``
     - Error status
     - 64
     - ``0``
     - read; writes ignored
     - No error source is modelled.
   * - ``0x68``
     - Error mask
     - 64
     - ``0``
     - read/write
     - Low 32 bits stored.
   * - ``0x70``, ``0x78``, ``0x90``
     - HEART-specific error words
     - 64
     - ``0``
     - read-only
     - Constant zero.

The stock 6.5.30 diagnostic's register test saves, writes six masked
patterns to, compares and restores widget ``0x28`` and ``0x68`` and
processor-interface ``0x50``, ``0x58``, ``0xb0``, ``0x10000``-``0x10018``
and ``0x30000``, all as 64-bit accesses, a *software expectation*. It
compares the low 20 bits of ``0x28``, bits 63:5 of ``0x50`` and
``0x58`` and the bits in ``0x7f7f`` of ``0xb0``. The model stores those
registers so the test reads back what it wrote; its masks say what the
test compares, not which bits HEART implements.

.. _sgi-heart-widget-registers:

Widget status, control and request timeout
------------------------------------------

The widget status word reads ``0x60`` on the ``octane2`` only. The
owner's revision F Octane2 returned exactly that value after PROM
startup (:ref:`sgi-heart-octane2-status`), so the value is
*established* for that board at PROM time. It is not a reset value, and
what the bits mean is *unknown*. The stock 6.5.30 diagnostic reads the
slot as 64 bits and expects bits 5 and 6 set and bits 7-15 clear, a
*software expectation*. The Octane's revision 4 status is unmeasured, so
the ``octane`` fails the read. A 32-bit read of either half and any
write fail on both machines.

The IP30 PROM reads, modifies and writes the 32-bit control word at
``0x24``, a *software expectation*. The model stores all 32 bits and
returns them; zero reset and full writability are *approximations*.

The request-timeout word is one stored 32-bit value. A 32-bit access at
``0x2c`` and a 64-bit access at ``0x28`` reach it; a 64-bit write keeps
its low 32 bits and a 64-bit read returns zero in the upper half. The
IRIX 6.5.30 kernel reads the 32-bit word while attaching I/O and writes
``0x80`` when the value is lower, a *software expectation*; from the
model's zero it takes that write. The owner's Octane2 read ``0x28``
after PROM startup (:ref:`sgi-heart-octane2-timeout`). The unit and the
effect of the timeout are *unknown*; the model times out nothing.

.. _sgi-heart-widget-errors:

Widget error registers
----------------------

The IRIX 6.5.30 kernel reads error status at ``0x60``, writes the value
back, zeroes the error command word at ``0x44`` and writes
``0x3ff62c07`` to the error mask at ``0x68``, a *software expectation*
that suggests write-one-to-clear status and a writable mask. The owner's
Octane2 read zero at ``0x60`` and ``0x3ff7fc0f`` at ``0x68`` after PROM
startup (:ref:`sgi-heart-octane2-widget`).

The model has no error source. Status reads zero and a 64-bit write to
it is accepted and changes nothing. The mask stores the low 32 bits of a
64-bit write and drives nothing. The error command word reads zero and
takes only a zero write.

After a bus error, including the one a guarded probe of an absent PCI
device takes, the IRIX 6.5.30 kernel reads ``0x60`` (64-bit), ``0x44``,
``0x14`` and ``0x1c`` (32-bit), then ``0x90``, ``0x70`` and ``0x78``
(64-bit), a *software expectation*. Linux names ``0x14``, ``0x1c`` and
``0x44`` as common widget registers. The model answers every one of
these reads with zero, which the kernel treats as no recorded error, an
*approximation*: what HEART records after a Bridge timeout is *unknown*.

.. _sgi-heart-memory:

Memory bank descriptors and the PROM's sizing probe
====================================================

The eight 32-bit descriptors at ``0x20``-``0x3c`` follow OpenBSD
``xheartreg.h`` and Linux ``heart.h``, which agree: bit 31 marks a valid
bank, bits 24:22 are a density code, bits 21:16 are one less than the
bank's size in 32 MiB units, and bits 8:0 are its base in 32 MiB units
above ``0x20000000``. Neither public kernel uses the density code to
size a bank. A 32-bit access reaches any word; a 64-bit access at
``0x20``, ``0x28``, ``0x30`` or ``0x38`` reaches two words, the lower
offset in the upper half. Words 0-3 map banks 0-3; words 4-7 are
stored and map nothing. Each owner's guide numbers its banks 1 to 4;
that guide bank 1 is descriptor 0 is *inferred*, not a measured wiring
fact.

A bank holds two matched DIMMs and banks fill from bank 1 without a gap
(:src:`007-3435-004 p. 54`, :src:`007-4207-002 p. 47`), *established*.
The device accepts a capacity per bank of zero or a power of two from
64 MiB to 2 GiB, set by the machine's ``bank<N>-size`` properties; the
products' own limits are on :ref:`sgi-ip30-memory`.

For a valid descriptor the model maps ``(size + 1) * 32 MiB`` bytes at
``0x20000000 + base * 32 MiB`` onto that bank's RAM, and remaps when a
descriptor changes. A window larger than the bank wraps onto it modulo
its capacity. A valid window on a bank with no RAM reads zero and drops
writes for the processor, and fails for DMA. Machine reset zeroes the
descriptors in the device, then the ``octane`` and ``octane2`` machines
write one valid descriptor per fitted bank, packed from base 0, so a
guest started without the PROM finds its RAM mapped. These are
*approximations*.

The PROM sizes each bank itself, a *software expectation* from the
shipped IP30 PROM. It clears the four descriptors with two 64-bit
writes, writes a temporary descriptor ``0x81bf0040`` (valid, 2 GiB,
density 6, base ``0x40``) to each bank, and tests presence at offsets 0
and 8. It then picks the capacity from a sixteen-word table of
candidate descriptors whose largest is 2 GiB. Below HEART revision 6,
when the provisional density code is odd, it compares a word at
base + ``0x80`` with one at base + 32 MiB + ``0x80`` and drops the bank
on a match. On revision 6 and later it writes zero at the bank base,
writes all ones at half the provisional size and reads the base back:
a nonzero read shows the write aliased onto the base and halves the
descriptor, and a zero read keeps it. Last, it assigns bases largest
bank first and writes the final descriptors. The IRIX 6.5.30 kernel
only reads the descriptors: it sums ``(size + 1) * 32 MiB`` over the
valid ones in banks 0-3 and never reads the density code.

For banks of 64 to 512 MiB, plain modulo mapping makes the below-revision-6
path choose a descriptor larger than the bank. Under a valid 2 GiB
descriptor with density code 6, the model therefore maps a bank offset ``o``
onto the fitted RAM with this projection, an *approximation* chosen so that
the PROM's own table names the fitted size on both revision paths:

.. list-table::
   :header-rows: 1

   * - Fitted bank
     - Backing offset under the temporary descriptor
   * - 64 MiB
     - ``(o & (32 MiB - 1)) | ((o & 64 MiB) >> 1)``
   * - 128 MiB
     - ``(o & (32 MiB - 1)) | ((o & 64 MiB) >> 1) | ((o & 1 GiB) >> 4)``
   * - 256 MiB
     - ``(o & (128 MiB - 1)) | ((o & 256 MiB) >> 1)``
   * - 512 MiB
     - ``(o & (128 MiB - 1)) | ((o & 256 MiB) >> 1) | ((o & 1 GiB) >> 2)``
   * - 1 and 2 GiB
     - ``o % capacity``

Every other descriptor uses ``o % capacity``. The projection applies
to each byte of an access, so a multi-byte access that crosses a
discontinuity is split correctly. It is not a measured HEART address
decoder.

On the model, the probe leaves these descriptors, still at the
temporary base ``0x40``, on both revision paths. The PROM's base
assignment then removes ``0x40``, so a single bank ends at base 0 with
the same upper bits.

.. list-table::
   :header-rows: 1

   * - Fitted bank
     - Descriptor after the probe
     - Density code
   * - 64 MiB
     - ``0x81010040``
     - 4
   * - 128 MiB
     - ``0x81030040``
     - 4
   * - 256 MiB
     - ``0x81470040``
     - 5
   * - 512 MiB
     - ``0x814f0040``
     - 5
   * - 1 GiB
     - ``0x819f0040``
     - 6
   * - 2 GiB
     - ``0x81bf0040``
     - 6

These are model results, not measurements. The owner's Octane2 PROM left
``0x81470000`` for its 256 MiB bank at base 0
(:ref:`sgi-heart-octane2-bank`), which matches. The PROM's table pairs its
rows by density code; that each code names one SDRAM part density is
*inferred* from the table alone. What the density code does to HEART's
address decoding, how a smaller DIMM answers under the temporary descriptor,
and the descriptors' reset values are *unknown*. A revision-matched
specification, or the final descriptors of a known DIMM population on a real
board, would settle them.

The PROM also writes mode bits 21, 11 and 4 before its probe and clears
bit 21 after it, writes ``0x00084000`` to memory refresh and eight
values ``(index << 16) | 0x21`` to SDRAM mode, a *software
expectation*. The model stores these writes with no electrical effect.

.. _sgi-heart-interrupts:

Interrupts
==========

OpenBSD ``xheartreg.h`` and Linux ``heart.h`` group the 64 pending bits
into five levels: bits 0-15 level 0, 16-31 level 1, 32-49 level 2, bit
50 level 3 (the timer) and 51-63 level 4 (errors; bit 63 is HEART's own
exception). Linux sends levels 0-4 to CPU interrupt lines IP2-IP6. Both
drivers set, clear and mask bits through the registers in the map and
read pending separately from the mask. These are *software
expectations*. The IRIX 6.5.30 kernel clears all four masks, writes
``0x8800440000000000`` to processor 0's mask before it starts Crossbow
and IOC3, reads ``0x10038`` in its dispatcher and acknowledges a source
by writing its bit to ``0x10028``.

The model drives five outputs to CPU IP2-IP6. Each is high while some
bit of its level is both pending and set in mask 0. Bit 63 also needs
mode bit 13 (interrupt enable, Linux ``HM_INT_EN``) before it reaches
IP6. A masked bit stays pending; unmasking it raises the line again
without a new event. Masks 1-3 are stored and drive nothing. These
output and retention rules are *approximations* compatible with the
drivers; HEART's assertion timing is *unknown*.

Devices raise interrupts with a Crosstalk write to HEART's widget offset
``0x80``, the destination OpenBSD and Linux program into the onboard Bridge,
a *software expectation*. The Crossbow specification gives the interrupt
packet's data the vector in bits 7:0 and assertion in bit 8, set for an
interrupt and clear for its withdrawal
(:src:`crossbow-asic-spec-12.0 p. 64-65`), *established* for Crossbow's own
packets. That Bridge packets to HEART carry the same fields is *inferred*
from the shared Crosstalk packet format. The model takes only 64-bit writes
there with vector below 64 and nothing above bit 8; it sets or clears that
one pending bit. Anything else fails and logs ``LOG_UNIMP``. This packet
side is separate from the processor's view of the widget window, where
offset ``0x80`` is not a register.

.. _sgi-heart-timer:

Counter and compare
===================

OpenBSD ``xheartreg.h`` describes the count register at ``0x20000`` as 52
bits advancing at 12.5 MHz, and names a 24-bit compare at ``0x30000`` and a
trigger at ``0x40000``; Linux agrees on width and rate. These are *software
expectations*. The model's count advances one tick per 80 ns of virtual time
from zero at reset and wraps at 52 bits, *approximation* for the phase: the
chip's starting value is *unknown*.

Compare stores the low 24 bits of a 64-bit write and reads them back.
It raises no interrupt and does not touch the counter. The trigger
register is not modelled. The compare and reload rule is *unknown*; no
public source gives one. The stock diagnostic's register test writes
and reads compare, which is why it is stored.

.. _sgi-heart-onewire:

One-wire master
===============

OpenBSD ``xheartreg.h`` and Linux ``heart.h`` place a MicroLAN
(one-wire) master at ``0xb8``, a *software expectation*. The model hangs the
processor module's board record on it (:ref:`sgi-ip30-board-identity`).
OpenBSD writes a 64-bit command with the pulse width in bits 19:10 and
the sample point in bits 9:2, polls bit 1 for completion and reads the
wire level from bit 0. Its slots are reset (500, 65), read (6, 13),
write one (6, 110) and write zero (80, 30), *software expectation*.
The IP30 PROM writes ``0x82104``, a reset slot, to the 32-bit low word
at ``0xbc`` and polls bit 1 there.

A write to ``0xb8`` (64-bit) or ``0xbc`` (32-bit) starts one slot on the
device's one-wire bus and clears bits 1 and 0. The model takes one command
unit as one microsecond and completes the slot after pulse plus sample units
of virtual time, then sets bit 1 and the sampled level in bit 0. An empty
bus samples one; a present part pulls the reset sample low, as a DS2505
presence pulse does after a reset low of at least 480 µs
(:src:`ds2505-datasheet p. 15`), *established* for that part. The unit scale
and the clock divider at ``0xb0``, which the model stores but ignores, are
*approximations*; HEART's real slot timing is *unknown*. A command written
while a slot is busy replaces it and logs ``LOG_GUEST_ERROR``. Reset clears
the control word and cancels a slot.

.. _sgi-heart-ecc:

Memory ECC
==========

Linux ``heart.h`` names the mode bits involved: 0 enables corrected
memory error reports, 1 uncorrectable reports, 15 global ECC, 19
single-bit error generation, 20 double-bit generation, 21 forced write
and 25 correction lock. It places the memory error address and data at
``0x98`` and ``0xa0`` and cause at ``0x10040``. These names and
placements are *software expectations*. None of these sources
gives HEART's encoder, correction or latch rules, so the ECC model is a
functional subset, *approximation* throughout. It covers aligned
eight-byte processor accesses to fitted RAM only.

The stock 6.5.30 diagnostic writes one scratch doubleword with mode
bit 19 set, restores the mode, reads the word back and polls cause, a
*software expectation*. Its data decodes syndrome ``0x13`` as data bit
0 and ``0x23`` as data bit 1.

Single-bit errors:

- An aligned eight-byte write with bits 19, 15 and 0 set, and 20, 21
  and 25 clear, stores the data and marks that RAM word as single-bit
  faulty. Writing the mode alone marks nothing.
- On a later aligned eight-byte read of the word, with bit 15 clear the
  read returns the data with bit 0 inverted and records nothing. With
  bit 15 set and bit 0 clear it returns the stored data and records
  nothing. With both set it returns the stored data, saves the physical
  address with syndrome ``0x13`` and the data with bit 0 inverted, sets
  cause bit 0 and pending bit 63. Each such read reports again and
  replaces the saved record.

Double-bit errors:

- An aligned eight-byte write with bits 20, 15 and 1 set, and 19, 21
  and 25 clear, marks the word as double-bit faulty.
- On a read with bit 15 clear the read returns the data with bits 0 and
  1 inverted. With bit 15 set the read fails with a bus error, so the
  processor takes its data bus error exception and the load does not
  complete. When bit 1 is also set, the read first saves the address
  with syndrome ``0x30``, the data with bits 0 and 1 inverted, and sets
  cause bit 1 and pending bit 63. With bit 1 clear it fails without
  recording anything. ``0x30`` is ``0x13`` XOR ``0x23``,
  *inferred* for a linear code.

The saved address holds physical address bits 33:0, the syndrome in
bits 41:34 and the requesting processor in bits 44:43; bit 42 is always
clear. No public source specifies this layout, so it is an
*approximation*. Cause bits clear by writing one to ``0x10040``;
pending bit 63 clears through ``0x10028``. Neither repairs memory or
clears the saved record, and clearing pending does not reassert it from
a cause bit still set. An aligned eight-byte write with bits 19, 20 and
25 clear repairs a faulty word, with or without bit 21. Machine reset
clears all faults and records.

The model refuses, without changing RAM or any record, an injection
without its enables or together with bit 21, any partial or unaligned
write while bit 19, 20 or 21 is set, a partial or unaligned access
touching a faulty word, a read of a faulty word while bit 20 or 21 is
set, a read of a double-bit faulty word while bit 19 is set, any
injection, forced write or faulty-word access while bit 25 is set,
both generation bits at once, a DMA access touching a faulty word, a DMA
write while either generation bit is set, and a descriptor write while
any word is faulty. Each refusal logs ``LOG_UNIMP`` and fails the
transaction.

.. _sgi-heart-dma:

PCI DMA through Crossbow
========================

The technical report has HEART handle I/O requests to memory and
monitor them for coherence (:src:`octane-technical-report p. 2-15`),
but no public source gives HEART's request decode, response format or
coherence rules. OpenBSD programs the onboard Bridge's direct map for
widget 8 and adds ``0x80000000`` to a physical address to make a PCI
DMA address (:doc:`bridge`), a *software expectation*.

The model accepts a Crosstalk read or write addressed to HEART when
every byte falls in a valid descriptor window of a fitted bank. It uses
the Crosstalk address as Bridge produced it, adds no offset, and reads
or writes the same RAM, through the same descriptor mapping and probe
projection, that the processor sees. Coherence comes from QEMU's shared
RAM, not from modelled snooping, so a coherent-request attribute
changes nothing. A request outside any fitted window logs ``LOG_UNIMP``
and fails; HEART's real response is *unknown*. Each access of up to
eight bytes is checked before any byte is written, but a longer DMA
split into several accesses can fail part way through. All of this is
*approximation*.

.. _sgi-heart-reset:

Reset and migration
===================

Reset zeroes mode, the memory controller words, all eight descriptors,
the flow-control and divider words, compare, the four masks, pending,
cause, the saved ECC record and all faults, the widget control,
request timeout and error mask, and the one-wire control word, and
cancels a one-wire slot. The counter restarts from zero. RAM keeps its
contents. The interrupt outputs are recomputed. These are
*approximations*; the chip's power-on state is *unknown*.

Migration carries the same state, the counter value, faulty-word list
and any unfinished one-wire slot's remaining time. The counter resumes
from its saved value at the destination's virtual time. A faulty-word
list that does not match the configured banks fails the load. Bank
capacities, revision and identity are machine configuration and must
match on both sides. Migration works only between identical builds.

.. _sgi-heart-measurements:

Measurements
============

These reads come from the :ref:`sgi-measured-octane2`, HEART revision F
(widget revision field 6), in the PROM Command Monitor after the
firmware had initialised the machine. None is a reset value, and none
was preceded by a write.

.. list-table::
   :header-rows: 1

   * - Address
     - Width
     - Value
     - Register
     - Revision
     - When
   * - ``0x18000000``
     - 64
     - ``0x000000006c00106d``
     - widget identification
     - F (field 6)
     - PROM monitor, after initialisation
   * - ``0x00002000``
     - 64
     - ``0``
     - low memory
     - F (field 6)
     - PROM monitor, after initialisation
   * - ``0x18000008``
     - 64
     - ``0x0000000000000060``
     - widget status
     - F (field 6)
     - PROM monitor, after initialisation
   * - ``0x0ff00020``-``0x0ff0002c``
     - 32, four words
     - ``0x81470000``, ``0``, ``0``, ``0``
     - bank descriptors 0-3
     - F (field 6)
     - PROM monitor, after initialisation
   * - ``0x18000060``, ``0x18000068``
     - 64, two words
     - ``0``, ``0x3ff7fc0f``
     - widget error status and mask
     - F (field 6)
     - PROM monitor, after initialisation
   * - ``0x18000028``
     - 64
     - ``0x28``
     - widget request timeout
     - F (field 6)
     - PROM monitor, after initialisation

.. _sgi-heart-octane2-id:

Identification and low memory
-----------------------------

.. code-block:: text

   >> dump -d -x 0x9000000018000000#1
   0x9000000018000000:           6c00106d
   >> dump -d -x 0x9000000000002000#1
   0x9000000000002000:                  0

The first read decodes as revision 6, part ``0xc001``, manufacturer
``0x036``, bit 0 set, upper half zero. The second, at physical
``0x2000``, returned zero. Both completed without an exception. The low
memory read shows that the address answers, not what backs it or
whether it aliases RAM. Neither read shows other widths, writes or the
Octane's HEART.

.. _sgi-heart-octane2-status:

Widget status
-------------

.. code-block:: text

   >> dump -d -x 0x9000000018000008#1
   0x9000000018000008:                 60

One read. It shows neither read side effects, the value at other widths
nor what bits 5 and 6 mean.

.. _sgi-heart-octane2-bank:

Bank descriptors
----------------

.. code-block:: text

   >> dump -w -x 0xffffffffaff00020#4
   0xffffffffaff00020:   81470000          0          0          0

Descriptor 0 decodes as valid, density 5, size field 7 (256 MiB), base
0 (physical ``0x20000000``) under the public field definitions; the
other three are invalid. This agrees with the 256 MiB the inventory
reports. It shows the PROM's final descriptors for this one population,
not which sockets hold the DIMMs, the provisional descriptors or a reset
value.

.. _sgi-heart-octane2-widget:

Widget error status and mask
----------------------------

.. code-block:: text

   >> dump -d -x 0x9000000018000060#2
   0x9000000018000060:                  0           3ff7fc0f

Both registers exist on revision F. The mask value is the one firmware
left; it is not a reset value and does not show which bits are
writable.

.. _sgi-heart-octane2-timeout:

Request timeout
---------------

.. code-block:: text

   >>dump -d -x 0x9000000018000028#1
   0x9000000018000028:                 28

The value after PROM startup, not a cold-reset value. It shows no write
behaviour and nothing about the 32-bit view.

.. _sgi-heart-limits:

Model limits
============

- One processor. Status, processor ID and the interrupt outputs follow
  that profile; masks 1-3 drive nothing and inter-processor interrupts
  have no second target.
- Mode bits other than 0, 1, 13, 15, 19, 20, 21 and 25 are stored with
  no effect, including the reset, refresh and cached-PROM bits Linux
  names.
- No error source: bus-error address and miscellaneous words, the
  processor-interface access error word and the widget error registers
  never record anything. Bridge errors do not reach HEART.
- No timer interrupt: compare is stored only and trigger is absent.
  Level 3 rises only when software or an interrupt packet sets bit 50.
- The descriptor projection, the modulo wrap and the absent-bank
  responses are functional choices, not HEART's address decoder.
- No processor cache coherence protocol; DMA and the processor share
  RAM directly.
- ECC covers only the subset above: no check-bit injection, no
  correction lock, no faulty-word DMA, fixed corrupt bits and syndromes,
  and reports set cause and pending at the moment of the read. While
  mode bit 19, 20 or 21 is set, every RAM access takes the slower
  checked path and partial writes fail.
- Processor-interface accesses to an unmodelled offset log
  ``LOG_UNIMP``; reads return zero and writes are dropped, without a bus
  error. Widget-window accesses to an unmodelled offset or width log
  ``LOG_UNIMP`` and fail, so the processor takes a bus error. The stock
  6.5.30 graphics diagnostic reads widget ``0x10`` as 64 bits and
  branches on zero, a *software expectation*; on the model that read
  fails this way. HEART's value there is *unknown*.
- Busy one-wire replacement logs ``LOG_GUEST_ERROR``.
- The Octane's HEART revision 4 has no measured status word, so the
  ``octane`` refuses that read.
- Migration works only between identical builds.

.. _sgi-heart-tests:

Tests
=====

``tests/qtest/sgi-heart-test.c`` drives a standalone ``sgi-heart``. It
covers descriptors for 64, 128 and 256 MiB banks, two-bank isolation,
rejected capacities and the 64-bit descriptor clear, the probe projection,
and the PROM's sizing sequence on both revision paths for every bank size
from 64 MiB to 2 GiB (``/heart/prom-probe/*``). These check the descriptor
the probe leaves at base ``0x40``; the revision-4 cases for 64 to 512 MiB
fail under plain modulo mapping. It also covers widget identity, control,
request timeout, error and diagnostic registers, interrupt packets, pending
and mask behaviour on all five levels, processor status, the one-wire master
with and without a part, slot replacement, Crosstalk routing and DMA through
a Bridge, the ECC subsets, reset and migration.

``tests/qtest/sgi-heart-cpu-test.c`` runs code on R10000 and R12000
processors: a DMA into RAM the processor then executes, and double-bit
ECC reads that must take the data bus error exception with the right
EPC and branch-delay state, leave the destination register untouched,
and succeed after repair.

``tests/qtest/octane-test.c`` checks HEART inside the ``octane`` and
``octane2`` machines: identity at both widths and the refused widths
(``/octane/heart-identity``, ``/octane2/heart-identity``), the Octane2
status word, widget control and error registers, the stored diagnostic
registers on both machines, one-wire timing with the divider written,
HEART's level 0-2 outputs, and SCSI DMA from a QLogic
controller into HEART RAM.
