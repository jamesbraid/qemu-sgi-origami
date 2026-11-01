.. SPDX-License-Identifier: BSD-3-Clause

.. _sgi-rad1-audio:

======================
RAD1: audio controller
======================

Used in :ref:`sgi-ip30` (Octane, Octane2, onboard) and :ref:`sgi-ip27`
(Origin 2000 and Onyx2, on the BaseIO-G's MediaIO daughtercard). RAD1 is
SGI's PCI audio controller, PCI identity ``10a9:0005``. It is onboard on
the Octane and Octane2 system board, at device 3 of the onboard Bridge,
and on the MediaIO daughtercard of the Origin 2000 and Onyx2 BaseIO-G, at
device 7 of that board's Bridge. It moves audio between host-memory rings
and the analog, AES3 and ADAT interfaces, and reports progress through a
status ring in host memory. QEMU models it in
``hw/audio/sgi-rad1-audio.c`` as two types: ``sgi-rad1-audio`` for
MediaIO, PCI revision ``0x70``, and ``sgi-rad1-octane`` for the Octane,
PCI revision ``0xc0``. The two share the register layout and engines but
accept different access sets. It is unrelated to the PsiTech RAD1 graphics
card, which has its own model and page.

.. seealso::

   Platform pages: :ref:`sgi-ip30`, :ref:`sgi-ip27`.

No published register specification for this chip has been found. Facts
below are rated *established* (a cited manual or a measurement of an
identified machine), *observed* (a published capture), *inferred*,
*approximation* (a model choice, with its limit stated) or *unknown*.
What the shipped IRIX 6.5.30 audio drivers do is a *software
expectation*: it shows what software needs, not what the silicon does.

.. _sgi-rad1-sources:

Sources
=======

- *OCTANE Technical Report*. Built-in audio sits behind the Bridge that
  serves the built-in peripherals, separate from the Bridge for the four
  expansion slots :src:`octane-technical-report p. 2-19`. Baseline audio
  generates the media timeline clock and RAD associates unadjusted system
  time (UST) with media sample counts
  :src:`octane-technical-report p. 3-3 to 3-5`. Neither page gives a
  register.
- *Origin and Onyx2 Theory of Operations*: MediaIO carries a RAD audio
  ASIC on the BaseIO-G's PCI bus :src:`007-3439-002 p. 71-72`. *Onyx2
  Deskside Workstation Owner's Guide*: the IO6G panel's two AES3 BNCs and
  two video-sync loop-through BNCs :src:`007-3454-005 p. 26-27`.
- Published inventories and boot logs: an OpenBSD 4.3-beta boot on an
  Octane with HEART revision 4
  :src:`miod-openbsd-sgi-story-4 line 965`; Linux boots of one
  dual-R14000, HEART revision F Octane in 2017
  :src:`lore-kinard-octane-2017 dmesg line 161, lspci line 44` and 2019
  :src:`lore-kinard-octane-2019 dmesg line 151`; a BaseIO-G Origin 2000
  ``hinv -v`` :src:`nekonomicon-17194 owner's hinv -v post`; an Onyx2
  inventory :src:`sgistuff-collection-2200 hinv section`.
- PROM-monitor reads on the :ref:`sgi-measured-octane2`, listed under
  :ref:`sgi-rad1-measurements`.
- OpenBSD sgi port at ``2df91d80c104636deeef8124252ddd5951b55fbf``:
  ``sys/dev/pci/pcidevs.h`` lines 125-130 and 9434-9438 (vendor ``0x10a9``
  is SGI, device ``0x0005`` is ``RAD1``) and ``sys/arch/sgi/xbow/xbridge.c``
  lines 1138-1150 (RAD1 gets the unswapped PCI accessors), as software
  expectations. OpenBSD attaches no driver to the function, and Linux
  mainline at ``db2ddb87143519e20a95aa36c60b36107b736a58`` has no RAD1
  device constant in ``include/linux/pci_ids.h``.
- The IRIX 6.5.30 Octane audio driver (SHA-256
  ``e5fa4e36e4f9100a8dbebcbf1d707145f5c8164b69af2fbbeca490ab81d3be08``)
  and the IRIX 6.5.30 Origin audio driver (SHA-256
  ``7928b382eecc2c62039c3a09aa7e420b531c0a7d5dc50567f9e3aa43666bc6f5``),
  read as shipped binaries. Every register below comes from them.

.. _sgi-rad1-identity:

Identity and placement
======================

The OpenBSD Octane boot prints ``"SGI Rad1" rev 0xc0`` at Bridge device 3,
function 0 :src:`miod-openbsd-sgi-story-4 line 965`, *observed*.
The 2017 Linux capture reports the same function as ``10a9:0005``, class
``0x000000``, revision ``c0``
:src:`lore-kinard-octane-2017 dmesg line 161, lspci line 44`; the 2019
capture of the same machine repeats the zero class
:src:`lore-kinard-octane-2019 dmesg line 151`. The reference Octane2 returned
the class and revision word ``0x000000c0`` (:ref:`sgi-rad1-octane2-config`),
*established* for that machine. ``sgi-rad1-octane`` presents class
``0x000000`` and revision ``0xc0`` on both the ``octane`` and ``octane2``
machines. That every Octane board carries this revision is *inferred*
from three machines.

PCI Status on ``sgi-rad1-octane`` reads ``0x0480``: slow DEVSEL timing
and fast back-to-back capable (bits 10:9 and 7, :src:`pci-local-bus-2.2`).
That matches the reference Octane2's chip
(:ref:`sgi-rad1-octane2-config`), *established* for that chip. The bits
are read-only and survive a write and a machine reset. That every Octane
board reads the same is *inferred*.

The BaseIO-G ``hinv -v`` lists ``0x10a9:0x0005`` at PCI slot 7
:src:`nekonomicon-17194 owner's hinv -v post`, *observed*, and the
``io6g`` preset of :doc:`baseio` places ``sgi-rad1-audio`` there. Both
IRIX drivers read configuration word ``0x08`` and split the revision byte
into a board number (high nibble) and a chip revision (low nibble),
*software expectation*; the inventory prints the pair, so ``0xc0`` appears
as "RAD revision 12.0" on the Octane2 and a BaseIO-G inventory shows "RAD
revision 7.0" :src:`sgistuff-collection-2200 hinv section`. The drivers
choose their board programming from that number: 7 for MediaIO, 12 for the
Octane, and a third value, 13, for another board. MediaIO revision
``0x70`` is *inferred* from the printed "7.0". The MediaIO class code is
*unknown*; the model presents ``0x0401``, multimedia audio, an
*approximation*. Class and revision are read-only and survive reset in
both types.

BAR0 is a 32-bit memory BAR of ``0x2000`` bytes in the model; a sizing
write reads back ``0xffffe000``. Both drivers map exactly ``0x2000`` bytes
of BAR0 and a separate ``0x20`` bytes of configuration space, *software
expectation*: a smaller BAR leaves IRIX without a register base. The
physical BAR size is *unknown*. Neither type requests an interrupt pin.
The reference Octane2's RAD1 reads interrupt pin 0, *established* for
the Octane chip (:ref:`sgi-rad1-octane2-config`).

.. _sgi-rad1-register-map:

Register map
============

Offsets are from BAR0. Registers are 32 bits. "Stored" means the model
keeps the last accepted write and returns it on read, with no other
effect; no physical readback is known, so every stored register is an
*approximation*. Reset is zero throughout, also an *approximation*. The
values IRIX writes are each a *software expectation*.

.. list-table::
   :header-rows: 1
   :widths: 13 19 10 58

   * - Offset
     - Name
     - Access
     - Notes
   * - ``0x04``-``0x2c``
     - Sample positions
     - R
     - One word per channel field. Advanced by the channel engines; see
       :ref:`sgi-rad1-positions`. Writes are refused.
   * - ``0x30``
     - UST counter
     - R
     - See :ref:`sgi-rad1-ust`. Writes are refused.
   * - ``0x34``
     - Sync status
     - none
     - Seen only as a status-slot mirror, where the model writes zero.
       A BAR0 access is unimplemented.
   * - ``0x40``
     - UST control
     - W
     - Octane only: ``0x0e``, ``0x0a``, ``0x02``. Unimplemented on
       MediaIO.
   * - ``0x44``
     - ADAT input control
     - R/W
     - Bit 0 runs the ADAT input, bit 2 the ADAT-input subcode channel.
       IRIX writes ``0xa0000008`` at start.
   * - ``0x48``
     - AES3 input control
     - R/W
     - Bit 0 runs the AES3 input, bit 2 the AES3-input subcode channel.
       IRIX writes ``0x10`` at start.
   * - ``0x4c``
     - Analog input interface
     - R/W
     - Bit 0 runs the analog input. On MediaIO bit 27 clocks the serial
       word (:ref:`sgi-rad1-mux`). IRIX writes ``0x05000000`` on MediaIO
       and ``0x03000000`` on the Octane.
   * - ``0x50``
     - ADAT output control
     - R/W
     - Bit 0 runs the ADAT output. The Octane driver writes ``0x60000000``.
   * - ``0x54``
     - AES3 output control
     - R/W
     - Bit 0 runs the AES3 output. Bits 2 and 3 are subcode flags,
       stored. The Octane driver writes ``0x40000000``.
   * - ``0x58``
     - Analog output control
     - R/W
     - Bit 0 runs the analog output. The driver's idle value is
       ``0x20000000`` and its running value ``0x20000001``.
   * - ``0x5c``
     - Status DMA control
     - R/W
     - See :ref:`sgi-rad1-status-dma`.
   * - ``0x70``
     - Analog output parameters
     - R/W
     - Stored. The Octane driver writes ``0x1500``; bit 8 is inferred
       mute and bit 6 accompanies output subcode. Neither takes effect.
   * - ``0x74``, ``0x78``
     - Start-up words
     - R/W
     - Stored. IRIX writes ``0x08000010`` and ``0x00fac688``.
   * - ``0x7c``
     - Analog gain
     - R/W
     - Stored. Four gain bytes, input left and right then output left and
       right; IRIX writes ``0xd6d6d6d6``. No gain is applied.
   * - ``0x80``
     - Central control
     - R/W
     - Stored. IRIX starts from all ones and narrows it to ``0xffe20000``;
       clock programming sets and clears one selected bit from bit 8 up.
   * - ``0x84``
     - Board control
     - R/W
     - MediaIO only, stored; the Octane driver never writes it and the
       Octane type refuses it.
   * - ``0x88``, ``0x8c``
     - Serial latch and data
     - R/W
     - Stored. On MediaIO they carry the serial word
       (:ref:`sgi-rad1-mux`). The IRIX 6.5.30 kernel writes ``0x8c`` =
       2 and ``0x88`` = 3 at start, and ``0x88`` = 2 then 0 at halt; the
       public Linux RAD1 driver writes only ``0x88`` = 3 and ``0x8c`` =
       2, once. Each is a *software expectation*. On the Octane,
       ``0x88`` = 2 or ``0x8c`` = 3 silences the analog output
       (:ref:`sgi-rad1-limits`).
   * - ``0x90``
     - Interface selection
     - R/W
     - Stored. IRIX writes 2, and 3 for some output parameters.
   * - ``0xa0``
     - Clock word
     - R/W
     - Stored. IRIX writes ``0x02000001`` at start. No rate changes.
   * - ``0xa4``-``0xb4``
     - Clock selection
     - R/W
     - Octane only, stored; unimplemented on MediaIO.
   * - ``0xb8``, ``0xbc``
     - Start-up words
     - R/W
     - Stored. IRIX writes ``0x9fffffff``.
   * - ``0x400``-``0x528``
     - DMA descriptors
     - R/W
     - Ring addresses and control words; see :ref:`sgi-rad1-descriptors`.
   * - ``0x1000``-``0x121f``
     - Output subcode
     - R/W
     - Stored metadata for the AES3 and ADAT outputs; see
       :ref:`sgi-rad1-subcode-windows`.

Any offset not listed is unimplemented in both types.

.. _sgi-rad1-ust:

UST counter and control
-----------------------

Both drivers read ``0x30`` twice about 10 µs apart and print "RAD UST
not incrementing" if it advanced by fewer than 3, *software expectation*.
That ``0x30`` counts time is *inferred*; its period, reset value and
wrap are *unknown*. On the reference Octane2 it reads zero and does not
advance over several seconds at the PROM menu, before any driver has run
(:ref:`sgi-rad1-octane2-ust`), *established* for that chip at that
point.

On MediaIO the driver never writes a start command, so the counter is
*inferred* to run from reset. The model counts one tick per 800 ns of
virtual time since reset, an *approximation* taken from one of the two
UST scales the Origin driver uses.

The Octane driver writes ``0x0e`` then ``0x0a`` to ``0x40`` to enable
the counter and ``0x02`` to disable it, *software expectation*. The model
holds the count on ``0x0e`` and arms; starts counting on ``0x0a`` only
when armed; holds the count and disarms on ``0x02``. Reset clears the
count and leaves it stopped. Any other value, a lone ``0x0a``, a read of
``0x40`` or a write of ``0x30`` is refused. While running it counts one
tick per 960 ns of virtual time, keeping the fractional remainder and
wrapping at 32 bits. The 960 ns period follows the fixed divisor the
Octane driver uses to scale UST; the reset state and the effect of each
value are each an *approximation*. The stopped zero count the model starts in
matches what the reference Octane2 reads at its PROM menu. Reads never
advance either counter.

.. _sgi-rad1-positions:

Sample positions
----------------

Each channel type reads two position words, A and B, chosen from a table
in the driver, *software expectation*. A holds a 14-bit count in bits
31:18; bits 17:0 receive the low bits of the UST counter as the count
moves. The driver splices those bits into its timestamps.

.. list-table::
   :header-rows: 1
   :widths: 28 14 14 44

   * - Channel (type)
     - A
     - B
     - B field
   * - Analog input (0)
     - ``0x14``
     - ``0x18``
     - bits 31:18
   * - Analog output (1)
     - ``0x2c``
     - ``0x18``
     - bits 17:4
   * - AES3 input (2)
     - ``0x0c``
     - ``0x10``
     - bits 31:18
   * - AES3 output (3)
     - ``0x24``
     - ``0x28``
     - bits 31:18
   * - ADAT input (4)
     - ``0x04``
     - ``0x08``
     - bits 31:18
   * - ADAT output (5)
     - ``0x1c``
     - ``0x20``
     - bits 31:18
   * - AES3-input subcode (6)
     - ``0x10``
     - ``0x10``
     - A at bits 17:4
   * - ADAT-input subcode (7)
     - ``0x08``
     - ``0x08``
     - A at bits 17:4

The model counts frames, not words, an *inferred* reading of the driver's
arithmetic. A subcode channel shares its B field with its parent input,
so the model moves only the subcode channel's bits 17:4 and leaves bits
31:18 to the parent. The general rule that a bits-31:18 field carries a
UST latch in bits 17:0 would collide with the analog output's field in
``0x18``, so the model latches only into the A words. On MediaIO the
latch is the counter when the engine steps; on the Octane it is the
counter at the boundary of the last frame moved. After enabling the
analog output the drivers poll its B field and need it to move within
about 500 µs.

.. _sgi-rad1-descriptors:

DMA descriptors
---------------

Each channel has two descriptor sets, A and B, of address low, address
high and control, *software expectation*:

.. list-table::
   :header-rows: 1
   :widths: 34 33 33

   * - Channel (type)
     - Set A: low, high, control
     - Set B: low, high, control
   * - Status ring
     - ``0x500``, ``0x528``, ``0x504``
     - ``0x4b4``, ``0x4b8``, ``0x4bc``
   * - Analog input (0)
     - ``0x4d0``, ``0x510``, ``0x4d4``
     - ``0x400``, ``0x404``, ``0x408``
   * - Analog output (1)
     - ``0x4f8``, ``0x524``, ``0x4fc``
     - ``0x40c``, ``0x410``, ``0x414``
   * - AES3 input (2)
     - ``0x4c8``, ``0x50c``, ``0x4cc``
     - not used by the model
   * - AES3 output (3)
     - ``0x4f0``, ``0x520``, ``0x4f4``
     - not used by the model
   * - ADAT input (4)
     - ``0x4c0``, ``0x508``, ``0x4c4``
     - not used by the model
   * - ADAT output (5)
     - ``0x4e8``, ``0x51c``, ``0x4ec``
     - not used by the model
   * - AES3-input subcode (6)
     - ``0x4e0``, ``0x518``, ``0x4e4``
     - ``0x448``, ``0x44c``, ``0x450``
   * - ADAT-input subcode (7)
     - ``0x4d8``, ``0x514``, ``0x4dc``
     - not used by the model

A channel control word is ``(tag << 3) | (-units << 7)`` in 32-bit
arithmetic, where the tag is the channel type and one unit is 128 bytes.
A 4 KiB analog output ring is ``0xfffff008``. The driver writes both sets
alike before setting the enable bit. Whether hardware alternates between
them is *unknown*. The model latches set A at the enable edge and walks
that ring alone.

On MediaIO the whole span ``0x400``-``0x528`` is stored. On the Octane
the span from ``0x400`` is descriptor RAM: sixteen entries of address
low, address high and control, twelve bytes apart. IRIX links to entry
1 and Linux to entry 7, each programmed with a copy of the working
descriptor, a *software expectation*. The sixteen-entry count follows
from the four-bit link field, and the rest of the layout from those two
entries, *inferred*. Entry 15 shares its words with the status ring's
set B. Every entry word takes 32-bit reads and writes, except that a
write to a channel's own set while that channel runs is refused. Above
the descriptor RAM, the low-address words ``0x4c0``, ``0x4c8``,
``0x4d8``, ``0x4e8`` and ``0x4f0``, which the Octane driver writes at
start with the engines off, are stored with no effect.

When an Octane output pass ends without end of descriptor, the model
loads the entry that control bits 6:3 name into the working registers
and plays on from its start, a model choice: both drivers link to a copy
of the working descriptor. A link to an entry that reads all zero logs
``LOG_UNIMP`` and stops the channel with its start bit still set, since
what hardware does there is *unknown*. An entry with no address, no
length, more than 1 MiB or a part frame logs ``LOG_GUEST_ERROR`` and
stops the same way.

.. _sgi-rad1-subcode-windows:

Output subcode windows
----------------------

The drivers write AES3 and ADAT output subcode with 64-bit stores and
never read it back, *software expectation*: ADAT in two buffers of 15
doublewords at ``0x1000`` and ``0x1080``, AES3 in two buffers of 16 at
``0x1100`` and ``0x1180``, and two more AES3 doublewords per buffer at
``0x1200`` and ``0x1210``. Both types store the windows and drive no
signal from them. MediaIO stores ``0x1000``-``0x10f7`` and
``0x1100``-``0x121f`` in 4-byte accesses and treats ``0x10f8``-``0x10ff``
as unimplemented. The Octane stores aligned 4- and 8-byte accesses to the
listed buffers and refuses the holes ``0x1078``-``0x107f`` and
``0x10f8``-``0x10ff``. A 4-byte Octane access at ``+0`` of a doubleword
selects its numeric low half and ``+4`` its high half. Physical packing
is *unknown*.

.. _sgi-rad1-mux:

MediaIO serial word
-------------------

On MediaIO the driver shifts a 16-bit word out most significant bit
first: each bit in ``0x8c`` bit 0, clocked by ``0x4c`` bit 27 going from
clear to set, then latched by writing ``0x88`` = 2 and then 3,
*software expectation*. What the word selects on the daughtercard is
*unknown*. The model decodes it into the read-only ``mux-word`` property
and a trace event and does nothing else with it. The driver keeps
``0x4c`` bit 0 set while it shifts, so the bit-bang does not stop the
analog input.

.. _sgi-rad1-status-dma:

Status DMA
==========

The status ring is up to 128 slots of ``0x80`` bytes in host memory. A
completion writes a slot as follows, *software expectation* for the
layout the drivers read:

- ``+0x44``-``+0x74``: copies of registers ``0x04``-``0x34``.
- Even words of ``+0x00``-``+0x38``: each channel's current ring
  pointer, low 32 bits, at ``+0x00`` type 4, ``+0x08`` type 2, ``+0x10``
  type 0, ``+0x18`` type 7, ``+0x20`` type 6, ``+0x28`` type 5, ``+0x30``
  type 3 and ``+0x38`` type 1. The model writes a pointer once its channel
  has started since reset, and its stop-time value after a stop. The
  Octane writes zero for a channel never started. Odd words are left
  alone.
- ``+0x78``: receiver and video flags. Bit ``0x40`` is ADAT present;
  bits ``0x80`` and ``0x100`` are video field events.
- ``+0x7c``: completion word. Bit 31 is done, bit 19 means no valid AES3
  input, bits 26:24 an AES3 rate code.

The model brings every running channel up to the present first and writes
the done word last. With no inputs connected the flags word is zero and
the done word ``0x80080000``.

Writes to ``0x5c``, *software expectation*:

- ``0x40001`` starts one transfer into the slot at the set-A base. The
  driver then polls ``+0x7c`` bit 31, 100 times at 5 µs. The model
  completes it before the write returns, an *approximation*.
- ``0x40003`` with an interval in bits 16:2 starts continuous completions
  around the ring. IRIX 6.5.30 writes ``250 * 33 + 1`` = 8251 there, so
  ``0x480ef``. That the field counts 33 MHz PCI clocks, about 250 µs, is
  *inferred*; the model uses that unit. Set A's limit gives the slot count
  as ``0x78 - slots * 0x80``; 128 slots is ``0xffffc078``.
- ``0x1fffc`` stops the ring.

Continuous completions start at slot 1, the first the driver checks, and
run on an absolute schedule: completion *k* is due *k* intervals after the
start, so a late timer catches up back to back. A timer more than a ring
behind restarts the schedule from the present. Each is an *approximation*.
The driver needs completions about every 250 µs: its mixer refills the
output only about ``0xcc`` words ahead of the last pointer it saw.

MediaIO accepts any limit, using one slot when it does not decode; an
interval of zero stops the ring, and one below 330 clocks is raised to
330 to protect the host. Other values are stored. The Octane accepts only
the driver's geometry: a one-shot with set A limit ``0xfffffffc``, when
the ring is stopped; a continuous start with a nonzero interval and both
sets naming the same address with limit ``0xffffc078``, when stopped; and
the stop word. Anything else is refused.

.. _sgi-rad1-channels:

Audio channels
==============

From the enable edge each running channel walks its ring a frame at a
time, wrapping to the start with no re-arm, paced by a virtual-clock
timer at its sample rate. Outputs read frames from the ring and pass
them to a host audio voice; inputs take frames from the voice, or
silence, and write them into the ring. Clearing the enable bit stops the
channel where the sample clock has reached; its pointer and positions
hold. Enabling again starts from the ring base while the position counts
carry on. The enable edge, continuous wrap and hold are *inferred* from
the driver's programming; restart from base is an *approximation*.

A frame is one 32-bit word per channel: two for analog and AES3, eight
for ADAT. Each word holds a 24-bit signed sample in bits 23:0,
sign-extended through 31:24. IRIX writes the left channel in the first
word, *software expectation*; that the converter plays it as left is
*inferred*. The host voice is 16-bit stereo: output takes bits 23:8,
input fills bits 23:8 and leaves 7:0 zero. ADAT channels 3 to 8 are
dropped on output and silent on input. The subcode channels write zero
records, 256 bytes for type 6 and 128 bytes for type 7.

Rates, each an *approximation*:

- Analog input and output, AES3 output and ADAT output: 44.1 kHz, the
  software rate the driver gives the analog output clock. Which bits of
  ``0xa0`` and ``0xa4`` select a hardware rate is *unknown*.
- ADAT input: 48 kHz, the driver's ADAT clock default.
- AES3 input: the ``aes-input-rate`` property, or 48 kHz with none.
- MediaIO subcode channels: one record per frame of the parent input.
- Octane AES3-input subcode: one record per 192-frame AES3 block at
  48 kHz, 250 a second. The driver unpacks 192 bits of channel status
  per record and advances its frame count by 192 per record, so the block
  size is *inferred*.

The analog pair uses the ``audiodev`` property. MediaIO's AES3 and ADAT
interfaces use the optional ``aes-audiodev`` and ``adat-audiodev``; an
unbound interface drops output and records silence. An input voice opens
the first time its channel runs, so with a capture-capable backend the
guest records the host's input. With no backend, or one that cannot
capture, the input records silence.

On the Octane only the analog input (``0x4c`` bit 0, both sets the same
nonzero address, control ``0xfffff000``), the analog output (``0x58`` =
``0x20000001``, control ``0xfffff008``) and the AES3-input subcode
channel (``0x48`` bit 2, control ``0xfffff030``) run, each on a 4 KiB
ring. An enable without valid sets is refused and changes nothing.
Writing bit 0 of ``0x44``, ``0x48``, ``0x50`` or ``0x54``, or bit 2 of
``0x44``, is refused. ``0x58`` accepts only ``0x20000000`` and
``0x20000001``.

.. _sgi-rad1-receivers:

Digital inputs and video reference (MediaIO)
--------------------------------------------

What is cabled to the MediaIO's digital inputs and genlock BNCs is host
configuration, given by properties that are not migrated and must match
on both ends:

- ``aes-input-rate``, 0 (none), 32000, 44100 or 48000. A locked source
  makes every done word carry rate code 3, 2 or 1 with bit 19 clear:
  ``0x83000000``, ``0x82000000`` or ``0x81000000``. The driver decodes
  codes 7, 5 and 4 to the same rates; choosing 1 to 3 is an
  *approximation*.
- ``adat-input``: a present source sets ``+0x78`` bit ``0x40`` on every
  completion.
- ``genlock``: ``none``, ``ntsc`` or ``pal``. The driver counts slots
  carrying ``0x80`` or ``0x100``, divides elapsed time by the count and
  classifies 59.94 Hz or 50 Hz fields, *software expectation*. The model
  sets one bit for each field begun since the last completion, at most
  two, alternating ``0x80`` on even fields and ``0x100`` on odd. Which bit
  marks which field is *unknown*. Register ``0x34``'s sync-present flags
  stay zero because their polarity is *unknown*.

The Octane type refuses all three at realize.

Interrupts
==========

Neither type raises an interrupt or requests an interrupt pin. The
drivers poll registers and the status ring and register no data or
status interrupt; the Octane driver's imports include no interrupt
registration, *software expectation*. The reference Octane2's RAD1
requests no interrupt pin in its configuration header
(:ref:`sgi-rad1-octane2-config`), *established* for that chip. Whether
the silicon can interrupt by another route is *unknown*. Bridge, Crossbow and HEART report transport errors as
their own pages describe.

DMA and byte order
==================

Every status and sample transfer goes through the device's PCI DMA
address space, so the Bridge's direct, translated and 64-bit
addressing and its byte swapping apply (:doc:`bridge`). The model writes
status words and moves samples as little-endian PCI words. The drivers
read the status ring with plain big-endian loads, so with the slot's DMA
swap bits clear the Bridge's lane reversal puts the done bit in bit 31
of the word the driver loads; setting a swap bit reverses it. RAD's own
lane wiring is *unknown*; little-endian words are an *approximation*.

Register accesses are little-endian PCI words too. With the slot's
programmed-I/O swap bit clear, as IRIX leaves it, a big-endian 32-bit
load returns the register's numeric value. OpenBSD's choice of unswapped
accessors for RAD1 and the drivers' plain word loads agree,
*software expectation*. The Bridge splits a doubleword into two 4-byte
cycles, lower address first, unless the slot's 64-bit bit is set
(:doc:`bridge`).

On MediaIO a status or sample DMA failure logs ``LOG_GUEST_ERROR``; a
failed slot mirror stops the continuous ring and a failed ring transfer
stops its channel. On the Octane any failed status write stops the ring
and publishes no done word, and a failed sample transfer stops the
channel without advancing positions for the failed chunk; earlier chunks
keep their progress. No error register or interrupt is synthesised.
These are each an *approximation*.

Reset and migration
===================

Reset stops every engine and clears all registers, descriptors,
positions, pointers and output subcode to zero, *approximation*. The
MediaIO UST counter restarts from zero and runs; the Octane counter reads
zero, stopped and disarmed. Host voices stay open.

Migration works only between identical builds. ``sgi-rad1-audio`` and
``sgi-rad1-octane`` carry separate state. The Octane saves the counter,
its phase and its enable and arm state, and rebases every engine's time
reference on load so migration adds no ticks, frames or completions. A
state whose ring geometry or schedule could misdirect DMA fails to load.

.. _sgi-rad1-measurements:

Measurements
============

These reads come from the :ref:`sgi-measured-octane2` in the PROM Command
Monitor, after the firmware had initialised the machine and before IRIX
loaded. No read is a reset value. The single read of configuration
``0x10`` was reported as a value only; the full header read below repeats
it with its monitor output.

.. list-table::
   :header-rows: 1
   :widths: 30 10 16 18 26

   * - Address
     - Width
     - Value
     - Chip revision
     - When
   * - ``0x1f023008``, Bridge device 3 configuration ``0x08``
     - 32-bit
     - ``0x000000c0``
     - ``0xc0``, inventory "12.0"
     - PROM monitor, after PROM start-up
   * - ``0x1f023010``, Bridge device 3 configuration ``0x10``
     - 32-bit
     - ``0x00600000``
     - ``0xc0``
     - PROM monitor, after PROM start-up
   * - ``0x1f023000``-``0x1f02303f``, the whole configuration header
     - 32-bit
     - see below
     - ``0xc0``
     - PROM monitor, after PROM start-up
   * - ``0x1f700030``, BAR0 ``0x30``, twice, several seconds apart
     - 32-bit
     - ``0``, ``0``
     - ``0xc0``
     - PROM monitor, after PROM start-up

.. _sgi-rad1-octane2-config:

Configuration header
--------------------

.. code-block:: text

   >> dump -w -x 0x900000001f023008#1
   0x900000001f023008:         c0
   >> dump -w -x 0xffffffffbf023000#16
   0xffffffffbf023000:      510a9    4800006         c0          0
   0xffffffffbf023010:     600000          0          0          0
   0xffffffffbf023020:          0          0          0          0
   0xffffffffbf023030:          0          0          0          0

.. list-table::
   :header-rows: 1

   * - Offset
     - Value
     - Fields
   * - ``0x00``
     - ``0x000510a9``
     - vendor ``0x10a9``, device ``0x0005``
   * - ``0x04``
     - ``0x04800006``
     - Status ``0x0480``, slow DEVSEL timing and fast back-to-back
       capable; Command ``0x0006``, memory space and bus master
   * - ``0x08``
     - ``0x000000c0``
     - class ``0x000000``, revision ``0xc0``
   * - ``0x0c``
     - ``0``
     - cache line size, Latency Timer, header type 0, no BIST
   * - ``0x10``
     - ``0x00600000``
     - BAR0, 32-bit memory at ``0x600000``
   * - ``0x14``-``0x3f``
     - ``0``
     - no further BARs, no subsystem identity, no expansion ROM, interrupt
       pin 0 and line 0

Aligned 32-bit reads of Bridge device 3, function 0, on IP30 revision B
with HEART revision F. The identity, class, revision, Status and
interrupt pin are *established* for this chip: the function requests no
interrupt pin, and its Status reports slow DEVSEL timing and fast
back-to-back capability (bits 10:9 and 7, :src:`pci-local-bus-2.2`). The
same machine's inventory prints "RAD audio revision 12.0". Command and
BAR0 are what the firmware wrote. BAR0 is the base the PROM had
assigned, not a sizing response, so it says nothing about the BAR's
size. The header shows neither cold reset nor write masks.

.. _sgi-rad1-octane2-ust:

UST counter at the PROM
-----------------------

.. code-block:: text

   >> dump -w -x 0xffffffffbf700030#1
   0xffffffffbf700030:          0
   >> dump -w -x 0xffffffffbf700030#1
   0xffffffffbf700030:          0

The two reads of BAR0 ``0x30``, through Bridge device window 3 at the
BAR's PCI address ``0x600000``, were several seconds apart, within about 8 s by the
HEART counter (:ref:`sgi-measured-octane2-counter`). Both returned zero, so at the PROM
menu the Octane UST counter holds zero and does not advance, *established*
for this chip at that point. That it stays stopped until a driver
enables it is *inferred*; that zero is its power-up value, rather than a
value the PROM left, is not shown.

.. _sgi-rad1-limits:

Model limits
============

- Every stored register's readback, every reset value, the BAR size, the
  MediaIO class code and the MediaIO PCI status bits are model choices.
  The MediaIO type reports medium DEVSEL timing and fast back-to-back
  capability; no source gives either.
- On the Octane, while ``0x88`` holds 2 or ``0x8c`` holds 3, the analog
  output sends silence to the host and the transfer carries on, so
  positions and the start bit advance as before. A write to either
  register first plays the frames already due under the old state. The
  IP30 PROM writes ``0x88`` = 2 after its boot tune and the IRIX 6.5.30
  kernel at halt, and both start sequences write the release values
  ``0x88`` = 3 and ``0x8c`` = 2, each a *software expectation*. No traced
  sequence writes ``0x8c`` = 3. That these values mute, and that the
  transfer runs on while the output is silent, are *inferred*; no public
  source ties either register to the channel.
- Gain, clock-generator rates, PLL lock, external clocks, analog input
  selection, the serial word and the MediaIO mute bits have no effect.
- The Octane runs no AES3 or ADAT audio channel, no ADAT-input subcode
  channel and no video reference. Their enable bits are refused. The
  Octane driver also writes ``0x40000000`` and ``0x40000001`` to ``0x58``
  at boot; the model refuses both.
- MediaIO ignores set B, starts a channel whose control tag does not
  match its type after logging ``LOG_GUEST_ERROR``, clamps a ring longer
  than 1 MiB and refuses a ring that is not a whole number of frames.
- Subcode records are zero; nothing on the host supplies AES3 channel
  status or ADAT user bits.
- A host input that delivers fewer frames than the sample clock owes is
  padded with silence, so recordings have gaps rather than stretching.
- MediaIO: an unimplemented offset logs ``LOG_UNIMP`` and reads zero; a
  write to a read-only register logs ``LOG_GUEST_ERROR``; an access other
  than 4 bytes fails as an invalid access (``LOG_INVALID_MEM``).
- Octane: any refused access, including 1-, 2- and 8-byte accesses
  outside the subcode windows, misaligned accesses and unrecognised
  values, logs ``LOG_UNIMP``, changes no state and fails the access back
  to the Bridge, which reports it to the guest as a PCI error.

Tests
=====

``tests/qtest/octane-test.c``, the ``/octane/rad-*`` and
``/octane2/rad-pci-identity`` cases, covers the identity word through the
Bridge configuration window on both machines, the read-only PCI Status
``0x0480`` across a write and a reset, BAR sizing, start-up
storage and refused enables, the subcode windows under all four Bridge
width and swap settings, one-shot and continuous status DMA through
direct, translated and 64-bit Bridge addresses, the analog output and
input clocks, the AES3-input subcode channel, ring overrun, sample
conversion, the relay and DAC mutes over a played span, descriptor links
to entries 1 and 7 past the first lap and to a never-programmed entry,
DMA failures, refused accesses, the UST counter's enable
sequence, word value and wrap, and migration.

``tests/qtest/sn0-baseio-test.c``, the ``sn0/baseio/io6g-rad1-*`` cases,
covers the MediaIO slot and identity, register storage, status DMA, every
data channel and subcode channel, the digital receiver and genlock
properties, the serial word, the UST counter and migration.

``tests/unit/test-sgi-rad1-audio-pcm.c`` pins the ring-word and host
sample conversions. The functional IRIX diagnostic matrix in
``tests/functional/mips64/test_irix_diag.py`` plays audio through
MediaIO to a host wav file and checks that nonzero samples arrive.
