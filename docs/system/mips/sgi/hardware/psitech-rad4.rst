.. SPDX-License-Identifier: BSD-3-Clause

.. _sgi-psitech-rad4:

============================================
PsiTech RAD4 and RAD4-KM: PCI graphics heads
============================================

Used in :ref:`sgi-ip27` (Origin 200; the RAD4-KM's Origin 2000 placement
is not offered). The PsiTech RAD4 is a third-party PCI graphics card for
SGI systems: an 8 MB framebuffer behind a PLX PCI 9060-compatible bus
interface, with an RGB561-compatible RAMDAC, *inferred* from the driver's
register use. The RAD4-KM is the same product line with keyboard and mouse
connectors on the card. PsiTech's owner's guide puts the RAD4 in the
Origin 200 and the RAD4-KM in the Origin 2000, which has no keyboard or
mouse port of its own (:src:`psitech-rad4-owners-guide-1.3 Description`).
QEMU models both as one PCI type, ``psitech-rad4`` in
``hw/display/psitech-rad4.c``, selected by ``product=rad4`` or
``product=rad4-km``. The RAD4-KM keyboard and mouse controller is a child
device, ``psitech-rad4-km`` in ``hw/input/psitech-rad4-km.c``. QEMU offers
the card on the Origin 200 topologies only (:ref:`sgi-sn-graphics-pipes`).

.. seealso::

   Platform pages: :ref:`sgi-ip27`, :ref:`sgi-sn`.

The model is a functional profile for one software stack: PsiTech's RAD4
release 1.3k for IRIX 6.5, its kernel driver and its standalone X server,
in any of the driver's 13 display modes. No board revision, part marking
or capture of a real card is available, so the board-level behaviour
below is mostly inferred from that software and the public specifications
of the bus interface and RAMDAC. Facts are rated as described in
:ref:`sgi-ratings`.

.. _sgi-psitech-rad4-sources:

Sources
=======

- The *RAD4 Owner's Guide*, software revision 1.3, 15 November 1997,
  :src:`psitech-rad4-owners-guide-1.3`: the PCI identities, the product
  roles, the device nodes and the RAD4-KM connectors.
- The *PLX PCI 9060 Data Sheet*, version 1.2, December 1995,
  :src:`plx-pci9060-data-sheet-1.2`, for the bus interface's interrupt,
  DMA and serial EEPROM control registers. The *PLX PCI 9054 Data Book*,
  version 1.0, :src:`plx-pci9054-data-book-1.0`, agrees on the register
  offsets for the compatible family.
- IBM's *RGB561 Architecture*, Preliminary revision 1.1, 23 January 1995,
  :src:`ibm-rgb561-architecture-1.1`, for every RAMDAC register, table and
  pixel format.
- The SCN2681/SCC68681 dual UART data sheet,
  :src:`philips-scc68681-duart`, for the RAD4-KM controller's register
  layout.
- The 93C46 serial EEPROM data sheet, :src:`microchip-93c46`, for the
  configuration EEPROM's command set.
- The *RAD4C-KM Graphics Head* product sheet, publication RAD4-DS-1099,
  :src:`psitech-rad4c-km-ds-1099`.
- PsiTech's RAD4 release 1.3k for IRIX 6.5 (kernel driver, standalone X
  server, the ``rad4setres`` mode tool and the ``rad4d`` refresh daemon),
  read as binaries, as software expectations: the mode records, the
  register access sequences, the EEPROM record layout, the DMA descriptor
  chains and the keyboard and mouse controller sequence.

No Linux or OpenBSD driver exists for the RAD4.

.. _sgi-psitech-rad4-identity:

Identity and placement
======================

The owner's guide's installation example gives the ``hinv`` line for each
card: vendor ``0x12d1``, device ``0x104`` for the RAD4 and ``0x105`` for
the RAD4-KM (:src:`psitech-rad4-owners-guide-1.3 Installation`),
*established*. The guide describes both under one "RAD4 Graphics Head"
section with an 8 MB framebuffer and a 200 MHz RAMDAC. That the two cards
share a framebuffer, RAMDAC and register layout is *inferred* from that
presentation and from the driver attaching to both; no RAD4-KM capture
exists.

The model presents:

.. list-table::
   :header-rows: 1

   * - Field
     - RAD4
     - RAD4-KM
     - Rating
   * - Vendor, device
     - ``12d1:0104``
     - ``12d1:0105``
     - *established*
   * - Revision
     - ``0x03``
     - ``0x03``
     - *approximation*
   * - Class
     - ``0x0380``, other display controller
     - same
     - *approximation*
   * - Subsystem
     - ``0000:0000``
     - same
     - *approximation*
   * - Interrupt pin
     - INTA
     - INTA
     - *approximation*
   * - BAR0
     - 128 bytes, memory, bus interface registers
     - same
     - *approximation* (size)
   * - BAR2
     - 16 MiB, memory, framebuffer and board registers
     - same
     - *approximation* (size)
   * - BAR3
     - none
     - 1 MiB, memory, keyboard and mouse controller
     - *approximation*
   * - Expansion ROM
     - 8 MiB, DMA register aliases
     - same
     - *software expectation* (size)

The driver maps its BAR0, BAR2 and expansion ROM resources and requests
an 8 MiB window on the expansion ROM, so a smaller declared ROM size does
not satisfy it, *software expectation*. The revision, class, subsystem
and BAR sizes of a real card are *unknown*; a configuration-space dump of
a real card would settle them.

``-device psitech-rad4,bus=pci,addr=N`` places the card in Origin 200
:term:`Bridge` slot 5, 6 or 7 (:ref:`sgi-sn-graphics-pipes`). BAR2 is larger
than a Bridge device window, so IRIX 6.5.30 reaches it through the
Bridge's large PCI memory range (:ref:`sgi-bridge-pio`), *software
expectation*.

.. _sgi-psitech-rad4-registers:

Register map
============

Every BAR0, BAR2 and expansion-ROM access the driver makes is an aligned
32-bit load or store, *software expectation*. The model accepts only that
width; any other access fails in QEMU's memory core as a decode error.
Unlisted offsets read as zero, ignore writes and log ``LOG_UNIMP``.

BAR0: bus interface
-------------------

Offsets follow the PLX 9060 local configuration and runtime register
layout (:src:`plx-pci9060-data-sheet-1.2 Tables 45, 47, 51, 57`).

.. list-table::
   :header-rows: 1

   * - Offset
     - Name
     - Width
     - Reset
     - Access
     - Notes
   * - ``0x14``
     - Stored register
     - 32
     - ``0``
     - RW
     - Written by the driver at attach. Stored only.
   * - ``0x18``
     - Local bus region descriptor
     - 32
     - ``0``
     - RW
     - Stored only. The driver's value selects a 32-bit local bus.
   * - ``0x40``
     - Mailbox
     - 32
     - ``0``
     - RW
     - See :ref:`sgi-psitech-rad4-eeprom`.
   * - ``0x68``
     - INTCSR
     - 32
     - ``0x00010100``
     - RW, mask ``0x080f1f07``
     - See :ref:`sgi-psitech-rad4-interrupts`.
   * - ``0x6c``
     - Serial EEPROM control
     - 32
     - ``0``
     - RW, mask ``0x0700000f``
     - Bits 24-27 drive the EEPROM. See
       :ref:`sgi-psitech-rad4-eeprom`.

BAR2: framebuffer and board registers
-------------------------------------

.. list-table::
   :header-rows: 1

   * - Offset
     - Name
     - Reset
     - Access
     - Notes
   * - ``0x000000``-``0x7fffff``
     - Framebuffer
     - ``0``
     - RW
     - 8 MiB of 32-bit pixels, written under the plane mask.
   * - ``0x900000``
     - RGB561 index, low byte
     - ``0``
     - RW
     - Bits 7-0 of a 32-bit store.
   * - ``0x910000``
     - RGB561 index, high byte
     - ``0``
     - RW
     -
   * - ``0x920000``
     - RGB561 data, single-cycle
     - ``0``
     - RW
     - Auto-increments the index after each access.
   * - ``0x930000``
     - RGB561 data, multi-cycle
     - ``0``
     - RW
     - Window attribute, gamma, colour and cursor tables.
   * - ``0xa00000``, ``0xa01000``, ``0xa03000``
     - Horizontal timing banks
     - ``0``
     - RW
     - 1,024 words each. See :ref:`sgi-psitech-rad4-modes`.
   * - ``0xb00000``
     - Vertical timing bank
     - ``0``
     - RW
     - 2,048 words.
   * - ``0xd00000``
     - Plane mask
     - ``0xffffffff``
     - RW
     - See :ref:`sgi-psitech-rad4-framebuffer`.
   * - ``0xf00000``
     - Board control and status
     - ``0``
     - RW
     - Bit 1 stored. Bit 5 reads the frame event.

The four RGB561 ports in address order match the chip's index-low,
index-high, data and multi-cycle ports
(:src:`ibm-rgb561-architecture-1.1 p. 1, 27`), *inferred* from the
driver's use of them. The ports take their payload from the low eight bits
of the store. The driver's mode loader does not use bank ``0xa02000``,
and no observed driver path touches ``0xc00000`` or ``0xe00000``; the
model leaves all three unsupported.

Expansion ROM: DMA channel 0
----------------------------

.. list-table::
   :header-rows: 1

   * - Offset
     - Name
     - Reset
     - Access
     - Notes
   * - ``0x100``
     - DMAMODE0
     - ``0``
     - RW
     - Bit 10 (done interrupt) acts. The rest, including bit 9
       (chaining), is stored; the model always chains.
   * - ``0x110``
     - DMADPR0
     - ``0``
     - RW
     - Descriptor pointer. Reloaded from each descriptor.
   * - ``0x128``
     - DMACSR0
     - ``0x00001010``
     - RW
     - See :ref:`sgi-psitech-rad4-dma`.

The offsets are the 9060's own
(:src:`plx-pci9060-data-sheet-1.2 Tables 47, 51, 57`) and the 9054's
(:src:`plx-pci9054-data-book-1.0 Tables 11-76, 11-80, 11-86`),
*established*. That the board exposes them through its expansion-ROM
resource is a *software expectation*: the driver
starts DMA there. The PCI address, local address and byte count registers
of the 9060 (``0x104``, ``0x108``, ``0x10c``) are not implemented.

.. _sgi-psitech-rad4-board-control:

Board control and frame events
------------------------------

The board-control register at BAR2 ``+0xf00000`` has two jobs the driver
relies on, *software expectation*. Bit 5 (``0x20``) cleared and later set
brackets mode programming. Bit 1 (``0x02``) asks for an interrupt at the
next frame: the driver sets it when a palette or cursor update waits for
the next frame, its interrupt handler tests bit 5 and clears bit 1, and it
clears bit 1 around every DMA transfer.

The model raises a frame event once per frame period of the recognised
mode, at the mode's nominal refresh rate in virtual time. It latches the
event, and reports it in bit 5 and in INTCSR bit 15, only while bit 1 is
set. A write with bit 1 clear while an event is pending acknowledges it.
That the board gates the event on bit 1 is *inferred* from the driver,
which could never clear an unrequested event during a transfer. Whether
the board also gates the readable status, or latches it and gates only the
interrupt, is *unknown*; no shipped program appears to poll the register,
and a capture with bit 1 clear on a real card would settle it. The model
stores bit 1 only; other written bits read back as zero.

.. _sgi-psitech-rad4-modes:

Display modes
=============

The kernel driver holds 13 mode records, all four bytes per pixel with a
row stride of four times the width, *software expectation*:

.. list-table::
   :header-rows: 1

   * - Mode
     - Pixels
     - Refresh label (Hz)
     - PLL bytes (index ``0x21``, ``0x22``)
     - Pixel clock field (Hz)
   * - 0
     - 1280 x 1024
     - 60
     - ``0x87``, ``0x0b``
     - 108,000,000
   * - 1
     - 1280 x 1024
     - 72
     - ``0xee``, ``0x1c``
     - 130,821,428
   * - 2
     - 1600 x 1200
     - 60
     - ``0xc7``, ``0x10``
     - 148,500,000
   * - 3
     - 1600 x 1200
     - 72
     - ``0xd0``, ``0x0f``
     - 178,200,000
   * - 4
     - 2048 x 1000
     - 60
     - ``0xe6``, ``0x11``
     - 199,941,176
   * - 5
     - 1200 x 1600
     - 76
     - ``0xfb``, ``0x16``
     - 186,000,000
   * - 6
     - 1600 x 1200
     - 79
     - ``0xfb``, ``0x16``
     - 186,000,000
   * - 7
     - 640 x 480
     - 72
     - ``0x1c``, ``0x0c``
     - 31,968,750
   * - 8
     - 800 x 600
     - 56
     - ``0x5f``, ``0x16``
     - 36,000,000
   * - 9
     - 1024 x 768
     - 60
     - ``0xac``, ``0x1c``
     - 64,232,142
   * - 10
     - 1024 x 768
     - 70
     - ``0xa3``, ``0x16``
     - 75,000,000
   * - 11
     - 1920 x 1080
     - 75
     - ``0xe6``, ``0x11``
     - 199,941,176
   * - 12
     - 1024 x 1024
     - 60
     - ``0xa6``, ``0x14``
     - 84,975,000

The largest image, mode 11, uses 8,294,400 bytes and fits the 8 MiB
framebuffer. The driver programs every mode with one sequence: board
control with bit 5 clear; index ``0x21`` and the two PLL bytes through the
data port; the vertical bank; the three horizontal banks; index ``0x20``
and its sync value; board control with bit 5 set. Horizontal words count
four-pixel groups and the vertical words count lines, *software
expectation*. Three option fields of the EEPROM configuration word,
bits 8, 10 and 12-13, change the bank contents and the sync value (1, or
``0x23`` when bits 12-13 equal 2) but not the bank lengths or PLL bytes.
They give eight distinct sequences per mode. ``rad4setres`` labels the
fields horizontal sync polarity, vertical sync polarity and sync type;
what they do electrically is *unknown*.

With a 33 MHz reference clock the RGB561 PLL formula
(:src:`ibm-rgb561-architecture-1.1 p. 36`) reproduces every record's
clock field to the hertz, so the board's reference clock is *inferred* to
be 33 MHz. Mode 9 asks for 64.2 MHz with the range code the specification
gives for 65 to 128 MHz; whether the PLL locks there is *unknown*.

The timing-word bit meanings are not public, so the model does not
interpret the banks. It stores them for readback and recognises a mode by
matching the whole write sequence against the 104 sequences the driver
can produce, held in the generated table
``hw/display/psitech-rad4-modes-fixture.h``, an *approximation*:

- A board-control write with bit 5 clear arms recognition from any state,
  stops a recognised mode's frame events and display updates until the
  new sequence completes, and also acknowledges a pending frame if bit 1
  is clear.
- Each following write must match at least one remaining sequence. The
  closing board-control write with bit 5 set at a sequence's last position
  selects that mode: the display takes its geometry and stride and frame
  events start at its refresh label. An interrupt handler's acknowledging
  control write in mid-sequence is ignored.
- If the first write after arming is not the write of index ``0x21``,
  the model disarms without logging.
- Any other mismatch fails recognition, logs ``LOG_GUEST_ERROR`` and
  leaves the display unchanged until the next arming write. A timing-bank
  write, or a data-port write at index ``0x20`` to ``0x22``, outside a
  sequence fails the same way.

Until a mode is recognised, and after reset, the display shows mode 0's
geometry, blank. Frame events at the label rate differ from the rate the
inferred clock gives, ``clock / (4 x htotal x vtotal)``, by up to 0.6 Hz.

.. _sgi-psitech-rad4-framebuffer:

Framebuffer and plane mask
==========================

Each framebuffer pixel is a 32-bit word: red, green and blue in bits
23-16, 15-8 and 7-0, and an overlay and window-ID byte in bits 31-24. The
X server's depth-24 visuals use exactly those red, green and blue masks,
*software expectation*, and the RGB561 configuration it programs reads the
low 24 bits as direct colour (:src:`ibm-rgb561-architecture-1.1 p. 9,
47`), *established* for that configuration.

A CPU store at BAR2 merges under the plane mask:
``new = (old & ~mask) | (value & mask)``. The X server sets ``0x00ffffff``
for colour drawing and ``0x0f000000`` for its 4-plane overlay visual, so
the mask must apply to ordinary drawing, *software expectation*. The
plane-mask arithmetic is the general raster convention; which mask bits
reach which board plane is *unknown*. The model does not mask reads.

With the device property ``overlay-index-compat`` on, the default, a CPU
store under the mask ``0x0f000000`` whose value has no bits outside 3-0
writes that value into bits 27-24, an *approximation* chosen for the
shipped X server. It draws Motif's greyed menu labels with a tiled fill
that stores raw 4-bit tile values unshifted, while all its other overlay
drawing shifts the index to bit 24. Under a plain plane mask those labels
are transparent and the window behind shows through. A real card would
likely show the same defect; a photograph of a 4Dwm menu on one would
settle it. DMA never uses this rule. ``overlay-index-compat=off`` gives the
plain plane mask.

.. _sgi-psitech-rad4-ramdac:

RAMDAC display path
===================

The model converts each framebuffer word to a displayed colour the way
the RGB561 does under the 4:1 basic serializer the driver selects
(:src:`ibm-rgb561-architecture-1.1 p. 9, 15-21, 29-34, 46-48`),
*established* for the paths listed. Board wiring of the window-ID input is
not public: the model feeds it bits 31-24, the same byte as the overlay
port, *inferred*. The X server loads its frame-buffer window attribute
table in blocks of 16 identical entries and sets the auxiliary-table
alignment bit, so its displayed result does not depend on bits 27-24 and
cannot tell the two wirings apart.

- **Window attribute tables.** CONF/1, CONF/4 and the segment registers
  decide which window-ID bits address the frame-buffer table (FB_WAT,
  index ``0x1000``), the overlay table (OL_WAT, ``0x1400``) and the two
  16-entry auxiliary tables (``0x0e00`` and ``0x0f00``). Address bits a
  table does not receive come from its segment register. The
  specification's CONF/1 note says zero instead; the two agree while the
  segment registers are zero, as the X server leaves them.
- **Frame-buffer layer.** 8-bit index mode looks the pixel up in the
  colour table at ``START_ADDR x 64 + index``. Grey scale shows an 8-bit
  value on all three components. True colour takes 24-bit, or 8-, 12- or
  16-bit R3G3B2, R4G4B4 or R5G6B5 widened by repeating each component from
  its top bit. The buffer-select bit picks the upper buffer.
- **Overlay layer.** Where the auxiliary overlay entry enables the overlay
  and the OL_WAT entry's 8-bit or double-buffered 4-bit index mode gives a
  non-transparent value, the pixel shows colour-table entry
  ``START_ADDR x 64 + data``. Transparency compares with zero, all ones or
  a chroma key under its mask. For 4-bit data the model compares the
  4-bit value and uses ``0xf`` for all ones, *approximation*; the
  specification does not say.
- **Gamma.** Each displayed component addresses its own 256-entry 10-bit
  gamma table (``0x3000``, ``0x3400``, ``0x3800``) unless the layer's
  auxiliary entry sets bit 2. The display surface carries eight bits per
  component, so the model shows each entry's upper eight bits.

The multi-cycle port at BAR2 ``+0x930000`` loads every table with more
than one byte per entry: two cycles for a window attribute or gamma entry
(bits 9-2, then bits 1-0 in bits 7-6), three for a colour or cursor table
entry. The index advances once per complete entry, and writing either
index register restarts a partial entry
(:src:`ibm-rgb561-architecture-1.1 p. 1, 4`), *established*. A register
write that changes how stored pixels display repaints the whole frame.

.. _sgi-psitech-rad4-cursor:

Hardware cursor
---------------

The 64 x 64 two-bit cursor is composited into the displayed image, as the
RAMDAC overlays it on the pixel stream, so screendumps and VNC clients
show it where the guest put it. The model implements the enable bit
(index ``0x0030`` bit 0), the hot spot (``0x0034``-``0x0035``, six bits
each), the position (``0x0036``-``0x0039``, 16-bit signed), the pixel map
(``0x2000``-``0x23ff``, leftmost pixel in bits 7-6) and the colour table
(``0x0a10``-``0x0a1f``) (:src:`ibm-rgb561-architecture-1.1 p. 3, 27,
37-39, 45`), *established*. Pixel value 0 is transparent and values 1 to
3 select table entries ``0x0a11`` to ``0x0a13``. Cursor pixels bypass
gamma. That each three-byte table entry is red, green, blue in that order
is *inferred* from the driver's palette upload.

.. _sgi-psitech-rad4-eeprom:

Configuration EEPROM and mailbox
================================

BAR0 ``0x6c`` drives a serial EEPROM: bit 24 clock, bit 25 chip select,
bit 26 data to the EEPROM, bit 27 data from it
(:src:`plx-pci9060-data-sheet-1.2 Table 45`). The driver's commands are
the 93C46 64 x 16 set: READ, WRITE, EWEN and EWDS, each a start bit, two
opcode bits and six address bits (:src:`microchip-93c46`). The opcode
match is a *software expectation* from the driver; that the part is a 93C46 is
*inferred*.

The driver reads 40 bytes from byte address ``0x58`` at attach. The first
word gives the mode index in bits 6-0, the option bits 8 and 10, and a
selector in bits 12-13, *software expectation*. When ``rad4setres``
changes the mode, the driver writes a changed word back to the record, so
the mode also holds at the next attach, *software expectation*.

The model's EEPROM is 64 words. READ answers a dummy zero after the last
address bit and then streams words, most significant bit first,
continuing into the next word while chip select stays high. WRITE programs
its word when chip select drops, only between EWEN and EWDS; otherwise it
is ignored and logged ``LOG_GUEST_ERROR``. Programming completes at once,
so the driver's busy poll ends on its first sample; a real part takes
milliseconds, an *approximation*. ERASE, ERAL and WRAL log ``LOG_UNIMP``
and change nothing.

Without a backing image the EEPROM starts as 128 zero bytes, which the
driver reads as mode 0 with every option clear. The optional ``eeprom``
property names a drive holding a 128-byte image, each word stored most
significant byte first. The model reads it at realize and writes each
programmed word through, so a guest's mode choice survives a restart of
QEMU.

A nonzero mailbox at BAR0 ``0x40`` tells the driver that a firmware
initializer has already set the mode and RAMDAC, and the driver skips its
own
setup, *software expectation*. The model's mailbox resets to zero, so the
driver always programs the card. What a real card holds there after power
on is *unknown*.

.. _sgi-psitech-rad4-dma:

DMA
===

PsiTech's buffered X server renders into a host-memory copy of the screen
and the driver moves it to the card with DMA channel 0 in chaining mode,
*software expectation*. The unbuffered server, the default, draws through
BAR2 and never starts DMA.

DMACSR0 follows the 9060 table
(:src:`plx-pci9060-data-sheet-1.2 Table 57`), *established*:

- Bit 0, enable, is stored. Clearing it while a transfer runs pauses it.
  Setting it again resumes, *inferred*: the table states the pause only.
- Bit 1, start, with enable set, starts a chain from DMADPR0 and clears
  Done. A start while a transfer runs is ignored and logged
  ``LOG_GUEST_ERROR``. It reads as zero.
- Bit 2, abort, with enable clear, stops the channel and sets Done. With
  enable set it is ignored and logged ``LOG_GUEST_ERROR``. It reads as
  zero.
- Bit 3 clears the channel's interrupt and reads as zero.
- Bit 4, Done, is read-only and resets set.
- Bits 7-5 are stored. Channel 1 (bits 15-8) is not modelled: its enable
  and user bits are stored, its Done reads set and any command logs
  ``LOG_UNIMP``.

Each descriptor is four 32-bit words in local memory: PCI address, local
address, byte count (23 bits) and next pointer. The next-pointer word is
loaded into DMADPR0 and describes its own descriptor: bit 1 ends the chain
after it, bit 2 asks for an interrupt when it completes, bit 3 sets the
direction (1 local to PCI), bits 31-4 address the next descriptor
(:src:`plx-pci9060-data-sheet-1.2 section 3.6.2, Table 51`; the 23-bit
count, :src:`plx-pci9054-data-book-1.0 Table 11-79`), *established*. The
model walks the chain on every start, whatever DMAMODE0 bit 9 holds; block
(non-chaining) mode is not implemented. The driver writes one descriptor per
scanline at the top of local memory and ends with a next-pointer word of
``0x6``, *software expectation*. Local memory in the model is the
framebuffer.

- Bit 0 of a pointer is reserved on the 9060; the 9054 uses it to fetch
  descriptors from host memory. The model always reads local memory and
  logs ``LOG_GUEST_ERROR`` for a pointer with bit 0 set, *inferred*: the
  sheet does not say what the part does.
- Byte *i* of a transfer moves between PCI byte *P + i* and local byte
  *L + i*. Only bytes inside the span are written, at any alignment
  (:src:`plx-pci9060-data-sheet-1.2 p. 6, 9, 26`), *established*.
- An upload merges each byte under its lane of the plane mask, as a CPU
  store does, *inferred*: the driver's CPU and DMA paths share one mask
  register and one size threshold. No board document says the mask
  reaches DMA.
- A descriptor or span outside the 8 MiB local memory, or a failed host
  access, stops the channel with Done clear and logs
  ``LOG_GUEST_ERROR``; the driver's timeout and abort recover. The model
  stops walking a chain after 524,288 descriptors, the number local
  memory can hold, so only a chain that loops is cut short, an
  *approximation*: the part would loop for ever.

A start only latches the chain. The model moves it at host speed, at most
256 KiB per step, steps one nanosecond of virtual time apart, so the main
loop runs between steps. The 9060 is rated up to 132 MB/s
(:src:`plx-pci9060-data-sheet-1.2 p. 1`); the model is usually faster, an
*approximation*. The order of what the guest sees is unchanged: a
descriptor's own interrupt follows its data, and Done and the done
interrupt follow the chain's last byte. The driver allows each transfer
``size / 20 + 400000`` microseconds before it aborts, *software
expectation*, so faster completion only moves a transfer further from that
limit.

.. _sgi-psitech-rad4-byte-order:

Byte order
----------

The 9060 has no byte-order control: PCI byte lane *n* is local byte lane
*n* on a 32-bit local bus (:src:`plx-pci9060-data-sheet-1.2 p. 18`),
*established*. IRIX 6.5.30 leaves the slot's swap bits clear for this
card, so a big-endian 32-bit CPU store of *V* arrives as PCI dword *V*
(:ref:`sgi-bridge-dma-byte-order`), *software expectation*. The model
therefore keeps a CPU store's value as the local dword's value, moves DMA
bytes lane for lane, and reads descriptors as the values the CPU stored.
The board's own memory lane order is *unknown* and invisible to software.

.. _sgi-psitech-rad4-interrupts:

Interrupts
==========

The card has one PCI interrupt, INTA. The model implements these INTCSR
bits (:src:`plx-pci9060-data-sheet-1.2 Table 45`):

.. list-table::
   :header-rows: 1

   * - Bit
     - Meaning
     - Reset
     - Rating
   * - 8
     - PCI interrupt enable
     - 1
     - *established*
   * - 11
     - PCI local interrupt enable
     - 0
     - *established*
   * - 15
     - Local interrupt input active, read-only
     - 0
     - *established* bit; source *inferred*
   * - 16
     - Local interrupt output enable
     - 1
     - *established*
   * - 18
     - Local DMA channel 0 interrupt enable
     - 0
     - *established*
   * - 21
     - DMA channel 0 interrupt active, read-only
     - 0
     - *established* bit

The PCI line is asserted while bits 15, 8 and 11 are set, or while the
RAD4-KM controller's interrupt is asserted
(:ref:`sgi-psitech-rad4-km`). Bits 2-0, 12-8, 19-16 and 27 store; the
rest read as derived. A write that requests SERR# (bit 2) logs
``LOG_UNIMP``. Doorbells, aborts, BIST and the LSERR# and SERR# sources
are not modelled.

Bit 15 is set by a pending frame event or by the DMA interrupt reaching
the local output (bits 16 and 18 set). The 9060 sends DMA interrupts only
to its local output, and its suggested route to the host is that output
wired back to the local input
(:src:`plx-pci9060-data-sheet-1.2 sections 3.6.1, 3.10.2`). This card is
*inferred* to be wired that way: the driver enables the DMA interrupt only
towards the local output and
looks for bit 21 only after bit 15. Bit 21 follows the DMA interrupt
whether or not bit 18 is set, *inferred*: the table names no enable for
it. Chain completion with DMAMODE0 bit 10 set, and completion of a
descriptor that asks for one, raise the DMA interrupt; DMACSR0 bit 3
clears it.

.. _sgi-psitech-rad4-reset:

Reset
=====

Reset stops frame events and DMA channel 0, clears the framebuffer, the
timing banks, the RGB561 register file, the window attribute, colour and
cursor tables and the BAR0 registers, sets the plane mask to all ones,
INTCSR to ``0x00010100`` and DMACSR0 to ``0x00001010`` (both channels'
Done set, :src:`plx-pci9060-data-sheet-1.2 Table 57`), and returns the
display to mode 0's geometry, blank, with no mode recognised. The EEPROM
keeps its contents, as a nonvolatile part does, and loses write enable.
The RAD4-KM controller resets with the card.

The RGB561 specification exempts its tables from reset and expects
software to load them (:src:`ibm-rgb561-architecture-1.1 p. 1`). The model
clears them for a fixed post-reset state, and resets the gamma tables to
the linear ramp a bypassed pixel would show rather than zero, so a guest
that never loads them keeps its colours. Each is an *approximation*. Cold,
warm and PCI reset of a real card are *unknown*.

.. _sgi-psitech-rad4-migration:

Migration
=========

``psitech-rad4`` migrates its PCI state, the BAR0 registers, the EEPROM
contents and serial state, the framebuffer, the plane mask, the RGB561
register file and tables, the timing banks, board control and any pending
frame, the recogniser's state and mode, and a DMA transfer in flight: the
descriptor being moved, its progress, the next pointer and the chain's
descriptor count. Incoming state with an out-of-range mode, recogniser or
serial position is refused. ``psitech-rad4-km`` migrates its channels and
registers, and its keyboard and mouse their own queues. Migration works
only between identical builds.

.. _sgi-psitech-rad4-km:

RAD4-KM keyboard and mouse
==========================

The owner's guide says a RAD4-KM has at least two connectors and that
either takes the keyboard or the mouse; IRIX presents them as
``/dev/rad4_kbd`` and ``/dev/rad4_pntr``, apart from the system's own
keyboard and mouse devices
(:src:`psitech-rad4-owners-guide-1.3 Origin 2000 installation, Appendix E`),
*established*. The connector type and electrical interface are *unknown*.

Under ``product=rad4-km`` the model adds BAR3, 1 MiB, holding sixteen
controller registers 64 KiB apart, each reached with aligned 32-bit
accesses. The driver only creates its keyboard and mouse devices for
device ``0x105`` and uses the registers below, *software expectation*.
Their layout matches the SCN2681/SCC68681 dual UART index for index
(:src:`philips-scc68681-duart`); that the card carries that family or a
compatible part is *inferred*. The 64 KiB stride is the card's address
decode, *inferred*.

.. list-table::
   :header-rows: 1

   * - Index
     - Read
     - Write
     - Model
   * - ``0x0``, ``0x8``
     - MR1/MR2
     - MR1/MR2
     - Writes go to MR1, then MR2 until the command register resets the
       pointer. Reads log ``LOG_UNIMP``.
   * - ``0x1``, ``0x9``
     - Status
     - Clock select
     - Status bit 0 RxRDY, bit 2 TxRDY. Clock select is stored.
   * - ``0x2``, ``0xa``
     - (none)
     - Command
     - Receiver and transmitter enable and disable; command 1 resets the
       MR pointer. Commands 2 to 5 are accepted with no effect, an
       *approximation*: on the chip family, resetting the receiver or
       transmitter also disables it. Commands 6 and 7 log ``LOG_UNIMP``.
   * - ``0x3``, ``0xb``
     - Receive data
     - Transmit data
     - Channel A carries the keyboard, channel B the mouse.
   * - ``0x4``
     - (none)
     - Auxiliary control
     - Stored. The driver writes zero.
   * - ``0x5``
     - Interrupt status
     - Interrupt mask
     - Status bits 0 and 1 are channel A's TxRDY and RxRDY, bits 4 and 5
       channel B's.
   * - ``0xd``
     - (none)
     - Output port configuration
     - Stored. The driver writes zero.
   * - ``0xe``, ``0xf``
     - (none)
     - Set and reset output bits
     - Stored.
   * - ``0x6``, ``0x7``, ``0xc``
     - Counter, interrupt vector
     - same
     - Never used by the driver. Log ``LOG_UNIMP``.

A read of an index marked "(none)" logs ``LOG_UNIMP`` and returns zero.

The driver resets each channel with the PS/2 reset command and assigns
the keyboard or mouse role from the reply, *software expectation*. The
model instead connects QEMU's PS/2 keyboard to channel A and PS/2 mouse to
channel B, an *approximation*: both answer the driver's probe as a real
keyboard and mouse would, so the roles it detects are the same. Transmit
is always ready once enabled. A data read with nothing queued returns zero
and logs ``LOG_GUEST_ERROR``.

The controller's interrupt is asserted while any status bit under the
mask is set, and shares the card's INTA. The driver also writes a separate
board-level interrupt-enable register whose address is *unknown*; the
model has no such register.

RAD4C-KM
--------

PsiTech's 1999 product sheet describes a later RAD4C-KM: 8 MB VRAM, a
programmable video clock up to 200 MHz, an 8-bit overlay, 3 x 256 x 10-bit
gamma tables, a 3 x 10-bit DAC and two 9-pin DIN PS/2-compatible keyboard
and mouse connectors (:src:`psitech-rad4c-km-ds-1099 p. 1-2`),
*established*. The sheet gives no PCI identity and no register
information. QEMU does not model it.

.. _sgi-psitech-rad4-limits:

Model limits
============

- Revision, class, subsystem and BAR sizes are model choices; no
  configuration-space dump of a real card exists.
- Only the driver's 13 mode sequences produce a display. Timing words are
  matched, not interpreted. Frame events run at the label rate.
- Only one card per machine is supported. With two or more, the driver
  arms recognition on every card when a device is opened and each display
  freezes.
- The plane mask's bit-to-plane mapping is the generic arithmetic.
  ``overlay-index-compat`` departs from it on purpose.
- Overlay formats with an underlay, serializer modes other than 4:1 basic,
  the 16-bit and variable overlay ports, overlay modes other than index,
  index and grey-scale pixels other than eight bits, direct RGB mode,
  buffer B of a 24-bit pixel and buffer B of an 8-bit overlay are not
  implemented. The first such pixel in each display pass logs
  ``LOG_UNIMP`` and the frame-buffer layer, or the pixel's low bits, are
  shown. CONF/3 is never read: BGR order is ignored silently and RGB
  order is always used.
- Cursor and cross-hair blinking and the cross-hair are not implemented.
  The cursor position's upper bits read back as written, not as the sign
  value the specification gives.
- DMA channel 1, block mode, demand mode and 64-bit addressing are not
  implemented. Transfers run at host speed.
- The EEPROM programs instantly. ERASE, ERAL and WRAL are not
  implemented.
- The board-control register's physical effects, the mailbox's power-on
  value and cold, warm and PCI reset of a real card are unknown.
- The RAD4-KM register layout is the plain RAD4's. Its controller part,
  connectors and board-level interrupt gate are unknown, and keyboard and
  mouse sit on fixed channels. The controller's reset-receiver and
  reset-transmitter commands do not disable the channel.
- Unlisted register offsets and RGB561 indices log ``LOG_UNIMP``;
  recognition failures, rejected DMA descriptors and EEPROM writes without
  enable log ``LOG_GUEST_ERROR``; accesses of other widths fail as decode
  errors.

.. _sgi-psitech-rad4-tests:

Tests
=====

``tests/qtest/psitech-rad4-test.c`` covers the PCI identity of both
products and the RAD4's missing BAR3; the BAR0 registers and the INTCSR
table; EEPROM reads, writes before and after EWEN, EWDS, the sequential
wrap and a backing drive read back by a second QEMU instance; the plane
mask and the overlay-index rule on and off; the RGB561 data-port
auto-increment and the round trips of every multi-cycle table; recognition
of every mode for two option words, rejection of altered sequences, modes
that share PLL bytes or banks, all 16 option words for mode 9, run-time
mode changes with a frame pending, migration and reset; frame interrupts,
their request bit and acknowledgement; display dirty tracking; overlay
composition, chroma keys and window attribute addressing; every
frame-buffer pixel format and the gamma tables; the composited cursor; and
DMA: register state before start, uploads, the mask, descriptor and done
interrupts, pointer bit 0, the driver's chain layout, a 1,024-descriptor
chain, unaligned spans, out-of-range descriptors, abort, pause and resume,
reset and migration mid-transfer and a looping chain. Its
``/psitech-rad4-km/`` cases cover the RAD4-KM identity, the shared BAR0
and BAR2 layout, channel status, the aggregate interrupt reaching PCI and
a PS/2 reset round trip on each channel.

``tests/qtest/sgi-graphics-pipes-test.c`` checks the Origin 200 slots the
card may occupy and the refusals for others.
``tests/functional/mips64/test_graphics_matrix_thorough.py`` runs the
``rad4_*`` rows of ``tests/functional/mips64/graphics-matrix``: IRIX
6.5.30 boots with the card at slot 5 and the login screen, window and
desktop menus, a ``rad4setres`` mode switch and the buffered server are
checked against recorded screen regions.
