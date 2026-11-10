.. SPDX-License-Identifier: BSD-3-Clause

.. _sgi-kona:

===============================
InfiniteReality: graphics pipes
===============================

Used in :ref:`sgi-ip27` (Onyx2) and :ref:`sgi-ip35` (the Onyx 3400 with
InfiniteReality3 on the ``origin3000`` machine). The Onyx 3000 G-brick holds
InfiniteReality3 and InfiniteReality4 pipes (*established*,
:src:`007-4264-002 pp. 39-43`); it is described on :ref:`sgi-bricks`,
and the Onyx 3400 with InfiniteReality3 builds it
(:ref:`sgi-sn-onyx3400-ir`). The model covers the InfiniteReality2,
InfiniteReality2E and InfiniteReality3 products.

.. seealso::

   Platform pages: :ref:`sgi-ip27`, :ref:`sgi-ip35`.

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
interface is public. The model is built from what the IP27 :term:`PROM`, the IO6
PROM, the IRIX 6.5.30 kernel, its X server and OpenGL library, and the
firmware IRIX loads into the pipe send and expect, read as binaries. Most
behaviour below is therefore a *software expectation* or an
*approximation*, and the board identities are *observed* in published
inventories. Facts are rated as described in :ref:`sgi-ratings`.

.. _sgi-kona-sources:

Sources
=======

- The Onyx2 deskside Owner's Guide, :src:`007-3454-005 p. 28`: the XG
  ASIC on the geometry board as the pipe's :term:`XIO` attachment.
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
     - The GE14-4 and GE16-4 parts are *observed*. The GE14-2 part is an
       *approximation*, since no cited capture shows one, and it presents
       the GE14-4's chip words, also an *approximation*.
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
space and the one :term:`Hub` widget the pipe's DMA and host writes go to; ``pi``
names the node whose processor interface counts the pipe's credits
(:ref:`sgi-kona-credits`); ``manufacturer``, ``part`` and ``revision``
build the identification register; ``probe-value`` is register ``0x64``;
``list-memory-size`` sizes resident display-list memory (default 15 MiB,
a nonzero multiple of 4,096 up to 64 MiB, a model choice); ``host-gpu``
lets GL draws use the host's GPU through Mesa instead of the CPU
rasterizer, with the same board memory as the result; ``record`` names a
file that receives the stream the guest sends the pipe
(:ref:`sgi-kona-record`).

.. _sgi-kona-register-map:

Register map
============

Offsets are from the widget's 16 MiB XIO window. Registers are 32 bits,
big-endian. Accesses must be 4 or 8 bytes. All offsets come from the IO6
PROM and the IRIX 6.5.30 kernel, X server and firmware loader, each a *software
expectation*; reset values are model choices unless stated, since no
pipe has been read.

.. sgi-registers: base=0x2000b000000 width=32
   skip=0x64:probe-value,0x800230:write-only,0x8003c0:write-only,0xc00108:write-only
   skiprow="XG map RAM:bulk"
   qemu="-M origin2000,nodes=1,x-uncatalogued-graphics=on -smp 1 -m 64M -device sgi-kona,id=kona0,slot=io4"

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
each an *approximation*. An access past the selected count is traced and logged
``LOG_UNIMP``.

.. _sgi-kona-commands:

Command page and grammars
=========================

Every command reaches the pipe as 32-bit words on the command page at
``0xc10000``. Four senders share the page, each with its own grammar, each
a *software expectation* read from the senders:

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
(:src:`io6-prom-6.156 textport`), each a *software expectation*, except where
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
other three words are zero, an *approximation*. The PROM waits for the
credit register to read zero before each command, up to 5,000 reads, a
*software expectation*.

Kernel commands
---------------

The tokens the IRIX 6.5.30 kernel and its graphics initialiser send, with
the words after the token, are each a *software expectation*. What the pipe does
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
   * - ``0x1e4``
     - 4
     - Pipe-memory reserve (privileged): kind, key, page, count. Kind 0
       with key 0 stores the attribute stack's store, base = ARM base +
       page x 2 KiB and size = count x 2 KiB, which the push handler
       reads (*established* for the routine; that kind 0 selects it is
       *inferred*). Any other key changes nothing. The model takes a
       display-list region as relocating list addresses and consumes
       the rest.
   * - ``0x1e1``, ``0x172``
     - 4, 1
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
rectangles. Word counts come from the X server's send sites, each a *software
expectation*. The model draws them into the colour planes with its own
rules, each an *approximation*: boxes count y down from the top display line, a
colour-index pixel resolves through X colour-map entry ``0x2d00`` plus the
index read as 10:10:10, and clip lists hold up to 64 rectangles.

The GL client's selectors are framed by a table built from the OpenGL
library's senders and executed by the model's own fixed-function renderer,
with display lists run from list memory and textures held per context.
How a real pipe renders is out of scope of this page and *unknown*;
selectors whose word count is unsettled are not framed.

Queries
~~~~~~~

A query asks the pipe a question and waits on the return page. The page
holds a completion word at offset 0, a count of reply words at offset 4
and the reply from offset 8. The client polls word 0 with no timeout, so a
query the pipe leaves unanswered hangs it (*established*, from the
firmware's wait loop). The pipe finishes the read-back for every handler
a valid request reaches, with a count of 0 and an error for a bad name
(*established*). The exceptions are the bare ``0x10b`` and a bare texel
read-back, which get no completion word (*inferred*; both are described
below), and the colour-table and pixel-map reader, which returns without
opening the read-back for a table record of -1 or a range past twice the
table width, a call the client's own checks keep a valid program from
making (*established*).

The generic state query ``0x4c`` takes one word, the parameter name. The
pipe answers 171 names (*established*, from the firmware's row table), each
with a fixed reply length in words: one for most, two for polygon mode and
the depth clear value, three for the point attenuation and sprite axis and
translation, four for the light-model ambient colour, fog colour, depth
range, accumulation clear, viewport, scissor box, colour clear, colour
write mask, blend colour, fog offset value and reference-plane equation,
and sixteen for the three matrices and the colour matrix. The depth range
(two doubles, four words) and the depth clear value (one double, two
words) are doubles (*established*). A name with no row, such as ``0xb14``,
gets the empty reply, a count of 0, and an invalid-enumerant error
(*established*). The model answers that way, except for 0xb00, the
current colour, which the client reads with 0x3b and never sends
here; the model answers it with four floats, an *approximation*. For a name with a row whose
value the model does not keep, the model replies zeros of the row's
length, an *approximation*, and logs ``LOG_UNIMP``. The fog function name ``0x812a``, whose
reply count the pipe reads from a word before the reply, is answered
empty without an error; its length is *unknown*, and the model's row of
0 words is a labelled *approximation*. The reference table is itself
inconsistent on this name: its row says the client answers it, and its
text says the client asks through the fog-function reader.

The current-value query ``0x3b`` takes one word and answers 13 words:
colour (four floats), colour index, normal, texture coordinates and the
edge flag at offset 0x30 (*established*). The client reads the edge flag
as a float and truncates it for integer and boolean reads, so a true flag
is a float of at least 1.0 and a false one 0.0 (*established*). Which bit
pattern the pipe writes for it is *inferred*; the model writes 1.0 and
0.0. The push reply's edge flag, word 12 of its current-value section,
takes the same float; what encoding that path uses is *unknown*, an
*approximation* chosen to match the query, whose record the section follows.
The raster-position query ``0x3d`` answers 15 words with the valid
flag at offset 0x34 (*established*); whether the flag is an integer or a
float is *unknown*, and the model writes the float 1.0 or 0.0, an
*approximation*.

The light-attribute query ``0x10b`` (one word) is sent by the client
library when it copies a context, and the client waits on the return page
for it (*established*, from the library). The engine's handler for the
token has no read-back start, so a bare send gets no completion word
(*inferred*), and the words the client would take (about 46 floats from
offset 8 of the return page, with gaps, then 12 words per light) are not
mapped to state (*unknown*). The model frames
the word, logs ``LOG_UNIMP`` and posts no reply, so a client that sends it
waits. The firmware also injects the token inside a push's read-back, only
when the mask has the lighting bit ``0x40`` (*established*); there the
push's completion ends the client's wait. A captured context-copy exchange
would settle it.

The push-attributes token ``0xc9`` takes the attribute mask, and the client
waits on the return page after every push. The pipe's handler opens the
read-back before it tests the mask and closes it on every path, a push onto
a full stack included (*established*, as are the 19 mask bits it tests).
That the finish handler then leaves a non-zero completion word, so each
``0xc9`` completes, is *inferred*, as for every query. Bits ``0x10000`` and ``0x20000`` are never tested and send
nothing, so a push of ``0x20000`` is an ordering wait. The model completes
every push: with no context current the reply is the completion word alone.
A mask naming the texture (``0x40000``) or transform (``0x1000``) group gets
the groups the model holds and none of those two groups' words, whose sizes
are *unknown*, an *approximation* that logs ``LOG_UNIMP``; the placement of
a section after an unsized ``0x1000`` section is likewise unknown. The
model's pop restores the texture and transform state it holds.

The restart token ``0x1bc`` takes one word, 0. The client sends it when an
instruments buffer is registered again, and the pipe reprograms its
select-feedback output channel from the saved start words if a feed is
active (*established*). The model frames the word and does nothing.

The user clip-plane command ``0xb4`` is nine words: the plane name and
four doubles, high word first (*established*). The generic enable and
disable ``0x24`` and ``0x25`` take clip planes ``0x3000`` to ``0x3005``
only; any other name from ``0x3000`` to ``0x3fff`` records an
invalid-enumerant error (*established*). The model frames ``0xb4`` as nine
words and records that error.

.. _sgi-kona-window-id:

Window-id ownership
~~~~~~~~~~~~~~~~~~~

The window and clip set-up request ``0x1f3`` carries the window's display
id in word 9 and an "obscured" flag in word 10. The pipe encodes them into
one raster register: bit 0 is the test enable (word 10 nonzero), bits 12:1
a reference id and bits 24:13 a mask (*established*, from the firmware).
Word 9 from 0 to 0x3e gives reference and mask over the six main-plane
bits; 0x3f, the popup, gives bit 11 alone; 0x40 to 0x5f gives the overlay
group, word 9 less 0x40, over bits 10:6; any other value, including all
ones, the kernel's null clip, gives the ordinary pair 0x3f under 0x3f
(*established*). All ones is therefore not a case of its own.

The compared value is the painted pixel word shifted right by 2, so a main
id painted in bits 7:2 is tested at bits 5:0 and an overlay or popup group
painted in bits 12:8 at bits 10:6 (*inferred*). A set mask bit is a
compared bit and a fragment passes when ``(plane ^ reference) & mask`` is
zero (*inferred*; the opposite polarity would compare nothing for any pair
the encoder makes). The model tests only while word 10 is set, which the
encoding states (*established*).

Whether the popup compare matches the way the popup is painted is
*unknown*. The compare tests bit 11, but the painter writes the popup as
overlay group 31 and never sets it. The model passes a pixel whose group
field is all ones for a popup window, an *approximation* of the leading
reading that group 31 means the popup. A capture of a popup validated
through the clip path would settle it. How the display chip composes the
overlay layers over the main one is *unknown*; the model shows an overlay
or popup window across its rectangle.

.. _sgi-kona-setup-rounding:

Set-up rounding
~~~~~~~~~~~~~~~

The geometry engine places window coordinates on a 12.8 fixed-point grid
by a single-precision subtract of 49152.0 (*inferred*, from the engine
microcode's store data). Whether that subtract rounds a tie to even or
truncates is *inferred*: the one diagnostic tie vector fits both, and
nearest-even is assumed. The model rounds each vertex to the nearest
1/256, ties to even, an *approximation* of that rule. The host renderer
accepts a draw under this rule, but it is handed the window coordinates
unrounded, so at a vertex exactly half way between two grid steps it
places the vertex by the host GL's own rounding, not by ties to even. The
scalar rasterizer and the host path can therefore differ at such a vertex;
no test compares them, so that difference is an untested limit. The tie
rule for pixel centres on an edge, and the sample point, are an
*approximation*. A capture of a triangle with a vertex half way between
two grid steps would settle the first.

.. _sgi-kona-x-polylines:

X polylines and points
~~~~~~~~~~~~~~~~~~~~~~

The polyline tokens ``0x15c`` and ``0x15e`` are seven words each, and the
single forms ``0x15b`` and ``0x15d`` are one and two words (*established*
for the sender: all four builds of the X server write the absolute
remainder as the token and one word, the relative remainder as the token
and two). What the pipe takes in is *inferred* from the senders alone. The
token map's count of two is not the absolute single form's framing: a
two-word read would swallow the first word of the next command after every
polyline of two to six points. The sender sends the remainder's earlier
points ahead of the token as data words, one to four of them for a
polyline of two to five points, and they join the line.

The engine's absolute and relative point converters both read the words
the position load ``0x12a`` stores, the drawable's x and y (*established*),
and add them to every point (*inferred*, from the engine handlers; that a
relative sum chains point to point from that origin and restarts at the
next token is likewise *inferred*). The X server's senders do not zero the
position: its graphics-context load takes the drawable's own x and y, and
zero comes only from the screen-wide senders (*established*). So the
addition is what turns window-relative points into screen points. The
model adds the position to every point and line vertex, absolute or
relative, and keeps a relative sum origin-relative.

Rectangles and boxes (``0x152`` to ``0x155``) take the same addition in the
model, to both corners of a box (*inferred*: the pipe uses whatever position
was last loaded, and nothing read shows a box exempt). Eighteen of the
nineteen X-server box senders zero the position before a box, so the add
rarely shows; the nineteenth reaches the position through a loader that
writes computed values. The one recorded desktop session has four boxes
after a nonzero origin among its 209 rectangle and box commands (201 are
``0x152``, 4 are ``0x153``, 3 are ``0x154`` and 1 is ``0x155``), so the add
does occur. A recorded command stream shows that the origin was nonzero,
not where the box landed, so the session cannot separate "the box adds the
origin" from "the box is exempt". A screen capture of a box drawn after a
nonzero origin, with the position and the box words known, would: the box
lands offset by the origin if it adds it, and at its own words if exempt.
Pixel writes arrive absolute and are not offset; whether their senders zero
the position first is *unknown*.

The model takes a polyline token's words as its last
vertices, after any vertices an earlier data token queued. A polyline
that has fewer than two vertices in all draws no segment; whether it
draws its one point is *unknown*, and the model draws it, an
*approximation*. How the pipe picks line pixels (the tie rule, the end
point) is *unknown*, so the model's line drawing is an *approximation*.

The background-point tokens ``0x15f`` to ``0x161`` draw each point in the
GC's background colour without the loaded stipple, which the ordinary
point tokens apply (*inferred*). Word 4 of the graphics-context load is
the plane write mask, written under ``0xffffff`` (*established*), and the
X server sets its bit 31 for an opaque stipple, the only wire difference
between that fill and a plain one (*established*; the meaning is
*inferred*). Whether the board honours the bit is *unknown*, since the
chip probably masks it. The model reads the bit as sender intent and
paints the background where the stipple is clear, an *approximation*. A
raster-side observation of the pixels an opaque stipple writes would
settle it.

Texel readback
~~~~~~~~~~~~~~

``0x52`` reads a texture level back: 18 words, the target, the width and
height, the format, type and storage code of the answer, the region's
first and last texel, and five words copied from the level's load: the
size exponents (word 14, load header word 16), the format and layout byte
(word 15), the first-level bank in bit 28 of word 13 and the two bank
addresses (words 16 and 17, load header words 18 and 19). The client
library sends it bare when the answer fits 16368 bytes and brackets it
with ``0x1f5`` and ``0x1f6`` when it does not (*software expectation*,
from a Quake GL trace). A bracketed read completes by output DMA. A bare
read is answered on the return page: the data at offset 8, then a nonzero
word 0, which the client polls with no timeout; an unanswered read hangs
the client. The order is *inferred*, as for the other return-page
answers.

The model answers every bare read. It finds the level by target, size
exponents, bank (bit 28 of word 13, against bit 3 of the load's word 17)
and address (word 16, against the load's word 19 in bank one and word 18
in bank zero). The model does not use the storage code to pick the level,
an *approximation* and not the pipe's rule: payload word 5 describes the
source texels to the pipe (*established*), so a read in storage ``0xd``
with format 6 and type 3 reads memory as 16-bit components. That
the address is word 16 in both banks is *inferred* from six reads in one
Quake GL trace, five of them in bank one. Two textures at one address
leave the read unanswerable. The reply is that level in the
requested format and type: red, green, blue, alpha, RGB, RGBA, luminance
and luminance-alpha, as unsigned bytes or unsigned shorts (each byte
times 257), tightly packed. Luminance and luminance-alpha put an RGB sum
in front of the destination unless the level holds a single channel: the
sum of the three colour channels clamped at 1.0, with alpha kept (the
stage selection is *established* from the firmware, the clamped sum is
*inferred*, and the read of an RGBA texel as luminance has not been seen
in a recording). A single-channel level, held in the red channel, is
returned as it is; a texel upload to a luminance texture takes red alone.
The reading of format 6, type 3 and storage ``0xd`` as luminance in
unsigned shorts is *inferred* from the model's own format table and the
2-byte texel the firmware reads; the code meanings beyond the texel size
are *unknown*. The conversion is an *approximation*: the model holds
levels as 8-bit RGBA and the pipe's own conversions are *unknown*.

The sum does not touch a 16-bit single-component level. One resolver
decides it for the texel read and the pixel read alike, and a source of
one component expands to (L, 0, 0, fill), so the sum is L and a 16-bit
word comes back unchanged (*inferred*, from the firmware's expansion; no
recording shows it and there is no hardware measurement). A read as
format 6, type 3, storage ``0xd`` of a level that holds 16-bit words
therefore returns those words as they sit in texture memory, not a decoded
colour, and the library loads them back unchanged under storage ``0xd``
(*established* for the sequence, from the Quake GL trace; *inferred* for
the raw words). The model applies this to a level loaded under ``0xd``
(16-bit luminance), ``0x10`` (four bits a component) or ``0x14`` (five
bits and a one-bit alpha). A ``0xd`` load of format 6, type 3 keeps its
word whole, the high byte in red, which a luminance sample reads, and the
low byte in green. A ``0x10`` or ``0x14`` level is held as 8-bit
components, so the word returned is the model's packing. The lane layout
of storage ``0x14`` (red in bits 15:11, green 10:6, blue 5:1, alpha in
bit 0) and of the four-component 4-4-4-4 form of ``0x10`` (the first texel
in the top 16 bits of a word) is *established*, from the firmware's
formatter and a second source. The model's reduction of
8-bit components to 4 or 5 bits (it truncates when packing and replicates
the top bits when unpacking), its alpha lane for the formats without alpha
(it writes 1 or ``0xf``), and its handling of the other formats that share
storage ``0x10`` (3-3-2, RGB2, RGBA2 and the quad-group formats, whose
layouts differ and which it packs as 4-4-4-4) are each an *approximation*. The raw return is keyed in the model on the
read's code ``0xd`` together with the held level's storage; the pipe keys
it on the read's code alone, as the description of the source texels
(*established*, the raw return *inferred*). A level held as ``0x30`` and
read with code ``0xd`` is a layout mismatch the pipe's behaviour for
which is *unknown*; the model returns the colour sum, an *approximation*. The
bracketed copy form (format 7, type 3, storage ``0x28``) is unchanged. A
read the model cannot produce, for want of a matching level or of the
format, type or size, is answered with zeros, so the client draws a black
texture rather than hanging, and logged ``LOG_UNIMP``. A bare read with
no current return page is logged ``LOG_GUEST_ERROR`` and posts nothing.
A bracketed read that is not the copy form is answered by output DMA
with the same packed image the bare form returns. It is written from the
first byte of the range the ``0x1f5`` describes: the high address word is
the map page, the low word the page offset plus ``0x9000``, the length the
padded image size, the line length equal to the length and the stride zero,
on output-DMA channel 0. There is no return-page header and no completion
word. The ``0x1f6`` then posts engine event ``0x100`` with the context's
slot, the bytes left over (the length less those delivered) and a status
word whose bit 11 (``0x800``) is overflow (*established* for the framing,
from the firmware; *inferred* that the image is the bare form's, and the
padding is *unknown*, zero in the model). The model delivers the image only
when the range is one line with no stride and holds the whole image; any
other bracketed read is answered with zeros, as before, and logged
``LOG_UNIMP``.

Levels are found by place, not storage code
~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~

A bind carries no storage code for its level and the pipe keeps none per
level. It finds the image by the cache entry number (or, for entry
``0xffffffff``, by the level table and the first-level bank) and samples
the words found there in the format of bind word 2, bits 20:14, whatever
storage loaded them (*established* from the firmware, which has no
comparison or store of a storage code; the client sequence is from a Quake
GL trace). So a 16-bit level that the library loads, reads back, frees,
retakes and loads again under storage ``0xd`` is bound as ``0x10`` or
``0x14`` and samples the original texels.

The model keys a level by target, size exponents, bank, address and a
volume's layout mode, and a later load at the same place overwrites the
level whatever storage and layout family it names. A load of another
format that rewrites the whole level replaces it. One that rewrites only
a part of it is refused, as the model cannot merge two formats' texels.
The bind's storage code selects no level. A level that holds 16-bit
words (a ``0xd`` load of format 6, type 3, see above) is decoded as the
bind's colour format when the bind names ``0x10`` or ``0x14``, with the
layouts above, and sampled as luminance when it names ``0xd``. Any
other pairing samples the model's texels as they are, an *approximation*:
the pipe would reinterpret the words the level holds. A border read of such
a decoded level is refused.

Texture loads across a context switch
~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~

A ``0x174`` streams its rows straight into texture memory as they arrive;
the pipe stages no copy of the texels, and rows already written stay
there across a switch (*established*). What a switch saves and restores
for each context is the load's control state: the load registers, the
count of words still owed, the pass-through counter, the continuation and
the contents of the engine FIFO. It saves no texels, and a half-done
load carries on from the restored state (*established*; that the raster
boards have no path to pull texels back from memory is *inferred*).
``0x176`` reads one word, adds one to a 20-bit counter and stores the
result in the entry; it checks nothing about whether the level's rows have
all arrived (*established*).

Texture memory is one pool for the whole pipe. The kernel gives each owner
disjoint blocks and tells the pipe each block's offset through two
registers the context switch saves, one for loads and one for drawing
(``0x5001`` and ``0x101a``). Each holds the block's page number shifted
left 18 bits and arithmetic-shifted right 6, so it is sign-extended
(*established*, from the pipe firmware). A swap-in zeroes them once the
transfer is done, as a swap-out does, and only a reserve leaves the value
alone (*established*). Level addresses in
loads and binds are relative to the block (*software expectation*), so
the hardware adds the offset; that it adds it is *inferred*. The tables
from entries to addresses belong to each context (*software expectation*).
The 513-entry id table belongs to the firmware, not to a context, and is
flushed at every activation, so no load or bind id outlives a switch
(*software expectation*, from the pipe firmware). Processes in a
kernel share group can share one block (*software expectation*).

The model keeps each context's load as the context's own. A load that is
finished and waiting for its ``0x176``, or whose rows are due by DMA, is
set aside with its context at a switch and returns with it, whatever the
context that ran meanwhile had in flight, including a load with rows still
owed. A load still taking FIFO rows travels with the open command and
comes back with it. No switch refuses a restore, drops a level or lets
one context's load finish another's. Staging the rows instead of writing
them through is an *approximation* with the same visible result, since
the model publishes the level at ``0x176``. The model completes the level
from the current context's own load and does not depend on which context
loaded last.

Texture objects are keyed by context and entry, so the pool is not
shared: two contexts that load the same entry number hold two textures,
as the kernel's disjoint blocks and per-context tables make them, but the
global-offset registers (value and zeroing as above) are not modelled, a block shared within a share
group is not shared, and the flush of the id table at activation is not
modelled. These are limits of the model, not hardware behaviour.

The model also discards the current context's own finished load in three
places where the hardware keeps the rows already written. This is an
*approximation*: a finished load waiting for its ``0x176`` is dropped when
the context sends a data-run command other than ``0x174`` (a polygon
stipple row run, the pop-lights words or a bitmap), when it sends
DrawPixels, or when it sends a ``0x174`` for a different entry. The
hardware has no record to lose in these cases, so a ``0x176`` after any of
them would still find its rows.

Detail textures
~~~~~~~~~~~~~~~

The GL client defines a detail texture as the object with target
``0x8095`` (``GL_DETAIL_TEXTURE_2D_SGIS``), one level, and sends nothing
until a level is defined and resident (*software expectation*, from the
client library). The level loads as an ordinary ``0x174`` whose header
word 0 is ``0x8095`` and whose word 10 is the cache entry, then ``0x176``
finishes it. It is sent once per bank with bit 3 of word 17 flipped, and
the same image goes to both banks. A Quake GL trace carries a 128x128
image in storage code ``0x14`` (RGB5) at address zero in both banks
(*software expectation*). The model stores it as a one-level 2D image
under its own target, keyed by entry and bank.

``0x17d`` binds the detail: six words, the entry (or ``0xffffffff``), the
s and t exponents, the format row and the two-word address pair. The pipe
never resolves the entry to an image. It keeps the row and the pair and
writes the pair word the base texture's bank selects, word 4 in bank one
and word 5 in bank zero, to the detail-location register, so the hardware
reads texture memory at that address (*software expectation*, from the
pipe firmware). ``0xffffffff`` skips releasing the entry's load id and the
bind goes ahead. The detail level, the add or modulate mode, the channel
select and the filter-table slot come from the base texture's bind, not
from this command (*software expectation*). The model finds the image by
that address, the base's bank and the size; the entry word only narrows
the search to one texture first, and
``0xffffffff`` searches all of them. An ordinary 2D load at the address
also qualifies, as the hardware cannot tell them apart in memory. Only the
firmware's choice of pair word is *established*. That the word it picks
equals a load's address word 19 in bank one and word 18 in bank zero is
*inferred*, and no recording tells the two words apart, since the Quake GL
trace has address zero in both banks.

When no detail applies, the pipe magnifies linearly and the base texture
shows: with no ``0x17d`` since reset, or when the detail's format row
differs from the base's, the base bind forces magnification code 1 (the
mode-1 ``0x1d5`` bind does the comparison; the ``0x106`` form does
neither). The pipe has no flat-colour state, so a flat white or black
surface is not a hardware behaviour (*software expectation*, from the
pipe firmware). The model does the same for every case where no detail
applies. It logs ``LOG_UNIMP`` once per binding when it cannot find the
image, or finds two at one address, and it compares internal formats
where the pipe compares rows. A row is a public internal format with a
storage code, and several rows can share a code, so two rows with one
internal format and different codes differ on the pipe and not in the
model; the tests agree while each row names one internal format
(*inferred*).

The combine is add, base plus K(lambda) times the detail minus one half,
or modulate, that product times the base texel, with K read from the
stored doubled detail curve at 64 times 2 to the power lambda
(*inferred*). The curve's 64 samples sit at log2(i/64), detail samples
doubled and sharpen samples negated, with default points (-4, 1) and
(0, 0) (*software expectation*, from the client library). Each sample is
quantised to a 10-bit code: the value is clamped to -1024 to 1023, halved
arithmetically while it lies outside -128 to 127, counting 0 to 3 halvings,
and sent as ``((n & 0xff) << 2) | count``, so a code decodes as the signed
mantissa in bits 9:2 times 2 to the power of the exponent in bits 1:0,
over 128 (*established*). The model decodes the codes that way.
Sharpen binds use bind mode 2 and detail binds mode 1 (*established*). The
detail level in the bind is the public level negated and stored 0 to 4,
public -4 to 0 (*established*), and the model reads it so. When both the
2D and 3D enables are set, 3D wins (*established*); the model's draw does
the same. The model computes in double precision with the public
weight, half the stored sample, times twice the detail minus one, which
is the same product (*approximation*). The pipe's widths and rounding, and
how it derives the detail's s and t from the base's s and t and the level,
are *unknown*; the position is inferred to be the base level-0 coordinate
times 2 to the power of the level's magnitude, with K twice the stored
curve value (*inferred*). The model scales the base coordinates by the
ratio of the two sizes and by two to the power of minus the detail level,
which differs from that inference when the sizes differ, an
*approximation*.

A bind that follows a format-row mismatch forces magnification code 1 for
every mode-1 bind of a 2D target, whatever its code (*established*); the
model applies the fallback to detail magnifications and has not been
checked against other codes. ``0x17c`` leaves the r and LOD scales as they
were, since it never writes the registers for them (*established*); the
model has not been checked against that.

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
*inferred* and one an *approximation*. Record serials are synthetic. The
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
always passes and every reply is posted at once, each an *approximation*. No
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

The colour chain is 10-bit colour-map entries, then a 12-bit gamma
index. The model follows it for a pixel that comes from a colour map (the
X colour maps and the overlay and popup maps): the 10-bit output sits in
index bits 11:2 and its top two bits repeat in bits 1:0, ``c << 2 | c >> 8``
(*inferred*, from the address rules in the pipe's diagnostic, which never
force the low bits to zero and tie them to bits 11:10; zero fill and an
8-bit-based widening disagree with them; the rule is for legacy
addressing, flag bit 18 clear). A pixel that does not go through a colour
map is direct colour with only 8 bits, and no widening rule is known for
it: the model repeats the byte, ``v << 4 | v >> 4``, an *approximation*.
The textport's colour map keeps 8-bit entries (the textport load narrows
its 10-bit components as it stores them), so a textport pixel takes the
direct-colour rule, also an *approximation*. A capture with a marker
table, 0x3ff selecting entry 0xfff against 0xffc, would settle the
widening.

The gamma output reaches the video-output chips and the DACs as bits
11:4, truncated: bits 3:0 of the output are unconnected on the base
board, and nothing in the register map rounds or dithers (*inferred*: it
rests on the board connection data in the shipped diagnostics, a software
expectation that this page cannot cite by a public document number; no
published description of the link was found). The
chip between the gamma output and the DACs also resizes, so the unity-size
end-to-end reading is *inferred* too. The model shows the entry's top eight
bits. The DAC widens its 8-bit input in the start-up palette bypass mode
by zero fill, the two low bits of its 10-bit converter input zero, not by
rounding (*inferred*: it rests on a description of such a bypass mode for
a DAC of this kind. No public document for this board's DAC was found,
so none is cited, and it holds only if the board's part behaves that
way).

The gamma load ``0x12f`` fills three components of 4,096 entries;
channels select a linear ramp or the loaded table. The XMAP cursor is
64x64 at two bits per pixel with three colours, up to 64 images loaded
with ``0x130``; the kernel selects channel, image, position and mode
through the indirect pair with selectors ``0x100``-``0x103``, a *software
expectation*. Only mode 0 (off) and 2 (on) are modelled. The XMAP cursor
is injected ahead of the colour map and gamma, so its colours pass through
the channel's gamma table like a colour-map pixel (*inferred*; whether a
separate bypass for cursor pixels exists is *unknown*).

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

.. _sgi-kona-record:

Recording the stream
====================

With ``-device sgi-kona,record=FILE``, or ``record`` set on the machine's
pipe, the pipe saves what the guest sends it and what it answers, so the
stream can be replayed into another model of the same board and the
answers compared in order. It is off by default. A pipe without ``record``
pays one pointer test at each recording point, no more than a disabled
trace point costs. The file is created when the pipe is realized and
closed when QEMU exits. A reset during the run puts a marker in the file
and does not restart it. A recording is for a straight run from reset: an
incoming migration or snapshot load changes the guest state without a
marker.

The format is binary and versioned. All integers are little-endian. The
file is not compressed. It begins with the 8 bytes ``KONAREC`` and a NUL,
then a ``u32`` format version, 1 here, then records. A record is a tag
byte and its fields, with no padding:

.. list-table::
   :header-rows: 1

   * - Tag
     - Fields
     - Meaning
   * - ``C``
     - part ``u32``, revision ``u32``, rm_count ``u32``, texture_mb
       ``u32``, n ``u8``, n bytes of ``product``
     - The board, once, first. The product name is not NUL-terminated and
       is empty when the pipe has none.
   * - ``W``
     - size ``u8``, off ``u32``, val ``u64``
     - A guest store of 4 or 8 bytes at byte offset ``off`` in the pipe's
       register aperture, in the order the pipe received it. ``val`` is
       the numeric value, not byte-swapped. A store whose words the pipe
       holds behind its work is recorded when it arrives.
   * - ``R``
     - size ``u8``, off ``u32``, val ``u64``
     - A guest load and the numeric value the pipe answered. A load
       identical to the one before it, with no other record between, is
       not kept.
   * - ``D``
     - src ``u8``, addr ``u64``, n ``u32``, n bytes
     - Guest memory the pipe read, as read, at the point of the read.
       The bytes are in memory order, as the guest stored them.
   * - ``O``
     - src ``u8``, addr ``u64``, n ``u32``, n bytes
     - Guest memory the pipe wrote: replies, the reason and return pages,
       image-DMA results and rows read back from the framebuffer. The
       bytes are in memory order.
   * - ``I``
     - kind ``u8``, vector ``u8``, delivered ``u8``
     - An interrupt the pipe raised, kind 0 general or 1 geometry engine,
       and whether the fabric accepted it.
   * - ``M``
     - kind ``u32``, arg ``u32``
     - A marker. Kind 1 is a reset, 2 a buffer swap taking effect (``arg``
       is the context), 3 a retrace completion (``arg`` is its tag) and
       ``0x100`` a mark made by writing a value to the pipe's
       ``x-record-mark`` property (``arg`` is the value).
   * - ``E``
     - count ``u64``
     - The end, written when the file is closed: the number of records
       before it. A file with no ``E`` was cut off, and its last record
       may be partial.

``src`` of a ``D`` or ``O`` record is 1 for a direct access to host memory:
the reason page and its read-back, the return page, and every image-DMA
write (the pipe's reply to a pixel or texture read). It is 2 for an
image-DMA read through the map RAM, 3 for a display-list fetch and 4 for
the rows of a pixel rectangle. For 1 to 3, ``addr`` is the guest physical
address of the first byte as the pipe issues it, before the fabric adds
the Hub widget. Image-DMA reads and writes are recorded one chunk at a
time, each of at most 4,096 bytes and never across a 16 KiB map-RAM page,
each at its own address; a reader that wants the run joins contiguous
chunks. A list is one record per range, under the address of the range's
first byte, and a range that crosses pages is one run of bytes. For 4,
``addr`` is the byte offset of the row in the buffer the transfer's page
list describes, from the start of that buffer: the transfer's offset plus
the row number times its pitch, not a plane coordinate. A fetch or access
that fails is not recorded.

The file leaves out:

- accesses the model refuses because they arrive from inside a graphics
  head's own work, and stores after a context has failed;
- an interrupt the pipe cannot send, for want of a destination or a fabric
  address;
- the board's configuration beyond the ``C`` record (geometry, raster
  manager type, channels, probe value, list memory size, serial and flash
  contents), which a replay model must be given out of band; one-wire and
  flash reads appear as ordinary ``R`` records;
- changes the pipe makes on its own timers, such as cause bits set at a
  retrace, which a replay sees only through the ``R`` answers.

Identical consecutive loads are dropped because polling loops would
otherwise dominate the file. That is safe for a replay that checks the
answers in order, because the interrupt cause register is the only
register whose read does anything, and when it does it writes ``O`` and
``I`` records, which end the run of identical loads. A reader must not
count loads. A load the model refuses is not recorded either.

The file is buffered. It is flushed every 4,096 records, after the ``C``
record and after every marker, so a script that writes to ``x-record-mark``
and then reads the file sees everything before its marker. A process
killed without a chance to exit loses the records since the last flush,
and the last record may be partial. The first failed write is reported and
stops the recording without an ``E`` record.

``W`` and ``R`` have the layout of the same-named records in the VPro
model's recording in the Iris emulator, and the header has the same shape:
a magic, a ``u32`` version, little-endian integers. Only those are shared.
Iris's reader knows ``W``, ``R``, ``B``, ``b`` and ``H``, and rejects any
other tag, so it rejects this file at its first record, ``C``. The six
tags ``C``, ``D``, ``O``, ``I``, ``M`` and ``E`` are new here, and ``B``,
``b`` and ``H`` do not occur. The magic differs (``KONAREC`` for
``ODSYREC``). ``D`` and ``O`` carry a byte count where
Iris has quadword blocks and single bytes, and ``M`` carries a kind and
an argument where Iris's ``H`` carries a plane hash. Iris drops identical
consecutive loads only until the next store or mark; here any other record
ends the run. The file is not gzip: compress it with ``gzip`` for Iris's
container.

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
- Multisample is not drawn: the visual is accepted and drawing is
  single-sample. The control, sampling and depth-clear words, and the
  resolve and coverage rules, are not modelled; the resolve rule and the
  sample positions are *unknown*.
- Accumulation is single-pass floating point for every scale. That the
  pipe splits a scale of exactly 1.0 into two half passes is *established*
  from the sequencing, but its storage format is *unknown*, so no visible
  difference follows.
- The DAC cursor load (display request 56, 263 words) is framed and
  consumed; the image and colours are not kept and the cursor is not drawn.
- Select mode returns hit records; feedback mode returns no records.
- A small texture level's origin is not taken from the last of its copies,
  and a load at a new address does not retire the levels it overlaps; the
  model keys levels by place instead.
- Before the loader releases the controller's firmware, the display state
  is channel 0 over the whole area. The stored video combination runs at
  the release (``+0x800328`` written 1), not at power-up, which is where the
  model is an *approximation* of a pipe that starts the stored combination
  itself.
- The detail combine is the model's reading: its arithmetic widths,
  rounding and coordinate derivation are *unknown*, and only the channel
  select Quake GL uses (colour) has been exercised against a guest.

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
- ``tests/qtest/sn0-origin2000-kona-record-test.c``: a scripted image DMA
  with ``record`` set; the file's header, stores, loads, DMA bytes,
  interrupt, markers and end count.
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
  the GL command grammar, texel readback (bare and bracketed) and the
  detail texture load, binding and fallback.
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
