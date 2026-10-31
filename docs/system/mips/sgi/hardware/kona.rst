.. SPDX-License-Identifier: BSD-3-Clause

.. _sgi-kona:

===============================
InfiniteReality: graphics pipes
===============================

Used in :ref:`sgi-ip27` (Onyx2). The Onyx 3000 G-brick holds
InfiniteReality3 and InfiniteReality4 pipes (*established*,
:src:`007-4264-002 pp. 39-43`); it is described on :ref:`sgi-bricks`,
and the Onyx 3400 with InfiniteReality3 builds it
(:ref:`sgi-sn-onyx3400-ir`). The model covers the InfiniteReality2,
InfiniteReality2E and InfiniteReality3 products.

InfiniteReality is the graphics pipe of the Onyx2 deskside and rack. A pipe
is a set of boards: a geometry board whose XIO-to-graphics ASIC (XG) ends
the host path (:src:`007-3454-005 p. 28`, *established*), one, two or four
raster managers each with its texture memory, and a display generator.
Linux and
OpenBSD call the widget KONA. QEMU models the pipe as ``sgi-kona`` in
``hw/misc/sgi-kona.c``, with its DMA engine in ``sgi-kona-dma.c``, its
command tables in ``sgi-kona-ports.c``, and its OpenGL decoder and texture
store in ``sgi-kona-gl-decode.c`` and ``sgi-kona-texture.c``. The
``onyx2`` machine fits one at its catalogue slot. On the Origin and
Onyx2 machines ``-device sgi-kona`` fits one where the topology's
catalogue allows a pipe; ``x-uncatalogued-graphics=on`` lifts the check,
which the register-contract qtests use. No chip revision is modelled: the widget
answers revision 0.

No register specification, schematic or capture of the pipe's host
interface is public. The model is built from what the IP27 PROM, the IO6
PROM, the IRIX 6.5.30 kernel, its X server and OpenGL library, and the
firmware IRIX loads into the pipe send and expect, read as binaries. Most
behaviour below is therefore a *software expectation* or an
*approximation*, and the board identities are *observed* in published
inventories. Facts are rated as described in :ref:`sgi-ratings`.

.. _sgi-kona-sources:

Sources
=======

- The Onyx2 deskside Owner's Guide, :src:`007-3454-005 p. 28`: the XG
  ASIC on the geometry board as the pipe's XIO attachment.
- Published inventories of real pipes, cited as *observed*: an Onyx2
  deskside InfiniteReality2 with ``hinv``, board list and ``gfxinfo -v``
  (:src:`irixnet-post-32192`), the same class of deskside's PROM
  ``hinv -v`` (:src:`nekonomicon-16723250`), a deskside board list
  (:src:`sgistuff-collection-2200`), and an Onyx2 rack InfiniteReality3
  with ``hinv -v -m``, board list and ``gfxinfo -v``
  (:src:`nekonomicon-16285`). No cited capture shows a two-engine GE14-2
  pipe, so the GE14-2 and DG5-2 part numbers the model reports are an
  *approximation*.
- Montrym, Baum, Dignam and Migdal's InfiniteReality paper,
  :src:`montrym-infinitereality-1997`, for the board architecture and the
  colour-index map size.
- SGI's *InfiniteReality Graphics Upgrade for Onyx*,
  :src:`108-0132-002 p. A-4`, for the flash part's identification codes.
- The IP27 PROM, :src:`ip27-prom-6.156 graphics classifier`, and the IO6
  PROM shipped with IRIX 6.5.30, :src:`io6-prom-6.156 textport`, as
  software expectations: the widget classifier, the textport command
  grammar, the mailbox protocol and the flash header check.
- The IRIX 6.5.30 IP27 kernel, X server, OpenGL library, pipe firmware
  and flash utility, read as binaries, as software expectations: the
  register uses, the command tokens and their word counts, the DMA packet,
  the interrupt handlers and the flash operation.
- Linux ``arch/mips/include/asm/xtalk/xwidget.h`` and OpenBSD
  ``sys/arch/sgi/xbow/xbowdevs``, as software expectations: the widget
  manufacturer and part numbers.

.. _sgi-kona-identity:

Identity and placement
======================

The widget answers manufacturer ``0x2aa`` and part ``0xc102`` in the
generic XIO widget identification register. Linux and OpenBSD name those
numbers KONA, a *software expectation*, and a published PROM inventory of
a real deskside prints ``KONA Graphics Board`` for the pipe
(:src:`nekonomicon-16723250`), *observed*. The
IP27 PROM's classifier gives part ``0xc102`` board type ``0x51`` and the
name ``kona`` and requires bits 2:0 of widget register ``0x64`` to read
zero before it accepts the board
(:src:`ip27-prom-6.156 graphics classifier`), a *software expectation*.
Chassis wiring, widget numbers, board records and the catalogue defaults
are on :ref:`sgi-onyx2` (:ref:`sgi-onyx2-pipe-identity`,
:ref:`sgi-onyx2-one-wire`); the catalogue's pipe options are on
:ref:`sgi-sn-graphics-pipes`.

The one-wire chain carries the geometry board's record and, on a rack,
the Ktown host board's record. Serials and barcodes are marked synthetic
values (``EMUG00``, ``EMUG01``), an *approximation*; part numbers, names and
revisions follow the configured product. The chain answers at ``0x98`` and
at ``0x9c`` with one shared state: the PROM reads the first offset and the
kernel the second, a *software expectation*, and that both reach one chain
is *inferred*. The one-wire family code ``0x09`` is a model choice.

The IRIX 6.5.30 kernel classes the pipe at attach by two reads through the
indirect pair at ``0x8003c0`` and ``0x8003d0``: after selector ``0x8800``,
low bits 1 make it an InfiniteReality3 and 2 an InfiniteReality4; after
``0xb00``, bit 10 makes it a Reality. Otherwise ``GE16-`` in the geometry
board's one-wire name makes it an InfiniteReality2E, and its absence an
InfiniteReality2. That is a *software expectation*. The model answers
selector ``0x8800`` with 1 for ``ir3`` and 0 otherwise, and ``0xb00`` with
0, and only when a product is configured.

.. _sgi-kona-products:

Products and settings
=====================

The ``product`` property selects one configuration whole: the board set,
the identity records and the answer to the configuration request
(:ref:`sgi-kona-config`). Each value is built from a published inventory;
a setting no source gives is refused at realize with the reason.

.. list-table::
   :header-rows: 1

   * - ``product``
     - ``hinv`` name
     - Geometry
     - Raster manager
     - Texture per RM
     - Board set
   * - ``ir2``
     - InfiniteReality2
     - ``ge14-4`` (default) or ``ge14-2``
     - ``rm7``
     - 64 MB
     - 1
   * - ``ir2e``
     - InfiniteReality2E
     - ``ge16-4``
     - ``rm9``
     - 64 MB
     - 4
   * - ``ir3``
     - InfiniteReality3
     - ``ge16-4``
     - ``rm9``
     - 256 MB
     - 5

The product names follow what IRIX prints, *observed*: a GE14 and RM7
pipe prints InfiniteReality2 on an Onyx2 (:src:`irixnet-post-32192`), a
GE16-4 with 256 MB texture prints InfiniteReality3
(:src:`nekonomicon-16285`). The board-set numbers are the classes the
IRIX 6.5.30 kernel records for each name, a *software expectation*.
``ir2e`` combines the rack capture's GE16-4 and
RM9 with 64 MB texture boards. The texture board's part is the RM7
pipe's, an *approximation*: no cited capture shows an RM9-64's boards.
Three names are refused:
``ir``, since IRIX names no Onyx2 pipe InfiniteReality; ``reality``,
since no capture gives its boards; and ``ir4``, since the only captured
InfiniteReality4 is an Onyx 350's, whose host board this model does not
attach.

.. list-table::
   :header-rows: 1

   * - Setting
     - Values
     - Notes
   * - ``geometry``
     - ``ge14-4`` 030-1129-002 rev F, four engines; ``ge14-2``
       030-1051-002 rev K, two engines; ``ge16-4`` 030-1398-001 rev E,
       four engines
     - *Observed* parts. The GE14-2 presents the GE14-4's chip words, an
       *approximation*.
   * - ``rm-type``
     - ``rm7`` 030-1054-001 rev G; ``rm9`` 030-1402-001 rev E
     - Fixed by the product; another value is refused.
   * - ``rm-count``
     - 1 (default), 2, 4
     - Four needs a Ktown record: a deskside pipe holds at most two.
   * - ``texture-mb``
     - 64 (TM7 030-1053-001 rev F), 256 (030-1588-001)
     - Fixed by the product (64 for ``ir2`` and ``ir2e``, 256 for
       ``ir3``); another value is refused. The 256 MB board's name and
       revision are unknown and left blank.
   * - ``display``
     - ``dg5-8`` 030-1087-001, eight channels; ``dg5-2`` 030-1055-001,
       two channels; ``dg5-2-gvo`` 030-1242-001 rev D with a GVO record
     - Default ``dg5-2`` with a Ktown, ``dg5-8`` without. The GVO is an
       identity only: there is no video output.
   * - ``channels``
     - 0 or the display board's count
     - Any other count is refused.
   * - ``pixel-depth``
     - ``small`` (default), ``medium``
     - Refused when the raster managers' pixel memory cannot hold it.

The pixel-depth check follows the IRIX X server's rule: raster managers
times 20,971,520, divided by the height times the width rounded up to a
multiple of 160, must be
at least 8 for Small and 16 for Medium, a *software expectation*. Large
and Extra-Large are refused, an *approximation*: their visuals are
untested.

Without ``product`` the pipe is unconfigured: setting any other
configuration property is refused, the class reads stay logged zeros and
the configuration answer carries seven fields and the pool
descriptors only. That answer matches no
real pipe. The machine catalogue always sets a product.

Other properties: ``fabric`` and ``fabric-hub-widget`` name the address
space and the one Hub widget the pipe's DMA and host writes go to; ``pi``
names the node whose processor interface counts the pipe's credits
(:ref:`sgi-kona-credits`); ``manufacturer``, ``part`` and ``revision``
build the identification register; ``probe-value`` is register ``0x64``;
``list-memory-size`` sizes resident display-list memory (default 15 MiB,
a nonzero multiple of 4,096 up to 64 MiB, a model choice); ``host-gpu``
lets GL draws use the host's GPU through Mesa instead of the CPU
rasterizer, with the same board memory as the result.

.. _sgi-kona-register-map:

Register map
============

Offsets are from the widget's 16 MiB XIO window. Registers are 32 bits,
big-endian. Accesses must be 4 or 8 bytes. All offsets come from the IO6
PROM and the IRIX 6.5.30 kernel, X server and firmware loader, *software
expectations*; reset values are model choices unless stated, since no
pipe has been read.

.. list-table::
   :header-rows: 1

   * - Offset
     - Name
     - Reset
     - Access
     - Model behaviour
   * - ``0x000004``
     - Widget identification
     - ``0x0c102555``
     - R
     - Revision 31:28, part 27:12, manufacturer 11:1, bit 0 one.
   * - ``0x000024``
     - Widget control
     - 0
     - R/W
     - Stored, no effect.
   * - ``0x000034``, ``0x00003c``
     - Interrupt destination, upper and lower
     - 0
     - R/W
     - Upper bits 19:16 target widget, 15:0 address 47:32; lower is
       address 31:0. See :ref:`sgi-kona-interrupts`.
   * - ``0x00005c``
     - Interrupt vectors
     - 0
     - R/W
     - General vector 7:0, geometry-engine vector 16:9.
   * - ``0x000064``
     - Classification
     - ``probe-value``
     - R
     - Bits 2:0 must be zero to attach; bits 5:3 size the map RAM.
   * - ``0x00007c``
     - Low window word
     - 0
     - R/W
     - Stored. Bit 0 parks returned credits.
   * - ``0x000098``, ``0x00009c``
     - One-wire slot
     - 0
     - R/W
     - Bridge-style bit slot, completes in the write.
   * - ``0x100000``-``0x1fffff``
     - XG map RAM
     - 0
     - R/W
     - 262,144 32-bit entries, all bits stored.
   * - ``0x80007c``
     - DMA FIFO push
     - none
     - W
     - Words for the kernel command framer. Placement is a model choice.
   * - ``0x800208``
     - Credit
     - 0
     - R
     - Held words shifted left one, mask ``0x7fe``.
   * - ``0x800210``
     - Credit configuration
     - 0
     - R/W
     - Stored; the kernel writes ``0x700``. No effect.
   * - ``0x800220``
     - Reset argument
     - 0
     - R/W
     - Stored.
   * - ``0x800230``
     - Interrupt enable
     - 0
     - W
     - Stored, reads zero. Enables the video and low-water causes.
   * - ``0x800238``
     - Interrupt cause
     - 0
     - R/W1C
     - Low-water bit 1, video 3, mailbox 4, geometry engine 5, fatal 8.
   * - ``0x800240``
     - Mailbox
     - 0
     - R/W
     - Head of a five-word reply queue; any write advances it.
   * - ``0x800248``
     - Retrace tag
     - 0
     - R
     - Tag of the last retrace request completed.
   * - ``0x800300``
     - Reset strobe
     - 0
     - R/W
     - Stored.
   * - ``0x800308``
     - Internal port enable
     - 0
     - R/W
     - Stored.
   * - ``0x800328``, ``0x8003b0``
     - Loader words
     - 0
     - R/W
     - Stored and traced; the loader writes 1 and 5.
   * - ``0x8003a8``
     - Activity
     - 0
     - R/W
     - Bit 2 set by consumed command words; a write with bit 2 clears it.
   * - ``0x8003c0``, ``0x8003d0``
     - Indirect selector and data
     - 0
     - W, R/W
     - Cursor selectors ``0x100``-``0x103``; class reads ``0x8800``,
       ``0xb00``.
   * - ``0x8003e8``
     - Internal address
     - 0
     - R/W
     - Selects a word of the pipe's internal address space.
   * - ``0x8003f0``
     - Internal data
     - none
     - R/W
     - Reads or writes the selected word; does not advance.
   * - ``0x8003f8``
     - Internal write and advance
     - none
     - W
     - Writes the selected word and adds 4 to the address.
   * - ``0xc00108``
     - Bracket
     - 0
     - W
     - Bit 17 force, bit 16 privileged. See :ref:`sgi-kona-commands`.
   * - ``0xc10000``-``0xc13fff``
     - Command page
     - none
     - W
     - Every command word. See :ref:`sgi-kona-commands`.

Any other offset reads zero and drops writes, logging ``LOG_UNIMP``. The
interrupt-cause register drains queued completion reasons when it is read
or written.

8-byte accesses
---------------

An 8-byte store at the command page's offset 0 carries two data words,
high word first, which is how the X server's bulk data helpers store
pairs, a *software expectation*; that the pipe takes them as two words is
*inferred*. Any other 8-byte access is split into two 4-byte accesses,
high word at the lower address, and logged ``LOG_UNIMP``, an
*approximation*.

Internal address space
----------------------

The selector, data and advancing-write registers reach a 32-bit address
space inside the pipe. The PROM and the kernel read the flash header
through it, and the IRIX firmware loader writes the pipe's controller
firmware through it, a *software expectation*. The model backs three
ranges, an *approximation* for each:

- ``0x800000``-``0x87ffff``: the 512 KiB flash (:ref:`sgi-kona-flash`),
  read big-endian. Host writes are dropped.
- ``0x0``-``0x2b`` and ``0x4000000``-``0x414e62f``: the controller's reset
  code, image and geometry microcode as the loader writes them. Stored,
  never executed.
- ``0x200050``: the load control word. The loader writes 1 before its
  first data word and 0 after its last. On the 1-to-0 edge the model
  traces the load record: each section's start, word count and SHA-256.

Other internal addresses read zero and drop writes, which are traced.
Whether the data register advances on hardware is *unknown*: every reader
selects a new address first. The advancing write is what the loader relies on, a
*software expectation*.

XG map RAM
----------

The kernel fills the map RAM with one entry per 16 KiB device page and at
attach writes ``0x10000000 | i`` to each entry and requires the read-back
to match, a *software expectation*. Bits 5:3 of register ``0x64`` give the
entry count, ``N << 15`` with 0 meaning 8; the default ``probe-value`` of 0
selects the full 262,144. An entry the kernel's page mapper writes has
bits 31:28 = 2 and a 4 KiB frame number in 27:0; the model translates
device addresses only through entries of that form. Entry width and reset
contents are *unknown*; storing 32 bits and clearing to zero are
*approximations*. An access past the selected count is traced and logged
``LOG_UNIMP``.

.. _sgi-kona-commands:

Command page and grammars
=========================

Every command reaches the pipe as 32-bit words on the command page at
``0xc10000``. Four senders share the page, each with its own grammar, all
*software expectations* read from the senders:

- The IO6 PROM's textport: a sync word 1, then a command and its
  arguments (:ref:`sgi-kona-textport`).
- The kernel: a 9-bit token and a fixed word count, bracketed by the
  bracket register.
- The X server: tokens ``0x128``-``0x161`` and ``0x1a6``, written as
  plain words without a bracket.
- The OpenGL library: 16-bit GL selectors with their arguments, written
  through its user mapping without a bracket.

A store at page offset 0 or ``0xc`` is a plain word: the next argument of
an open command, or else a new token. A store at ``0xc + 0x20 * t`` opens
token ``t`` with the stored value as its first argument, the form an
alternative X server module uses. The model accepts slot stores only at
``0xc`` within a slot; a store anywhere else on the page, a slot naming an
unknown token, or a slot store while a command is still open is logged
``LOG_GUEST_ERROR`` and the partial command dropped. These decode rules
are *inferred* from the stores the senders make; what the low address
bits do on hardware is *unknown*.

The kernel writes ``0x30000`` to the bracket register before a packet's
token, ``0x10000`` before its arguments and 0 after, a *software
expectation*. The model reads bit 17 as force: an open kernel command is
abandoned and logged, and an open user command is set aside until the
kernel packet ends. Bit 16 marks the following words as a kernel command's.
Both readings are *inferred* from the register table in the IRIX 6.5.30
pipe-manager tool, read as a binary.
Other bits are logged ``LOG_GUEST_ERROR``.

How the pipe tells the textport and GL grammars apart is *unknown*: GL
selectors 1 to ``0xa`` and ``0xc`` share the textport's numbers. Before the
first context activation the model reads the page as textport and kernel
commands only. After it, a word whose low 16 bits are a GL selector opens
that command, and a bare 1 is held until the next word decides between the
textport sync word and a GL buffer configuration. That rule is an
*approximation* and misreads a buffer configuration whose first word is a
textport command number.

Tokens the model has no word count for, and words that fit no grammar,
are logged ``LOG_GUEST_ERROR`` and dropped.

.. _sgi-kona-textport:

Textport commands
-----------------

Each command follows sync word 1. Counts and meanings are the IO6 PROM's
(:src:`io6-prom-6.156 textport`), *software expectations*, except where
noted.

.. list-table::
   :header-rows: 1

   * - Command
     - Words
     - Meaning
     - Model
   * - 1
     - 1
     - Mode value
     - Framed, no effect.
   * - 2
     - 1
     - Select colour-map entry
     - Sets the draw colour index.
   * - 3
     - 4
     - Load colour-map entry: index, then red, green and blue times 4
     - Stores the entry; drawn pixels change colour.
   * - 4
     - 2
     - Raster position
     - Sets the origin for command ``0xa``.
   * - 5
     - 4
     - Filled rectangle, inclusive
     - Fills in the draw colour.
   * - 6
     - 2
     - Point
     - Plots in the draw colour.
   * - 7
     - 2
     - Pointer position
     - Framed, no effect.
   * - 8
     - 2
     - Screen size
     - Traced; the framebuffer stays 1280x1024.
   * - 9
     - 1
     - Sync probe
     - Queues mailbox reply ``0x1234abcd``.
   * - ``0xa``
     - 6 halfwords, then bits
     - Bitmap: width, height, x and y origin, x and y advance, then
       1 bit per pixel
     - Draws set bits at the raster position less the origin, then
       advances it.
   * - ``0xc``
     - 1
     - Graphics information
     - Queues five mailbox words: 0, 0, 0, 1280, 1024.

That commands 2 and 3 carry an index and pre-scaled components, and the
bitmap header's field order, are the model's reading of the IO6 PROM's
command sends, an *approximation*: no specification states them. The PROM
reads graphics-information words 3 and 4 as width and height, and falls
back to 1280x1024 when they are negative, a *software expectation*; the
other three words are *approximated* as zero. The PROM waits for the
credit register to read zero before each command, up to 5,000 reads, a
*software expectation*.

Kernel commands
---------------

The tokens the IRIX 6.5.30 kernel and its graphics initialiser send, with
the words after the token, are *software expectations*. What the pipe does
with each is *unknown*; the model's action is listed.

.. list-table::
   :header-rows: 1

   * - Token
     - Words
     - Model action
   * - ``0x1f1``
     - 3
     - Registers the reason page (:ref:`sgi-kona-interrupts`).
   * - ``0x1f2``
     - 3
     - Registers the return page answers go to.
   * - ``0x1f8``, ``0x1dd``
     - 1
     - Posts the word as a completion reason.
   * - ``0x1e9``
     - 1
     - Answers the configuration request (:ref:`sgi-kona-config`).
   * - ``0x1ec``
     - 1, then the operation's words
     - Flash operation (:ref:`sgi-kona-flash`).
   * - ``0x1fd``, ``0x1f9``
     - 3, 4
     - Make a context current, fresh or restored.
   * - ``0x1fc``
     - 1
     - Switch the current context out; posts reason ``0x101``.
   * - ``0x1f3``, ``0x1ee``
     - 11, 3
     - Place the current context's GL window.
   * - ``0x1f4``
     - 2
     - Consumed, no effect.
   * - ``0x1e7``
     - 1
     - Retrace request; completes its tag at a retrace.
   * - ``0x1e6``
     - 3
     - Display mode. Consumed, no effect.
   * - ``0x1eb``, ``0x1c8``, ``0x1ed``
     - 6, 2, 3
     - DMA packet, list-write record, DMA pool (:ref:`sgi-kona-dma`).
   * - ``0x1f5``, ``0x1f6``
     - 6, 2
     - Read-DMA bracket for pixel and select-mode returns.
   * - ``0x1e4``, ``0x1e1``, ``0x172``
     - 4, 4, 1
     - Pipe-memory pool packets. A display-list region relocates list
       addresses; the rest are consumed.
   * - ``0x1e3``, ``0x1e2``
     - 8, 6 and data
     - Pool swap-out and swap-in. Swap-out posts reason ``0x102``.
   * - ``0x1df``, ``0x1de``
     - 1, 0
     - Suspend, posting reason ``0x104`` with no reply words; resume.

The model keeps at most 64 logical contexts, its own bound. A context
beyond that, or a context whose state cannot be allocated, stops the
virtual machine with an internal error naming the command, rather than
evicting a context.

X server and GL commands
------------------------

The X server's tokens cover graphics-context and clip loads, colour-map,
gamma and cursor loads, display-ID maps, pixel setup, write, read, copy
and finish, stipples, points, lines, segments, spans, boxes and
rectangles. Word counts come from the X server's send sites, *software
expectations*. The model draws them into the colour planes with its own
rules, *approximations*: boxes count y down from the top display line, a
colour-index pixel resolves through X colour-map entry ``0x2d00`` plus the
index read as 10:10:10, and clip lists hold up to 64 rectangles.

The GL client's selectors are framed by a table built from the OpenGL
library's senders and executed by the model's own fixed-function renderer,
with display lists run from list memory and textures held per context.
How a real pipe renders is out of scope of this page and *unknown*;
selectors whose word count is unsettled are not framed.

.. _sgi-kona-config:

Configuration answer
====================

The graphics initialiser sends ``0x1e9`` and reads ``0x778`` bytes from
return page offset 8, which the kernel keeps as the pipe's board
information; every later process reads it from there, a *software
expectation*. The model writes the block, then a nonzero word 0, which is
the only thing the waiting process polls. Every field is a 32-bit
big-endian word, an *approximation*: the readers' widths do not settle it.

For a configured pipe the block carries the board set, engine and
raster-manager counts, ``4MB external`` FIFO memory, the display channel
count at ``0x14`` and ``0x44``, the video option code, external gamma, a
presence word and texture size code per raster manager, the active engine
mask, 1280 and 1024 at ``0x84`` and ``0x88``, the pixel depth at ``0x8c``,
the chip identity words ``gfxinfo -v`` prints, and a ``0x4c``-byte
identity record for each raster manager, texture board, display board
and option. The chip identity words are those of the captured pipes
(:src:`irixnet-post-32192`, :src:`nekonomicon-16285`), *observed*; the
words that differ between captures follow the geometry board, one
*inferred* and one *approximated*. Record serials are synthetic. The
geometry and Ktown records stay zero: the kernel keeps its own one-wire
reading of them.

Four pipe-memory pool descriptors follow at ``0x90 + 0x1c * class``:
co-processor, display lists (15 MiB), geometry engines and texture. Their
words are model choices within the kernel's checks, an *approximation*;
the texture class follows ``texture-mb`` and the geometry class's copy
count the engine count.

.. _sgi-kona-flash:

Flash EEPROM operation
======================

The pipe keeps a 512 KiB flash: the controller's boot image from offset 0,
a video format combination at ``0x20000`` behind the marker
``0xfacefeed`` and its word count, and a twelve-word header at
``0x7ff00``. The PROM and the kernel read the header through the internal
port at ``0x87ff00`` and accept header word 7 only as 2, a *software
expectation*. The size is *inferred* from the erase length and the
header's place.

The IRIX flash utility sends ``0x1ec`` and an operation number, and the
pipe's firmware answers. The operations are read from that firmware, a
*software expectation*:

.. list-table::
   :header-rows: 1

   * - Op
     - Words after the number
     - Effect
     - Reply
   * - 1, write
     - type; offset if type 0; count; at least one data word
     - Type 0 at the offset, type 1 at ``0x20000`` with marker and count,
       type 2 at the header
     - 1 on success, 0 on failure
   * - 2, read
     - byte offset, byte length
     - Copies whole words from the offset rounded down
     - The words
   * - 3, reply buffer
     - page word, offset word, length
     - Sets the reply buffer, the same place ``0x1f2`` sets
     - None
   * - 4, check
     - one unused word
     - Transaction-bus loopback
     - 1
   * - 5, identify
     - one unused word
     - Reads the part's identification
     - ``0x1fc40000``
   * - 6, erase combination
     - none
     - Zeroes the four marker bytes
     - None

A reply writes the word count at reply-buffer word 1 and the words after
it, then 1 in word 0. A write longer than ``0x7ffb`` words fails before its
data, and an unknown type takes its data and stores nothing; both are
logged ``LOG_GUEST_ERROR`` and answer 0. A read whose length is not whole
words or does not fit the 16 KiB buffer is logged and answers nothing.
Bytes past 512 KiB are dropped on write and read as zero.

The identification word's first two bytes are the manufacturer and device
codes printed in :src:`108-0132-002 p. A-4`, an example rather than a
capture; the other two bytes are *unknown* and answer zero. The check
always passes and every reply is posted at once, *approximations*. No
factory image has been captured, so the flash starts zero with header
word 7 set to 2, an *approximation*. The flash keeps its contents across
reset and migration. With a ``drive`` property, or the machine's
``<slot>-kona-flash`` store (:ref:`sgi-sn-firmware-supply`), the flash
loads from a 512 KiB image and writes every programmed byte through to
it, so a stored video combination is kept for a later run; at the next
graphics initialization the pipe runs the combination stored at
``0x20000``.

.. _sgi-kona-credits:

Credits and flow control
========================

The IRIX kernel charges one graphics credit in the storing processor's
interface for each uncached store into the pipe's graphics page, and waits
for the count to return to its total before it attaches the next process,
a *software expectation*. The model returns one credit per charged store
to the Hub that sent it, once the words the store carried are taken: at
once for a word a framer consumed, and with the packet's last word for one
still held, so a packet's credits return together. An 8-byte store earns
one credit. Words a display list fetches from memory earn none. That the
pipe returns credits as its front end takes words is *inferred*.

While bit 0 of the low window word is set, returned credits are held and
go back when the kernel clears it. The kernel parks the target this way
when it switches a graphics thread out, a *software expectation*; how the
pipe holds them is *unknown*.

The credit register at ``0x800208`` counts the words the framers hold and
the words of framed packets not yet consumed, shifted left one and masked
with ``0x7fe``. The PROM waits for zero and the kernel waits while the
value is above zero, a *software expectation*; the two readings agree only
on zero, and that the field counts held words is the model's reading,
*inferred*. While display-list work is pending the register reads
``0x7fe``, full, an *approximation*: the kernel's flow-control handler then
enables the low-water cause and sleeps.

A display list a processor invokes runs in slices on a worker, so the
store returns while the pipe works. Command-stream stores queue behind the
work; the interrupt cause, interrupt enable, mailbox, retrace tag,
identification, low window word, activity and credit reads act at once;
every other access waits for the work to finish. Which registers a busy
pipe serves is *inferred* from the kernel's use. Past a million waiting
words a store waits for the work, logged ``LOG_GUEST_ERROR``; stores
outside a processor are not credit-bounded and can reach that.

.. _sgi-kona-interrupts:

Interrupts and completions
==========================

The kernel programs one destination and three vectors, writing the vector
register as ``0x20000 | xg << 18 | ge << 9 | general``, a *software
expectation*. The model sends a vector as an assert-only XIO interrupt
write to the programmed destination, in the generic widget layout taken
from the IMPACT model, *inferred*. With no destination programmed it logs
``LOG_GUEST_ERROR`` and sends nothing. The XG vector is stored and never
sent.

The general vector carries the cause bits:

- Mailbox, bit 4: set when a reply or completion word is queued. A
  write-1-to-clear that leaves another word queued sets it again at once.
  Clearing it also consumes an unread completion word at the queue's head,
  since the kernel's DMA wait reads the mailbox once and writes nothing
  back, a *software expectation*.
- Video, bit 3: set at a retrace that completes a retrace request, and
  sent only while the enable register has bit 3 set. Whether the bit also
  rises at retraces with nothing completed is *unknown*; the model does
  not raise it then.
- Fatal, bit 8: set when a DMA whose sender waits fails. No vector is
  sent, an *approximation*.
- Low water, bit 1: raised, with the vector, when display-list work and
  every word behind it are done, if the enable register has bit 1 set.
  The kernel enables it only while a thread waits for the pipe to drain, a
  *software expectation*; when a real pipe raises it is *unknown*, and the
  model's rule is an *approximation*.

The mailbox at ``0x800240`` holds at most five words. Any write advances
it without touching the cause register, as the PROM's reply protocol
expects, a *software expectation*. A completion that finds the queue full
is logged ``LOG_GUEST_ERROR`` and lost.

The geometry-engine vector carries completion reasons. The model writes a
reason, big-endian, from word 0 of the page ``0x1f1`` registered, sets
cause bit 5 and sends the vector. The kernel's handler reads word 0 once
per interrupt and writes it back ORed with ``0xbad00000``, a *software
expectation*. So the model posts one reason at a time: a later reason
waits until the page shows that rewrite, checked on cause-register access
and every millisecond of virtual time, up to 16 waiting. A reason with no
page registered is logged and dropped. Posting at once rather than after
queued work is an *approximation*.

The return page ``0x1f2`` registers takes configuration, flash, video and
read answers: data first, then a nonzero word 0. Page words with bit 28
set are host physical pages; otherwise the address is a device address
translated through the map RAM. Both readings are *inferred* from the
kernel's two uses.

.. _sgi-kona-dma:

DMA
===

The kernel's DMA packet is token ``0x1eb`` and six words: the device
address shifted right 12; its low 12 bits ORed with the tag ``0x9000``;
the total byte count; the per-line byte count; the gap between lines; and
``0x100`` when the sender waits for completion. This is a *software
expectation*. A completion posts mailbox word ``0x100`` under cause bit 4
and sends the general vector; that the pipe posts the packet's last word
is *inferred*.

What the packet's bytes feed depends on what precedes it, an
*approximation* resting on the senders:

- After a ``0x1c8`` record (pipe-memory address, byte count), one line of
  exactly that count goes into list memory at that address.
- After an X pixel write opened with its DMA mode bit, or a GL image,
  bitmap, texture or filter load waiting for its rows, the bytes feed that
  command, read through the map RAM.
- Otherwise the packet is logged ``LOG_UNIMP`` and moves nothing.

A packet whose tag is wrong, whose lines are not whole words dividing the
total, or whose span exceeds 64 MiB completes as failed, logged
``LOG_GUEST_ERROR``. A word 6 other than 0 or ``0x100`` is logged and
posts nothing.

``0x1ed`` registers the DMA pool the GL client's list calls read: a page
word, a tagged offset and a length, both multiples of 16 KiB and at most
256 MiB, the bound being the model's. ``0x16d``, ``0x16e``, ``0x170`` and
``0x171`` run lists from list memory or the pool, ``0x167`` pulls a block
from the pool and ``0x16f`` moves one in list memory. Pool pages are
translated through the map RAM at each read.

The packet names no destination widget. Every DMA and host write goes to
the one widget ``fabric-hub-widget`` names, an *approximation* that cannot
express a pipe mastering several Hubs. Guest words are big-endian and
stored whole. An access aimed at these registers from inside a graphics
head's own work is refused, logged ``LOG_GUEST_ERROR``, and returns an
access error.

.. _sgi-kona-raster:

Raster memory
=============

No capture gives the pipe's framebuffer organisation, so board memory is
the model's own layout, an *approximation*: front and back colour banks of
1280x1024 32-bit pixels, a tag byte per pixel for each bank saying whether
the pixel holds a textport index, an X index or direct RGB, list memory, a
24-bit depth buffer and a stencil byte per pixel. Rows count up from the
bottom display line. A tagged index resolves through its colour map at
scanout. The X colour map holds 32,768 entries, the colour-index map size
the published paper gives (:src:`montrym-infinitereality-1997`).

Texture memory is held as 32-bit texels plus coverage, per context. The
model allows host memory of nine times the reported texture memory, its
own bound.

.. _sgi-kona-display:

Display
=======

Channel 0 shows the whole 1280x1024 managed area at reset; the other
channels, up to the display board's count, start off. That is the format
the PROM falls back to, and a real display generator would start the
combination in its flash, so the start-up state is an *approximation*.

The X server drives the display generator with token ``0x1a6``: a
command number from 0 to ``0x5c``, its data words and a checksum equal to
the number plus the data words modulo 2^32, a *software expectation*.
Lengths come from the X server's send sites; where none is known, the
model searches for the checksum for up to 1,024 words, *inferred*, and
otherwise gives up and logs. The model answers the board and channel
configuration queries the X server waits for on the return page, and acts
on format loads, channel enable and disable, resize, blanking, gamma map
selection, cursor priority and channel source. Other commands are framed
and consumed.

The gamma load ``0x12f`` fills three components of 4,096 entries;
channels select a linear ramp or the loaded table. The cursor is 64x64 at
two bits per pixel with three colours, up to 64 images loaded with
``0x130``; the kernel selects channel, image, position and mode through
the indirect pair with selectors ``0x100``-``0x103``, a *software
expectation*. Only mode 0 (off) and 2 (on) are modelled.

The retrace runs at 60 Hz on channel 0, an *approximation*: ``0x1e6``
names a timing-table index the model does not decode, and no timing
table is held. A ``0x1e7`` request completes at the next retrace, one per
retrace in order, up to 16 queued; past that it is logged
``LOG_GUEST_ERROR`` and left for the kernel's timeout. Completing a
request sets the retrace tag and, for a double-buffered GL context,
exchanges its banks.

Reset
=====

Reset abandons display-list work and every word queued behind it, clears
every register above to zero, the map RAM, the mailbox, the reason queue,
the framers and their open commands, all contexts and textures, the colour
maps, gamma, cursor images and board memory, and returns the channels to
their start-up state. The firmware load record and stored load words are
cleared. The flash keeps its contents; only an operation in progress ends.
After reset the textport expects a sync word. How the pipe's reset domains
behave is *unknown*; these are model choices.

Migration
=========

The device state is version 6, with subsections for board memory,
scanout, contexts, textures, DMA, credits, flash and the rest. Display-list
work is finished before the machine stops and again before saving, so no
work in progress is migrated. Loading refuses a stream whose map RAM or
firmware-load sizes differ, and the ``list-memory-size`` must match. A
host-GPU renderer's state is not migrated: it is stopped before loading.
Migration works only between identical builds.

Measurements
============

No InfiniteReality pipe has been read for this model. Every register
value above comes from software or a model choice. A recorded register
trace of a real pipe through the IRIX kernel, or a read of a pipe's flash
and map RAM after power-on, would settle the reset values and the open
questions below.

.. _sgi-kona-limits:

Model limits
============

- No geometry engine, raster manager or display generator is emulated.
  The model decodes the host command grammars and draws with its own
  rules; the controller firmware and geometry microcode the loader writes
  are stored and never run.
- One managed area of 1280x1024 is drawn whatever mode or combination the
  guest loads. Display-mode commands ``0x1e6``, display-ID configuration
  ``0x133`` and ``0x1ff`` are consumed without effect.
- The low-water and activity rules are the model's readings; the
  interrupt enable reads zero.
- The kernel's interrupt handlers write ``0x800228``, which is not
  modelled; that write, and any other unlisted register access, logs
  ``LOG_UNIMP``.
- The DMA FIFO push register's placement at ``0x80007c`` is a model
  choice; the kernel's own offset is ambiguous.
- A list-write DMA packet's address is used as given, not translated
  through the map RAM.
- Unsupported cursor channels, offsets, positions and modes, and
  unsupported X and GL forms, log ``LOG_UNIMP``. Malformed commands, full
  queues and unregistered pages log ``LOG_GUEST_ERROR``.
- The GVO option, genlock, stereo, swap groups and multiple pipes are not
  modelled. ``reality``, ``ir`` and ``ir4`` are refused.
- More than 64 logical contexts stops the virtual machine.

Tests
=====

The qtests fit the pipe with ``-device sgi-kona`` on
``-M origin2000,nodes=1,x-uncatalogued-graphics=on`` unless noted.

- ``tests/qtest/sn0-origin2000-kona-test.c``: identity and aperture,
  one-wire records, plain-storage registers, reset, credit register,
  textport commands and mailbox protocol, map RAM and migration.
- ``tests/qtest/sn0-origin2000-kona-textport-test.c``: bitmap placement
  against a recorded PROM label.
- ``tests/qtest/sn0-origin2000-kona-config-test.c``: each product's
  ``0x1e9`` answer byte for byte against the captures, the class reads and
  every refusal.
- ``tests/qtest/sn0-origin2000-kona-bringup-test.c``: the internal port,
  firmware load record and activity latch.
- ``tests/qtest/sn0-origin2000-kona-eeprom-test.c``: the flash operation
  and header reads.
- ``tests/qtest/sn0-origin2000-kona-dma-test.c`` and
  ``sn0-origin2000-kona-xdma-test.c``: the DMA packet, list records,
  refusals, completions and X image DMA.
- ``tests/qtest/sn0-origin2000-kona-pi-credit-test.c``: credits returned
  to the Hub from processor stores.
- ``tests/qtest/sn0-origin2000-kona-scanout-test.c``: board memory,
  scanout, retrace period and their migration.
- ``tests/qtest/sn0-origin2000-kona-video-test.c``: the video command,
  its replies and the gamma load.
- ``tests/qtest/sn0-origin2000-kona-fill-test.c``,
  ``sn0-origin2000-kona-glyph-test.c`` and
  ``sn0-origin2000-kona-pixel-test.c``: X fills, one-bit drawing,
  stipples, spans and pixel writes.
- ``tests/qtest/sn0-origin2000-kona-context-test.c``,
  ``sn0-origin2000-kona-gl-test.c`` and
  ``sn0-origin2000-kona-3d-test.c``: context swap, GL windows and swap,
  and the GL command grammar.
- ``tests/qtest/sgi-kona-read-defense-test.c``: malformed command-page
  input through MMIO and migration.
- ``tests/qtest/sn0-onyx2-test.c``, ``sgi-graphics-pipes-test.c``,
  ``sn0-display-heads-test.c``, ``sgi-replay-test.c``,
  ``sgi-migration-roundtrip-test.c`` and
  ``sgi-synthetic-serial-test.c``: the pipe in Onyx2 topologies, the
  catalogue's products, screendump, recorded command replays, migration
  and synthetic serials.
- ``tests/functional/mips64/test_onyx2_deskside_prom.py`` and
  ``test_onyx2_rack_prom.py``: the stock IP27 PROM's ``hinv -v`` names
  the pipe in each chassis.
