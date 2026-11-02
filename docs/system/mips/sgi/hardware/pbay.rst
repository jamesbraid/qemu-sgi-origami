.. SPDX-License-Identifier: BSD-3-Clause

.. _sgi-pbay:

====================================
Power-bay controller: L1 power link
====================================

Used in no machine. The L1 (:doc:`l1`) of an Origin 3000 rack polls the
rack's power bay over a serial link, and ``sgi-pbay`` is the bay's end of
that link. No machine instantiates it. The controller model's ``pbay``
property (:ref:`sgi-l1-pbay`) takes the other end of the link.

.. seealso::

   - L1 (:doc:`l1`): the system controller whose power-bay task is the
     other end of the link.

The power bay supplies the rack's bricks. A controller in the bay answers
the L1 over one serial line in framed request and reply transactions. This
page describes that link as the model serves it: the framing, the commands
the bay's application answers and the L1's initialisation sequence. It does
not describe the bay's boards, which the model does not have. Facts are
rated as described in :ref:`sgi-ratings`.

.. _sgi-pbay-sources:

Sources
=======

- The bay's controller image as shipped in the IRIX 6.5.14 to 6.5.30
  distributions and its patch sets, and the L1 package for the Origin 300
  that drives the link, read as binaries. Statements taken from them are
  *software expectation* unless they carry another rating. No manual
  describes the wire protocol. The L1 software guide documents only the
  operator's ``pbay`` command.
- The stuffing is the one in the public IPMI specification's serial
  framing, *inferred* from the match.

Link
====

.. list-table::
   :header-rows: 1

   * - Item
     - Value
     - Rating
   * - Medium
     - one UART on the L1, 19200 baud
     - *software expectation*
   * - Bay address
     - ``0x4400``, sent little-endian as ``00 44``
     - *software expectation*
   * - Stuffing
     - ``0xa0``, ``0xa5`` and ``0xaa`` travel as ``0xaa 0xb0``, ``0xaa
       0xb5`` and ``0xaa 0xba``; the frame ends with ``0xa5``
     - *software expectation*
   * - Start byte ``0xa0``
     - the L1's UART handler sends it and ignores every byte before one
       it receives, so a reply must start with it; the bay's own start
       byte is written by the flash loader, which is not in the
       application image
     - *software expectation* for the L1; *unknown* for the loader
   * - Little-endian 16-bit fields
     - destination, source and checksum
     - *software expectation*

The bytes on the character device are the framed, stuffed bytes: the
L1's UART handler adds and strips the start byte, the escapes and the
terminator, and the L1's transaction code above it works on unframed
bodies, *software expectation*. The model sends a start byte before each
reply because the L1's handler needs one.

The frame body, before stuffing, is the destination (2 bytes), the source
(2), a zero, a control byte (bits 0 to 4 a tag, bit 7 set in a reply and
the tag copied), the command, and at offset 7 a request's data or a
reply's completion code, with a reply's data from offset 8. The checksum is
``0xffff - (sum of the preceding bytes + their count + 1)`` in 16 bits,
sent low byte first, over the unstuffed bytes; both ends use it, *software
expectation*. A reply is addressed to the request's source and comes from
the request's destination, except the ``0x02`` reply, which always comes
from ``0x4400``, *software expectation*. The bay's reply to a ``0xc0``
request from an L1 at ``0x0102`` is ``02 01 00 44 00 84 c0 00 12 00 60 a0
d4 39 80 c5 fb`` before stuffing, and the ``0x02`` reply to the same L1 is
``02 01 00 44 00 85 02 00 00 44 e2 fe``.

The bay ignores a request whose destination is not ``0x4400``, except
commands ``0x02`` and ``0xc0``, *software expectation*.

Receive rules
-------------

The receive rules of the bay are in its flash loader, not the image, so
each is *unknown*. The model drops a frame whose checksum is wrong or that
is shorter than a header and checksum, and logs it with ``LOG_GUEST_ERROR``;
the same follows for the rest, each an *approximation*: a bare ``0xa0``
abandons the frame in progress and begins a new one; an escape followed by a
byte other than ``0xb0``, ``0xb5`` or ``0xba`` drops the frame up to its
terminator; a frame that ends inside an escape is dropped; a body longer
than 256 bytes is dropped up to its terminator (the application cannot hold
more, *inferred*). The loader may answer a bad frame with completion code
``0xff`` instead; the model does not.

Commands
========

.. list-table::
   :header-rows: 1

   * - Command
     - Reply data, after the completion code ``0x00``
     - Rating
   * - ``0x02``
     - ``00 44``, the bay's address, whatever the destination
     - *software expectation*
   * - ``0x0a``
     - ``01 08 02``; the meaning is *inferred*
     - *software expectation*
   * - ``0x0b``
     - one byte, the bit number 1 to 8 of a one-hot internal state code,
       or 0; the model's ``state-code`` property holds the code and it
       defaults to none
     - *software expectation*; the code's source is *unknown*
   * - ``0x10``, ``0xa2``
     - no reply
     - *software expectation*
   * - ``0x11``
     - sixteen zero bytes
     - *software expectation*
   * - ``0x12``
     - none: a header and completion code only. The L1 sends the bay
       address as data, *inferred* to be an address assignment; the model
       keeps its own address
     - *software expectation*
   * - ``0x13``
     - ``22``, *inferred* to be a device type
     - *software expectation*
   * - ``0xc0``
     - ``12 00 60 a0 d4 39 80``: version ``0x0012`` (00.18), a build time
       word and the flash chunk size. The model's ``flash-chunk`` property
       sets the last byte; its default is ``0x80`` and the L1 caps a chunk
       at ``0x8c``, so the property accepts 1 to ``0x8c``
     - *software expectation*
   * - ``0xc1``, ``0xc2``
     - flash data and end: the application's table does not hold them and
       would answer completion code ``0xc1``, so the loader must answer
       them first, *inferred*. The model logs ``LOG_UNIMP`` and replies
       ``0xc1``
     - *unknown*
   * - ``0x20``
     - a tunnelled request, below
     - *software expectation*
   * - any other
     - a header-only reply with completion code ``0xc1``
     - *software expectation*

A status of ``0xff`` left by the loader changes the replies of a command in
the table: every one except ``0xa2`` answers a header-only completion code
``0xff`` (``0x10`` included, which is otherwise silent; ``0xa2`` stays
silent), and a command not in the table still answers ``0xc1``,
*software expectation*. The loader's rule for setting it is *unknown*, so
the model's ``frame-status`` property holds it, default 0; only ``0xff``
changes a reply.

Tunnelled requests
------------------

A ``0x20`` request carries an IPMI-shaped inner message from offset 7: the
target byte (``0x22``, the byte ``0x13`` returned), the network function
shifted left two with the logical unit in bits 0 and 1, the command number,
then the data from offset 10, *software expectation*. The reply carries, in
order from offset 7: the link completion code ``0x00``, a copy of the
request's offset 7 byte, the network function byte plus 4, the command number,
the inner completion code ``0x00`` and then the pair's data,
*software expectation*. The L1 requires both completion codes zero, the
command equal, the response bit set and the network function equal to the
request's; it does not read the echo or the addresses, *software
expectation*. The model reads a request shorter than a pair needs as zeros,
an *approximation* of the bay reading stale bytes. A tunnelled request with
fewer than the three bytes that name the target, function and command is
logged with ``LOG_GUEST_ERROR`` and not answered, also an *approximation* of
the bay answering from whatever its buffer held. A network function other
than ``0x00``, ``0x10``, ``0x18`` and ``0x28``, or a command that is not in
that function's list, gets a header-only reply with completion code
``0xc1``, *software expectation*. With ``frame-status`` at ``0xff`` a
request of a listed network function instead gets link completion code
``0xff``, the echo, the network function plus 4, the command and inner
completion code ``0xff`` with no data, whatever the command, and any other
network function gets no reply, *software expectation*.

.. list-table::
   :header-rows: 1

   * - Pair
     - Data after the inner completion code
     - Rating
   * - ``0x00,0x00``
     - ``00 22 22 00 00``
     - *software expectation*
   * - ``0x10,0x2d``
     - the sensor number is the first data byte; 1 to 14 give ``00 00
       <mask> 00``, see below; 15 gives the input snapshot and ``00``; 0 and
       16 and above give completion code ``0xc1``
     - *software expectation*
   * - ``0x18,0x03``
     - none; role *inferred* to be reset or DC-port control. The model logs
       ``LOG_UNIMP`` for the output sequence
     - *software expectation*
   * - ``0x10,0x23``
     - none: a header-only reply with completion code ``0xc1``
     - *software expectation*
   * - ``0x18,0x04``
     - ``56 00``
     - *software expectation*
   * - ``0x28,0x10``
     - ``10 00 10 00`` whatever the request carries
     - *software expectation*
   * - ``0x28,0x11``
     - a streamed read, see below
     - *software expectation*
   * - ``0x28,0x20``
     - ``01 0c`` and twelve zeros
     - *software expectation*
   * - ``0x28,0x23``
     - a record read, see below
     - *software expectation*
   * - any other pair
     - a header-only reply with completion code ``0xc1``
     - *software expectation*

Sensor reads
~~~~~~~~~~~~

Each sensor input is read through the bay's input port, whose snapshot the
model takes from its ``input-snapshot`` property (default 0) for every
input. Sensors 1 to 6 give four inputs masked by ``1 << (n - 1)`` in
mask bits 0 to 3 and one masked by ``0x04`` in bit 4; sensors 7 to 14 give two
inputs masked by ``1 << (n - 7)`` in bits 0 and 1, *software expectation*.
The bay reads different banks for the inputs of a sensor, so real inputs may
differ where the model's follow one snapshot, an *approximation*; what the pins
carry is *unknown*. The L1 takes mask bit 0 of a supply sensor as not
inserted and bits 1 to 3 as fail, predictive fail and AC fail, and bit 0 of
a DC-port sensor as not present and bit 1 as fail, *software expectation*.
Sensor 15 replies on a later sample; the model replies at once and logs
``LOG_UNIMP``, an *approximation*, and the L1 never asks for it.

Record reads
~~~~~~~~~~~~

The request data is a reservation (2 bytes), a record id (2), an offset and
a count; the bay uses only the record id's low byte (offset 12) as the key
and the count (offset 15), *software expectation*. The reply data is the
next record id (2 bytes, little-endian) and then the record's bytes, the
record's own id first. Keys 0 and 1 give record 1, keys 2 and 3 give records 2
and 3, keys 4 to 9 give records 4 to 9, keys 10 to 12 give records 10 to 12,
and the next id is the record's number plus one, except key 12, which gives
``0xffff``. Keys 13 and above give completion code ``0xc1``. A count of
``0xff`` returns the table: 32 bytes for keys 0 to 2, 16 for keys 3 and 4 to
11, 48 for key 12. Any other count from 1 to the table size returns that
many bytes; otherwise the body is empty, except that keys 4 to 9 return
their key byte, *software expectation*. Record 12's length byte says 49
bytes where the table holds 48; the model returns the table, an
*approximation*. The records read as sensor data records, *inferred*: eight
sensors from number 7, six from number 1, a controller locator and device
locators with addresses ``0xa0`` to ``0xae``, and a full sensor.

Streamed read
~~~~~~~~~~~~~

Pair ``0x28,0x11`` reads a device: the first data byte is the device
(``0xa0``, ``0xa2`` up to ``0xae``; any other gives completion code
``0xc1``), the next two are written to the bay's latch, and the byte at
offset 13 is a count, 0 meaning 256, *software expectation*. The reply
carries the count and then that many samples. What the samples are is
*unknown*, *inferred* to be a FRU EEPROM per device; the model returns zeros
and logs ``LOG_UNIMP``. The latch's bit meanings are *unknown*.

The L1 initialises the link with ``0x10`` and ``0x02`` to address 0, stores
the replied address, then ``0x12`` and ``0x13`` to it, the ``0x00,0x00``
pair, whose data bytes the L1 keeps and uses to decide whether to count
supplies and read records, and the ``0x28,0x23`` record loop from id 0 until
it reads a next id of ``0xffff``, *software expectation*.

State and reset
===============

The model holds the receive buffer and its escape and discard flags. Reset
clears them; the properties are configuration and survive it. The receive
state is migrated.

Interface
=========

``sgi-pbay`` is a plain device. Its ``chardev`` property names the
character device the framed bytes travel on, joined to the L1's ``pbay``
character device. ``flash-chunk`` and ``state-code`` are described above.
``input-snapshot`` and ``frame-status`` can also be set while the machine
runs. The read-only ``rx-len`` property exposes the bytes received of the
current frame for tests.

Model limits
============

- **No boards.** The bay's supply, fan and temperature state, the meaning
  of the sensor pins, the FRU contents and the latch's registers are
  *unknown*. The model reports clear inputs unless told otherwise, zero FRU
  data and no fan or temperature readings. The microcontroller part is
  *unknown*.
- **No loader.** Flash data, flash end, erase and verify, what makes the
  loader flag a frame and its framing are *unknown*. A flash through
  the model does not complete.

Tests
=====

``tests/qtest/sgi-pbay-test.c`` drives the device over a socket. It
reproduces the worked frames of the link and the tunnelled exchanges byte for
byte, and covers stuffing in both directions, the checksum, the destination
rule, each answered command, every tunnelled pair, the error replies and the
flagged-frame status, the record keys and counts, the sensor masks, the
streamed read, one-byte-at-a-time and back-to-back input, resynchronisation, the flash chunk
limit, reset and a save and load with a frame cut inside an escape.
