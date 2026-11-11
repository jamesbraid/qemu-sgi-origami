.. SPDX-License-Identifier: BSD-3-Clause

.. _sgi-heart:

======================================
HEART: processor and memory controller
======================================

Used in :ref:`sgi-ip30`, on the Octane and Octane2 system board.

.. seealso::

   Platform pages: :ref:`sgi-ip30`.

.. seealso::

   - :term:`Hub` (:doc:`hub`): the :term:`SN0` node controller, the processor and memory
     controller of an SN0 node, as HEART is for the Octane.

HEART is the processor, memory and I/O controller of the IP30 board. It
sits between the processor, the SDRAM
banks and the :term:`Crossbow` crossbar (:src:`octane-technical-report p. 2-15`),
where it is widget 8 (:src:`miod-openbsd-sgi-story-4 lines 1308-1352`).
QEMU models it as
``sgi-heart`` in ``hw/misc/sgi-heart.c``. The ``octane`` machine fits
HEART revision 4 and the ``octane2`` machine HEART revision 6, which
IRIX reports as revision F (:ref:`sgi-ip30`). The model is a functional
profile for one processor: it gives the stock IP30 :term:`PROM` and IRIX 6.5.30
the registers they use, and it is not a revision-matched description
of the chip.

Facts are rated as described in :ref:`sgi-ratings`. No HEART
specification is public, so most register behaviour here is
approximation. What the stock PROM and IRIX kernel did on this model is
a *software expectation* observed on the model: it shows what that
software asks of HEART and what the model answered, not what HEART
does.

.. _sgi-heart-sources:

Sources
=======

- SGI's *OCTANE Technical Report*,
  :src:`octane-technical-report p. 2-15-2-19`: HEART's processor,
  memory and I/O units, its place between processor, memory and
  Crossbow, and its monitoring of I/O writes for cache coherence.
  Printed pages 2-21 to 2-23 describe memory banks. It gives no register
  values.
- The Owner's Guides :src:`007-3435-004 p. 54` (Octane) and
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
- The same PROM, :src:`ip30-prom-4.17`, and the IRIX 6.5.30 IP30
  kernel it boots, run on the ``octane2`` model with QEMU's trace events
  recording every HEART and :term:`Bridge` register access. These are software
  expectations observed on the model. The trace separates PROM from
  kernel accesses only by a gap in timing after the disk load, so which
  of the two made an access near that boundary is *inferred*, and a
  loader stage in between is not ruled out.
- Reads taken on the reference Octane2 (:ref:`sgi-heart-measurements`).

.. _sgi-heart-identity:

Identity and placement
======================

HEART is Crossbow widget 8, *observed* in a published Octane capture
(:src:`miod-openbsd-sgi-story-4 lines 1308-1352`) and agreed by OpenBSD
``ip30.h`` and Linux. Its processor-interface block starts at physical
``0x0ff00000`` in OpenBSD ``xheartreg.h`` and Linux ``heart.h``, a
*software expectation*; the reference Octane2 answered descriptor reads
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
     - *established*: read on the reference Octane2
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
reads ``0x118`` on revision 4 and ``0x8118`` on revision F: bit 4 marks
processor 0 active, bits 3:0 give widget 8 (Linux names the low nibble
the widget ID) and bit 8 is the bit Linux names big-endian. Revision F
also sets bit 15, which no public source names. The processor ID
register at ``0x50000`` reads zero. The reference Octane2, revision F
with one processor, reads status ``0x8118`` and processor ID zero at
PROM time (:ref:`sgi-heart-octane2-piu`), *established* for that
revision, and the ``octane2`` model matches it. Every IP30 runs
big-endian, so that revision 4 sets bit 8 is *inferred*; the rest of the
revision 4 word is *unknown*, and the ``octane`` word is an
*approximation*. The IRIX 6.5.30 kernel takes the low nibble of status as the
destination widget for PCI DMA, a *software expectation*; with a zero
nibble it would send SCSI DMA to widget 0. On the model the PROM reads
status and the processor ID register repeatedly during startup
(:src:`ip30-prom-4.17 processor status`), a *software expectation*
observed on the model; what it would do with another answer is not
shown.

OpenBSD reads the processor ID register to learn which processor is
running (``include/asm.h`` lines 16-19, ``sgi/locore.S`` lines 93-100
and ``sgi/ip30_nmi.S`` lines 25 and 56-62 under ``sys/arch/sgi`` at
``2df91d80c104``), treating zero as processor 0. Linux names status
bits 7:4 as the processor-active bits and mode bits 63:60 as
processor disables (``arch/mips/include/asm/sgi/heart.h`` lines 158-160
and 230-240 at ``db2ddb871435``) but uses neither. Neither driver reads
processor presence from HEART: both scan the PROM's per-processor
blocks in memory. These are each a *software expectation*.

.. _sgi-heart-registers:

Register map
============

The processor-interface block is 64-bit registers at physical
``0x0ff00000``, big-endian. The model takes 32- and 64-bit accesses;
narrower or unaligned accesses are refused by QEMU's memory core. Names
follow Linux ``heart.h``. Every reset value is the model's choice
(*approximation*); no HEART reset value is published or measured.

.. sgi-registers: base=0x0ff00000
   map=/machine/peripheral/heart:0:0x0ff00000
   qemu="-M none -cpu R10000 -device sgi-heart,id=heart,widget-revision=4"

.. sgi-registers: base=0x0ff00000 variant="revision F"
   map=/machine/peripheral/heart:0:0x0ff00000
   qemu="-M none -cpu R10000 -device sgi-heart,id=heart,widget-revision=6"

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
     - ``0``; ``0x0920000000000000`` on revision F
     - read/write
     - Stored whole. Only bits 0, 1, 13, 15, 19, 20, 21 and 25 change
       behaviour (:ref:`sgi-heart-interrupts`, :ref:`sgi-heart-ecc`).
       Revision F resets the request-limit fields 59:57, 56:54 and
       53:51 to 4, *inferred* (:ref:`sgi-heart-octane2-piu`).
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
     - ``0x118``; ``0x8118`` on revision F
     - read-only
     - Constant (:ref:`sgi-heart-identity`).
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
     - Mask 0 drives processor 0's five levels; masks 1-3 drive level 4
       of processors 1-3.
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
     - Bit 0 corrected and bit 1 uncorrectable memory error, bit 16 widget
       error (:ref:`sgi-heart-dropped-requests`).
   * - ``0x20000``
     - Count
     - 64
     - ``0``
     - read-only
     - 52-bit counter (:ref:`sgi-heart-timer`).
   * - ``0x30000``
     - Compare
     - 64
     - ``0``; ``0xffffff`` on revision F
     - read/write
     - Low 24 bits stored with no effect. The revision F reset value is
       *inferred* (:ref:`sgi-heart-octane2-counter`).
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

.. sgi-registers: base=0x11000000 skip=0x0,0x4,0x20
   map=/machine/peripheral/heart:1:0x11000000
   qemu="-M none -cpu R10000 -device sgi-heart,id=heart,widget-revision=4"

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
   * - ``0x08`` / ``0x0c``
     - Status
     - 64 / 32
     - ``0x60``
     - read-only
     - The 64-bit read of ``0x08`` is the status zero-extended and the
       32-bit read of ``0x0c`` is its low word. A 32-bit read of
       ``0x08`` fails.
   * - ``0x10``, ``0x18``, ``0x30``, ``0x38``, ``0x48``, ``0x50``
     - Error address upper and lower, interrupt destination upper and
       lower, link configuration, flush
     - 64
     - ``0``
     - read-only
     - Constant zero; see the note on 64-bit reads below.
   * - ``0x14``, ``0x1c``
     - Error address upper and lower
     - 32
     - ``0``
     - read-only
     - Constant zero.
   * - ``0x20``
     - Control, 64-bit view
     - 64
     - control word
     - read-only
     - The control word zero-extended; see the note on 64-bit reads below.
   * - ``0x24``
     - Control
     - 32
     - ``0``
     - read/write
     - Stored with no effect. Bits 11:10 are strobes and read zero.
   * - ``0x28`` / ``0x2c``
     - Request timeout
     - 64 / 32
     - ``0``
     - read/write
     - One 32-bit word in both views.
   * - ``0x40``
     - Probe store; error command word, 64-bit view
     - 64
     - ``0``
     - write zero only
     - The widget probe's doubleword zero store completes and is dropped. A 64-bit read is constant zero; see the note on 64-bit reads below. Other widths, other values and 32-bit reads are unimplemented.
   * - ``0x44``
     - Error command word
     - 32
     - ``0``
     - read; write zero only
     - A nonzero write fails.
   * - ``0x60``
     - Error type
     - 64
     - ``0``
     - write-one-to-clear
     - Bits 2 and 3 record dropped writes and reads.
   * - ``0x68``
     - Error mask
     - 64
     - ``0``
     - read/write
     - Low 32 bits stored.
   * - ``0x70``, ``0x78``
     - PIO error address, upper and lower
     - 64
     - ``0``
     - read-only
     - Where a dropped write went.
   * - ``0x90``
     - PIO read time-out address
     - 64
     - ``0``
     - read-only
     - Where a dropped read went.

The Crossbow datasheet places each common widget register on an eight-byte
boundary with its data in bits 31:0, and allows access only by a :term:`Crosstalk`
double-word packet. A 64-bit read of the ID (``0x00``), status (``0x08``)
and request timeout (``0x28``) slots is *established* for one initialized
Octane2 with HEART revision F (``0x6c00106d``, ``0x60`` and ``0x28``).
The 64-bit reads of the other common slots (``0x10``, ``0x18``, ``0x20``,
``0x30``, ``0x38``, ``0x40``, ``0x48``, ``0x50``) were not measured. The
model answers them with the 32-bit register zero-extended, and zero where
it holds no register. That is an *inferred* approximation from the slot
layout, not a hardware claim. The block ends at ``0x58``. A 64-bit read
at ``0x58`` and a 32-bit read of a low word not listed above fail. Any
64-bit write to these slots fails other than a zero store to ``0x40``,
which succeeds and is dropped.

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

The widget status word reads ``0x60`` on both machines. The
reference Octane2 returned exactly that value after PROM
startup (:ref:`sgi-heart-octane2-status`), so the value is
*established* for that board at PROM time. It is not a reset value, and
what the bits mean is *unknown*. The stock 6.5.30 diagnostic reads the
slot as 64 bits and expects bits 5 and 6 set and bits 7-15 clear, a
*software expectation*. The Octane's revision 4 status is unmeasured, so
the ``octane`` answering ``0x60`` is an *approximation* that lets the
diagnostic's read complete. The 32-bit read of the low word at ``0x0c``
returns the same value. A 32-bit read of ``0x08``, the reset value and
any write are *unknown* and fail on both machines.

The IP30 PROM reads, modifies and writes the 32-bit control word at
``0x24``, a *software expectation*. On the model it reads zero and
writes ``0x4008``, and later reads that back and writes ``0x4c08``
(:src:`ip30-prom-4.17 widget registers`). Linux ``xwidget.h`` names bits
11 and 10 the receive and transmit retry-counter clears, and the same
bits of the Bridge's control register read back zero, so the model
treats them as strobes: a write stores the other 30 bits and those two
read zero, *inferred*. The PROM's ``0x4c08`` then reads back as
``0x4008``. Zero reset and the writability of the other bits are
each an *approximation*. The reference Octane2 reads ``0x4218`` at PROM time
(:ref:`sgi-heart-octane2-widget-more`). Bits 11:10 match. Bits 9 and 4
are set there and clear on the model; whether the chip resets them set
or the PROM writes them on hardware is *unknown*.

The request-timeout word is one stored 32-bit value. A 32-bit access at
``0x2c`` and a 64-bit access at ``0x28`` reach it; a 64-bit write keeps its
low 32 bits and a 64-bit read returns zero in the upper half. On the model
the PROM writes ``0x28`` to the 32-bit word
(:src:`ip30-prom-4.17 widget registers`), and the IRIX 6.5.30 IP30 kernel,
observed on the model, later writes ``0x9c40`` and reads it back. Each is
a *software expectation*. Neither writes the 64-bit view. The reference
Octane2 read ``0x28`` after PROM startup (:ref:`sgi-heart-octane2-timeout`);
that its PROM wrote that value is *inferred* from the same value on the
model. The unit and the effect of the timeout are *unknown*; the model times
out nothing.

.. _sgi-heart-widget-errors:

Widget error registers
----------------------

No public source defines HEART widget registers at ``0x60``, ``0x68``,
``0x70``, ``0x78`` or ``0x90``. Linux and OpenBSD define ``0x44`` only
as the common widget error command word
(``arch/mips/include/asm/xtalk/xwidget.h`` line 26 at ``db2ddb871435``,
``sys/arch/sgi/xbow/widget.h`` lines 22-53 at ``2df91d80c104``).

On the model the PROM writes ``0x3ff7fc0f`` to the error mask at
``0x68``, later writes zero to ``0x44``, writes ``0x3ff7fc0f`` to error
status at ``0x60`` and acknowledges pending bits 51-58
(:src:`ip30-prom-4.17 widget registers`). The IRIX 6.5.30 IP30 kernel,
observed on the model, writes zero to the mask, reads status (zero on
the model) and writes zero to it, zeroes ``0x44``, reads cause and
writes zero to it, then writes ``0x3ff62c07`` to the mask. These are
each a *software expectation*; they suggest
write-one-to-clear status and a writable mask. The reference Octane2
read zero at ``0x60`` and ``0x3ff7fc0f`` at ``0x68`` after PROM startup
(:ref:`sgi-heart-octane2-widget`); that the PROM wrote that mask is
*inferred* from the same value on the model. What each bit means is
*unknown*.

The only error source the model has is a request the Crossbow drops
(:ref:`sgi-heart-dropped-requests`). A 64-bit write to the error type
clears the bits it sets. The mask stores the low 32 bits of a 64-bit
write; bits 2 and 3 enable pending bit 63 for a dropped request, and no
other bit drives anything. The error command word reads zero and takes
only a zero write.

After a bus error, including the one a guarded probe of an absent PCI
device takes, the IRIX 6.5.30 kernel reads ``0x60`` (64-bit), ``0x44``,
``0x14`` and ``0x1c`` (32-bit), then ``0x90``, ``0x70`` and ``0x78``
(64-bit), a *software expectation*. Observed on the model, the kernel
reads each of these, and processor-interface ``0x88``, ``0x90``,
``0x98``, ``0xa0`` and ``0xa8``, at least three times before the login
prompt. Linux names
``0x14``, ``0x1c`` and ``0x44`` as common widget registers, and the
processor-interface words as bus-error address and miscellaneous,
memory-error address and data, and processor-interface access error,
with no field layout (``arch/mips/include/asm/sgi/heart.h`` lines 40-43
and 97-102). The model answers every one of
these reads with zero unless a request the Crossbow dropped set them,
which the kernel treats as no recorded error, an *approximation*: what
HEART records after a Bridge timeout is *unknown*.
At the reference Octane2's PROM menu, with no error pending, the widget
words ``0x1c``, ``0x44``, ``0x60``, ``0x70``, ``0x78`` and
``0x90`` read zero, matching the model. The processor-interface words do
not: ``0x88`` reads ``0x000000ffffffffff``, ``0x90`` ``0x7fffff`` and
``0x98`` ``0x20f30688`` (:ref:`sgi-heart-octane2-piu`).

.. _sgi-heart-dropped-requests:

Requests nothing answers
------------------------

A processor request HEART sends out of widget 8 that the Crossbow drops,
to an empty or dead port, an illegal widget or HEART's own port, gets no
response. SGI's *Octane ide Reference Guide* (108-0217-002, printed page
A-3) shows a read of empty widget 13 (``0x900000001d00002c``) taking a
data bus error and leaving widget error type ``0x8`` (PIO read time-out),
``0xd000`` at ``0x90``, cause ``0x10000`` and pending bit 63, with the
processor's IP6 set, *established* for that read. That a store fails the
same way, by HEART's own timer, and sets type bit 2 (PIO write time-out)
is *inferred* from the names IRIX gives the type bits. The layouts of
``0x70``, ``0x78`` and ``0x90`` are *inferred* from how IRIX decodes
them.

The model takes the Crossbow's report of the drop (the :term:`XIO` port
interface's ``request_dropped``) and:

- for a store, sets type bit 2 and latches the processor's physical
  address: bits 39:32 in ``0x70`` bits 7:0, the processor number in
  ``0x70`` bits 23:22 and bits 31:0 in ``0x78``. Bit 15 of ``0x70``,
  which IRIX reads as a read-or-write flag of unknown sense, stays
  clear. The store itself completes, as every R10000-family uncached
  store does in this tree.
- for a read, sets type bit 3 and latches the processor number in
  ``0x90`` bits 19:18, the widget in bits 15:12 and widget offset bits
  23:12 in bits 11:0. The read still fails, so the processor takes a
  data bus error.
- sets cause bit 16, and pending bit 63 if that type bit is set in the
  error mask at ``0x68``. Bit 63 then reaches IP6 of every processor
  whose mask arms it.

Writing the type bits back clears them, and writing cause bit 16 back
clears it, without touching bits 0 and 1. The latches keep their words
until the next error of their kind. The Crossbow hands HEART an XIO
address, not the window the processor used: an offset of less than
16 MiB is latched as a small-window address and anything larger as a
large-window one, so a large-window access below 16 MiB to a link that
went down is latched with its small-window address. The processor
number is zero with one processor fitted.

Three choices here are approximations, each with a register test that
fails without it:

- The report is made while the request is dropped. HEART's time-out
  interval is *unknown*; the IRIX kernel's guarded store probe finds cause
  bit 16 set after a few register reads and a ``sync``, so the report is
  visible by then, but nothing bounds it from below
  (``/heart/drop/no-delay``).
- The first error of each kind is held: a second dropped store before
  type bit 2 is cleared leaves ``0x70`` and ``0x78`` alone, and the
  same holds for a dropped load and ``0x90``. Hardware may overwrite instead
  (``/heart/drop/hold-and-clear``).
- Mode bit 13 does not gate a widget error's bit 63, though it still
  gates bit 63 set by a memory error or by software. The PROM sets bit
  13 before any guest runs, so whether HEART applies it to widget
  errors is *unknown* (``/heart/drop/ignores-mode``).

The error mask enabling only pending bit 63, with the type bit and
cause bit 16 latched whatever it holds, is *inferred* from its name
(``/heart/drop/error-mask``). Whether the Crossbow also flags a request
to a dead link is *unknown* and nothing models it.

.. _sgi-heart-memory:

Memory bank descriptors and the PROM's sizing probe
====================================================

The eight 32-bit descriptors at ``0x20``-``0x3c`` follow OpenBSD
``xheartreg.h`` and Linux ``heart.h``, which agree: bit 31 marks a valid
bank, bits 24:22 are a density code, bits 21:16 are one less than the bank's
size in 32 MiB units, and bits 8:0 are its base in 32 MiB units above
``0x20000000``. Neither public kernel uses the density code to size a bank.
A 32-bit access reaches any word; a 64-bit access at ``0x20``, ``0x28``,
``0x30`` or ``0x38`` reaches two words, the lower offset in the upper half.
Words 0-3 map banks 0-3; words 4-7 are stored and map nothing. Linux reads
only words 0-3 and its header notes eight slots in four banks of two
(``arch/mips/include/asm/sgi/heart.h`` lines 15-19 and 72-77,
``arch/mips/sgi-ip30/ip30-setup.c`` lines 48-79 at ``db2ddb871435``).
OpenBSD reads all eight (``sys/arch/sgi/sgi/ip30_machdep.c`` lines 94-140 at
``2df91d80c104``). The six-bit size field allows at most 2 GiB a bank.
On revision F words 4-7 answer 32-bit reads with zero
(:ref:`sgi-heart-octane2-piu`). Whether HEART maps a bank through them is
*unknown*. Each Owner's Guide numbers its
banks 1 to 4; that guide bank 1 is descriptor 0 is *inferred*, not a
measured wiring fact.

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
each an *approximation*.

The PROM sizes each bank itself, a *software expectation* from the
shipped IP30 PROM, observed on the model
(:src:`ip30-prom-4.17 memory sizing`). On the model it touches only
words 0-3. It clears the four descriptors with two 64-bit
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
bank first and writes the final descriptors. While it probes banks 1-3
it parks bank 0's descriptor invalid at base ``0x1ff``. On a 256 MiB
``octane2`` the sequence ends with ``0x81470000`` in word 0 and zero in
words 1-3, the value the reference Octane2 reports. The IRIX 6.5.30 kernel
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

These are model results, not measurements. The reference Octane2 PROM left
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
values ``(index << 16) | 0x21`` to SDRAM mode
(:src:`ip30-prom-4.17 mode writes`), a *software expectation*
observed on the model. Every mode write is a read-modify-write. Later
it sets mode bits 61-63, which Linux names processor disables for
processors 1-3, then bit 17 (Linux ``HM_CACHED_PROM_EN``), bit 13
(``HM_INT_EN``), and finally bits 0-3, 5-8, 10, 12, 14-16 and 25,
leaving ``0xe00000000203fdff``. The reference Octane2's PROM leaves
``0xe92000000203fdff``: the same bits plus 4 in each of the three
request-limit fields at bits 59:51. The model resets those fields to 4
on revision F, so its run leaves the same word, and to zero on revision
4 (:ref:`sgi-heart-octane2-piu`). Linux ``heart.h`` names bits 0-10 and 16
error enables, of which the PROM leaves bit 9 clear, bit 12
``HM_DATA_CHK_EN``, 14 ``HM_IO_COH_EN``, 15 ``HM_GLOBAL_ECC_EN`` and 25
``HM_COR_ECC_LCK`` (``arch/mips/include/asm/sgi/heart.h`` at
``db2ddb871435``). The IRIX 6.5.30 IP30 kernel, observed on the model,
reads that value and does not change it. The model stores these writes with no
electrical effect, except as :ref:`sgi-heart-interrupts` and
:ref:`sgi-heart-ecc` describe.

.. _sgi-heart-interrupts:

Interrupts
==========

OpenBSD ``xheartreg.h`` and Linux ``heart.h`` group the 64 pending bits
into five levels: bits 0-15 level 0, 16-31 level 1, 32-49 level 2, bit
50 level 3 (the timer) and 51-63 level 4 (errors; bit 63 is HEART's own
exception). Linux sends levels 0-4 to CPU interrupt lines IP2-IP6. Both
drivers set, clear and mask bits through the registers in the map and
read pending separately from the mask. Each is a *software
expectation*. At start-up Linux writes zero to all four masks,
acknowledges every bit through ``0x10028`` and adds the error sources
``0x8ff8000000000000`` to processor 0's mask: the HEART exception, its
own bus error and the Crossbow and widget errors
(``arch/mips/sgi-ip30/ip30-irq.c`` lines 271-286 at ``db2ddb871435``).
OpenBSD acknowledges everything first, then zeroes the masks, and never
enables bits 50-63 (``sys/arch/sgi/xbow/xheart.c`` lines 157-177 at
``2df91d80c104``). Both send inter-processor interrupts by writing a bit
to ``0x10020``. OpenBSD names bit 41 the power button, 42 + *n* debug
and 46 + *n* the IPI for processor *n*. Linux never writes ``0x10040``;
no public source says how a cause bit clears.

On the model, the PROM zeroes masks 0-2 after walking a one through all 64
bits of mask 3, then enables bit 41, the processor 0 bus error (59) and the
HEART exception (63) in mask 0. It polls pending rather than enabling a
source it is waiting on, reads cause, writes zero to it and acknowledges
through ``0x10028``. It later writes ``0x8c00020000000039`` to mask 0,
adding bit 58 and device bits 0 and 3-5, and toggles bit 41 off and on
(:src:`ip30-prom-4.17 interrupt masks`). The IRIX 6.5.30 IP30 kernel,
observed on the model, clears all four masks, writes ``0x8800440000000000``
(bits 42, 46, 59, 63) to processor 0's mask, adds bits 16, 17, 41, 57 and 58
and the level-0 device bits 3-6 as it attaches, reads ``0x10038`` in its
dispatcher and acknowledges a source by writing its bit to ``0x10028``. It
writes ``0x10020`` only for device bits, reads cause and writes zero to it,
and leaves masks 1-3 at zero. These are each a *software expectation*. At the
reference Octane2's PROM menu mask 0 reads ``0x8c0002000000003d``, with
device bit 2 set beside the model run's bits, masks 1-3 read zero and
pending holds the timer bit 50 (:ref:`sgi-heart-octane2-interrupts`).

The model drives five outputs to processor 0's IP2-IP6. Each is high
while some bit of its level is both pending and set in mask 0. Bit 63
also needs mode bit 13 (interrupt enable, Linux ``HM_INT_EN``) before it
reaches IP6. A masked bit stays pending; unmasking it raises the line
again without a new event. Three more outputs carry level 4 (IP6) of
processors 1-3, each under the same rule with that processor's mask:
Linux arms its second processor's own bus-error bit, 60, in mask 1, and
the IRIX 6.5.30 kernel arms bit 63 in the mask of every processor it
starts, so an error reaches every processor that armed it. Levels 0-3
of processors 1-3 drive nothing. These output and retention rules are
each an *approximation* compatible with the drivers; HEART's assertion
timing, and which processor takes an error several have armed, are
*unknown*.

Devices raise interrupts with a :term:`Crosstalk` write to HEART's widget offset
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
trigger at ``0x40000``; Linux agrees on width and rate. Each is a *software
expectation*. The reference Octane2's counter advanced at 12.5011 MHz
against a host clock, to about 0.1% (:ref:`sgi-heart-octane2-counter`), so
12.5 MHz is *established* for revision F. The model's count advances one
tick per 80 ns of virtual time from zero at reset and wraps at 52 bits,
*approximation* for the phase: the chip's starting value is *unknown*.

Linux uses the counter as its clocksource and calibrates the processor
clock by waiting 1,250,000 ticks for it to advance
(``arch/mips/sgi-ip30/ip30-timer.c`` lines 22-48 and ``ip30-setup.c``
lines 84-102 at ``db2ddb871435``); OpenBSD uses it as a timecounter
(``sys/arch/sgi/xbow/xheart.c`` lines 80-88 at ``2df91d80c104``).
Neither programs compare, trigger or the timer bit. On the model the
PROM reads the counter in wait loops, the longest about 6,000,000 ticks,
and never touches compare (:src:`ip30-prom-4.17 counter`). These are
each a *software expectation*; all of them need the counter to advance.

Compare stores the low 24 bits of a 64-bit write and reads them back.
It raises no interrupt and does not touch the counter. The trigger
register is not modelled. The compare and reload rule is *unknown*; no
public source gives one. The reference Octane2 reads compare as
``0xffffff`` with pending bit 50 set at its PROM menu, which the PROM does
not write on the model, so that ``0xffffff`` is the reset value and that a
match sets bit 50 are both *inferred* (:ref:`sgi-heart-octane2-counter`).
The model resets compare to ``0xffffff`` on revision F and to zero on
revision 4, and never sets bit 50. The stock diagnostic's register test writes
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
each an *approximation*; HEART's real slot timing is *unknown*. The reference
Octane2 reads the divider as ``0x3131`` and control as ``0x3`` at its PROM
menu (:ref:`sgi-heart-octane2-piu`). A command written
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
placements are each a *software expectation*. None of these sources
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

The model refuses, without changing RAM or any record, an injection without its
enables or together with bit 21, any partial or unaligned write while bit 19,
20 or 21 is set, a partial or unaligned access touching a faulty word, a read
of a faulty word while bit 20 or 21 is set, a read of a double-bit faulty word
while bit 19 is set. It also refuses any injection, forced write or faulty-word
access while bit 25 is set, both generation bits at once, a DMA access touching
a faulty word, a DMA write while either generation bit is set, and a descriptor
write while any word is faulty. Each refusal logs ``LOG_UNIMP`` and fails the
transaction.

A write the checked path accepts, including a forced write with bit
21 set, stores its data in RAM, an *approximation*: no public source
says what a forced write does to the data. Code fetched from those
bytes afterwards sees the new contents on the model, because it has no
instruction cache and discards the code it translated from them. What
the R10000's instruction cache holds after such a store on real
hardware is *unknown*.
The 6.5.30 IP30 PROM, observed on the model after a machine reset,
writes test patterns this way with bit 21 set over the first page of
SDRAM, which holds the exception vectors in the low window, before it
copies its own handlers there, a *software expectation*. Before the
model discarded those translations, the processor kept running the
previous system's vector code and hung after "Running power-on
diagnostics".

.. _sgi-heart-dma:

PCI DMA through Crossbow
========================

The technical report has HEART handle I/O requests to memory and
monitor them for coherence (:src:`octane-technical-report p. 2-15`),
but no public source gives HEART's request decode, response format or
coherence rules. Both public drivers write ``0x00800000`` to the onboard
Bridge's direct-map register: widget 8, offset 0, the 512 MiB add bit
clear (Linux ``arch/mips/pci/pci-xtalk-bridge.c`` line 695 at
``db2ddb871435``, OpenBSD ``sys/arch/sgi/xbow/xbridge.c`` lines
2048-2068 at ``2df91d80c104``). OpenBSD then adds ``0x80000000`` to a
physical address to make a PCI DMA address, so the Bridge passes the
physical address to widget 8 unchanged (:doc:`bridge`). Linux instead
issues 64-bit DMA addresses, ``0x8100000000000000`` plus the physical
address, with widget 8 in bits 63:60 (``pci-xtalk-bridge.c`` lines 29-40
and 663). These are each a *software expectation*.

On the model the PROM writes the same ``0x00800000`` and all of its DMA
uses the 32-bit direct window (:src:`ip30-prom-4.17 Bridge direct map`).
The reference Octane2's Bridge holds ``0x00800000`` at its PROM menu
(:ref:`sgi-bridge-octane2-status`), *established* as that firmware's
setting.
The IRIX 6.5.30 IP30 kernel, observed on the model, writes
``0x00820000``, which sets the bit Linux names ``BRIDGE_DIRMAP_ADD512``,
but all of its DMA before the login prompt uses 64-bit addresses to
widget 8. These are each a *software expectation*. On the model, either way
the physical address arrives at HEART unchanged.

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
each an *approximation*; the chip's power-on state is *unknown*.

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
(widget revision field 6), in the PROM :term:`Command Monitor` after the
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
   * - ``0x1800002c``
     - 32
     - ``0x28``
     - widget request timeout, 32-bit view
     - F (field 6)
     - PROM monitor, after initialisation
   * - ``0x18000010``
     - 64
     - ``0``
     - widget register ``0x10``
     - F (field 6)
     - PROM monitor, after initialisation
   * - ``0x1800001c``, ``0x18000044``
     - 32
     - ``0``, ``0``
     - widget error address lower, error command word
     - F (field 6)
     - PROM monitor, after initialisation
   * - ``0x18000024``
     - 32
     - ``0x4218``
     - widget control
     - F (field 6)
     - PROM monitor, after initialisation
   * - ``0x18000070``, ``0x18000078``, ``0x18000090``
     - 64
     - ``0``, ``0``, ``0``
     - HEART-specific widget error words
     - F (field 6)
     - PROM monitor, after initialisation
   * - ``0x0ff00000``
     - 64
     - ``0xe92000000203fdff``
     - mode
     - F (field 6)
     - PROM monitor, after initialisation
   * - ``0x0ff00030``-``0x0ff0003c``
     - 32, four words
     - ``0``, ``0``, ``0``, ``0``
     - bank descriptors 4-7
     - F (field 6)
     - PROM monitor, after initialisation
   * - ``0x0ff00050``, ``0x0ff00058``
     - 64
     - ``0x000fff001b100000``, ``0``
     - flow-control addresses
     - F (field 6)
     - PROM monitor, after initialisation
   * - ``0x0ff00080``
     - 64
     - ``0x8118``
     - status
     - F (field 6)
     - PROM monitor, after initialisation
   * - ``0x0ff00088``, ``0x0ff00090``
     - 64
     - ``0x000000ffffffffff``, ``0x00000000007fffff``
     - bus-error address, bus-error miscellaneous
     - F (field 6)
     - PROM monitor, after initialisation
   * - ``0x0ff00098``, ``0x0ff000a0``, ``0x0ff000a8``
     - 64
     - ``0x0000000020f30688``, ``0``, ``0``
     - memory-error address, memory-error data, access error
     - F (field 6)
     - PROM monitor, after initialisation
   * - ``0x0ff000b0``, ``0x0ff000b8``
     - 64
     - ``0x3131``, ``0x3``
     - MicroLAN clock divider, MicroLAN control
     - F (field 6)
     - PROM monitor, after initialisation
   * - ``0x0ff10000``-``0x0ff10018``
     - 64, four words
     - ``0x8c0002000000003d``, ``0``, ``0``, ``0``
     - interrupt masks 0-3
     - F (field 6)
     - PROM monitor, after initialisation
   * - ``0x0ff10030``, ``0x0ff10038``, ``0x0ff10040``
     - 64
     - ``0x0004000000000000``, ``0``, ``0``
     - pending, masked pending, cause
     - F (field 6)
     - PROM monitor, after initialisation
   * - ``0x0ff20000``
     - 64
     - four reads, ``0x29889feecb`` to ``0x29b06c17df``
     - counter
     - F (field 6)
     - PROM monitor, after initialisation
   * - ``0x0ff30000``
     - 64
     - ``0xffffff``
     - compare
     - F (field 6)
     - PROM monitor, after initialisation
   * - ``0x0ff50000``
     - 64
     - ``0``
     - processor ID
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

The value after PROM startup, not a cold-reset value. A later 32-bit read
at ``0x2c`` returned the same word, so the two views agree on
revision F:

.. code-block:: text

   >> dump -w -x 0xffffffffb800002c#1
   0xffffffffb800002c:         28

Neither read shows write behaviour.

.. _sgi-heart-octane2-widget-more:

Other widget registers
----------------------

.. code-block:: text

   >> dump -d -x 0x9000000018000010#1
   0x9000000018000010:                  0
   >> dump -w -x 0xffffffffb800001c#1
   0xffffffffb800001c:          0
   >> dump -w -x 0xffffffffb8000024#1
   0xffffffffb8000024:       4218
   >> dump -w -x 0xffffffffb8000044#1
   0xffffffffb8000044:          0
   >> dump -d -x 0x9000000018000070#2
   0x9000000018000070:                  0                  0
   >> dump -d -x 0x9000000018000090#1
   0x9000000018000090:                  0

A 64-bit read of widget ``0x10`` completes and returns zero on revision
F. The model returns zero there on every revision; that revision 4 does
so is *inferred*. The error address, error command and HEART-specific
error words read zero with no error recorded, as on the model. The
control word reads ``0x4218``: bits 14, 9, 4 and 3. Its low nibble is 8,
HEART's widget number. The model's run of the same PROM leaves
``0x4008`` (:ref:`sgi-heart-widget-registers`): bits 11 and 10 are clear
on both, and bits 9 and 4 are set on hardware only. Whether the chip
resets those two set, or the PROM writes them, is *unknown*.

.. _sgi-heart-octane2-piu:

Processor-interface registers
-----------------------------

.. code-block:: text

   >> dump -d -x 0x900000000ff00000#1
   0x900000000ff00000:   e92000000203fdff
   >> dump -d -x 0x900000000ff00080#8
   0x900000000ff00080:               8118         ffffffffff
   0x900000000ff00090:             7fffff           20f30688
   0x900000000ff000a0:                  0                  0
   0x900000000ff000b0:               3131                  3
   >> dump -w -x 0xffffffffaff00030#4
   0xffffffffaff00030:          0          0          0          0
   >> dump -d -x 0x900000000ff00050#2
   0x900000000ff00050:      fff001b100000                  0
   >> dump -d -x 0x900000000ff50000#1
   0x900000000ff50000:                  0

Mode ``0xe92000000203fdff`` matches the word the model's run of the PROM
leaves, ``0xe00000000203fdff``, in every bit but 59, 56 and 53. Under
Linux ``heart.h`` those are one bit in each of three three-bit fields,
maximum processor requests (59:57), maximum I/O requests (56:54) and
maximum pending I/O requests (53:51), each reading 4. Every mode write
the model saw the PROM make is a read-modify-write that leaves these
fields alone (:ref:`sgi-heart-memory`), so that HEART resets each field
to 4 is *inferred*. The reads were taken after firmware initialisation in
a session in which IRIX had also run, so a kernel write is not ruled
out. The model resets the fields to 4 on revision F and reads the
measured word after its PROM run.

Status ``0x8118`` decodes under Linux ``heart.h`` as widget number 8 in
bits 3:0, processor 0 active in bit 4 and big-endian in bit 8, with the
direct-connect (11), Triton (10) and R4000 (9) bits clear. Bit 15 is
set and no public source names it. The word is *established* for
revision F with one processor at PROM time, and the ``octane2`` model
returns it.

The bus-error address word reads ``0x000000ffffffffff`` and the
bus-error miscellaneous word ``0x00000000007fffff``. The memory-error
address word reads ``0x20f30688``, a physical address inside the one
fitted bank, with cause zero. The memory-error data and access-error words
read zero. No public source gives these fields, so what the nonzero
values record is *unknown*. They show that, on this board, these words
are not zero when no error is pending.

Descriptor words 4-7 answer 32-bit reads with zero. That shows the
offsets decode as storage or as zero, not whether HEART maps a bank
through them. The flow-control addresses read ``0x000fff001b100000`` and
zero; whether the PROM wrote the first is not shown. The processor ID
register reads zero from processor 0. All of these reads are *observed*.

The MicroLAN divider reads ``0x3131`` and control ``0x3``: bit 1, slot
done, and bit 0, the wire sampled high, after the PROM's last slot, *observed*.

.. _sgi-heart-octane2-interrupts:

Interrupt registers
-------------------

.. code-block:: text

   >> dump -d -x 0x900000000ff10000#4
   0x900000000ff10000:   8c0002000000003d                  0
   0x900000000ff10010:                  0                  0
   >> dump -d -x 0x900000000ff10030#3
   0x900000000ff10030:      4000000000000                  0
   0x900000000ff10040:                  0

Mask 0 enables bits 63, 59, 58, 41, 5, 4, 3, 2 and 0; masks 1-3 are
zero. The model's run of the same PROM leaves ``0x8c00020000000039``
(:ref:`sgi-heart-interrupts`), without bit 2. Pending holds bit 50 only,
the level 3 timer bit, which mask 0 does not enable. Masked pending reads
zero, consistent with pending AND mask 0. Cause reads zero.

.. _sgi-heart-octane2-counter:

Counter and compare
-------------------

.. code-block:: text

   >> dump -d -x 0x900000000ff20000#1
   0x900000000ff20000:         29889feecb
   >> dump -d -x 0x900000000ff20000#1
   0x900000000ff20000:         298eab68e5
   >> dump -d -x 0x900000000ff20000#1
   0x900000000ff20000:         299a0bd547
   >> dump -d -x 0x900000000ff20000#1
   0x900000000ff20000:         29b06c17df
   >> dump -d -x 0x900000000ff30000#1
   0x900000000ff30000:             ffffff

The counter advanced 375,407,256 ticks between the third and fourth reads,
over 30.03 s of host time: 12.5011 MHz, with keystroke timing limiting
the precision to about 0.1% (:ref:`sgi-measured-octane2-counter`). The
counter rate of 12.5 MHz is therefore *established* for this board. The
values say nothing about the counter's starting point.

Compare reads ``0xffffff``. The model saw the PROM never touch compare
(:ref:`sgi-heart-timer`), so that ``0xffffff`` is the chip's reset value
is *inferred*, and the model's revision F resets to it. Pending bit 50 was set while the counter's low 24 bits
had passed that value many times. That a compare match sets bit 50 is
*inferred* from this and the bit's public name, not shown by these
reads.

.. _sgi-heart-limits:

Model limits
============

- One processor. Status and processor ID follow that profile. Masks 1-3 drive
  only level-4 outputs, which no machine connects, and inter-processor
  interrupts have no second target.
- Mode bits other than 0, 1, 13, 15, 19, 20, 21 and 25 are stored with
  no effect, including the reset, refresh, cached-PROM and processor
  disable bits Linux names. Linux restarts the machine by setting mode
  bit 23 (``arch/mips/sgi-ip30/ip30-power.c`` lines 13-22); the model
  does not reset on that write.
- The only error source is a request the Crossbow drops: bus-error
  address and miscellaneous words, the processor-interface access error
  word and widget error type bits other than 2 and 3 never record
  anything. Bridge errors do not reach HEART.
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
  ``LOG_UNIMP`` and fail, so the processor takes a bus error.
- The bus-error address, bus-error miscellaneous and memory-error address
  words at ``0x88``, ``0x90`` and ``0x98`` read zero, where revision F
  reads nonzero values at PROM time with no error pending
  (:ref:`sgi-heart-octane2-piu`).
- A compare match never sets pending bit 50, which revision F's reads
  suggest it does (:ref:`sgi-heart-octane2-counter`).
- Widget control bits 9 and 4 reset to zero and the PROM leaves them
  clear, where revision F reads them set
  (:ref:`sgi-heart-octane2-widget-more`).
- Busy one-wire replacement logs ``LOG_GUEST_ERROR``.
- The Octane's HEART revision 4 has no measured widget status word or
  processor-interface status word. The ``octane`` machine answers ``0x60``
  and ``0x118``, each an *approximation*.
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
and mask behaviour on all five levels, the level-4 outputs of processors
1-3 (``/heart/processor-error-lines``), processor status on both revisions,
the revision F mode and compare reset values
(``/heart/revision-f-reset-words``), the one-wire master
with and without a part, slot replacement, Crosstalk routing and DMA through
a Bridge, the ECC subsets, reset and migration. ``/heart/drop/*`` checks
requests the Crossbow drops on HEART's port: the type bits, the latched
addresses and processor number for both windows, cause, pending bit 63
under the error mask, routing to each processor's mask, write-one-to-clear
and re-arming, the PROM and IRIX clear sequences, reads against writes,
the three approximations above, reset and migration.

``tests/qtest/sgi-heart-cpu-test.c`` runs code on R10000 and R12000
processors: a DMA into RAM the processor then executes, a forced-write
ECC store over code the processor is running
(``/heart/cpu/ecc-write-code``), and double-bit
ECC reads that must take the data bus error exception with the right
EPC and branch-delay state, leave the destination register untouched,
and succeed after repair. ``/heart/cpu/*/dropped-requests`` stores to
empty widget 14 and finds the store retired with IP6 pending and cause
bit 16 set after a HEART read and a ``sync``, then loads from empty
widget 13 and takes the data bus error.

``tests/qtest/octane-test.c`` checks HEART inside the ``octane`` and
``octane2`` machines: identity at both widths and the refused widths
(``/octane/heart-identity``, ``/octane2/heart-identity``), the Octane2 status
word, the measured status, mode, compare and widget ``0x10`` words across a
reset (``/octane2/heart-measured-words``), widget control and error registers.
It also covers requests to empty widgets 13 and 14 reaching HEART's report
(``/octane/heart-dropped-requests``, ``/octane2/heart-dropped-requests``), the
stored diagnostic registers on both machines, one-wire timing with the divider
written, HEART's level 0-2 outputs, and SCSI DMA from a QLogic controller into
HEART RAM.
