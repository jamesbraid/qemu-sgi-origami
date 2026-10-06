.. SPDX-License-Identifier: BSD-3-Clause

SGI PROM image inputs
=====================

One decoder, ``hw/mips/sgi/prom-image.c``, reads every PROM image the SGI
family machines take from the host: ``-bios`` and the I/O PROM below. It accepts a raw image or SGI's own
single-segment distribution container, uncompressed or gzip, and returns
both the file as supplied and its decoded payload. It does not choose a
machine, a guest address or a stored representation; each caller does.
Firmware bytes must be supplied by the user. The unit test
``tests/unit/test-sgi-prom-image.c`` builds synthetic containers only.

The accepted container subset was measured from the processor and I/O
PROMs distributed with IRIX 6.5 and 6.5.30. It describes those encodings,
not a general SGI container specification. A file without the container
signature is a raw image and is used as it is. A file with the signature
that does not match the subset below is refused rather than copied as a
raw image.

Container metadata
------------------

All numeric fields are big-endian. The container has a 4096-byte header
followed by one stored segment. The eight-byte signature at offset 0x40 is
0x4a464b535743534d. The decoder reads:

* Offset 0x48: eight-byte version 6.
* Offset 0x50: eight-byte complete file size.
* Offset 0x7c: four-byte segment count 1.
* Offset 0x90: eight-byte flags, 0x10 (stored) or 0x13 (gzip).
* Offset 0x98: eight-byte payload offset 4096.
* Offset 0xb0: eight-byte decoded payload length.
* Offset 0xb8: eight-byte compressed length, zero for 0x10.
* Offset 0xc0: eight-byte sum of the decoded payload's unsigned bytes.
* Offset 0xc8: eight-byte sum of the compressed payload's unsigned bytes,
  zero for 0x10.
* Offset 0xd0: eight-byte storage extent.

The revision at offset 0x3c, the segment name at 0x80 and the entry and
load addresses at 0xa0 and 0xa8 are metadata. They never decide whether
an image is accepted or where its bytes go. Individual flag bits and other
segment layouts have not been established; other versions, counts and
flag values are refused.

Payload validation
------------------

For flags 0x10 the payload follows the header and ends at the declared
file size. Its decoded length and storage extent must match, both
compressed fields must be zero and the decoded byte sum must match.

For flags 0x13 the declared compressed range holds exactly one gzip member
(RFC 1952). The decoder checks its CRC32 and ISIZE, consumes the whole
range, and requires the decoded length and both byte sums to match the
header. Concatenated members, truncated streams and wrong lengths are
refused. The observed I/O PROMs declare a storage extent larger than their
decoded payload.

Up to seven zero bytes may follow the compressed range. The observed
images have zero and six. This is a compatibility bound, not an SGI
padding rule; a longer or nonzero tail is refused.

Every caller passes the capacity the payload is copied into. A decoded
payload larger than that is refused, and a file too large to decode to
that capacity in any accepted encoding is refused before it is read.

Machine placement
-----------------

The Origin 200 and Origin 2000 copy the decoded payload over their boot
flash template. The Origin 300 requires a decoded payload of exactly its
PROM size. Container addresses never choose a guest destination.

I/O PROM flash
--------------

The Origin 2000 BaseIO card and the Origin 200 GIGAchannel cabinet carry
the I/O PROM in a 1 MiB CFI flash part, which the IP27 PROM's loader reads
and decompresses itself. The part therefore holds the original file, not
the decoded payload: container header, compressed bytes and all, followed
by erased bytes to the end of the part.

The family machine option ``io-prom=PATH`` gives every such part in the
topology that image; the boards' own ``firmware`` property does the same
for one part. The decoder validates the image first, and the whole file
must fit the part. A topology with no such part refuses ``io-prom``.

A part's ``-drive`` takes precedence. Its contents are used as they are,
and the image is not opened, so a missing or malformed image does not stop
a machine whose flash already has its own contents. With neither a drive
nor an image the part reads as before: absent on the GIGAchannel, blank on
the BaseIO card.

.. _sgi-prom-image-ratings:

Firmware carrier and configuration blocks
-----------------------------------------

The rest of this page states what the model layers cite for the
processor PROM's carrier, payload and configuration block. Each fact is
rated *established* (a cited public source), *inferred* (follows by the
stated step), *approximation* (an emulator choice standing in for
unknown hardware) or *unknown*. Several facts rest on measuring the
stock PROM images the user supplies; those say so, because the images
themselves are not public.

.. _sgi-prom-image-carrier:

Carrier and payload
~~~~~~~~~~~~~~~~~~~

SGI ships the processor PROMs as container files in the IRIX media
(``ip35prom.img`` for the IP35 generation), and IRIX's ``flash``
command writes them to the boot flash; its manual page describes
updating CPU and I/O PROMs and, by default, preserving the processor
speed and cache settings already stored (established, IRIX 6.5.30
``flash(1M)``, https://help.graphica.com.au/irix-6.5.30/man/1M/flash).
The *payload* is what follows the 4096-byte container header described
above; it is the image the processor executes.

For the IRIX 6.5.30 IP35 PROM the container holds one stored segment
whose load and entry addresses are both ``0xc00000001fc00000``, which
translates to the node-local physical address ``0x1fc00000``; the
payload is 1,476,264 bytes (``0x1686a8``) (measured from the stock
image).

.. _sgi-prom-image-trampolines:

Entry trampolines
~~~~~~~~~~~~~~~~~

The IP35 payload begins with ten entry trampolines, one every eight
bytes from ``0x1fc00000`` to ``0x1fc00048`` (measured from the stock
image). Software outside the PROM calls fixed entries in this range:
OpenBSD's IP27 port starts secondary processors through the PROM entry
at ``0x1fc00038`` (established for IP27, OpenBSD/sgi
``sys/arch/sgi/sgi/ip27.h``, ``IP27_PROM_LAUNCH_SLAVE``, at
``2df91d80c104636deeef8124252ddd5951b55fbf``). The model maps the
payload unchanged, so whatever the image holds at these offsets is what
runs; it neither generates nor patches trampolines.

.. _sgi-prom-image-ip35-views:

Origin 300 boot views (approximation)
~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~

Without a bound boot flash, ``origin300`` maps one read-only copy of
the supplied payload at four machine-bus addresses for node 0, each
exactly the payload's length:

.. list-table::
   :header-rows: 1

   * - View
     - Address
   * - CPU reset view
     - ``0x000_001fc00000``
   * - Local boot window base
     - ``0x100_18000000``
   * - Bounded high local view
     - ``0x100_1fc00000``
   * - Node 0 remote boot window base
     - ``0x100_38000000``

The second PROM selection, at ``0x100_1a000000``, is a separate erased
8 MiB view.

This is an emulator compatibility carrier. What it rests on:

- The reset view is where a MIPS processor fetches after reset
  (established, the MIPS reset vector at virtual ``0xbfc00000``, which
  is physical ``0x1fc00000``).
- The two window bases are the Bedrock's local and node-addressed boot
  windows as QEMU's Bedrock model decodes them (inferred; see
  :doc:`sgi-sn`).
- A real IP35 PROM trace copies ``0x1686a8`` bytes from the local boot
  window base into memory, which shows the image starts at that base
  and is read linearly for its full length (observed on hardware; the
  public identity of that trace is not established here, so this is
  not yet a citable source).
- The stock PROM later executes through a mapping whose physical base
  is ``0x1fc00000`` and keeps the low address bits, so it fetches
  payload offsets such as ``0x998`` and ``0x247c`` at the high local
  view (measured from the stock image).

Unknown: how an IP45 board's reset request selects flash rather than
memory, the fitted flash part, the full chip-select range and what
answers outside the image. The carrier maps only the payload's own
length at each view and repeats nothing, so it claims nothing about
those addresses. A public IP45 decoder description or a measurement on
an identified board would settle it. With a bound ``node0-flash`` the
Bedrock model's two-PROM boot flash replaces this carrier
(:doc:`sgi-sn`, "Firmware supply").

.. _sgi-prom-image-config-block:

Configuration block
~~~~~~~~~~~~~~~~~~~

Both PROM families keep a configuration block 0x60 bytes into the boot
flash, tagged with the magic ``ip27conf`` (``0x69703237636f6e66``). Its
layout is established from two public headers: OpenBSD/sgi
``sys/arch/sgi/sgi/ip27.h`` (``struct ip27_config``, used to tell
Origin 200 from Origin 2000) and the SGI-authored SN1 header in Linux
2.5.0, ``include/asm-ia64/sn/sn1/ip27config.h``, which shows the IP35
generation uses the same block. All fields are big-endian:

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

The declared fields take 76 bytes; the model treats the block as 0x50
bytes.

.. _sgi-prom-image-checksum:

Checksum adjustment (inferred)
^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^

When the machine changes fields in a block, it rewrites the checksum
adjustment so that the unsigned byte sum of the whole 0x50-byte block,
modulo 256, is what it was before the change. That the PROM checks this
sum is inferred, not established: the Linux header names the field
only as "used after config hdr overlay", and the rule comes from model
runs, where a Fuel block with machine type 2 failed the PROM's checksum
with the adjustment byte left at Origin 300's value and passed with the
byte recomputed this way. No public source states the checksum
algorithm.

.. _sgi-prom-image-ip27-values:

IP27 product values
^^^^^^^^^^^^^^^^^^^

The machine patches three fields into every node's block, from the
topology's ``ip27-firmware`` record in ``query-sgi-machines``:

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

Machine type codes are established from a published ``flash``
dialogue on an Origin 200 that offers ``(0) SN0 (1) Sn00``
(comp.sys.sgi.hardware, 2003-08-07). Origin 2000's 0, 8 and 1 are the
stock IP27 image's own values (measured from the stock image), so the
machine leaves a stock block unchanged there. Origin 200's 15 and 4
flash cycles have no public source (unknown); they are the emulator's
long-standing values.

.. _sgi-prom-image-ip35-values:

IP35 product values
^^^^^^^^^^^^^^^^^^^

For the Bedrock machines the node writes the processor mode word, the
low word of the processor clock, the machine type and a flash program
count of 1, then recomputes the checksum adjustment.

- Machine type codes: a real Fuel's ``flash -f`` prompts ``Enter machine
  type (0)SN1 (1)SN10 (2)SN11 (3)SN12`` and its owner enters 2
  (established, graphica, "SGI O350 Chimera notes", retained copy lines
  721-731). Fuel's 2 is therefore established. Origin 300's 1 is
  inferred: it is the value every Origin 300 run has used, and the
  stock PROM's CPU-configuration table supplies it with the Origin 300
  processor rows (measured from the stock image); no public Origin 300
  dialogue shows it.
- Processor mode word and clock: taken from the stock PROM's own
  CPU-configuration table row for the fitted processor (measured from
  the stock image). The published ``flash`` dialogue above lists the
  Fuel speeds; no public source gives the mode word for any IP35
  processor (unknown).
