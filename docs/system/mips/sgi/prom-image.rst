.. SPDX-License-Identifier: BSD-3-Clause

.. _sgi-prom-image-ratings:

======================================
SGI PROM images and node boot firmware
======================================

Every SGI machine in this family boots a stock SGI PROM: the IP27 PROM on
the Origin 200, Origin 2000 and Onyx2, the IP35 PROM on the Origin 300,
Fuel and the IP53 profiles (Origin 350, Onyx 350, Tezro), and the IP30
PROM on the Octane and Octane2. This page covers what QEMU does with those
images: the file formats it accepts, the flash parts it puts them in, the
configuration blocks it writes, the firmware data the PROM builds in
memory, and how the PROM finds PCI devices. The decoder is
``hw/mips/sgi/prom-image.c``. Initial flash contents come from
``hw/mips/sgi/initial-flash.c``, which the host tool
:doc:`/tools/qemu-sgi-machine-init` shares. The early per-processor PROM
window is ``hw/mips/sgi/sn-promdata.c``. Firmware bytes are never part of
QEMU. The user supplies them.

Facts are rated as described in :ref:`sgi-ratings`. Facts read from a
stock image are cited to that image's registry entry. The images are
public but carry no redistribution grant. What the stock PROM did on this
model is a *software expectation* observed on the model. It shows what the
PROM asks for, not what the hardware does.

Sources
=======

- The stock images :src:`ip27-prom-6.156`, :src:`ip35-prom-6.210` and
  :src:`ip30-prom-4.17`, and the I/O PROM :src:`io6-prom-6.156`
  distributed beside the IP27 PROM on the same media. The container layout, payload sizes,
  configuration blocks and entry trampolines below were read from them.
- SGI's *Origin200/2000 IP27 PROM Technical Reference Manual*,
  :src:`108-0170-001 p. 2-3`, for the IP27 flash's sector use.
- The IRIX 6.5.30 ``flash(1M)`` manual page, :src:`graphica-irix-flash-1m`,
  for how SGI updates processor and I/O PROMs.
- A published Fuel ``flash -f``
  session, :src:`graphica-o350-chimera-notes flash -f session`, for
  machine-type codes.
- The *Origin200 Owner's Guide*, :src:`007-3415-003 p. 12-13`, for the
  PCI option slot numbers.
- The Sharp *LH28F800SU 8M Flash Memory* datasheet,
  :src:`sharp-lh28f800su p. 9`, for the identifier words the Octane flash
  reports.
- OpenBSD ``sys/arch/sgi/sgi/ip27.h`` at ``2df91d80c104`` (``struct
  ip27_config``, ``IP27_CONFIG_OFFSET``, ``IP27_PROM_LAUNCH_SLAVE``), and
  the SGI-authored SN1 header ``include/asm-ia64/sn/sn1/ip27config.h`` in
  Linux 2.5.0, as software expectations for the configuration block.
- Linux ``arch/mips/include/asm/sn/klconfig.h``, ``sn/kldir.h`` and
  ``sn/sn0/kldir.h`` at ``db2ddb871435``, and OpenBSD
  ``sys/arch/sgi/include/mnode.h`` at ``2df91d80c104``, as software
  expectations for the low-memory structures the PROM builds.

Stock images
============

QEMU is developed against the PROMs shipped on the IRIX 6.5.30 media. The
registry identifies each by version, banner and payload SHA-256.

.. list-table::
   :header-rows: 1

   * - Registry id
     - Boards
     - Distribution file
     - Payload
   * - :src:`ip27-prom-6.156`
     - IP27 node boards: Origin 200, Origin 2000, Onyx2
     - ``ip27prom.img``
     - at most 1 MiB, copied over the node flash
   * - :src:`ip35-prom-6.210`
     - IP35-family node boards: Origin 300, Fuel, IP53 profiles
     - ``ip35prom.img``, 1,480,360 bytes
     - exactly 1,476,264 bytes (``0x1686a8``)
   * - :src:`ip30-prom-4.17`
     - IP30 system board: Octane, Octane2
     - the IP30 PROM image
     - at most 1 MiB, at offset 0 of the boot flash

The IP27 PROM loads a second image, the I/O PROM, from a flash part on the
BaseIO card or the GIGAchannel cabinet (`I/O PROM flash`_).

.. _sgi-prom-image-carrier:

Distribution container
======================

SGI ships processor PROMs as container files, and IRIX's ``flash`` command
writes them to the boot flash. Its manual page describes updating CPU and
I/O PROMs and, by default, keeping the processor speed and cache settings
already stored (:src:`graphica-irix-flash-1m`), *established*. The
*payload* is what follows the container's 4096-byte header. It is the
image the processor executes.

The IP35 container holds one stored segment. Its load and entry addresses
are both ``0xc00000001fc00000``, which is node-local physical
``0x1fc00000``, and its payload is 1,476,264 bytes
(:src:`ip35-prom-6.210 container header`), *established* for that image.
The container is exactly 4096 bytes longer than the payload.

The decoder accepts a raw image or this container, uncompressed or gzip,
and returns both the file as supplied and the decoded payload. It does not
choose a machine, a guest address or a stored form. Each caller does. The
accepted subset was measured from the processor and I/O PROMs distributed
with IRIX 6.5 and 6.5.30. It describes those encodings and is not a
general specification of SGI's format. The field meanings below are
*inferred* from those files.

A file without the container signature is a raw image and is used as it
is. A file with the signature that does not match the subset below is
refused, never copied as a raw image.

Container header
----------------

All numeric fields are big-endian. The eight-byte signature at offset
``0x40`` is ``0x4a464b535743534d``.

.. list-table::
   :header-rows: 1

   * - Offset
     - Width
     - Field
     - Accepted values
   * - ``0x48``
     - 8
     - version
     - 6
   * - ``0x50``
     - 8
     - complete file size
     - the file's length
   * - ``0x7c``
     - 4
     - segment count
     - 1
   * - ``0x90``
     - 8
     - flags
     - ``0x10`` stored, ``0x13`` gzip
   * - ``0x98``
     - 8
     - payload offset
     - 4096
   * - ``0xb0``
     - 8
     - decoded payload length
     - nonzero, at most the caller's capacity
   * - ``0xb8``
     - 8
     - compressed length
     - zero when stored
   * - ``0xc0``
     - 8
     - sum of the decoded payload's unsigned bytes
     - must match
   * - ``0xc8``
     - 8
     - sum of the compressed payload's unsigned bytes
     - zero when stored, else must match
   * - ``0xd0``
     - 8
     - storage extent
     - equal to the decoded length when stored

The revision at ``0x3c``, the segment name at ``0x80`` and the entry and
load addresses at ``0xa0`` and ``0xa8`` are metadata. They never decide
whether an image is accepted or where its bytes go. Individual flag bits
and other segment layouts are *unknown*. Other versions, counts and flag
values are refused.

Payload validation
------------------

A stored payload follows the header and ends at the declared file size.

A gzip payload's declared compressed range holds exactly one gzip member
(RFC 1952). zlib checks its CRC32 and ISIZE. The decoder requires the
member to consume the whole range and to decode to exactly the declared
length. Concatenated members, truncated streams and wrong lengths are
refused. The I/O PROMs measured declare a storage extent larger than
their decoded payload. The decoder does not check the extent of a gzip
segment.

Up to seven zero bytes may follow the compressed range. The images
measured have zero and six. This bound is a compatibility choice, not an
SGI padding rule. A longer or nonzero tail is refused.

Every caller passes the capacity the payload is copied into. A larger
decoded payload is refused. A file too large to decode to that capacity in
any accepted encoding is refused before it is read. The bound is the
header, zlib's worst-case expansion, the gzip framing and the seven-byte
tail.

Node boot flash
===============

Each machine puts the decoded payload in a flash part the processor
fetches from at reset. The fitted part is not identified for any of these
boards. The model's parts are QEMU flash devices with compatibility
identifiers, each an *approximation*.

.. list-table::
   :header-rows: 1

   * - Machine
     - Device
     - Size and geometry
     - Identifiers
     - Store
   * - ``origin200``, ``origin2000``
     - ``cfi.pflash02``, one per node
     - 1 MiB, 8 bits wide, sixteen 64 KiB sectors
     - ``0x01``/``0xd5``, unlock ``0x5555``/``0x2aaa``
     - ``node<N>-flash``, 1 MiB
   * - ``origin300``, ``fuel``, IP53 profiles
     - ``sgi-bedrock-flash``, two 8 MiB positions
     - 16 MiB, 64 KiB sectors
     - see :doc:`hardware/bedrock`
     - ``node<N>-flash``, 16 MiB
   * - ``octane``, ``octane2``
     - ``cfi.pflash01``
     - 1 MiB, 16 bits wide, 64 KiB sectors
     - ``0x00b0``/``0x66a8``
     - ``node0-flash``, 1 MiB

Every machine applies one binding rule (``machine-storage.c``):

- ``-bios`` fills each boot flash that has no store bound.
- With every boot flash bound, ``-bios`` is refused. A launcher passing a
  newer image to a machine created earlier is told so instead of booting
  the stored PROM.
- Without ``-bios``, every boot flash must be bound. The Hub machines
  waive this under qtest.
- Each store must be exactly the size ``query-sgi-machines`` lists for the
  configuration. A raw file must have that exact length, not a length the
  block layer rounds up to a sector.
- ``-drive if=pflash,unit=N`` still binds node ``N`` on the SN machines.
  Naming both forms for one node is refused.

The Octane refuses ``-kernel``. The PROM boots the operating system.

IP27 node flash
---------------

The IP27 PROM keeps firmware in the first fourteen 64 KiB sectors of the
node flash and the PROM environment and message log in the last two
(:src:`108-0170-001 p. 2-3`), *established*. The model's identifiers are
AMD's for the Am29F080, a model choice. They do not identify the fitted
part, which is *unknown*.

The flash answers at the reset view, node memory offset ``0x1fc00000``,
and through the Hub's local and node-addressed boot windows at Hub special
offsets ``0x10000000`` and ``0x30000000`` (:doc:`hardware/hub`). Writes
through the reset view reach the chip's byte decode. Commands through the
boot windows go through the Hub's flash controller.

A node with no store bound starts from a template. The ``-bios`` payload
is copied over it, then the configuration values are patched in
(`IP27 product values`_). The template is:

- ten entry slots at an eight-byte stride, slot 0 a branch to itself and
  the rest returns;
- a configuration block at ``0x60`` with timing constant ``0xb``, a Hub
  clock of 90 MHz, a real-time clock of 1.25 MHz, error correction on,
  flash program count 0, PROM version 6 and revision 11;
- an environment log in sector 14: ``0x504c4f47`` at flash offset
  ``0xe0010``, format 1 at ``0xe0014`` and sequence 1 at ``0xe0018``,
  with an erased first entry;
- every other byte erased.

The template's contents are an *approximation*. The sector holding the
log is *established*. No public source identifies the log's label,
version or sequence. A payload shorter than the flash leaves the
template's bytes behind it, so a short image still finds a formatted
block and log. A stock payload keeps its own bytes in those places.

.. _sgi-prom-image-ip35-views:

IP35 boot flash and views
-------------------------

The Bedrock has two PROM selections and a local and a node-addressed boot
space. Its flash register interface is on :doc:`hardware/bedrock`. A node
gets its PROM in one of two ways.

**Bound store.** With ``node<N>-flash``, or a 16 MiB raw pflash drive,
the node gets an ``sgi-bedrock-flash`` part. The lower 8 MiB of the store
back position zero and the upper 8 MiB position one. The local and
node-addressed boot spaces, 128 MiB each, are that part's decode, so the
PROM can program and erase it. The reset view and the high local view are
read-only views of position zero. They show the stored bytes, including
anything just programmed. A write to them is refused with
``LOG_GUEST_ERROR``.

**-bios only.** The node maps one read-only copy of the payload, which
must decode to exactly 1,476,264 bytes. Programming does not persist. A
write to the second selection is dropped with ``LOG_UNIMP``.

.. list-table::
   :header-rows: 1

   * - View
     - Address, node 0
     - Size
   * - CPU reset view
     - ``0x000_1fc00000``
     - payload length
   * - Local boot space base
     - ``0x100_18000000``
     - payload length with -bios, 128 MiB bound
   * - High local view
     - ``0x100_1fc00000``
     - payload length
   * - Node-addressed boot space base
     - ``0x100_38000000``
     - payload length with -bios, 128 MiB bound
   * - Second selection, local and node-addressed
     - ``0x100_1a000000``, ``0x100_3a000000``
     - 8 MiB reading erased, -bios only

These views are an emulator compatibility carrier, an *approximation*.
What they rest on:

- A MIPS processor fetches from virtual ``0xbfc00000``, physical
  ``0x1fc00000``, after reset, *established* for the architecture.
- The two boot space bases are the Bedrock's local and node-addressed boot
  spaces as the Bedrock model decodes them (:doc:`hardware/bedrock`),
  *inferred*.
- The stock IP35 PROM later executes through a mapping whose physical
  base is ``0x1fc00000`` and keeps the low address bits, so it fetches
  payload offsets such as ``0x998`` and ``0x247c`` at the high local view,
  a *software expectation* observed on the model.

The reset view routes each access by its cache attribute. An uncached
compatibility-segment access or a cacheable noncoherent (CCA 3) access
reaches the PROM. Coherent and explicitly uncached accesses reach node RAM
at the same offset. The stock PROM still executes noncoherently and
through the uncached segment after it enables RAM, and this choice serves
that while keeping RAM visible to everything else, an *approximation*.

*Unknown*: how an IP45 board's reset request selects flash rather than
memory, the fitted flash part, the full chip-select range and what answers
outside the payload. A public IP45 decoder description or a measurement
on an identified board would settle them.

IP30 boot flash
---------------

The Octane's flash part, its capacity and its Bridge select are *unknown*
(:ref:`sgi-ip30-flash-wiring`). The model fits QEMU's Intel command-set
part with the identifier words of the Sharp LH28F800SU, ``0x00b0`` and
``0x66a8`` (:src:`sharp-lh28f800su p. 9`), as a candidate part, an
*approximation*. It does not model the Sharp chip's extended status,
block locking or timing. The Bridge's flash port reaches it
(:ref:`sgi-bridge-flash`).

``-bios`` puts the payload at offset 0 and erases every other byte. The
IP30 flash carries no configuration block the machine formats. The
reference Octane2's ``flash`` utility reports a primary and a recovery
PROM header (`Measurements`_), so a stock part holds two images. The
model holds only what is supplied.

I/O PROM flash
--------------

The Origin 2000 BaseIO card and the Origin 200 GIGAchannel cabinet carry
the I/O PROM in a 1 MiB AMD command-set part on their Bridge
(:ref:`sgi-baseio-flash`, :ref:`sgi-gigachannel-flash`). The IP27 PROM's
loader reads and decompresses it itself, a *software expectation*. The
part therefore holds the original file, container header and compressed
bytes included, followed by erased bytes.

- The machine option ``io-prom=PATH`` gives every such part in the
  topology that image. A board's ``firmware`` property does the same for
  one part. The decoder validates the image, and the whole file must fit
  in 1 MiB. A topology with no such part refuses ``io-prom``.
- A store (``io<N>-flash``) or the board's ``flash-drive`` takes
  precedence. Its contents are used as they are and the image is not
  opened.
- With neither, the GIGAchannel part is absent and the BaseIO part reads
  zeros, not the ``0xff`` of an erased chip, an *approximation*.

.. _sgi-prom-image-trampolines:

Entry trampolines
=================

The IP35 payload begins with a table of entry trampolines at an
eight-byte stride from ``0x1fc00000``, ending before the configuration
block at offset ``0x60`` (:src:`ip35-prom-6.210 0x1fc00000`),
*established* for that image. This page gives no slot count. Software outside the PROM calls fixed
entries in this range. OpenBSD's IP27 port starts secondary processors
through the entry at ``0x1fc00038`` (``IP27_PROM_LAUNCH_SLAVE`` in
``ip27.h``), a *software expectation*. The model maps the payload
unchanged, so whatever the image holds at these offsets is what runs. It
neither generates nor patches trampolines in a supplied image. The IP27
template's ten slots exist only so a node with no image has a defined
reset vector.

.. _sgi-prom-image-config-block:

Configuration block
===================

Both PROM families keep a configuration block ``0x60`` bytes into the
boot flash, tagged with the magic ``ip27conf`` (``0x69703237636f6e66``).
OpenBSD ``ip27.h`` reads it to tell the Origin 200 from the Origin 2000,
and the SGI-authored SN1 header in Linux 2.5.0 shows the IP35 generation
uses the same block, a *software expectation*. All fields are big-endian:

.. list-table::
   :header-rows: 1

   * - Offset
     - Size
     - Field
   * - ``0x00``
     - 4
     - time constant
   * - ``0x04``
     - 4
     - processor mode word (the R10000-family SysAD reset mode bits)
   * - ``0x08``
     - 8
     - magic ``ip27conf``
   * - ``0x10``
     - 8
     - processor clock in Hz
   * - ``0x18``
     - 8
     - Hub or Bedrock clock in Hz
   * - ``0x20``
     - 8
     - real-time clock in Hz
   * - ``0x28``
     - 4
     - ECC enable
   * - ``0x2c``
     - 4
     - flash read cycles
   * - ``0x30``
     - 4
     - machine type
   * - ``0x34``
     - 4
     - checksum adjustment (``check_sum_adj`` in the Linux header)
   * - ``0x38``
     - 4
     - flash program count
   * - ``0x3c``
     - 4
     - flash write cycles
   * - ``0x40``
     - 12
     - PROM version, PROM revision, configuration-specific word

The declared fields take 76 bytes. The model treats the block as ``0x50``
bytes.

.. _sgi-prom-image-checksum:

Checksum adjustment
-------------------

When the machine changes fields in a block, it rewrites the checksum
adjustment so that the unsigned byte sum of the whole ``0x50``-byte block,
modulo 256, is unchanged. That the PROM checks this sum is *inferred*,
not established. The Linux header names the field only as "used after
config hdr overlay". The stock PROM rejects a Fuel block whose
adjustment byte does not restore the byte sum and accepts one that
does, a *software expectation*. No public source
states the algorithm. A trace of the PROM's checksum routine would settle
it.

.. _sgi-prom-image-ip27-values:

IP27 product values
-------------------

The machine patches five fields into every unbound node's block, and
``qemu-sgi-machine-init`` into every new node store: the fitted processor
module's mode word (``0x04``) and clock (``0x10``), from the processor
``processor=`` selects (:ref:`sgi-sn-origin200-processors`), and three
product values from the topology's ``ip27-firmware`` record in
``query-sgi-machines``. Every other field and the block's byte sum are
kept. A flash without the ``ip27conf`` magic at offset ``0x68`` is left
alone.

.. list-table::
   :header-rows: 1

   * - Product
     - Machine type
     - Flash read cycles
     - Flash write cycles
   * - Origin 200
     - 1
     - 15
     - 4
   * - Origin 2000, Onyx2
     - 0
     - 8
     - 1

The Origin 2000 values 0, 8 and 1
are the stock IP27 image's own (:src:`ip27-prom-6.156 configuration
block`), so a stock block is left unchanged there. The Origin 200 values
15 and 4 have no public source, *unknown*. The stock block is built for a
180 MHz processor with 1 MiB of cache (mode word ``0x1211a785``), so an
Origin 2000 or a default Origin 200 keeps its processor fields too. Any
other module gets the values the PROM's own reconfiguration dialogue
writes for it, which asks for the processor clock, cache size and mode
bits, as a field upgrade of the module requires, a *software
expectation*.

.. _sgi-prom-image-ip35-values:

IP35 product values
-------------------

A Bedrock node writes four fields from its catalogue record, then
recomputes the checksum adjustment:

.. list-table::
   :header-rows: 1

   * - Offset
     - Field
     - Value
   * - ``0x04``
     - processor mode word
     - the topology's ``ip35-firmware`` mode word, ``0x549aa985`` for the
       500 MHz R14000 with 2 MiB of secondary cache on Origin 300 and Fuel
   * - ``0x14``
     - low word of the processor clock
     - the fitted processor's clock
   * - ``0x30``
     - machine type
     - 1 on Origin 300, 2 on Fuel, 0 on the IP53 profiles
   * - ``0x38``
     - flash program count
     - 1, a model choice
   * - ``0x34``
     - checksum adjustment
     - keeps the block's byte sum

The machine applies them to the ``-bios`` payload, and to a bound store's
loaded copy at every start. It does not rewrite the store file. It skips
a bound store on an IP53 profile, whose block is authored with the store.
``qemu-sgi-machine-init`` writes the same fields into a new store.

- Machine type 2 for Fuel: a published Fuel ``flash -f`` session prompts
  ``Enter machine type (0)SN1 (1)SN10 (2)SN11 (3)SN12`` and the user
  enters 2 (:src:`graphica-o350-chimera-notes flash -f session`),
  *observed*. The stock PROM's CPU-configuration table carries 2 on its
  Fuel rows and 1 on its Origin 300 rows (:src:`ip35-prom-6.210
  CPU-configuration table`), a *software expectation*. Origin 300's 1 is
  *inferred* from that table. No public Origin 300 dialogue shows it.
- The stock PROM compares the block with the table row the board selects
  and prints ``CPU configuration values mismatch`` when they differ, a
  *software expectation*.
- Mode words come from that table's row for the fitted processor. No
  public source gives the mode word for any IP35 processor, *unknown*.
- The IP53 profiles' type 0 follows the stock PROM's classification of
  their modelled board word, a *software expectation*. It is not a reading
  of a real Origin 350 or Tezro flash.

IP35 position-one label
-----------------------

The stock IP35 PROM keeps its environment in position one. It accepts the
position only if a 28-byte label at position offset ``0x1e0000`` or
``0x1f0000`` carries ``0x504c4f49`` at record offset ``0x10``, and it picks
the newer copy by the generation word at ``0x18``. Its blank-media
formatter writes the first label with sixteen ``0xff`` bytes,
``0x504c4f49``, format 1 and generation 1, and leaves the second erased
(:src:`ip35-prom-6.210 position-one formatter`), a *software expectation*.
The update container carries no bytes at either offset.

QEMU never builds an unbound writable IP35 flash.
``qemu-sgi-machine-init`` builds a new 16 MiB store from the payload in
position zero and that label at store offset ``0x9e0000``, with every
other byte erased. The label is storage metadata reproduced from the
PROM's formatter, not a reading of a real board's flash.

Machine state files
===================

:doc:`/tools/qemu-sgi-machine-init` writes one raw file per store a
machine selection lists. It uses the code the machine uses for an unbound
part, so a machine started on the files reads the same flash as one
started with ``-bios``. It loads an IP27 or IP30 payload up to the store
size, and an IP35 payload up to half the store, which must equal the
node's PROM size. An I/O PROM's whole file must fit its part. Every node
of a machine gets the same boot PROM.

Firmware data in memory
=======================

The model synthesizes no firmware data structures. The stock IP27 and
IP35 PROMs build them in each node's low memory from what they probe, and
the operating system reads them there. The model's part is to present the
hardware the PROM probes.

KLCONFIG and the low-memory directory
-------------------------------------

On IP27 nodes the PROM leaves a directory of sixteen 64-byte entries at
node offset ``0x2000``, each valid entry signed ``0x434d5f53505f5357``.
The configuration graph (KLCONFIG) is at ``0x4000``, ``0xc000`` bytes,
headed by the magic ``0xbeedbabe``. Launch blocks are at ``0x2400`` and
the global data area at ``0x11000``. These are the offsets in Linux
``sn/kldir.h``, ``sn/sn0/kldir.h`` and ``sn/klconfig.h``, a *software
expectation*. The graph's board records carry the type codes in
``klconfig.h``, such as ``KLTYPE_IP27`` and ``KLTYPE_BASEIO``. Their
content comes from what the model answers to the PROM's discovery: board
identity registers, one-wire serial parts, widget identity and PCI
configuration space.

Node addressing
---------------

Out of reset every Hub and Bedrock answers as node 0, so each node's PROM
reaches its own memory with node field zero until it is numbered
(:doc:`hardware/hub`, :doc:`hardware/bedrock`). The PROM then assigns
node numbers and reaches its own copies through node-qualified
addresses. The model keeps a node's memory at its fixed placement and
changes only how the node answers.

PROM early window
-----------------

Before memory is sized, the stock IP35 PROM keeps its stack and
per-processor state in a 32 KiB window at node physical ``0x100000``,
held in the processor's cache. It marks that phase in bit 1 of
floating-point register 0, and it runs from flash and from a RAM copy
whose wired mapping points at ``0x1a00000`` (:src:`ip35-prom-6.210 early
phase`), a *software expectation* read from the image. Both Origin 300
processors use the same address, so a model that served the window from
shared RAM would let their stacks overwrite each other.

``sn-promdata.c`` gives every processor its own 32 KiB block at that
window on both node generations, an *approximation* of cache-resident
state:

- A processor arms its private phase when bit 1 is set and its program
  counter, translated through its MMU, lies in the flash or the RAM-copy
  range at ``0x1a00000``, 1 MiB on IP27 and 2 MiB on IP35.
- While armed, its window accesses go to its own block, and the RAM-copy
  range in its node-zero form resolves to its own node's memory. An
  address naming another node's number reaches that node. On IP35 the
  classification also counts an address naming the node's own programmed
  number as local.
- The phase ends when bit 1 is clear and the program counter has left
  both ranges. A poll on each processor every 10 ms of guest time applies
  that test, so release lags the hand-off by up to one period.
- On IP27 nodes (``promdata-fast``, on by default) the private block is
  mapped as RAM for cached accesses while the stack is in it. IP35 nodes
  default it off.

The model applies the same rule to the IP27 PROM. That the IP27 PROM uses
the same window, bit and RAM-copy address is an *approximation* checked
only by the model's boots.

PCI discovery
=============

Firmware and the operating system find PCI devices by reading Bridge
configuration space. On the model, the IP27 PROM reads a vendor ID at
every device number, and the IRIX 6.5.30 kernel probes again at its own
Bridge attach inside a bus-error guard. Both are *software expectations*
observed on the model. An empty device number fails the read back to the
processor as a bus error and returns all ones to a caller that sees only
the value (:ref:`sgi-bridge-config`).

The Origin 200 system board's Bridge carries base I/O at device numbers 0
and 1 (SCSI) and 2 (IOC3). Its three option slots are PCI IDs 5, 6 and 7
(:src:`007-3415-003 p. 12-13`), *established*. Published inventories show
the SCSI adapters at 0 and 1, the IOC3 at 2 and option cards at 5, 6 and
7. That IRIX's slot numbers are Bridge device numbers is *inferred*. The
model places devices to match and offers ``pci-5`` to ``pci-7`` as option
slots.

- Device 3 is not an option slot. The IRIX 6.5.30 kernel issues no
  configuration read to it at its attach, while it still probes empty
  neighbours, a *software expectation* observed on the model. A card
  there is invisible to IRIX.
- Device 4 is not an option slot. What, if anything, it connects to is
  *unknown*. IRIX would attach a card that answered there.

Measurements
============

.. list-table::
   :header-rows: 1

   * - Read
     - Value
     - Machine
     - When
   * - ``/usr/sbin/flash -V``, primary header
     - version ``4.17``, length 682,448 bytes, 7 reloads
     - :ref:`sgi-measured-octane2`
     - under IRIX
   * - ``/usr/sbin/flash -V``, recovery header
     - version ``4.9``, length 147,198 bytes
     - :ref:`sgi-measured-octane2`
     - under IRIX

The reference Octane2 runs the same PROM version as :src:`ip30-prom-4.17`
and holds a separate recovery image, *established* for that board. The
reads do not identify the flash part, its size or the images' offsets.
The flash contents were not dumped or hashed
(:ref:`sgi-measured-octane2-prom`). No IP27 or IP35 node flash was read on
a physical machine for this page.

Model limits
============

- No boot flash models an identified part. Geometry and identifiers are
  compatibility choices, and command timing is QEMU's.
- The IP27 template, the IP35 position-one label, the IP35 flash program
  count of 1 and the Origin 200 flash cycle counts are model choices.
- The checksum rule is inferred from model runs.
- With ``-bios`` only, an IP35 node's flash is read-only. Writes to the
  second selection log ``LOG_UNIMP`` and are dropped. With a bound store,
  writes to the reset or high local view log ``LOG_GUEST_ERROR`` and fail.
- The IP35 reset view's attribute routing and the boot views are a
  compatibility carrier, not a decode of the IP45 board.
- The Octane flash holds only the supplied image, with no recovery image.
- A blank BaseIO I/O PROM part reads zeros.
- The PROM early window, its arming test and its release lag are model
  choices. A processor that never arms keeps the window routed to its own
  node.
- The container decoder accepts only the measured subset.
- Migration works only between identical builds.

Tests
=====

- ``tests/unit/test-sgi-prom-image.c``: stored and gzip containers, raw
  images, malformed and truncated headers, the capacity limit and file
  loading, with synthetic containers only.
- ``tests/unit/test-sgi-initial-flash.c``: the IP27 template and payload
  overlay, a flash without the magic, the IP30, IP35 and I/O PROM layouts
  and the IP35 configuration writer.
- ``tests/unit/test-sgi-machine-init.c`` and
  ``tests/qtest/sgi-machine-init-test.c``: the tool's options, its store
  contents per machine, the IP35 payload size, containers, refusals and
  cleanup, and that a machine started on the files reads the same boot
  flash as one started with ``-bios``.
- ``tests/qtest/sgi-prom-container.h``: an independent container writer
  the machine tests use to prove ``-bios`` takes the container.
- ``tests/qtest/sn0-address-map-test.c``: the three IP27 flash views,
  autoselect, program and erase, and the configuration patch of a model
  and a container payload.
- ``tests/qtest/sgi-machine-storage-test.c``: store binding, sizes,
  partial binding with and without ``-bios``, persistence and the legacy
  drive forms.
- ``tests/qtest/sn0-gigachannel-test.c``: the I/O PROM flash,
  ``io-prom``, raw images, refusals and drive precedence.
- ``tests/qtest/sn1-address-map-test.c``: the exact IP35 payload size, a
  container payload, the configuration block, the bound flash backing the
  views, a non-raw store and migration.
- ``tests/qtest/octane-test.c``: the IP30 flash, execution from it, its
  straps, backend errors and migration of a bound store.
- ``tests/qtest/sn-promdata-window-test.c`` and
  ``tests/qtest/sn1-cross-node-test.c``: the early window's placement,
  its RAM-copy routing, migration and release at hand-off.
