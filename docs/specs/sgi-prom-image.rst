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
