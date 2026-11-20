.. SPDX-License-Identifier: BSD-3-Clause

.. _sgi-divo-page:

==================================
DIVO: digital video option board
==================================

Used in the Onyx2 and Origin 2000 (:ref:`sgi-ip27`): the DIVO is an
:term:`XIO` board that takes one XIO slot. The model fits it with
``-device sgi-divo`` and is off by default.

.. seealso::

   Platform pages: :ref:`sgi-ip27`.
   Related parts: :doc:`bridge` is the chip the board's two PCI functions
   sit behind; :doc:`pci-expansion` covers the board family the DIVO
   belongs to; :doc:`xt-digvid` is the other XIO video board the models
   carry; :doc:`at29c040a` is the flash part the DIVO's flash resembles.

The DIVO is a digital video input and output board. It is a
:term:`Bridge` with two PCI functions, called LINC 0 and LINC 1 here, one
per video pipe, and behind each a VIP chip (the PCI id database lists it
as "DiVO VIP"), FPGAs, a flash and a firmware that runs on the board. The
IRIX video daemon talks to that firmware through a mailbox in the LINC's
memory window, so a guest sees nothing of the board without something
that answers the mailbox.

QEMU models the board as ``sgi-divo`` (``hw/mips/sgi/board-pci-expansion.c``)
and each LINC as the PCI device ``sgi-divo-linc`` (``hw/misc/sgi-divo.c``).
The model is a "present, enumerates, opened by the video daemon" model: the
two functions, their config space, the window with its few live registers,
the mailbox, the flash, the packet ring, the status block and the interrupt,
and a firmware *stand-in* that answers the mailbox without running any board
code (see `Firmware stand-in`_). It plays video out: a memory-to-video
transfer completes at the frame rate and its frames go to a file sink (see
`Video out`_). Video in is not modelled.

Facts are rated as described in :ref:`sgi-ratings`.

Sources
=======

No specification of the LINC, the VIP or the board's other blocks is
public. The register and window facts come from a read of the shipped DIVO
2.4 IRIX driver and video library. Each is a *software expectation*: it
shows what the shipped software does with the board, not what the chips
are. The model never runs, and ships, no board firmware.

The video path (the status block, the DMA buffers and the packets that
complete them, the control words and the frame time) comes from a second
read of the 2.4 driver and library, made for this model. Each fact below
names where it was seen: the driver archive ``/usr/cpu/sysgen/IP27boot/divo.a``
(offsets in its interrupt, DMA and ioctl modules) and the video daemon's
plug-in ``/usr/lib32/dmedia/video/DIVO.so`` (addresses in its linked image).
The frame's pixel layout is the exception: no read settled it, and it is
marked *approximation* where it is used.

Where each fact was seen, as an offset in the driver archive's module or an
address in the linked plug-in. Interrupt module: the handler that reads the
ring at ``+0x38``, the dispatch on a packet's type at ``+0x1b0`` and the
completion of a DMA slot at ``+0x824``. DMA module: the buffer loop that
builds a descriptor and sends the mailbox words at ``+0x54``. Ioctl module:
the mailbox store, with its consumer-index wait, at ``+0x29c8`` and the
refresh of that index at ``+0x2bec``. Plug-in: the control-word builder at
``0x7d14c0``, the transfer start at ``0x7dab20``, the transfer manager that
sends a stream's buffers at ``0x7dad80`` and the transfer end at
``0x7db330``.

- The *DIVO (Digital Video Option) XIO Board Installation Instructions*,
  108-0173-001, for the slots that take the board.
- The DIVO Option and DIVO-DVC Option release notes shipped with the 2.4
  software, for the part numbers, the ``hinv`` lines and the flash timing.
- Published ``hinv -mv`` listings of boards with parts 030-1046-002,
  030-1305-001, 030-1305-003 and 030-1387-001.

Identity
========

.. list-table::
   :header-rows: 1
   :widths: 42 18 40

   * - Fact
     - Rating
     - Source
   * - The board is an XIO Bridge with two PCI functions of vendor
       ``0x10a9`` and device ``0x0002``, the only ids the driver
       registers for.
     - *software expectation*
     - The shipped DIVO 2.4 IRIX driver. pci.ids (pciutils/pciids at
       f56421679d66) names ``10a9:0002`` "Linc I/O controller".
   * - The driver reads the board's ID :term:`PROM` text and accepts the parts
       ``030-1046``, ``030-1305`` and ``030-1387``. A ``030-1046`` board needs
       a three-digit dash number or a revision letter ``C`` or later.
     - *software expectation*
     - The shipped DIVO 2.4 IRIX driver.
   * - A ``030-1046`` board has its second function in PCI slot 1; a
       ``030-1305`` or ``030-1387`` board has it in slot 2. The first is
       always in slot 0. The driver refuses any other pairing.
     - *software expectation*
     - The shipped DIVO 2.4 IRIX driver.
   * - The parts seen in public listings are ``030-1046-002`` (revisions F
       to H), ``030-1305-001`` (revision E) and ``030-1305-003``
       (revision A).
     - *observed*
     - Release notes example and published listings.
   * - The two functions raise INTA, one interrupt each.
     - *software expectation*
     - The shipped DIVO 2.4 IRIX driver.
   * - The board is a plain Bridge; its part number and revision are not
       read by the driver.
     - *inferred*
     - Nothing in the shipped software reads them. The model uses the
       default Bridge revision.
   * - The PCI class code of the LINC is not read by the driver.
     - *unknown*
     - The model reports multimedia video.

``-device sgi-divo,slot=io2`` places the board in an empty XIO slot as other
XIO PCI boards (:doc:`pci-expansion`). The ``product`` property picks
``divo`` (part ``030-1305-001``, revision E, LINC 1 in slot 2; the default)
or ``divo-1046`` (part ``030-1046-002``, revision F, LINC 1 in slot 1). The
barcode is the marked synthetic one the board family builds. The DIVO-DVC
(part ``030-1387-001`` with two ``030-1388`` codec cards) is not modelled; its
product name is refused. The installation guide excludes some slots (not
slot 1 of an Origin 2000 or an Onyx2 rackmount, and not slot 4 of an Onyx2
deskside). The model fits any empty XIO slot, an *approximation*.

Config space
============

.. list-table::
   :header-rows: 1
   :widths: 14 56 30

   * - Dword
     - What the driver does and the model gives
     - Rating
   * - ``0x04``
     - The driver ORs ``0x6`` in: memory space and bus master enable.
       Standard command register.
     - *software expectation*
   * - ``0x10``
     - Base of a 128 MiB memory window. LINC 1's base must be exactly
       LINC 0's plus ``0x8000000``, or the attach fails. The model has
       a 128 MiB 32-bit memory BAR here; the guest's allocator places it.
     - *software expectation*
   * - ``0x14``
     - Not a BAR. The model has none there, so it reads 0 and a write is
       dropped. The consumer index is in the status block, not here (see
       `Status block`_).
     - *software expectation*

Window map
==========

Offsets are bytes from the base of one LINC's window. The model keeps every
offset not listed below as plain storage, as the shipped software expects,
and that includes the one-off setup writes the driver makes. Accesses are
made through the Bridge's translated window, which IRIX uses for a base too
large for a device window. Software maps it asking for word values (the
Bridge's memory swap bit clear), an *inferred* choice from the one other
board that uses it. At that setting a 32-bit access is the same at the
device whatever the swap, and only a byte or halfword access differs.

.. list-table::
   :header-rows: 1
   :widths: 24 46 30

   * - Offset
     - Behaviour
     - Rating
   * - ``0x4400000`` + n * ``0x10000``, n = 0 to 31
     - Mailbox slot n, one 64-bit command word per store (see `Mailbox`_).
     - *software expectation*
   * - ``0x4c00800``
     - Reads ``0x000810a9``; the boot-mode sequence refuses a board that
       does not return it. The value looks like vendor ``0x10a9``,
       device ``0x0008``. The model ignores a write here.
     - *software expectation* for the value, *inferred* for the meaning
   * - ``0x4c00804``, ``0x4c0080c``, ``0x4c00f10``
     - Written once in the boot-mode sequence. Plain storage.
     - *software expectation*
   * - ``0x7800000``
     - LINC control. Plain storage except bit 6, which reads 0. The
       driver reads bit 6 after a release from reset and treats 1 as
       "still coming up".
     - *software expectation* for the use, *inferred* for the meaning
   * - ``0x7800018``
     - LINC status and acknowledge (see `Interrupt`_).
     - *software expectation*
   * - ``0x7800008``, ``0x7800020``, ``0x7800100`` to ``0x7802008``,
       ``0x781000c``
     - Control, interrupt-control, byte-bus and LED words the driver writes
       or read-modify-writes. Plain storage; the model does not mask the
       interrupt by bits 21:20 of ``0x7800020``.
     - *software expectation* for the accesses, *approximation* for the
       storage
   * - ``0x5000000`` to ``0x5000608``, ``0x7000040``, ``0x7400040``,
       ``0x7e08000``, ``0x7e08004``
     - Reached by the driver or through firmware reads. Plain storage.
     - *software expectation*
   * - ``0x7c00000`` to ``0x7c7ffff``
     - The flash (see `Flash`_).
     - *software expectation*
   * - Any other offset
     - Plain storage across the 128 MiB. The diagnostic ioctls read a
       word at a caller offset, which the storage answers.
     - *software expectation*

Interrupt
=========

The LINC raises INTA when the firmware has posted a packet. The model keeps
one pending bit, bit 20 of ``0x7800018``: it is set when the stand-in posts a
packet, drives the PCI interrupt line, and is write-1-to-clear. The handler
writes ``0x100000``, reads the register back and repeats while bit 20 stays
set (*software expectation*); a clear with a packet still unposted drops the
line, and a later packet raises it again. A write of ``0x300000`` clears bits
21 and 20 together. Bit 21 is the firmware-panic flag; the model never sets
it (*software expectation* for the use, *inferred* for the name). Other bits
of the register are plain storage.

Mailbox
=======

The host sends commands as 64-bit words in a 32-slot ring and reads the
board's consumer index from the status block before each store
(*software expectation*). The opcode is the top six bits of the word.

.. list-table::
   :header-rows: 1
   :widths: 14 56 30

   * - Opcode
     - Use and what the model does
     - Rating
   * - ``0x00``, ``0x02``
     - Board write of a byte or a word: the board address is bits 57:32 and
       the data the low bits. Accepted and dropped.
     - *software expectation* for the words, *approximation* for the drop
   * - ``0x03``
     - Control word: the control index is bits 57:52 and the value the low
       16 bits (32 for the event mask). The model keeps the last value of
       each index and acts on five: 0 (frame width in pixels), 1 (lines in a
       field), 2 (dwords in a row of memory), 6 (capture type) and ``0x13``
       (video DMA), all described in `Video out`_. The host also sends
       ``0x15`` (an event mask) and others; they are stored and have no
       effect, so no event packet is ever sent. The bits of a ``0x13``
       word above bit 1 hold stale address bits, which the model ignores.
     - *software expectation* for the word and the five indexes, *unknown*
       for the others
   * - ``0x09``
     - Announces the packet ring base in host memory. The shipped
       text describes it both as the second 4 KiB half and, elsewhere, as an
       8 KiB ring. The model takes the address as the ring base and
       restarts the ring at entry 0.
     - *software expectation* for the opcode, *unknown* for the extent
   * - ``0x0a``
     - Ring entry count, ``0x40``. Accepted.
     - *software expectation*
   * - ``0x0b``
     - Index of the last packet the host consumed. The model sets its next
       entry to the one after it.
     - *software expectation* for the opcode, *inferred* for the use
   * - ``0x0c``
     - Base of the host's 16 KiB packet buffer, which holds the status
       block. The ring base announced by ``0x09`` is 8 KiB above it, as
       the host's own layout says. The model keeps the address and writes
       the status block there (see `Status block`_).
     - *software expectation*
   * - ``0x0d``
     - The host-to-board clock offset. Accepted.
     - *software expectation*
   * - ``0x10``, ``0x12``
     - Read a word or halfword at a board address. The model replies (see
       `Firmware stand-in`_).
     - *software expectation*
   * - ``0x20``
     - Block send: a count of command words (at most 127) and a scratch
       slot 0 to 3, and a DMA address of the block. The words are the
       host's other command words, one per 8 bytes, and the model runs
       each as if it had come alone, so a control word in a block counts.
       The completion packet follows.
     - *software expectation*
   * - ``0x21``, ``0x24``
     - Video DMA buffer for slot 0 and slot 1: a page count in bits 57:49
       and the DMA address of the buffer's descriptor (see `Video out`_).
     - *software expectation*
   * - ``0x23``, ``0x25``, ``0x27``, ``0x28``
     - Packer or FPGA data block load: a DMA address.
     - *software expectation*
   * - ``0x26``
     - Abort of the video DMA, sent when a transfer fails. The model drops
       the queued buffers and stops.
     - *software expectation* for the use, *inferred* for the name
   * - Any other
     - Logged with ``LOG_UNIMP`` and dropped.
     - *approximation*

The DMA address in a word is ``address >> 7`` in bits 40:0 and
``address >> 56`` in bits 47:41, which holds seven of the address's top
eight bits (*software expectation*). Address bit 63 is therefore lost. The
model sets it (*inferred*): a widget on a :term:`Crossbow` has a number from
8 to 15, so bit 63 is always set, and the host's first ring announcement
carries ``0x10`` there, the low seven bits of the top byte ``0x90`` that the
same host's other DMA to the :term:`Hub` uses (widget 9).
Blocks are 128-byte aligned (*software expectation*).

The model's consumer index after each word is that word's slot, as if the
board drained the ring at once. The host treats the ring as full when its
next index equals the index it last read, so an index that tracks the last
slot written never blocks it (*inferred*).

The same index also arrives as an interrupt packet, type ``0x01000000`` with
the slot in word 1, which the host copies to a shadow and uses to wake a
writer that is waiting for room (*software expectation*). The model sends
one after every word except opcode ``0x0b``, whose own packet would
set off an endless exchange (*approximation*).

Status block
============

The host zeroes a 16 KiB buffer, announces its base with opcode ``0x0c`` and
the ring 8 KiB above it with ``0x09`` (*software expectation*). Before it
stores a mailbox word the host compares its next slot with the consumer
index it last saw and, when they are equal, reads the index again from dword
``0x14`` of that buffer, at most 200 times at 5 ms, then gives up with "the
firmware is not updating" and fails the store (*software expectation*). Two
doublewords at ``0x00`` and ``0x08`` are the board's clock and its frame
counter, which the host reads twice until both reads agree when a guest asks
for the time of a frame (*software expectation*).

The model writes the status block with the PCI DMA path, as it writes the
ring: the consumer index after every mailbox word, and the two doublewords at
each frame time of a running video stream. The clock is the virtual time in
nanoseconds, the unit being *inferred*: the host adds its own offset and
converts the sum as a time in nanoseconds. Before this model kept the index
in the status block it answered a config dword and the host's own wait never
saw it change; a guest that sent 32 words stalled at the wrap to slot 0 and
failed with that message, which is what a guest run showed.

Flash
=====

The flash is 512 KiB with the identification ``0x1fa4`` (Atmel part
``0x1f``, device ``0xa4``), 256-byte sectors and a toggle-bit status
(*software expectation*). It resembles the AT29C040A (:doc:`at29c040a`),
but its erase sequence is not one that data sheet defines, so the exact part
is *unknown*.

The model's flash is a byte array that starts erased (all ones) and takes
the command sequences the shipped library uses, each opened by the unlock
cycles ``0xaa`` at ``0x5555`` and ``0x55`` at ``0x2aaa``:

- ``0x90`` at ``0x5555`` enters identification mode, where a
  halfword read at 0 gives ``0x1fa4`` (*software expectation*); the model
  splits it as ``0x1f`` at offset 0 and ``0xa4`` at offset 1 (*inferred*,
  from the part's data sheet); ``0xf0`` leaves it.
- ``0xa0`` at ``0x5555`` loads one 256-byte sector: the next 256 bytes
  written take effect at once.
- ``0x80`` at ``0x5555``, the two unlock cycles again, then ``0x30`` at a
  sector erases it to ones. Further ``0x30`` writes erase further sectors
  until ``0xf0`` or another command, as the library erases several
  sectors after one prefix (*software expectation*). No whole-part erase
  is modelled.

Both a program and an erase finish at once: a poll sees the written byte,
or ``0xff`` with bit 3 set and a steady bit 6 after an erase. That is an
*approximation* of a part that needs milliseconds. The shipped software
compares the status bits as the data sheet describes the toggle (*software
expectation*).

The video library keeps ten flash sections in step with files shipped with
the software, checking each section's header, and burns any that differ
(*software expectation*). The board's firmware and FPGA images are
vendor software and the model holds none, so the flash starts erased and
the library burns every section on its first start, which the release notes
time at about 45 seconds a board. That is what a blank part does and is
the *approximation* chosen over shipping vendor images. The flash does not
persist across runs.

Packet ring
===========

Each LINC has a 64-entry ring of 128-byte entries in host memory, 8 KiB in
the second half of a 16 KiB buffer. At release from reset the driver writes
``0xffffffff`` to the dword at entry offset ``0x78`` and 0 to the dword at
``0x7c`` of every entry. An entry is valid when its dword at ``0x7c`` is
non-zero, which the board writes last; word 0 is the type. The host reads
from the entry after its saved index and stops at the first invalid one
(*software expectation*). The model writes its packets with the PCI DMA
path, as 32-bit little-endian words that a Bridge DMA window without byte
swapping delivers as the host's word values (*inferred*), starting at entry 0
after the ring is announced (*inferred*: the host's initial saved index is
not stated). A packet whose slot is still valid is dropped and logged with
``LOG_GUEST_ERROR``.

Video out
=========

A memory-to-video transfer, as the shipped video library runs it, is a
stream of buffers. The host starts the stream with control word ``0x13``
(bit 0 set), sends each buffer in turn and sends bit 0 clear when it has
sent the last one, without waiting for the buffers to play out. It then
waits for the completion of each buffer it sent. All of this is *software
expectation*.

Each buffer is a descriptor in host memory and a mailbox word, opcode
``0x21`` for the first buffer slot and ``0x24`` for the second, the host
alternating between them. The word carries the descriptor's DMA address like
a block (see `Mailbox`_) and the number of 16 KiB pages in bits 57:49. The
descriptor has a 0x50-byte header, whose first dword is the length in dwords,
whose dwords at ``0x44`` and ``0x48`` are the descriptor's own address and
whose dword at ``0x4c`` is a checksum, and then one doubleword for each page: the
page's bus address as the host's DMA call returned it. A page the host
reached with a 64-bit address has the hub's top nibble in the doubleword; a
page it reached through the Bridge's 32-bit direct window comes as that 32-bit
address sign-extended (*software expectation*). The board completes a buffer
with a packet of type ``0x8000`` whose word 1 is the slot (0 or 1), words 2 and
3 the board clock and words 4 and 5 the frame counter, high word first
(*software expectation* for the fields' use, *inferred* for the units).
The host's loop stops when it has sent as many buffers as it was asked for and
waits for each slot's completion before it frees the descriptor, and it does
not send a third buffer until the first has completed.

The pixels are the pages' bytes in the order they have in host memory. The
Bridge's DMA window delivers each dword of them reversed, so the model puts
each dword back (*inferred*; a guest run with a known picture showed it).

What the host tells the board about a frame is four control words, read
from the plug-in's memory-node control routine (``DIVO.so`` ``0x7d8100``,
the size routine at ``0x7d63a0``): word 0 is the width in pixels, word 1 the
lines in a field (243 or 288), word 2 the dwords in a row of memory and
word 6 the capture type. Word 2 is the packing's dwords for each smallest
group of pixels times the width over the pixels in a group, which is
width times bits a pixel over 32: 360 for 720 pixels at 16 bits, 540 at 24
bits and 720 at 32. Both 360 and 540 were seen from guests, the first from
``memtovid`` and the second from Inferno 6.2.5 (*software expectation*).
The capture type is 1 for a frame of both fields, whose word 1 is half the
buffer's lines, and other values for one field of word 1's lines. The model
plays a frame of both fields only for type 1 and one field otherwise, and
a reset sets type 1 (*approximation* for the other values: 4 is the value
the host writes when it is asked for 0 and for 4, and what it means is
not established).

The host chooses the packing among the 51 of the plug-in's table, at
``0x7e4af8``, each named as the packings of the owner's guide, appendix C
(007-3524-004, tables C-1 and C-2): from 8 to 64 bits a pixel, RGB and YCbCr,
4:4:4, 4:2:2 and the DV forms, and the reversed ``R`` order of each. It
does not send the packing to the board. The board learns it only from the
packer microcode the host loads with opcodes ``0x23``, ``0x25`` and ``0x27``,
the file named for the packing under ``/usr/dmedia/ucode/DIVO/`` and the
vendor's. The model keeps no copy and recognises none, so it cannot know the
packing. It reads it from the row length of control word 2, and plays three:

- 16 bits a pixel as ``VL_PACKING_R242_8``, bytes Cb, Y, Cr, Y, the common 4:2:2
  packing (guide figure C-6; *established* for the layout, *approximation*
  for choosing it by the row length);
- 24 bits a pixel as ``VL_PACKING_444_8``, bytes R, G, B (figure C-11);
- 32 bits a pixel as ``VL_PACKING_4444_8``, bytes R, G, B, A (figure C-14),
  the alpha ignored.

The two RGB ones are *approximation*: the model takes the memory to hold
RGB, as the guide's RGBA colour space, not YCbCr in the same packing, which
the host chooses with ``VL_COLORSPACE`` and the board never reports. A host
that sends a 24-bit YCbCr frame gets the wrong picture. The board converts
the memory's colour space to the video format's, when they differ, with a
transcoder (the guide, chapter 1 and appendix D, and the plug-in's colour
set-up at ``0x7dbdc0``, which writes the matrix); the model converts RGB to
the limited-range BT.601 YCbCr the sinks hold with the matrix of the guide's
appendix D and the average of each pair's chroma, so the sink's chroma
is an *approximation* of the board's filter. Any other row length, and any
other number of pages than the frame needs, is completed and not played,
with ``LOG_UNIMP``. The other packings, 10-bit and 12-bit ones, the 16-bit
words with padding and the DV forms, are not modelled.

The frame rhythm is the model's: with the stream started, a frame time
passes (1001/30000 s for 243 lines, 1/25 s otherwise, *approximation*), the
first queued buffer, if there is one, is played and completed, and the frame
counter, which starts at 0 with the stream, goes up by one. The page for a
buffer is read when its frame time comes, not when the host queued it. A buffer queued before the start word waits and plays from it. A frame
whose width or line count is not valid is completed and not played, with
``LOG_UNIMP``. A
stop lets the queued buffers finish and then ends the stream; the abort
opcode drops them and ends it at once (*approximation*: the shipped host
sends the abort only after an error). A third buffer, which the host
does not send, is dropped.

Where the frames go. A DIVO board has one ``out-sink`` link for each pipe on
the board and on each LINC: ``-device sgi-video-file-sink,id=rec,path=...``
and ``-device sgi-divo,slot=io2,id=divo,out-sink=rec`` for LINC 0 (pipe 0) and
``out-sink1=`` for LINC 1 (pipe 1). The sink is the one the DM2 uses
(:ref:`sgi-video-io`): a Y4M file, the raster and ``F30000:1001`` in the
header, one ``FRAME`` for each buffer played. Without a sink the stream still
completes and the frames are dropped. A guest run played a known picture and
a 20-frame clip, one frame to a transfer, and the file held the pictures.

Video in is not modelled: a capture transfer completes nothing.

Firmware stand-in
=================

This stand-in is an *approximation*. Without it the video daemon's first
read of a board register sends an opcode ``0x10`` word and waits for the
reply packet for ever. The stand-in is the model's whole replacement for the
board's firmware: it executes nothing and keeps no board memory. Every packet
it sends, replies and consumer-index packets alike, goes out 100 microseconds
after the store that caused it, an *approximation*: a real board answers
after its firmware runs, and a packet that arrived inside the store could beat
the host to its wait.

.. list-table::
   :header-rows: 1
   :widths: 22 78

   * - Word
     - Stand-in action
   * - ``0x10``
     - Post a packet of type ``0x10000``: word 1 is the value (always 0),
       word 2 the status (0).
   * - ``0x12``
     - Post a packet of type ``0x20000``, same words.
   * - ``0x20``
     - Read the count of command words (8 bytes each) from the DMA address
       through the PCI DMA path, run each as a command word of its own (the
       control words matter, see `Mailbox`_), and post type ``0x08000000``
       with the scratch slot in word 2.
   * - ``0x23``, ``0x25``, ``0x27``, ``0x28``
     - Read one 128-byte unit from the DMA address, discard it, and post
       ``0x20000000`` (video packer) or, for ``0x28``, ``0x40000000`` (audio
       packer), with the scratch slot in word 1. The slot is taken from bits
       50:49 as for ``0x20``.
   * - ``0x21``, ``0x24``, ``0x26``
     - The video DMA (see `Video out`_), which is not a stand-in action.

Limitation. The reply value is 0 for every address, not what the board
holds, and a board write sent through the mailbox is dropped, so nothing a
guest wrote that way reads back. Which of the two reply types goes with which opcode, how the byte count
of a packer block is carried, and which packer a given opcode loads are not
established (*unknown*); the model's choices only have to avoid the slot
fault the host raises for a slot of 4 or more, so the slot is always 0 to 3.
No event packet is ever sent. The frame-time completions of a video-out
buffer are not replies; they are in `Video out`_.

Each reply the stand-in sends is a packet type the shipped host
code accepts, and the host's own text says a read blocks until type
``0x10000`` or ``0x20000`` arrives (*software expectation*). Whether the
chosen values are what the firmware sends is not known.

Discriminating test. ``tests/qtest/sgi-divo-test.c`` sends each opcode and
checks the packet that lands in the ring: the type, the words, that the
interrupt rises and drops with the write-1-to-clear, and that an opcode the
stand-in does not know gets only its consumer-index packet. A different reply
type, a missing consumer-index packet or a stalled completion fails it.

Model limits
============

- The DIVO-DVC board and its codec cards, the VIP, the FPGAs, the video
  engine and video in are not modelled. The packers' microcode and the
  frame format words the host sends are accepted and not run.
- A video-out transfer plays 16, 24 and 32-bit frames (see `Video out`_)
  of the width in control word 0, at the model's frame rates, and no event, retrace or sync packet
  is sent: a guest that asks for one waits.
- Mailbox board writes are dropped and mailbox board reads return 0.
- The flash starts erased on every run and completes every program and
  erase at once.
- The two LINC functions are separate devices; the board has no state of
  its own beyond the Bridge's.
- Whether a real board's PCI window answers the Bridge's byte and halfword
  swap as the model's does, the exact flash part, the PCI class code, the
  Bridge revision, the ring's initial index and the byte order of the
  pixels on the board are not known.
- The Bridge model translates a 32-bit direct-window address into the
  hub's low 2 GiB. The pages the host reached that way sit at the hub
  address that has the same value, so the model reads them there, not
  through the window. That is the model's choice, taken from where a guest
  run found the picture; the Bridge's direct window for a page above 2 GiB
  is not settled here.

Tests
=====

``tests/qtest/sgi-divo-board-test.c`` fits the board on an Onyx2 and checks
the part, revision and device of each function for both products and the
refusal of an unknown one. It does not read the ID PROM bytes through the
Bridge: the part and revision reach the Bridge as properties and the
record layout is the Bridge's. ``tests/qtest/sgi-divo-test.c`` runs one
function behind the Bridge on the libqos head and checks its identity, the
BAR size and that dword ``0x14`` is not a BAR, the window storage and the
special words (the identification word, bit 6 of the control word, the
status register), the interrupt with write-1-to-clear, the mailbox with the
consumer index in the status block, the flash command sequences, the
stand-in's replies and completions into the ring, and the video-out stream:
a frame in the sink byte for byte (even pages by a 64-bit address, odd ones
by a 32-bit one), the completions a frame time apart in order with the clock
and frame counter, the 625-line rate, a buffer that is not a frame, the
abort and the stop. The board test also checks that each output's sink
reaches its LINC.
