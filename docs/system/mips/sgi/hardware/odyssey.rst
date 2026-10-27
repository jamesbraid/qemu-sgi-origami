.. SPDX-License-Identifier: BSD-3-Clause

.. _sgi-odyssey:

==========================
VPro graphics (Odyssey)
==========================

VPro is the graphics family SGI sold as V6, V8, V10 and V12. SGI's
upgrade guide fits it to both the Octane and the Octane2, given Crossbow
1.4 and the ER power supply (:src:`007-4210-003 preface`),
*established*. Published inventories name Octane V6, V8, V10 and V12
boards and a Fuel V10, *observed* (:ref:`sgi-odyssey-identity`). The
model places it in the Octane2, Fuel, Tezro and Onyx 350 machines, and
in the Octane only on request (:ref:`sgi-odyssey-placement`). Each board is one XIO widget whose
graphics engine gfxinfo calls BUZZ and whose display back end it calls
PB&J. QEMU models the board as ``sgi-odyssey`` in
``hw/misc/sgi-odyssey.c``, with the optional Mesa drawing path in
``hw/misc/sgi-odyssey-native-mesa.c``. No register specification of
any VPro board is public. The model is a functional profile built from
the public OpenBSD console driver, published inventories of real boards
and the behaviour of the shipped IRIX 6.5.30 kernel, X server and GL
library. It describes no particular ASIC stepping. The graphics overview
keeps a short summary (:ref:`sgi-graphics-odyssey`).

Facts are rated as described in :ref:`sgi-ratings`. Most register
behaviour here is *approximation* or *software expectation*: it shows
what the stock software asks of the board and what the model answers,
not what the board does.

.. _sgi-odyssey-sources:

Sources
=======

- OpenBSD ``sys/arch/sgi/xbow/odysseyreg.h``, ``odyssey.c`` and
  ``xbowdevs`` at ``2df91d80c104``, as software expectations: the mapped
  aperture, the command and data FIFO ports, two status registers, the
  console's fill, copy and glyph command words, and the widget identity.
- Linux ``arch/mips/include/asm/xtalk/xwidget.h`` at ``2f9060b1db4a``,
  as a software expectation: the widget manufacturer and part names.
- The *Octane/Octane2 VPro Graphics Upgrade Kit Installation Guide*,
  :src:`007-4210-003 preface`: VPro in the Octane and Octane2, and
  the Crossbow and power-supply conditions.
- Published inventories and gfxinfo listings of real boards:
  :src:`usenet-1154846834` (an Octane2 V12),
  :src:`usenet-au03s8` (an Octane V6),
  :src:`usenet-1131341991` (an Octane V6 inventory),
  :src:`usenet-3fef80f1` (an Octane V8 inventory),
  :src:`usenet-hvh0d` (an Octane V10 board name) and
  :src:`tingo-fuel-irix-console gfxinfo` (a Fuel V10).
- The IRIX 6.5.30 IP30 kernel, gfxinfo, hinv, X server and GL library,
  read as shipped binaries, as software expectations. This page credits
  them only for the identity and memory-word decode, the back-end
  version word, the id-plane layout, the shadow-page replay layout, the
  backing-store transfer forms, the ``0x0a5a`` fence word and one DMA
  header bit. Every other register role on this
  page is stated as what the model does.
- The same software run on the model, recorded with QEMU's
  ``sgi_odyssey_mmio`` trace event: what the guest sent the model, not
  what a board does.
- The reference Octane2's inventory
  (:ref:`sgi-measured-octane2-hinv`).

.. _sgi-odyssey-identity:

Products and identity
=====================

The board answers the XIO widget identity with manufacturer ``0x023``
and part ``0xc013``, a *software expectation* from OpenBSD ``xbowdevs``
and Linux ``xwidget.h`` (:ref:`sgi-graphics-widget-identities`). The
identity names the family. It does not select V6, V8, V10 or V12. The
model builds the word as revision in bits 31:28, part in bits 27:12,
manufacturer in bits 11:1 and bit 0 set, so a V12 reads ``0xac013047``.
Bit 0 set is an *approximation*.

The IRIX 6.5.30 kernel turns widget revisions 1 to 3 into a BUZZ A part
and 9 to 15 into a BUZZ B part, and rejects 0 and 4 to 8. gfxinfo prints
the minor number as the revision's offset less one, so revision 2 is A.1
and revision 10 is B.1. hinv names the board from the generation and the
memory size: A with 32 MB is V6, A with 128 MB V8, B with 32 MB V10 and
B with 128 MB V12. All three are *software expectations* read from the
shipped binaries.

The ``product`` property selects one board. It sets the revision, memory
size, bank count, back-end version and identity record together:

.. list-table::
   :header-rows: 1

   * - ``product``
     - Revision
     - gfxinfo BUZZ
     - Memory
     - Banks
     - PB&J
     - Identity record
     - Basis
   * - ``v6``
     - 2
     - A.1
     - 32 MB
     - 2
     - 1
     - ``ODY32``
     - BUZZ A.1, PB&J 1 and 32 MB *observed*
       (:src:`usenet-au03s8`); board name *observed*
       (:src:`usenet-1131341991`). Two banks *inferred*.
   * - ``v8``
     - 2
     - A.1
     - 128 MB
     - 4
     - 1
     - ``ODY128``
     - Board name *observed* (:src:`usenet-3fef80f1`). Revision, banks
       and PB&J *inferred* from the generation and size; no V8 gfxinfo
       listing was found.
   * - ``v10``
     - 10
     - B.1
     - 32 MB
     - 2
     - 1
     - ``ODY32VERSIONB``
     - B.1, PB&J 1 and 32 MB *observed* on a Fuel V10
       (:src:`tingo-fuel-irix-console gfxinfo`); Octane board name
       *observed* (:src:`usenet-hvh0d`). Two banks *inferred*.
   * - ``v12``
     - 10
     - B.1
     - 128 MB
     - 4
     - 1
     - ``ODY128VERSIONB``
     - B.1, PB&J 1, 128 MB, four banks and CAS latency 3 *observed*
       (:src:`usenet-1154846834`).

gfxinfo prints a fixed ``Banks: 2, CAS latency: 3`` line for any 32 MB
board and does not read those fields, so the 32 MB boards' two banks
are *inferred*. Every product is CAS latency 3. Whether every board
of one product shares a revision is *unknown*; each product's revision
is a model choice within its generation (*approximation*).
The identity records name the Octane boards. Fuel and Tezro boards are
other assemblies, and the model answers the Octane name on them too
(*approximation*).

Without ``product`` the device takes its own defaults, the V12 values,
and the ``revision``, ``memory-size``, ``memory-banks``, ``cas-latency``
and ``backend-version`` properties set each field alone. Realize refuses
a revision wider than four bits, a size other than 32, 64 or 128 MB, a
bank count other than 2 or 4 and a latency other than 2 or 3. The
machines' graphics catalogue offers only ``product=v12``; the other
products need ``x-uncatalogued-graphics=on`` on the machine
(:ref:`sgi-sn-graphics-pipes`).

.. _sgi-odyssey-placement:

Placement
=========

A VPro board is a widget on a crossbar port. The machine owns the
routing, interrupt delivery and DMA reach; the board owns its widget
registers, memory and display.

.. list-table::
   :header-rows: 1

   * - Machine
     - Port
     - Default
     - Basis
   * - ``octane2``
     - Crossbow widget 11
     - V12
     - *Established* on the reference Octane2, whose inventory shows a
       V12 at widget 11 (:ref:`sgi-measured-octane2-hinv`).
   * - ``octane``
     - Crossbow widget 11
     - none: the catalogue refuses VPro, and
       ``x-uncatalogued-graphics=on`` places any product
     - The upgrade guide asks for Crossbow 1.4 and the ER supply
       (:src:`007-4210-003 preface`, *established*), which this
       machine's Crossbow revision and supply do not present
       (:ref:`sgi-ip30-graphics`).
   * - ``fuel``
     - XBridge widget 13
     - V12
     - Widget 13 *observed* in a Fuel inventory
       (:src:`tingo-fuel-irix-console`). The published board is a V10;
       the V12 default is a model choice.
   * - ``onyx350-compat``, ``tezro-tower-multi``
     - PIC widget 12
     - none
     - Optional, ``slot=widget-12``. The IP53 page takes widget 12
       from published Tezro and Onyx 350 inventories (:ref:`sgi-ip53`);
       none is in the source registry, so here it is a model choice
       (*approximation*).

On the Octane2 the processor reaches the board through the short
window at physical ``0x1b000000`` and the long window at
``0xb000000000`` (:ref:`sgi-ip30-graphics`). On the SN1 machines a
processor store that the processor interface marks as counted returns
one graphics credit through the ``pi`` link after the board takes it.

.. _sgi-odyssey-registers:

Register map
============

The board decodes a ``0x410000``-byte aperture at widget offset zero,
big-endian. OpenBSD maps ``0x410000`` bytes there
(``odysseyreg.h``, a *software expectation*). IRIX 6.5.30 maps
``0x401000``; the difference is *unknown*, and the model covers the
larger one. A mapped aperture is not evidence of a linear framebuffer.

Every reset value below is the model's choice (*approximation*) unless
the notes say otherwise. Widths are bits. The configuration block below
``0x60`` takes an aligned 64-bit access or an aligned 32-bit access to
either half. The FIFO and port rows take the widths their row gives;
every other register takes aligned 32-bit accesses only.

.. list-table::
   :header-rows: 1

   * - Offset
     - Name
     - Width
     - Reset
     - Access
     - Notes
   * - ``0x00``
     - Widget identity
     - 32 or 64
     - see notes
     - read-only
     - :ref:`sgi-odyssey-identity`.
   * - ``0x08``, ``0x10``, ``0x18``
     - Status, error address upper and lower
     - 32 or 64
     - ``0``
     - read-only
     - No crosstalk error is ever posted.
   * - ``0x20``
     - Widget control
     - 32 or 64
     - widget number
     - read/write
     - Bits 3:0 reset to the board's own widget number; stored as 32
       bits with no effect.
   * - ``0x28``
     - Request timeout
     - 32 or 64
     - ``0x000fffff``
     - read/write
     - Stored with no effect.
   * - ``0x30``, ``0x38``
     - Interrupt destination upper and lower
     - 32 or 64
     - ``0``
     - read/write
     - Flag interrupt target (:ref:`sgi-odyssey-interrupts`).
   * - ``0x40``
     - Error command
     - 32 or 64
     - ``0``
     - read/write
     - A write acknowledges and reads back zero.
   * - ``0x1004``
     - Reset control
     - 32
     - none
     - write-only
     - Each write logs ``LOG_UNIMP`` and changes nothing.
   * - ``0x1014``
     - Interrupt flags
     - 32
     - ``0x200``
     - read
     - A write logs ``LOG_UNIMP`` and changes nothing.
   * - ``0x101c``
     - Interrupt flag clear
     - 32
     - none
     - write-one-to-clear
     -
   * - ``0x1034``, ``0x103c``
     - Interrupt enable set and clear
     - 32
     - ``0``
     - write-one-to-set, write-one-to-clear
     - ``0x1034`` reads the whole mask.
   * - ``0x1044``, ``0x104c``, ``0x1054``
     - Routed destination upper, lower, vectors
     - 32
     - ``0``
     - write-only
     - Softsync, retrace and DMA-done targets.
   * - ``0x1064``
     - Command FIFO status
     - 32
     - ``0x10030000``
     - read-only
     - Constant: enabled, empty, below low water.
   * - ``0x106c``
     - Memory configuration
     - 32
     - per product
     - read-only
     - :ref:`sgi-odyssey-memory-config`.
   * - ``0x1070``
     - Flow-control configuration
     - 32
     - ``0x0001c000``
     - read/write
     - Stored with no effect.
   * - ``0x1078``, ``0x1088``, ``0x1098``, ``0x10a8``, ``0x10b8``,
       ``0x10c0``
     - BUZZ scratch registers
     - 32
     - ``0``
     - read/write
     - Plain storage.
   * - ``0x107c``
     - Software sync token
     - 32
     - ``0``
     - read/write
     - Plain storage.
   * - ``0x10b4``, ``0x10bc``, ``0x10c4``, ``0x10cc``, ``0x10d4``
     - Swap control
     - 32
     - ``0``
     - read/write
     - :ref:`sgi-odyssey-swaps`.
   * - ``0x10d8``, ``0x10dc``
     - One-wire control
     - 32
     - ``0``
     - read/write
     - Two aliases (:ref:`sgi-odyssey-onewire`).
   * - ``0x4004``, ``0x400c``
     - Context shadow address upper, lower
     - 32
     - ``0``
     - write-only
     - :ref:`sgi-odyssey-context`.
   * - ``0x4034``, ``0x403c``
     - Host-write base upper, lower
     - 32
     - ``0``
     - write-only
     - :ref:`sgi-odyssey-fence`.
   * - ``0x100000``
     - User command FIFO
     - 32 or 64
     - none
     - write-only
     - :ref:`sgi-odyssey-commands`.
   * - ``0x110000``
     - Kernel command FIFO
     - 32 or 64
     - none
     - write-only
     - :ref:`sgi-odyssey-commands`.
   * - ``0x118000``
     - Context switch port
     - 64
     - none
     - write-only
     - :ref:`sgi-odyssey-context`.
   * - ``0x201594``
     - Command FIFO self-test
     - 32
     - ``0``
     - read-only
     - :ref:`sgi-odyssey-selftest`.
   * - ``0x233a00``, ``0x233a08``, ``0x233c00``, ``0x233c08``
     - I2C command and status, option and main ports
     - 32
     - ``0``
     - read-only
     - :ref:`sgi-odyssey-backend`.
   * - ``0x23380c``
     - Back-end version
     - 32
     - ``1``
     - read-only
     - :ref:`sgi-odyssey-backend`.
   * - ``0x400000``
     - Back-end write port
     - 32 or 64
     - none
     - write-only
     - :ref:`sgi-odyssey-backend`.

Only four of these are public. OpenBSD reads command FIFO status at
``0x1064`` and treats bits ``0x00008000`` and ``0x00020000`` as
threshold states, reads ``0x106c`` as a data-engine status whose low
seven bits it waits to see zero, and writes command words to
``0x110000`` and data to ``0x400000`` (``odysseyreg.h`` lines 23-31), a
*software expectation*. Every other offset, name and role in the table
is the model's (*approximation*), except where a section below ties it
to shipped software. The six BUZZ scratch registers are plain storage;
their real function is *unknown*. The request-timeout and flow-control
reset values are *approximations*.

.. _sgi-odyssey-memory-config:

Memory configuration
--------------------

The IRIX 6.5.30 kernel reads ``0x106c`` bits 31:30 as the memory size
(0 for 32 MB, 1 for 64 MB, 2 for 128 MB), bit 29 set as four banks and
clear as two, and bit 28 set as CAS latency 2 and clear as 3, a
*software expectation*. The model composes the word from the
properties and also sets bit 24, a constant whose meaning is *unknown*
(*approximation*). The low bits read zero, which OpenBSD reads as an
idle data engine. The V12 word is ``0xa1000000``.

.. _sgi-odyssey-interrupts:

Interrupts
----------

XIO interrupts are packets, not wires. The flag source sends one
interrupt-modify write each time ``flags AND enable`` changes between
zero and nonzero: set on the rising edge, clear on the falling edge.
Its target is ``0x30``/``0x38``: widget in bits 19:16 and vector in bits
31:24 of the upper word, and a 48-bit address from bits 15:0 of the
upper word and all of the lower. This decode is borrowed from the IMPACT
board's destination register, an *approximation*.

The model keeps the flags at ``0x1014``, clears them by writing ones
to ``0x101c``, and sets and clears the enable mask through ``0x1034``
and ``0x103c``. The routed sources share a second target at
``0x1044``/``0x104c``, widget in bits 28:25 of the upper word, with one
vector byte per source in ``0x1054``: softsync in bits 31:24, retrace in
bits 23:16, DMA-done in bits 15:8 and the flag source in bits 7:0,
which nothing raises. A routed source sends a single set packet with no
matching clear. All of these roles and layouts are the model's
(*approximation*). A source raised with no target programmed logs
``LOG_GUEST_ERROR`` and sends nothing.

The only flag the model raises is bit 9 (``0x200``), which it treats as
command FIFO low water. It is set at reset and after every command FIFO
store, because the model's FIFO drains at once. Raising it per store is
an *approximation*; the real FIFO's level crossings are *unknown*.

.. _sgi-odyssey-onewire:

Board identity part
-------------------

``0x10d8`` and ``0x10dc`` both drive one MicroLAN slot per write, with
the layout HEART, Bridge and Hub use: pulse width in bits 19:10, sample
offset in bits 9:2, completion in bit 1 and the sampled level in bit 0.
The slot completes inside the write. On the wire sits an emulated
DS2505 with family code ``0x0b`` and the ``nic-serial`` property as its
48-bit serial, a synthetic value by default. Its 160-byte image starts
with byte ``0x01``, four spaces and ``;``, then the field text and a
closing ``;``. Spaces pad it to one 32-byte page and the rest of the
image is ``0xff``. ``nic-record`` supplies the field text; without it
the product's ``Name:<board>`` is used, and without a product
``Name:ODY128VERSIONB``. The record layout and the two aliases are
*approximations*; the serial is synthetic.

.. _sgi-odyssey-backend:

Back end
--------

The display back end is an indexed register file reached through a
32-bit word stream at ``0x400000``. An 8-byte store is two words, upper
half first; a 4-byte store is one. A header word has tag ``0b11`` in
bits 29:28, a starting index in bits 27:14 and a count in bits 13:0.
The next ``count`` words go to consecutive indices; a zero count is a
pad. A word without the tag where a header is due is dropped. This
framing is the model's (*approximation*); OpenBSD writes this port with
64-bit stores in its setup stream, a *software expectation*.

The model decodes these indices:

- ``0x0000``-``0x1fff``: the colour lookup RAM, one 30-bit entry each,
  red in bits 29:20, green in 19:10, blue in 9:0
  (:ref:`sgi-odyssey-scanout`).
- ``0x2740`` and ``0x2780``: the option and main I2C command words.
  Their read-back is at ``0x233a00`` and ``0x233c00``, and status at
  ``0x233a08`` and ``0x233c08``. Status reads bit 22 (ready) always and
  bit 9 (done) when the last command was nonzero. No device sits on
  either bus, so every transfer acknowledges and returns zero
  (*approximation*). A guest polling for completion sees it at once;
  this says nothing about the board's PLL, DDC EEPROM or monitor.
- ``0x2802`` and ``0x2803``: the display channel's width and height
  (:ref:`sgi-odyssey-channel`).
- ``0x2900``-``0x291f``: display modes for main drawable ids 0 to 31.
- ``0x2920``-``0x2927``: display modes for overlay ids 0 to 7.

The first undecoded index or untagged header word logs ``LOG_UNIMP``;
all later ones, whatever their index, are silent.

``0x23380c`` returns the ``backend-version`` property in bits 2:0 and
zero elsewhere. gfxinfo prints those bits as the PB&J version, a
*software expectation*, and every capture in the identity table reads 1.
The IRIX 6.5.30 kernel's back-end initialisation also returns early
when bit 10 is set, a *software expectation* from the shipped kernel. The model answers 0 there; the bit's meaning is *unknown*, and a
read of the whole word on a real board would settle it.

.. _sgi-odyssey-selftest:

Command FIFO self-test
----------------------

``0x201594`` reads zero until the kernel command FIFO has taken one
fixed nine-store sequence (four 4-byte and five 8-byte stores, 14 words)
in order; it then reads ``0x003d0046``. A break in the sequence restarts
the match. This is a recognised-sequence answer, not a computed one
(*approximation*): what the board computes there, and what it would
return for any other stream, is *unknown*.

.. _sgi-odyssey-commands:

Command path
============

Two ports take command words: the kernel FIFO at ``0x110000`` and the
user FIFO at ``0x100000``. The model gives both the same grammar; only
the kernel FIFO feeds the self-test match. A 4-byte store is one
word and an 8-byte store is two, upper half first, an *approximation*
by analogy with the back-end port. Other widths log ``LOG_UNIMP`` and
are dropped. The FIFO is sequenced, not addressed: every store is the
next word of the stream, and nothing reads it back. The model executes
each word synchronously inside the store, so command status at
``0x1064`` always reads empty.

Every command word carries its operand count in bits 13:10, 15 meaning
16. The model reads that field instead of keeping a per-command table,
*inferred* from traced X server and kernel streams on the model, where
it accounts for every word. A word with a nonzero top byte and bit 23
clear takes no operands whatever its count field says, *inferred* from
the same traces. A word the model does not decode logs ``LOG_UNIMP``
once, and its operands are skipped by that count, so the parser stays
on command positions. A recognised register word whose effect the model
does not track is consumed silently.

The model decodes the console subset that OpenBSD's ``odyssey.c``
sends (solid fill, screen-to-screen copy and monochrome glyph
expansion), the X server's two-dimensional primitives, pattern quads
and image transfers, the drawable-id and overlay plane writes, and the
OpenGL immediate-mode stream the IRIX library sends. The command
encodings are *software expectations* read from those drivers and
traces; no published table defines them. How the board executes them
is outside this page.

.. _sgi-odyssey-context:

Context switch
--------------

The model takes an 8-byte store to ``0x118000`` with opcode 7 in bits
47:40 and bit 0 set as a context switch; it ignores the context id in
bits 31:16. It saves its open-primitive and current-attribute state
into the shadow page whose guest physical address is in
``0x4004``/``0x400c`` (bits 15:0 of the upper word and all of the
lower), then writes 1 to the 32-bit word at page offset ``0x8fc``. The
port, the word layout, the address pair and the completion word are the
model's (*approximation*). The page's ring and descriptor layout is
the one the IRIX 6.5.30 kernel reads back when it rebuilds an open
primitive, a *software expectation* from the shipped kernel; that the
board writes the page is *inferred*. A store with another opcode or bit
0 clear logs ``LOG_UNIMP``. A switch with no shadow address programmed
logs ``LOG_GUEST_ERROR`` and writes nothing.

.. _sgi-odyssey-fence:

Completion fence
----------------

The IRIX 6.5.30 GL library sends command ``0x0a5a`` as a host-write
fence, a *software expectation* from the shipped library. The model
reads its two operands as a fence slot in bits 14:0 with bit 15
requesting an interrupt, and a 16-bit token. It writes the token
big-endian to guest memory at the host-write base
(``0x4034``/``0x403c``) plus twice the slot, then sends the softsync
interrupt if asked. The operand layout, the base pair and the
interrupt are the model's (*approximation*). The write uses the
machine's physical address space directly, not an XIO DMA path
(*approximation*).

.. _sgi-odyssey-dma:

DMA
===

The model has a page-table engine for large image and texture
transfers. Command word ``0xe01`` and three operand words start it; the
model unpacks a translation-table physical address from operands 0 and
1, taking bits 16:0 and 29:23 of each as one 24-bit half. The table is a
32-byte header of eight big-endian words followed by 8-byte entries,
each a byte address in bits 63:16 and a length in bits 15:0, at 16 KiB
page granularity, ended by a zero-length entry. A table with no
terminator within 256 entries logs ``LOG_GUEST_ERROR`` and fails the
transfer.

The model reads the header as the board buffer (word 0, in 16-byte
units, ``0x2000`` for the screen), the rectangle origin and size (words
1 and 3), the source row skip and row length (words 5 and 6), a pixel
size code (word 7 bits 18:16), the DMA-done interrupt request (word 7
bit 31) and the direction (word 7 bit 24 set for board to host). The
descriptor packing, the table and header layouts and the direction bit
are the model's (*approximation*). Word 7 bit 30 set puts the bottom
row first; the IRIX 6.5.30 GL library sets it when source and
destination descriptions differ, a *software expectation* from the
shipped library. A completed transfer that asked for it sends the
DMA-done interrupt. Command word ``0xe03`` logs ``LOG_UNIMP`` and does
nothing.

Transfers that name a buffer other than the screen land in the model's
board-memory object store (:ref:`sgi-odyssey-memory`). The IRIX 6.5.30
kernel pages objects between board memory and host memory when a small
board's memory fills, with headers that give 2, 4 or 8 bytes a texel, a
*software expectation* from the shipped kernel. The model performs the
2- and 4-byte forms in a texel layout of its own choosing and logs the
8-byte form as ``LOG_UNIMP`` (*approximation*). Both directions use the machine's physical address
space directly, not an XIO DMA path (*approximation*).

.. _sgi-odyssey-memory:

Framebuffer and planes
======================

Every VPro board keeps commands, framebuffers and textures in one
SDRAM pool with no separate texture memory. The IRIX 6.5.30 kernel
manages that pool and pages objects out to host memory when it fills, a
*software expectation* from the shipped kernel. The board's address map, framebuffer base, stride, tiling and
pixel packing are *unknown*. The model therefore keeps its own planes,
each 1280 by 1024, none reachable through the aperture:

- two RGBA8 colour buffers, 0 and 1, red in the high byte;
- an 8-bit drawable-id plane, bits 7:3 the main id and bits 2:0 the
  overlay id;
- an 8-bit overlay pixel plane;
- a 24-bit depth plane in 32-bit words, reset to all ones;
- an 8-bit stencil plane;
- an object store for off-screen buffers and texture levels.

The object store holds at most 1024 objects in 128 MiB of 32-bit
texels, whatever the product. When an object needs more room than is
free, the model forgets the least recently used other objects until it
fits, then packs the store. A new object beyond the 1024 slots also
forgets the least recently used one. The first object forgotten each
way is logged under ``LOG_UNIMP``. A board keeps a level until something overwrites
it, so a forgotten texture that a client still samples draws
untextured here (*approximation*). A destination base beyond the
product's memory size logs ``LOG_GUEST_ERROR`` once per reset.

In the model, register ``0x0424`` in the command stream selects which
colour buffers a write reaches: bits 1:0 buffer 0 and bits 3:2 buffer 1
(*approximation*). Stereo is not modelled: a right-eye bit draws to the
same buffer as its left-eye bit. Out of reset the mask draws buffer 0
(*approximation*), so firmware and console drawing is visible.

.. _sgi-odyssey-display:

Display
=======

.. _sgi-odyssey-scanout:

Colour resolution
-----------------

Scanout reads each pixel's main drawable id, picks colour buffer 0 or 1
by that id's bit in the displayed-buffer register ``0x10d4``, and
resolves the stored word through the id's display mode. Mode bits 8:4
name a colormap group. Groups 26 and up are RGB: each 8-bit channel is
widened to 12 bits, mapped into the 1,536-entry gamma table at
``0x1a00`` and shown as the top eight bits of that entry. Below 26 the
word is an index into ``256 * group + (word & 0xff)``, or the 4,096
entries from 0 for group 0. A pixel whose overlay id's mode has bit 0
set and whose overlay byte is nonzero shows overlay colormap entry
``256 * (mode bits 7:3) + byte`` instead.

The id byte's layout, main id in bits 7:3 and overlay id in bits 2:0,
is how the IRIX 6.5.30 X server writes it, a *software expectation*
from the shipped server; it also allocates main id 0 like any other.
The overlay mode fields come from one value a guest wrote on the model
and are *inferred*. The rest of this resolution rule, including that it
happens per pixel at scanout, the group numbering, the 12-bit widening
and the gamma ranges, is the model's (*approximation*). Group 25
resolves as an index table and logs ``LOG_UNIMP`` once as unverified.

.. _sgi-odyssey-channel:

Display channel size
--------------------

The guest writes the active width and height to back-end indices
``0x2802`` and ``0x2803``: 1280 by 1024 from the PROM and kernel, and
1280 by 720 after the X server loads a 72 Hz 1280x720 format, *observed*
on the model with the ``v6`` product. That these indices set the
displayed size is *inferred* from those writes and the format gfxinfo
then reports. The model shows the top-left of its planes at the latest
nonzero size; a zero leaves the picture alone, and a size beyond 1280 by
1024 is cut to it with a ``LOG_UNIMP`` report. The timing table the
guest loads at indices ``0x2000``-``0x247f`` is not decoded, so the
frame rate stays 60 Hz at every size (*approximation*): 1688 by 1066
total pixels at 107,964,476 Hz.

.. _sgi-odyssey-swaps:

Retrace and buffer swaps
------------------------

``0x10d4`` holds one bit per window: which colour buffer it shows.
Writing ``0x10b4`` arms flips; writes accumulate. At each retrace the
model XORs the armed mask into ``0x10d4``, clears it and repaints, so a
frame is never shown half from each buffer. Writes to ``0x10c4`` set
displayed bits at once and writes to ``0x10cc`` clear them, the
kernel's immediate path. ``0x10bc`` is stored with no effect. A direct
write to ``0x10d4`` is stored.

Once the routed target is programmed, every retrace sends the retrace
interrupt, armed or not. These register roles, the displayed-buffer
encoding and the latch point are the model's (*approximation*); whether
the board can mask the retrace source is *unknown*.

OpenBSD's console uses 1280 by 1024 at 32 bits per pixel and draws its
cursor in software, a *software expectation*. The model has no hardware
cursor, no DDC or EDID and no second channel.

.. _sgi-odyssey-mesa:

Mesa renderer
=============

``x-mesa-renderer=on`` sends OpenGL primitives in RGBA modes to a host
Mesa renderer that draws into the two colour buffers. Each primitive is
drawn wholly by Mesa or, when the adapter refuses it, wholly by the
model's own software rasterizer, before the command returns. Colour-index
drawing always takes the software path. If the host has no usable
renderer, realize warns once and every draw takes the software path;
any other renderer creation failure fails realize. The option changes
host drawing only: guest-visible registers, planes and interrupts are
the same either way. It is off by default, the ``x-`` prefix marks it
experimental, and both ends of a migration must set it the same way.

The read-only properties ``x-mesa-unavailable``, ``x-mesa-draws`` and
``x-mesa-fallbacks-<reason>`` report whether the host renderer was
missing, how many draws Mesa took and how many fell back, by reason.
They are host diagnostics and are not migrated.

.. _sgi-odyssey-reset:

Reset
=====

No VPro reset value is published or measured. Reset is one domain: the
model returns every register in the map to its listed value, sets
interrupt flag ``0x200``, clears the enable mask, the lookup RAM, every
display mode, both colour buffers, the id, overlay, stencil and object
planes, fills the depth plane with all ones, restarts the command and
back-end parsers, puts the draw mask back to buffer 0 and the display
channel back to 1280 by 1024. The widget control register's low nibble
reloads the board's widget number. Properties, the identity, the memory
word and the one-wire image are configuration and survive reset. With
the Mesa renderer on, reset recreates it; a failure stops the machine.

Writes to ``0x1004`` log ``LOG_UNIMP`` and change nothing
(*approximation*). The board's warm, link and board-local resets are
*unknown*.

.. _sgi-odyssey-migration:

Migration
=========

The device migrates its registers, lookup RAM, display modes, overlay
display modes, parser and context state, both colour buffers, the
depth, stencil, id and overlay pixel planes, the used part of the object store, buffer selection, the
scanout's mode and retrace phase, and the GL state it keeps in its own
registers. The presented frame is not migrated; the destination repaints
from the planes. Derived state, including the object index and the
shared GL context's viewport, scissor and clip planes, is rebuilt after
load from the migrated words. The Mesa renderer is not migrated and is
recreated on load. Product, memory and one-wire properties must match
on both sides. Migration works only between identical builds.

.. _sgi-odyssey-measurements:

Measurements
============

The reference Octane2 reports a V12 board at widget 11 in its hardware
inventory (:ref:`sgi-measured-octane2-hinv`). No register of that board
was read for this page, so no value in the register map is a
measurement.

.. _sgi-odyssey-limits:

Model limits
============

- One fixed 1280 by 1024 plane set and a 60 Hz retrace at every display
  size. No hardware cursor, DDC, EDID, second channel or video timing.
- No uploaded program runs. Program uploads are stored; the model
  recognises the known geometry programs and does their work itself.
- The framebuffer, depth, stencil and object planes are model storage,
  not board memory at board addresses. Nothing in the aperture maps them.
- The object store is the same 128 MiB for every product and forgets
  objects when full, where a board would keep them.
- No crosstalk error, timeout or bus error is ever reported. The status
  and error registers read zero.
- Command status is constant and the low-water flag is raised after
  every store; FIFO occupancy, back-pressure and execution time are not
  modelled.
- The self-test register answers only one exact store sequence.
- Interrupt delivery reuses the IMPACT destination decode, and routed
  sources send set packets only.
- The identity record names the Octane board on every machine.
- Stereo and the right-eye bits are folded onto the left eye.
- Back-end indices outside the lookup RAM, display modes, channel size
  and I2C commands are not decoded, and the back-end read window at
  ``0x220000`` answers only the I2C words and ``0x23380c``.
- Accesses to an offset or width the map does not list log
  ``LOG_UNIMP``, read zero and drop writes without a bus error. Each
  offset and direction reports once, up to 64 distinct reports;
  undecoded command words have their own budget of 64. Reads of the
  write-only address registers and ``0x1004``, and writes to the
  identity, status, error address, memory configuration, I2C read-back
  and back-end version registers, log ``LOG_GUEST_ERROR``. Reads of
  ``0x101c`` and ``0x103c`` and writes to ``0x1064`` and ``0x201594``
  log ``LOG_UNIMP``, as does any write to ``0x1014``. Every access is traced with ``sgi_odyssey_mmio``,
  including the ones whose report is suppressed.

.. _sgi-odyssey-tests:

Tests
=====

``tests/qtest/sn1-odyssey-test.c`` holds the register contract, run on a
Fuel with the device's own defaults and split into groups by
``tests/qtest/sn1-odyssey-test-groups.txt``. The ``board`` group covers
identity, the configuration block's two spellings, widget control,
reset values, the interrupt registers and packet composition, the I2C
ports, command status, unmodelled offsets, scanout, colour resolution,
the channel size, swaps and retrace, buffer migration, texture DMA and
object-store migration. The ``profile`` group covers the four products'
identity, memory word, board name, back-end version and the one-wire
part. The ``fifo`` group covers command framing and unknown words, the
self-test sequence, the low-water interrupt, the console primitives,
image transfers, the fence and page-table DMA. The ``context`` group
covers the context switch and shadow page. The ``gl`` group and the
``texture``, ``texture-1d-clamp``, ``texture-1d-stored``,
``texture-1d-wide``, ``texture-2d-clamp``, ``texture-2d-stored`` and
``texture-3d`` groups cover the OpenGL command stream. ``tests/qtest/sn1-odyssey-query-test.c``
covers the current-attribute query records.

``tests/qtest/octane-vpro-test.c`` checks each product's identity and
memory size, and the default board's interrupt path to HEART, at
Crossbow widget 11 on the ``octane`` and ``octane2`` machines. ``tests/qtest/sn1-fuel-vpro-test.c``
does the same at XBridge widget 13 on the Fuel, and that a headless Fuel
has no board. ``tests/qtest/sgi-graphics-pipes-test.c`` checks the
catalogue's VPro offers and refusals and the widget 12 board on the
IP53 profiles. ``tests/qtest/sgi-migration-roundtrip-test.c`` migrates
the stored registers on a Fuel. ``tests/qtest/sn1-pi-gfx-test.c``
covers the counted-store credit return.

``tests/unit/test-sgi-odyssey-native-mesa.c`` covers the Mesa adapter's
binding, refusals and fallback, and
``tests/unit/test-sgi-odyssey-planes.c`` the plane views it draws
through.
