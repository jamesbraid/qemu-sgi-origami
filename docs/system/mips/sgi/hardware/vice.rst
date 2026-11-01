.. SPDX-License-Identifier: BSD-3-Clause

.. _sgi-vice:

========================================
VICE: video and image compression engine
========================================

Used in :ref:`sgi-ip32` (O2). VICE is a media processor on the
processor's system bus (SysAD), which it shares with the CPU
(:src:`099-0123-003 p. 20, 97`), *established*. It talks to CRIME over
handshake pins. It holds a scalar and vector signal processor (the MSP), a
bitstream processor (the BSP) and two DMA channels that move image data
between its own memories and main memory
(:src:`099-0123-003 p. 15-21`), *established*. QEMU models it as the
``sgi-vice`` sysbus device in ``hw/misc/sgi-vice.c``, with the MSP's
scalar unit in ``hw/misc/sgi-vice-msp.c`` and
``include/hw/misc/sgi-vice-msp.h``. No machine fits it. The ``o2``
machine in ``hw/mips/sgi/o2.c`` does not create it, and no other machine
does. The device is built only when ``CONFIG_SGI_VICE`` is set, which the
qtest fixture configuration selects, and the qtests reach it with
``-M none -device sgi-vice,...`` (see `Tests`_).

The model follows the *VICE Design Specification*. It covers the host
register file, the six local memories, the address translation buffer,
both DMA channels, the interrupt block and the MSP's scalar unit. The
vector unit and the BSP are not modelled. Anything the model cannot
perform stops the machine instead of returning a made-up result
(`Model limits`_).

Facts are rated as described in :ref:`sgi-ratings`.

.. _sgi-vice-sources:

Sources
=======

- The *VICE Design Specification*, document 099-0123-003, version 1.0,
  17 April 1997, :src:`099-0123-003`. It describes the third VICE
  revision, C. Citations on this page give the printed page
  number, which equals the page number of the PDF. The specification is
  marked proprietary and confidential in its own header. This page uses
  its prose, tables and field diagrams only.
- The CRIME 1.5 specification, :src:`sgi-crime-1.5-spec`, for the CRIME
  interrupt bit that carries VICE.
- Linux ``arch/mips/include/asm/ip32/crime.h`` at ``db2ddb871435``, which
  names that bit, as a software expectation. No public Linux, NetBSD or
  OpenBSD driver for VICE was found.
- This page uses no measurement of a physical machine.

.. _sgi-vice-identity:

Identity and placement
======================

The chip has a read-only ID register at offset ``0x08`` whose reset value
is ``0xe1`` (:src:`099-0123-003 p. 40`), *established*. The same page
set lists three revisions in bits 3:0, 1, 2 and 3 for revisions A, B and
C, with the ID value in bits 7:4 left blank
(:src:`099-0123-003 p. 44`). Whether a revision C part reads ``0xe1`` or ``0xe3``
is therefore *unknown*. The specification's own table says ``0xe1``. The
model returns ``0xe1`` and requires the ``part-id`` property to be 225
(``0xe1``); any other value fails realize.

Two pins, ``VICE_ID(1:0)``, select one of four 1 MiB windows from
``0x17000000`` to ``0x173fffff`` on the system bus
(:src:`099-0123-003 p. 22-23`), *established*. The window of a lone VICE
is ``0x17000000``. The specification says several VICE chips on one bus
were never tested. The model has no fixed address. It exposes one
1 MiB memory region, ``vice-host``, and the creator maps it. The qtests
map it at ``0x17000000``.

CRIME collects the VICE interrupt in bit 31 of its interrupt status,
routed to processor line IP2 (:src:`sgi-crime-1.5-spec PDF p. 106`),
*established*. Linux names that bit ``CRIME_VICE_INT``, *software
expectation*. The ``sgi-vice`` device has one output interrupt line.
Nothing connects it to :doc:`crime`, which does not model the VICE
interrupt.

The device has these properties:

``part-id``
  Must be 225. Default 0, which fails realize.
``device-clock-hz``
  Clock of the DMA engine and the MSP pipeline. Required, nonzero. The
  specification gives the internal VICE clock as 66 MHz
  (:src:`099-0123-003 p. 97`), *established*, but the model has no
  built-in default.
``msp-clock-hz``
  Clock of the free-running counter. Required, nonzero. The
  specification assumes 66 MHz (:src:`099-0123-003 p. 53`),
  *established*.
``stable-reference``
  Boolean, default off. Declares that ``device-clock-hz`` is a stable
  reference, so the model can measure the 16-clock minimum reset
  (`Reset`_). With it off the model never admits a reset release, so no
  DMA or MSP program can start.
``dma-memory``
  Link to a ``MemoryRegion``. The DMA engine reads and writes system
  memory through it. Default: the system memory region.

The device also has one named input line, ``external-reset``, for the
chip's reset pin (`Reset`_).

.. _sgi-vice-access:

Host access
===========

All registers sit on doubleword (8-byte) boundaries whatever their
width, and each register uses the low-order bytes of its slot
(:src:`099-0123-003 p. 39, 44`), *established*. The model is big-endian.
A 16-bit register is in bytes 6 and 7 of its slot, a 32-bit register in
bytes 4 to 7.

The model accepts 1, 2, 4 and 8-byte accesses that are naturally aligned
and stay inside one slot. Reads of bytes beyond the register's width
return zero. A partial write merges into the old value. A write that
touches none of the register's bytes is ignored. Four things need the
whole register in one write: descriptor words (16 bits), translation
entries (32 bits) and the two software-interrupt strobes (at least
4 bytes). Local memories take only writes of their publication unit
(`Local memories`_). A misaligned, slot-crossing or otherwise refused
access is a model refusal (`Model limits`_).

The specification says reads beyond the implemented part of an address
range return zeros so memory can grow later
(:src:`099-0123-003 p. 22`), *established*. The model does this for the
gaps in the memory map (`Local memories`_) and for the offsets from
``0x200`` to ``0xfff``. Writes to the memory-map gaps are discarded and log
``LOG_GUEST_ERROR``. Writes to ``0x200`` to ``0xfff`` are refused
(`Model limits`_).

.. _sgi-vice-registers:

Register map
============

Offsets are from the mapped base. Widths are the specification's. "Reset"
is the value after a device reset in the model. The specification page
column gives the page of the field table.

.. list-table::
   :header-rows: 1

   * - Offset
     - Name
     - Width
     - Reset
     - Access
     - Notes
     - Spec page
   * - ``0x000``
     - Reserved
     - 64
     - 0
     - read zero
     - Writes ignored.
     - 24
   * - ``0x008``
     - VICE_ID
     - 8
     - ``0xe1``
     - read
     - Write refused.
     - 44
   * - ``0x040``
     - MSP_CTL_STAT
     - 32
     - 0
     - read/write
     - Bit 0 go/running, bit 1 out of reset. See `MSP`_.
     - 49
   * - ``0x048``
     - MSP_ExcpFlag
     - 32
     - 0
     - read/write
     - Low 8 bits, one per exception.
     - 52
   * - ``0x050``
     - MSP_PC
     - 32
     - 0
     - read/write
     - Bits 15:2. Write sets the fetch PC, read returns the decode PC.
     - 50
   * - ``0x058``
     - MSP_BadAddr
     - 32
     - 0
     - read
     - Write refused.
     - 50
   * - ``0x060``
     - MSP_WatchPoint
     - 32
     - 0
     - read/write
     - Bits 31:3.
     - 50
   * - ``0x068``
     - MSP_EPC
     - 32
     - 0
     - read
     - Write refused.
     - 51
   * - ``0x070``
     - MSP_CAUSE
     - 32
     - 0
     - read
     - Write refused.
     - 51
   * - ``0x080``
     - BSP_SW_INT
     - 0
     - none
     - write
     - Sets interrupt bit 4. Read refused.
     - 47
   * - ``0x100``
     - MSP_D_RAM
     - 32
     - 0
     - read/write
     - Bits 2:0 grant the MSP data banks A to C. The specification
       defines only bits 2:0 (p. 48); the model keeps all 32 bits.
     - 48
   * - ``0x108``
     - VICEMSP_COUNT
     - 32
     - 0
     - read
     - Counts ``msp-clock-hz`` ticks. Write refused. The specification
       makes it read/write (p. 53); this is a model difference.
     - 53
   * - ``0x160``
     - VICE_INT
     - 9
     - 0
     - read
     - Writes ignored.
     - 46
   * - ``0x180``, ``0x1c0``
     - DMA_CTL_CH1, CH2
     - 16
     - ``0x10``
     - read/write
     - See `DMA control and status`_.
     - 64
   * - ``0x188``, ``0x1c8``
     - DMA_STAT_CH1, CH2
     - 16
     - ``0x10``
     - read
     - Write refused.
     - 65
   * - ``0x190``, ``0x1d0``
     - DMA_DATA_CH1, CH2
     - 16
     - 0
     - read/write
     - Fill pattern.
     - 66
   * - ``0x198``, ``0x1d8``
     - DMA_MEM_PT_CH1, CH2
     - 32
     - 0
     - read
     - Last system address, word aligned.
     - 66
   * - ``0x1a0``, ``0x1e0``
     - DMA_VICE_PT_CH1, CH2
     - 16
     - 0
     - read
     - Last local address. The model has no separate chroma half.
     - 66
   * - ``0x1a8``, ``0x1e8``
     - DMA_COUNT_CH1, CH2
     - 16
     - 0
     - read
     - Bytes left in the current descriptor.
     - 67
   * - ``0x1b8``
     - MSP_SW_INT
     - 0
     - none
     - write
     - Sets interrupt bit 2. Read refused.
     - 47
   * - ``0x1000`` to ``0x11f8``
     - DMA descriptors
     - 16
     - 0
     - read/write
     - Two channels of four sets of eight words.
     - 42
   * - ``0x2000`` to ``0x9fff``
     - Local memories
     - n/a
     - 0
     - see below
     - See `Local memories`_.
     - 24
   * - ``0xe000``
     - VICE_CFG
     - 16
     - 0
     - read/write
     - Bit 1 only.
     - 44
   * - ``0xe008``
     - VICE_INT_RESET
     - 9
     - none
     - write
     - Write one to clear. Read refused.
     - 45
   * - ``0xe010``
     - VICE_INT_EN
     - 9
     - 0
     - read/write
     - Bits 8:0.
     - 47
   * - ``0xf000`` to ``0xf1f8``
     - Translation buffer
     - 32
     - 0
     - read/write
     - 64 entries, one per slot. See `Address translation`_.
     - 38

The register offsets, widths and names are *established*
(:src:`099-0123-003 p. 39-43`), and so are the access types except in two
rows where the model differs from the specification: ``MSP_D_RAM`` keeps
bits the specification does not define, and ``VICEMSP_COUNT`` refuses
writes the specification allows. Both are *approximation*. The specification gives some reset
values as "unknown" or leaves them undefined: the PC, bad address and
EPC (:src:`099-0123-003 p. 50-51`), the descriptor words and the
translation entries (:src:`099-0123-003 p. 38, 42`). The model resets
all of them to zero, an *approximation*.

Registers the model does not accept are listed under `Model limits`_.

.. _sgi-vice-config:

Configuration
-------------

VICE_CFG has two fields. Bit 0, ``check_data_sysad``, enables checking of
the system bus data identifier. Bit 1 lets a DMA channel bypass address
translation when that channel also sets its own bypass bit
(:src:`099-0123-003 p. 44`), *established*. The model stores bit 0 and
does nothing with it, because it has no SysAD protocol. Writing any bit
above bit 1 is refused.

.. _sgi-vice-memories:

Local memories
--------------

The host and the MSP see VICE's memories in this layout
(:src:`099-0123-003 p. 24`), *established*. The model implements the
physical sizes below and returns zeros for the rest of each window, as
the specification says.

.. list-table::
   :header-rows: 1

   * - Offset
     - Size
     - Memory
     - Write unit
   * - ``0x2000``
     - 4 KiB
     - MSP instruction RAM
     - 4 bytes
   * - ``0x4000``
     - 2 KiB
     - BSP instruction RAM
     - 2 bytes
   * - ``0x5000``
     - 5 KiB
     - BSP table RAM, 1280 words of 22 bits
     - 4 bytes
   * - ``0x8000``
     - 2 KiB
     - Data RAM bank A
     - 1 byte
   * - ``0x8800``
     - 2 KiB
     - Data RAM bank B
     - 1 byte
   * - ``0x9000``
     - 2 KiB
     - Data RAM bank C
     - 1 byte

Gaps in the table read zero and discard writes with ``LOG_GUEST_ERROR``:
``0x3000`` to ``0x3fff``, ``0x4800`` to ``0x4fff``, ``0x6400`` to
``0x6fff``, ``0x9800`` to ``0xdfff`` and everything from ``0x10000``. The
BSP buffers at ``0x7000`` to ``0x7fff`` are not implemented and are
refused.

Writes must be a multiple of the write unit and must not cross the end of
the memory. Reads may use any width. Table words keep only bits 21:0, so
a stored ``0xffffffff`` reads back ``0x003fffff``. The sizes of the
instruction RAMs and the table follow the specification's entry counts
(1K x 16 for the BSP instructions, 1280 x 22 for the table,
:src:`099-0123-003 p. 24`). That the 4 KiB MSP instruction RAM holds
1K 32-bit words is *inferred* from the 4-byte fetch width. A host write
to the MSP instruction RAM while the MSP is running is refused.

.. _sgi-vice-interrupts:

Interrupts
==========

``VICE_INT`` (``0x160``) latches nine causes. Hardware sets them. Software
clears them by writing ones to ``VICE_INT_RESET`` (``0xe008``). Writes of
zero change nothing. ``VICE_INT_EN`` (``0xe010``) gates only the output
pin, not the status bits
(:src:`099-0123-003 p. 45-47`), *established*. The two software
interrupts latch whatever the enable bit says, *established*.

.. list-table::
   :header-rows: 1

   * - Bit
     - Cause
     - Set by the model when
   * - 0
     - DMA complete, channel 1
     - A descriptor with the halt bit finishes and the channel's
       interrupt enable (control bit 1) is set.
   * - 1
     - DMA error, channel 1
     - The channel faults. Not gated by control bit 1.
   * - 2
     - MSP software interrupt
     - Any write of at least 4 bytes to ``0x1b8``.
   * - 3
     - MSP exception
     - The MSP stops on an exception.
   * - 4
     - BSP software interrupt
     - Any write of at least 4 bytes to ``0x80``.
   * - 5
     - BSP exception
     - Never. There is no BSP.
   * - 6
     - Bad system-bus data
     - Never. There is no system-bus protocol.
   * - 7
     - DMA complete, channel 2
     - As bit 0, for channel 2.
   * - 8
     - DMA error, channel 2
     - As bit 1, for channel 2.

The bit assignments are from :src:`099-0123-003 p. 45-46`, *established*.
The specification names the pin ``Vice_Int_n`` (p. 18) and requires it
to be 1 in reset (p. 26), so the pin is active low, *established*. The
model's output line is high while ``VICE_INT & VICE_INT_EN`` is not zero
and is updated on every change to either register, to a status bit and
on a restore from a migration stream. That polarity is a model choice,
*approximation*, and differs from the pin. CRIME sees a registered
signal.

.. _sgi-vice-reset:

Reset
=====

The chip resets all internal state when ``ViceReset_n`` is held low for at
least 16 clocks, after the PLL has settled. The MSP and each DMA channel
have their own reset control that also needs at least 16 clocks
(:src:`099-0123-003 p. 26`), *established*. The host interface is not
resettable that way.

The model reproduces this with the device clock:

- A device reset clears the interrupt registers, the bank grants, the
  translation buffer, the descriptors, the DMA channels, every local
  memory and the counter. It resets the MSP and holds it in reset. The
  model clears the memories, which the specification does not say; this
  is an *approximation*.
- With ``stable-reference`` on, the first reset after the device is
  created leaves the chip ready. Any later device reset, and any high
  level on the ``external-reset`` input, puts it in external reset until
  the input is driven low again. The release is admitted only when the
  input stayed high for at least 16 device clocks. A shorter release is a
  refusal. During external reset DMA and MSP starts are refused and the
  engines stand still.
- With ``stable-reference`` off the chip is always in external reset.
- A DMA channel reset starts when control bit 3 is written as one. The
  channel then reads control ``0x18`` and status ``0x10``. A later write
  with bit 3 clear ends the reset if at least 16 device clocks have
  passed, and control reads ``0x10``. A write that does so too soon is a
  refusal. A write of GO together with the release is also refused.
- The MSP is held in reset at device reset. Writing ``MSP_CTL_STAT`` to
  release it before 16 device clocks have passed is a refusal. Writing it
  with bit 1 clear resets the MSP again and clears its registers, its
  watchpoint and its flags.

The 16-clock count is *established* for the specification's clock. The
choice to measure it in device clocks, and to refuse short releases
instead of resetting incompletely, is the model's.

.. _sgi-vice-dma:

DMA
===

Each of the two channels walks a list of four descriptor sets. It copies
a rectangle of bytes between system memory and one of VICE's local
memories, one direction per descriptor
(:src:`099-0123-003 p. 33, 64-72`), *established*.

.. _sgi-vice-dma-registers:

DMA control and status
----------------------

``DMA_CTL`` bits, as the model treats them (:src:`099-0123-003 p. 64`):

.. list-table::
   :header-rows: 1

   * - Bits
     - Name
     - Model
   * - 0
     - GO
     - A write of one starts the channel. The model clears it when the
       list completes. Writing zero never stops a transfer.
   * - 1
     - IE
     - Raises the completion interrupt for this channel.
   * - 2
     - STOP
     - Halts the channel at the next unit boundary with status code 1.
       It cannot be cleared without a channel reset.
   * - 3
     - RESET
     - See `Reset`_.
   * - 7:4
     - DESCR_PT
     - One-hot first descriptor set. A GO with zero or several bits set
       is refused.
   * - 8
     - TLB_BYP
     - Bypass translation, only if ``VICE_CFG`` bit 1 is also set.
   * - 9
     - FLUSH_BUF
     - Not supported. Setting bit 9 or any higher bit with GO is refused.

A control write that changes a bit other than GO, IE or STOP while the
channel is active is refused. So is a second GO while active.

``DMA_STAT`` holds the state: bit 0 done, bit 1 error, bit 2 active,
bit 3 reading, bits 7:4 the current descriptor set, bits 11:8 a status
code (:src:`099-0123-003 p. 65`). The model reports these codes:

.. list-table::
   :header-rows: 1

   * - Code
     - Meaning
     - When the model shows it
   * - 0
     - Idle
     - Reset.
   * - 1
     - Halted by STOP
     - After STOP drains.
   * - 2
     - Halted by the descriptor's halt bit
     - A list finished. Done is set, active clear.
   * - 3
     - Computing addresses
     - After GO and when a new descriptor is fetched.
   * - 6
     - Moving data on the internal bus
     - While a unit is staged.
   * - 7
     - Halted by a TLB miss or an out-of-range address
     - A fault. Error is set, active clear.
   * - 8
     - Halted by a write to a read-only translation
     - A fault.

Codes 4 and 5, which wait on CRIME, never appear because there is no
CRIME transaction.

.. _sgi-vice-dma-descriptors:

Descriptors
-----------

A set has eight 16-bit words: control, upper and lower system address,
width in bytes, stride in bytes, number of lines, local Y address and
local C address (:src:`099-0123-003 p. 42, 68-72`). The set starts at
``0x1000 + channel * 0x100 + set * 0x40`` and the words are 8 bytes
apart. Writing a word of the set that the channel is using is refused.

The model supports these control bits: bit 12 fill, bit 13 read (system
memory to VICE), bit 14 skip and bit 15 halt. A skip moves to the next
set without a transfer. Skip together with halt is refused. Bits 6:4,
the location field, are ignored and log ``LOG_UNIMP``. The specification
says they are not implemented and harmless
(:src:`099-0123-003 p. 69`). Any of bits 0 to 3, bit 7 and bits 11:8
(half-pel flags, chroma-only, interleave and the Y/C split modes) are
refused.

A descriptor is refused when:

- the width or the line count is zero, or their product exceeds 65535;
- there is more than one line and the stride has bit 15 set;
- the local Y address is not a multiple of 8, or does not fall in one of
  the six local memories (the local C address is stored and never used);
- fill is set without read, or the width is odd;
- the target is an instruction RAM or table RAM and the width, or for
  the MSP instruction RAM the system address or stride, is not a
  multiple of the memory's unit (8 bytes for the MSP instruction RAM, 2
  for the BSP instruction RAM, 4 for the table RAM).

The local address and the system address advance in raster order: row
times stride plus column. In fill mode the 16-bit ``DMA_DATA`` value is
repeated, high byte first, to fill the local memory
(:src:`099-0123-003 p. 66`). Fill never targets system memory, matching
the specification.

When a channel completes a set without the halt bit it moves to the next
set, wrapping from the fourth to the first. The specification tells
software to set the halt bit in the last valid set
(:src:`099-0123-003 p. 68`), *established*.

.. _sgi-vice-dma-timing:

Transfer steps
--------------

The model moves data in units of at most 8 bytes. A unit is first staged
(read from the source) and then committed (written to the destination),
each on its own device clock. The two channels alternate. Units bound for
the instruction RAMs and the table RAM are cut to their write unit. The
unit also stops at a row end, at a 64 KiB translation page end or at a
512-byte tile row end. System bytes are placed independently of the local
unit size. A source byte that reaches local memory is written in order.
Data RAM and instruction RAM writes during the commit step become visible
to host reads on that clock. Words written to the table RAM are masked to
22 bits.

An MSP load or store to a data bank reserves that bank for one MSP clock.
A DMA unit that would touch the bank in that clock waits for the next
one. The specification describes bank arbitration between the MSP and
DMA (:src:`099-0123-003 p. 48`), *established*. The model's clock-level
ordering is an *approximation*: the specification gives no cycle table for
it.

The specification says DMA runs at the full internal bus rate
(:src:`099-0123-003 p. 66`). The model's rate of at most 8 bytes per
channel per device clock is an *approximation*, and there is no SysAD
transaction timing.

System memory is read and written through the ``dma-memory`` region. The
target must be populated RAM that is not a RAM device and not read-only
for a write. Anything else (ROM, MMIO, an unpopulated hole, a discarded
range) is a refusal, not a DMA error.

While the MSP is running, a DMA to or from the MSP instruction RAM is
refused. The specification says DMA may write that RAM while the MSP
executes (:src:`099-0123-003 p. 30`), so the model is narrower than the
hardware here.

.. _sgi-vice-translation:

Address translation
-------------------

The translation buffer has 64 entries, one per 64 KiB page of a 4 MiB
window. Bit 0 is valid, bit 1 allows writes, bits 31:16 are the physical
page number, and bits 15:2 are undefined and cannot be set
(:src:`099-0123-003 p. 38`), *established*. A write that sets any of bits
15:2, or is narrower than 32 bits, is refused.

A DMA system address selects one of two windows
(:src:`099-0123-003 p. 33-37`), *established*:

.. list-table::
   :header-rows: 1

   * - Window
     - Index
     - Offset
   * - ``0x00800000`` to ``0x00bfffff``, linear pages
     - Bits 21:16
     - Bits 15:0
   * - ``0x10800000`` to ``0x10bfffff``, frame-buffer tiles
     - Bits 21:19 then 11:9
     - Bits 18:12 then 8:0

A translation to an entry that is not valid, or an address outside both
windows, stops the channel with status code 7 and the error interrupt. A
write through an entry without the write bit stops it with code 8. An
address above 32 bits also gives code 7. The fault keeps the bytes
already committed and the remaining count. A channel in the error state
accepts a new GO only after a channel reset.

With ``VICE_CFG`` bit 1 and the channel's bypass bit both set, the
descriptor's address is physical. The model still limits the range to
32 bits.

.. _sgi-vice-msp:

MSP
===

The MSP runs a program from its 4 KiB instruction RAM at ``0x2000`` to
``0x2fff``. The host loads the program, writes ``MSP_PC`` and then
``MSP_CTL_STAT`` (:src:`099-0123-003 p. 30`), *established*. The model
requires the PC write before every start, and 16 device clocks since the
last reset.

``MSP_CTL_STAT`` bit 1 releases the MSP from reset and bit 0 starts it.
Reading bit 0 returns whether it is running and bit 1 whether it is out of
reset (:src:`099-0123-003 p. 49`), *established*. Writing bit 0 as zero
to a running MSP lets the instructions in flight finish and then stops
it. Writing bit 1 as zero resets it.

The model's scalar unit is a simplified version of the specification's
five-stage pipeline (:src:`099-0123-003 p. 124-143`). It fetches two
words at a time, issues one scalar instruction per device clock, and
models operand forwarding, load interlocks and a branch penalty. It
executes the MIPS integer subset listed in
:src:`099-0123-003 p. 122-124`: add, subtract, logic, shifts, set on less
than, load upper immediate, branches and jumps with a delay slot, byte,
half and word loads and stores, ``BREAK``, and the coprocessor register
moves that reach the host registers. Multiply, divide, system call and
coprocessor branches are not in the set and raise the reserved-instruction
exception.

Loads and stores reach only the three data banks, ``0x8000`` to
``0x97ff``. Coprocessor 1 moves reach the host registers at ``0x100`` and
above, and coprocessor 3 moves reach the descriptor sets
(:src:`099-0123-003 p. 39`), *established*. Any other coprocessor
operation or register is a refusal.

Exceptions are imprecise: the MSP stops issuing, completes what is in
flight, then halts and latches ``VICE_INT`` bit 3
(:src:`099-0123-003 p. 27`), *established*. The model sets these codes in
``MSP_CAUSE`` bits 6:2, with the branch-delay flag in bit 31, and the
matching bit in ``MSP_ExcpFlag`` (:src:`099-0123-003 p. 27-29, 51-52`):

.. list-table::
   :header-rows: 1

   * - Code
     - Name
     - Model raises it for
   * - 0, 1
     - Load and store address error
     - An address outside ``0x8000`` to ``0x97ff`` or a misaligned one.
       ``MSP_BadAddr`` keeps the address.
   * - 2
     - Breakpoint
     - ``BREAK``.
   * - 3
     - Watchpoint
     - A load or store whose address, ignoring the low 3 bits, equals a
       nonzero ``MSP_WatchPoint``. The access completes.
   * - 4
     - Scalar reserved instruction
     - An undefined opcode or SPECIAL function.
   * - 5
     - Vector reserved instruction
     - Never. See `Model limits`_.
   * - 6
     - Contention
     - A load or store to a bank not granted in ``MSP_D_RAM``. A load
       reads zero and a store is dropped.
   * - 7
     - Instruction fetch error
     - A PC outside ``0x2000`` to ``0x2ffc``. ``MSP_BadAddr`` is not
       updated.

When several exceptions fire at once the model reports the highest by the
specification's priority order (fetch, breakpoint, scalar and vector
reserved, contention, load, store, watchpoint). The cause and EPC
registers are updated only while every flag is clear
(:src:`099-0123-003 p. 29`), *established*.

The model sets ``VICE_INT`` bit 3 when the MSP halts after an exception.
A MSP that halts on request does not.

.. _sgi-vice-counter:

Counter
-------

``VICEMSP_COUNT`` counts ticks of ``msp-clock-hz`` from the last reset as
a 32-bit value. The specification makes it writable for diagnostics
(:src:`099-0123-003 p. 53`), *established*. The model refuses writes.

.. _sgi-vice-migration:

Migration
=========

The device migrates as ``sgi-vice`` version 1. The stream carries the
registers, the translation buffer, both channels with their staged bytes,
the MSP's registers and pipeline stages, the pending timer and both
clocks, and each local memory. It requires the same ``part-id`` and clock
properties on both sides. On load the model shifts its stored times by the
difference between the two virtual clocks, rejects a stream whose channel
state is inconsistent and re-evaluates the interrupt line. Migration works
only between identical builds. The MSP part is the subsection
``sgi-vice-msp``.

.. _sgi-vice-limits:

Model limits
============

- **Refusals.** When the model meets an access or state it does not
  implement, it logs ``LOG_UNIMP`` as ``sgi-vice: unsupported <reason> at
  <address>``, returns a bus error (``MEMTX_ERROR``) to the initiator,
  stops its timers, and asks the machine to stop with the
  ``internal-error`` run state. This is a model choice to avoid inventing
  hardware behaviour; the hardware does not do it. A DMA channel recovers
  with a channel reset. The MSP recovers with an MSP reset. Clearing
  the reset input restores the rest.
- **Registers and ports that are refused**: ``0x010`` to ``0x038`` (this
  includes the host snoops of the BSP mailboxes), ``0x078``, the BSP
  registers ``0x110`` to ``0x158`` and ``0x168`` to ``0x178``, ``0x1b0``,
  ``0x1f0``, ``0x1f8``, ``0x1200`` to ``0x1fff`` and the unassigned
  ranges from ``0xe018`` on that are not memory. Reads from ``0x200`` to
  ``0xfff`` return zero. Writes there are refused.
- **BSP**: no bitstream processor, its registers, its FIFOs, its
  mailboxes or its instruction and table execution. Memories exist so
  that code can be loaded and read back.
- **MSP vector unit**: any vector instruction, vector load or store, or a
  group that pairs a vector instruction with a scalar one stops the model.
  The vector reserved-instruction exception never fires.
- **MSP scalar details**: the model stops on signed overflow in ``ADD``,
  ``ADDI`` and ``SUB``, on a halfword at the upper half of a word, on
  noncanonical encodings, on a branch whose target cannot be fetched, on
  a branch or ``BREAK`` in a delay slot, on a delay-slot operand that
  depends on the previous two instructions, and on the other cases in the
  qtest. The specification treats a halfword access that is not word
  aligned as an address error (:src:`099-0123-003 p. 27`), so the model
  stops where the hardware may raise the exception.
- **DMA**: no half-pel flags, chroma-only, interleave or Y/C split modes
  (a descriptor that asks for one is refused), no flush bit, no
  chroma-address field, no data-RAM location selection by the control
  word. Codes 4 and 5 are never reported.
- **System side**: no SysAD protocol, no CRIME transaction, no system-bus
  error interrupt (bit 6), no multi-chip ID decoding.
- **Timing**: the rates in `Transfer steps`_ and the MSP's cycle counts
  are each an *approximation*. The specification's pipeline description and the
  qtests agree on what happens, not on how many nanoseconds it takes.
- **No machine instantiates it.** The device is a part model and has no
  CRIME wiring, no firmware path and no guest driver.

.. _sgi-vice-tests:

Tests
=====

``tests/qtest/sgi-vice-test.c`` runs ``-M none`` with the device mapped
at ``0x17000000`` and a ``memory-transaction-test`` device to observe
refused accesses. Some cases add the ``sgi-dma-word-test`` fixture. It
covers:

- identity, read-only registers, the padding rules and the partial-write
  rules (``identity``, ``read-only``, ``padding``, ``padding-pending``);
- the memory map, the write units, the gaps and the table masking
  (``memories``, ``ram-views``);
- the interrupt block, the software strobes and the output pin
  (``interrupts``, ``completion-enable``);
- DMA in both directions, chaining, wrap, skip, stride, fill and the
  address bounds (``transfer``, ``chain``, ``stride``, ``skip-progress``,
  ``fill-progress``, ``address-bounds``, ``local-dma``);
- translation, the linear and tiled indices, permission changes and
  bypass (``translation``, ``translation-indices``,
  ``permission-changes``);
- STOP, reset durations at three clocks, peer reset and recovery
  (``boundaries``, ``reset-duration``, ``other-clocks``,
  ``reset-restart``, ``peer-reset``);
- refusals, late faults and held descriptors (``unsupported``,
  ``late-fault``, ``held-descriptors``, ``descriptor-edits``);
- the counter, concurrent channels and the typed-unit and odd-address
  cases (``counter``, ``concurrent``, ``typed-*``, ``odd-*``,
  ``msp-system-alignment``);
- migration at several points (``migration``, ``typed-migration``,
  ``typed-write-migration``).

``tests/qtest/sgi-vice-msp-test.c`` covers the scalar unit with assembled
programs: arithmetic, ALU forms, registers, branches and jumps with delay
slots, load interlocks, local memory, address, contention and watchpoint
exceptions, imprecise drain and priority, the fetch PC, reserved
and breakpoint instructions, coprocessor moves, reset and halt, branch
penalties, DMA and MSP bank contention, and migration with the pipeline
full, waiting on a target or between slices. It also pins the
refused word forms (``held-words``, ``held-data-cases``).

Both tests run only when ``CONFIG_SGI_VICE`` is set in the device
configuration, which the qtest fixture build selects.
