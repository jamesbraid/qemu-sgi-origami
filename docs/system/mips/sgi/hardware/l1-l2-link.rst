.. SPDX-License-Identifier: BSD-3-Clause

.. _sgi-l1-l2-link-page:

=========================================
L1 to L2 link: rack controller attachment
=========================================

Used in :ref:`sgi-ip35` (Origin 300 and Fuel), :ref:`sgi-ip53` (Origin
350, Onyx 350, Tezro and the Onyx4 host), the Origin 300 PCI expansion
module (:doc:`pci-expansion`) and :term:`NUMAlink` module
(:ref:`sgi-bricks-numalink-module`), and the Origin 3000 and Onyx 3000
C-, I-, N-, V-, R- and CX-bricks (:ref:`sgi-bricks`), which the
``origin3000`` machine's topologies build.

.. seealso::

   Platform pages: :ref:`sgi-ip35`, :ref:`sgi-ip53`.

.. seealso::

   - :doc:`The L1 page <l1>`: the controller's identity, UART, framing
     and packets (:ref:`sgi-l1-framing`) and routing (:ref:`sgi-l1-routing`),
     which this link builds on.
   - :doc:`l1-brick-profiles`: the Origin 3000 brick controllers that carry
     the link.

This page is the L1's link to an L2, the rack controller that reaches every
brick's L1 over USB: the chardev that stands in for the USB bulk pair, the
router segments that carry requests toward the L2, the programs the L2
runs, the configuration request and the table exchange between L1s. The
L1's identity, framing and routing are on :doc:`the L1 page <l1>`.

.. _sgi-l1-l2-link:

L2 link
=======

An L2 is the rack controller that reaches every brick's L1 over USB. The
model has none. The optional ``l2`` property is a chardev that stands for
the USB bulk pair toward whatever plays the L2: the project's own
stand-in for a test or a companion process, not a hardware wire. With
the property unset, or while no peer is connected, nothing changes: a
request for controller type 1 or 2 gets the router error reply with
detail 8, and request ``0x000b`` answers the stored setting. The
read-only ``l2-link-up`` property says whether a peer is connected.

``l2-partition-flag`` (0 to 3, default 0) is the initial value of the
two-bit setting that request ``0x000b`` returns and ``0x1008`` writes.
The stock :term:`PROM` fetches a partition record from an L2 only after a
nonzero answer, so a test starts that fetch by setting it. It is not the
partition ID that ``0x0009`` returns. What the setting means on a real
controller is *unknown*.

Machine options
---------------

A machine reaches a controller's L1 with three list options, each entry
``<index>=<value>`` where *index* is the zero-based position of the
controller in the selected topology, the same numbering as
``controller-chardev`` (:ref:`sgi-pci-expansion-controller-inputs`):

``controller-l2-chardev.N=<index>=<chardev-id>``
  Sets the ``l2`` property of that controller's L1 to an existing
  chardev. Each chardev serves one controller; a second entry for the
  same controller or the same chardev id, a chardev that does not
  exist, or one that ``controller-chardev`` already uses is refused.
  The catalogue lists each C-brick L1's link as a serial line of kind
  ``l2-link`` (the other bricks' L1s take the same option but have no
  line). A launcher with no L2 peer to attach leaves the line's chardev
  unbacked, may report that and does not refuse the launch; the
  machine runs and the L1 answers as with the property unset. A consumer
  that does not know the kind ignores the line. Router machines always
  list the links. On the Origin 3200 and Onyx 3200 (the ``origin3200``
  and ``onyx3000`` topologies) the line names the ``l2-controller``
  setting (:ref:`sgi-settings`), off by default, whose target is the
  lines rather than a property, and a launch binds only the lines whose
  setting is on. The owner's guides split the cases by rack height: the
  L2 is included with every tall rack that holds C-bricks, optional in a
  short rack, and required with an R-brick
  (:src:`007-4311-002 p. 42`, :src:`007-4240-002 p. 6, 15, 75, 82-83`,
  :src:`007-4264-002 p. 24, 26, 96`, :src:`007-4653-001 p. 116` for the
  Origin 3900), *established*. The catalogue takes a rack with no
  routers to be a short rack, which holds for every current offering.

``controller-l2-partition-flag.N=<index>=<0 to 3>``
  Sets ``l2-partition-flag`` on that controller's L1. It needs no
  chardev, and a value outside 0 to 3 is refused.

``controller-pbay-chardev.N=<index>=<chardev-id>``
  Sets the ``pbay`` property of that controller's L1 to an existing
  chardev, joining it to a power bay (:ref:`sgi-l1-pbay`; the bay end is
  the ``sgi-pbay`` device). The same rules as the L2 link apply, and a
  chardev that ``controller-chardev`` or ``controller-l2-chardev`` uses is
  refused too. The L1 refuses to start on a brick type that does not run the
  power-bay task. The option sets no timing: the ``pbay-tick-us`` and
  ``pbay-poll-ms`` properties are reached with ``-global sgi-l1.<property>``.

All accept an expansion controller and a controller that no module owns
and exactly one compute node names, which is a C-brick's L1 in the
Origin 3000 topologies. A module-owned L1 serves several boards, so an
entry for one, or for an index beyond the topology, is refused. For
example, the two-partition Origin 3400 gives its first partition a link
and a nonzero setting with::

   -chardev socket,id=l2a,path=l2.sock \
   -M origin3000,topology=origin3400-two-partitions,controller-l2-chardev.0=0=l2a,controller-l2-partition-flag.0=0=1

Setting ``controller-l2-chardev`` blocks migration for the reason given
under Limits; ``controller-pbay-chardev`` blocks it because the bay's state
lies outside the machine.

Link format
-----------

The real link carries one router segment per USB transfer. A stream
socket has no transfer boundary, so each message on the chardev is a
one-byte kind followed by its body. Kind 1 is a segment, and its first
two bytes (big endian, counting themselves, at least 6 and at most
1,024) delimit it. Other kinds are reserved: the L1 logs one with
``LOG_GUEST_ERROR`` and skips the byte. The format is the project's own
(``include/hw/misc/sgi-l1-l2-link.h``); the segment inside it is the
field map of the controller's management USB stream, *software expectation* for
the L1 side and *inferred* for what the L2 expects:

.. list-table::
   :header-rows: 1
   :widths: 12 12 76

   * - Offset
     - Width
     - Field
   * - 0
     - 2
     - Segment length, including itself.
   * - 2
     - 1
     - Format byte, 2 as the L1 builds it.
   * - 3
     - 1
     - Flag byte, meaning *unknown*; the model sends 0.
   * - 4
     - 1
     - Low byte of the sequence number.
   * - 5
     - 1
     - Bit 7 set on the first segment of a packet; bits 6:0 the segments
       left, counting this one on the first. A packet is up to 127
       segments, each carrying one less than the last.
   * - 6
     - 1
     - Class: request 0, response 1, event 2.
   * - 7
     - 1
     - Source port, meaning *unknown*; the model sends 0.
   * - 8
     - 4
     - Destination router address.
   * - 12
     - 4
     - Source router address.
   * - 16
     - 1
     - Descriptor count.
   * - 17
     - 1
     - High byte of the sequence number.
   * - 18
     - 2
     - Request or response code.
   * - 20
     - 8 each
     - Descriptors. A scalar is ``00 00 0000`` and a 32-bit value.

Every segment that crosses the link in either direction is the trace
event ``sgi_l1_l2_segment`` with its bytes, so a run's traffic can be
read from the trace.

.. _sgi-l1-l2-user-space:

Running the L2's own programs
-----------------------------

The L2's own programs, taken from the shipped controller image, run under
the PowerPC Linux user-mode emulator (``qemu-ppc``, built with the
``ppc-linux-user`` target) against the unpacked image as ``-L`` root with
``-r 2.4.7``. Two pieces of this tree replace what their kernel gave
them. Neither changes the emulator itself.

``qemu-sgi-l2-hub`` is the host end of the L1 device nodes. It stands for the
L2's USB host controller and hubs, owns the links to the L1 models and
logs everything that crosses them with a timestamp::

   qemu-sgi-l2-hub --dir DIR [--log FILE]

It listens on ``DIR/l1.sock`` (a stream socket) for L1 models and on
``DIR/dev.sock`` (sequenced packets) for the plugin. An L1 connects with
its ``l2`` chardev as a client, for example ``-chardev
socket,id=l2a,path=DIR/l1.sock,reconnect-ms=1000``. Each link gets the lowest
free slot from 0 to 39, which is a USB attach, and a close is a detach. A
slot's 9-byte device record is synthetic (bus 1, two hub ports, device
address slot + 2, ports 1 and slot + 1): its layout is a *software expectation*, the
values the real L2 would show are *unknown* until a rack topology supplies
them. A segment that reaches the hub before the program has opened the
slot's node is queued, up to 256 segments, and delivered in order on open.
Links and nodes are served independently: a program that stops reading a
node, or an L1 that stops reading its link, delays only that slot, and
traffic from several L1s interleaves freely, as in the two-partition
Origin 3400 with both C-brick L1s linked.
A segment the program writes goes to the L1 with the length field set to
the transfer's length, as the kernel driver does. Closing a node resets the
device, which the hub logs; the link has no message for a reset, so the L1
does not see it. The messages between the hub and the plugin are in
``include/hw/misc/sgi-l2-hub.h``.

``sgi-l2-shim`` (``contrib/plugins``) is a TCG plugin for ``qemu-ppc``. With
``hub=DIR`` it serves the programs' system calls on the L1 nodes:

- ``open`` of ``/dev/sgil1_cs`` and ``/dev/sgil1_N`` returns a socket
  connected to the hub, so ``read``, ``write`` and ``poll`` run on the host
  with one message per transfer. A slot with no L1 is ``ENODEV`` and one
  already open is ``EBUSY``.
- The driver's ioctls are answered: revision 2 (``0x40045306``), the device
  record by slot or by node (``0xc0095307``, ``0x40095304``), and the four
  resets (``0x20005301`` to ``0x20005303``, ``0x20005305``), which the hub
  logs. Any other ioctl on a node is ``ENOTTY``.
- ``/dev/mem`` is a regular file in the root. The programs map the flash
  at physical ``0xfe000000`` (``0xfc000000`` first). The emulator cannot pass
  those offsets to the host, because 32-bit PowerPC hands it the offset
  as a signed number, so the plugin has the guest repeat the call with an
  offset into the file. The file holds 36 MiB: the first 4 MiB stand for
  the empty bus at ``0xfc000000`` and the flash is the 4 MiB at file offset
  ``0x2000000``. A fresh flash is all ones. The daemon reads the flashed
  version from the window, and only the controller image's payload (the
  file without its 0x2c-byte file header) placed at window offset 0 passes
  its header check; the sizes and checksums are a *software expectation*, that the
  flash holds the payload there is *inferred*. The plugin does not fill the
  flash: whoever prepares the file writes the payload.
- Stores into the flash window are decoded after they happen and the plugin
  corrects guest memory: auto-select presents manufacturer ``0x00010001``
  and device ``0x22282228`` at offsets 0 and 4 until a reset command,
  program clears bits only, sector erase fills the sector with ones
  (31 sectors of 128 KiB, then 8 of 16 KiB) and chip erase fills the
  window, and the command cycles leave nothing behind. All operations
  complete at once. The command addresses, the doubled halfwords and the
  sector map are *inferred*; the plugin does
  not model the toggle bits the real chip shows during an operation.

Without ``hub`` the plugin only logs the opens and ioctls on ``/dev``
descriptors, which is how the system calls a program needs are collected.
``log=FILE`` redirects the log, ``flash-log=on`` logs each store into the
window and ``trace=on`` logs every ``/dev`` open and the ioctl results.

Limits. A sequenced-packet socket does not behave exactly as the driver
does: a read into a short buffer truncates instead of failing, an empty
non-blocking read is ``EAGAIN``, and a second write does not wait for the
first. The programs read with a buffer larger than any segment and only
when ``poll`` reports data, so none of these paths is expected to run.
Each program that maps the flash keeps its own copy of the window for
restoring command words and for the program step, so two programs that
program the same words see each other's results in the shared file but
not in that copy. The plugin instruments every store, which slows the
programs: a loop of 200 million stores took 2.2 times as long with the
plugin loaded (0.65 s to 1.44 s on the development host). The USB-level
rules of the real link (descriptors, endpoint halts, zero-length packets)
are not exercised.

Requests toward the L2
----------------------

A plain request from the node for type 1 or 2 is forwarded as one
request segment when every argument is an integer, because no other
descriptor form is known. A request with another argument type
keeps the router error reply. The stock PROM sends its partition-record
fetch another way, as the multi-frame transport (below) carrying a
complete inner message for controller type 1, task ``0xa``; the inner
message already has the segment layout, so the model forwards it as it
arrived. In a boot run of the stock firmware it was a 28-byte request
segment with code 1 and one integer argument, 4 on the C-brick in bay 4,
which the firmware sent after asking ``0x000b`` and followed with
``0x1006`` once the reply came. What the argument means is *inferred*:
an L2's partition service reads it as a brick identifier.
While a peer is connected every profile accepts that transport for a
request whose inner destination is controller type 1 or 2. On a profile
without an IRouter of its own, any other chunk frame is handled as it is
with no peer. A packet of more than one fragment is not forwarded.

In both forms the source word is rewritten to the controller's own
rack and bay key in bits 31:12, the requester's UART index and
subchannel in bits 11:5 and task 21 in bits 4:0, the packing the
multi-frame transport uses for its replies. That the controller's key
fills the rest is an *approximation*; the real base is *unknown*.
The peer addresses its response to that word, which is how the model finds
the requester.

Responses from the L2
---------------------

A response segment whose destination names a requester with a request
pending is returned to it.

- To a plain request, the first descriptor is the reply code and every
  further integer descriptor is one integer field of the ordinary reply.
  A response of more than one segment, or with a descriptor that is not
  a scalar, is logged with ``LOG_UNIMP`` and the requester gets the
  router error reply.
- To a chunk-transport request, each segment returns as frames of at most
  113 payload bytes. The first frame of a segment is ``0x3001`` and
  carries the segment's remaining-segment count in its count byte and the
  frame's own payload length in its length field. Each later frame is
  ``0x3003`` while the segment has more, and ``0x3002`` on the frame that
  exhausts it; their count and length fields are zero. A segment that fits
  one frame is a single ``0x3001``.

  The stock boot firmware was run against this (a stand-in peer, the
  two-partition Origin 3400, a nonzero ``l2-partition-flag`` on the first
  C-brick). A 36-byte segment in one ``0x3001`` frame was accepted and its
  integer used. A 300-byte segment in three frames, with the length field
  of the ``0x3001`` frame holding the whole 300, made the firmware reject
  the frame as shorter than that length; with that field holding the
  frame's own 113 bytes it was accepted, whether the later frames carried a zero length or
  their own. That is the reader's behavior in a model run, for this
  firmware version only, not a capture of an L2. A reply of two segments was accepted with each segment's
  first frame ``0x3001`` and also with only the first, so which the
  firmware expects of a second segment, and whether it reads one, is
  *unknown*. The firmware's wait of about four seconds for a reply is
  *inferred*: with no reply it reported that the wait had failed and went
  on with the partition ID its controller holds.

Segments that are not responses (requests and events from the L2),
responses that name no requester or one with nothing pending, and
continuations out of order are logged and dropped, except the one
request described next. The L2's other housekeeping requests are not
served, because nothing published says what the L1 answers.

.. _sgi-l1-l2-config:

The L2's configuration request
------------------------------

The L2 daemon sends each attached L1 a recurring request to destination
``0x0ffff00e``: class request, code 0, three descriptors, the integer 6,
the L2's serial setting as a string (or the integer 0 when it has none)
and an integer (*software expectation* for the shipped 1.44.0 daemon). Attaching
gives the L2 no route to an L1. The L1 becomes routable at the L2 only
when it answers this request, because the daemon registers it from the
reply. With the link up, the model answers a request with exactly these
three descriptors, the first the integer 6 and the third an integer.
Anything else is logged with ``LOG_UNIMP`` and not answered, except a
six-descriptor selector 2 or 13 request (:ref:`sgi-l1-table-exchange`). A controller
whose rack or bay is unset does not answer either, because nothing says
what a real one reports. A request cut short of its three descriptors is
not answered. A string descriptor whose offset or length lies outside the
segment, or whose text is not terminated inside it, is read as no serial
text and the reply goes out without one; what a real L1 does with such a
request is *unknown*, so each is an *approximation*.

Before it builds the reply, on every selector 6 request that is answered, the
controller starts the L1-to-peer-L1 table exchange of selectors 2 and 13
(next section). Neither selector is a request to the L2 and neither crosses
the L2 link.

The reply is one segment: class response, destination the request's
source word, status byte 0, the request's code, and these descriptors
(*software expectation*): the integer 7; the request's third
descriptor, echoed; the record count ``N``; the record size 0x70; a bytes
descriptor holding ``N`` index bytes, each a record's position; a bytes
descriptor holding ``N`` records of 0x70 bytes. A nonzero third request
descriptor makes the reply the single integer 7 (*software expectation*). The reply
carries the request's two sequence bytes unchanged; that a real L1 copies
them is *inferred*, and what it puts there otherwise is *unknown*, so the
copy is an *approximation*.

The source word is ``(identity << 12) | 0x0e`` with the identity the rack
in bits 15:6 and the bay in bits 5:0; that these are the controller's own
settings is *inferred*. The records come from the controller's module table
of fifteen entries of 0x70 bytes: its own entries at 0 to 4, the entries
learned from the peer on serial port 1 at 5 to 9 and from port 2 at 10 to 14
(*software expectation* for the stores; the naming is *inferred*). An entry is listed
when its word at +0x60 is nonzero (*software expectation*), and a group of five whose
first entry's string at +0x40 repeats an earlier group's is left out
(*software expectation* for the omission; that the L2 is the consumer of the
entries learned from a peer is *inferred*). The index array holds each
listed entry's table index. A record has a nonzero word at +0x00, the rack
in the word at +0x08 and the bay in the word at +0x0c, big endian
(*software expectation* as the fields the L2 routes by); the word at +0x60 is 1, an
*approximation* since its meaning is *unknown*. Every other field is zero
except two the model chooses: the text at +0x40, which holds the brick's
module name so the repeat test has something to compare, and the serial
text at +0x64 of the controller's own entry 0, the request's text cut to 11
characters (its role is *inferred*). A request whose text cannot be read
leaves the stored text unchanged, an *approximation*. The controller's own
entries are itself at 0 and, at 1, its I-brick (or, for an I-brick, its
host C-brick), with the rest empty. What fills the real table is *unknown*,
so those entries are an *approximation*.

The reply's descriptors are built the way the L1's packet builder builds
them. The integers have type ``0x00``. The index array (descriptor 4) is
referenced in place, type ``0x01``, and the record block (descriptor 5) is a
copy, type ``0x09``; the two are different bytes in one packet
(*software expectation* for the values the L1 builds, *inferred* that they reach the
wire unchanged, because the L1's USB path was not traced for a rewrite). The
low three bits of a type are the kind (0 integer, 1 bytes, 2 string); bit 3
(``0x08``) is a construction flag, "free the caller's buffer", that stays in
the type byte, and readers test the kind bits only. A data descriptor is
eight bytes: type, segment byte, 16-bit length and a 32-bit offset from the
start of the segment record, which sits 0x10 bytes before the first wire
byte. The segment byte is the 1-based number of the segment that holds the
payload, so it is 1 on both blobs of this single-segment reply
(*software expectation*: the L1's own reader returns nothing for a data descriptor
whose segment byte is 0). A reply of more than eight records would continue
into a second segment whose descriptors carry 2 (*inferred*); the model's
table never reaches that size. The reply follows the sheets' reading of the
1.44.0 package for one controller type and is not claimed to reproduce a
real L1's.

No timeout is modelled: the PROM's own wait for the reply is its
concern. If the peer disconnects, a plain request still waiting gets the
router error reply and pending state is cleared.

.. _sgi-l1-table-exchange:

The table exchange with a peer L1
---------------------------------

Selectors 2 and 13 are the L1's exchange of module-table entries with a
peer L1 over a peer serial port, not requests to the L2: selector 2 goes to
port 1 and selector 13 to port 2, posted to that port's writer only when its
handles are open (*software expectation* from one reading of the 1.44.0 package for
the Fuel, Origin 300 and PE builds; which package a C-brick ships is
*inferred*). A C-brick opens ports 1 and 9, so a paired C-brick is port 1,
selector 13 is never sent, and selector 2 is sent only while the pair's
``peer`` link is set on both controllers and ``network-422`` is up on both
(*inferred*: the receiver gets a packet only through a port of its own that
is open). A controller that is not named, or is restarting, does not
answer, as for selector 6. The
peer is a direct call between the paired controllers; the RS-422 framing is
not modelled. ``sgi_l1_peer_segment`` traces each packet.

The request is one 628-byte segment: class 0 (*inferred*, the
builder's class argument), code 0, destination ``0x0ffff00e``, the sender's
task 14 word as source, six descriptors (*software expectation*): the selector as an
integer, then five byte copies, type ``0x09``, segment byte 1, length 0x70,
the sender's own entries 0 to 4. The receiver clears the five entries of the
arrival port's group, copies descriptors 1 to 5 into them, each the smaller
of 0x70 and descriptor 1's length, and sets the first entry's +0x60 to 1
(*software expectation*). Nothing is stored when the status byte is nonzero or the
arrival is not a peer port. It then answers with a response, class 1,
status 0, to the request's source word, the same selector and its own entries
0 to 4, also when it stored nothing (*software expectation* from the branch order).
The sender stores the response's entries as the group for the selector it
sent. A selector 2 or 13 request that arrives on the L2 link is answered the
same way and stores nothing; no sender of one is known (*software expectation* for the
branch, *unknown* whether any L2 sends it).

Not modelled: the follow-up that sends the opposite selector to the other
peer port after a store (what it is for is *inferred*), a second peer port,
the two-descriptor form of selector 6 (the daemon never sends it; such a
request is logged and not answered) and a reply of more than eight records,
which would need a second segment. Whether a port that is initialised but
uncabled counts as open is *unknown*; a controller with no ``peer`` sends
nothing.

The learned entries and the serial text are not migrated. Only a request
that arrived on an L2 link, or the exchange that request starts, fills them,
and a controller with an L2 link blocks migration, as does a machine that
holds one for its peer; they are cleared when the controller itself starts.

Limits
------

- There is no USB device: no descriptors, endpoint halt clearing,
  zero-length packets or 64-byte packetisation. Those are the L1's
  firmware behaviour and wait for the firmware L1.
- Attach, device-reset and endpoint-reset control messages are not
  defined; the peer learns of the L1 by the connection.
- Setting ``l2`` blocks migration and snapshots, whether or not a peer is
  connected, because the peer's state is outside the machine. The blocker
  is process-wide, so it also covers a paired L1 that has no ``l2`` chardev.
- The link state is not part of the saved device state.
