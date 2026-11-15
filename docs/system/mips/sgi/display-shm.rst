.. SPDX-License-Identifier: BSD-3-Clause

.. _sgi-display-shm:

=====================================================
Shared-memory display for external viewers
=====================================================

An SGI machine can publish the picture of each of its graphic heads to
another process, so that a viewer outside QEMU can show the consoles of
several machines in one window. The machine property ``display-shm`` turns
this on. The pixels go through a shared-memory segment, one copy of the
changed rows per frame on the QEMU side and none on the reader's, and a
local stream socket tells the reader which head changed and which rows.

This page is the contract a reader implements against. It describes
software conventions between QEMU and a viewer, not hardware, so it carries
no ratings. ``include/ui/sgi-display-shm.h`` is its C form; the two change
together, and ``tests/qtest/sgi-display-shm-test.c`` checks the layout
against the layout this page states.

Keyboard and mouse input do not use this channel. A viewer sends them over
QMP with ``input-send-event`` (see :ref:`sgi-display-shm-input`). The
control protocols are the interchange page's concern and stay out of it
(see :doc:`interchange`).

Machines
========

Every SGI machine type takes the properties: ``octane``, ``octane2``, ``o2``,
``origin200``, ``origin2000``, ``onyx2``, ``origin300``, ``origin3000``,
``origin350``, ``onyx4``, ``onyx350``, ``tezro`` and ``fuel``. A machine
publishes one head for each graphic console it has when its board is built,
in console order. A machine with no graphic console, or none yet because no
graphics device was fitted, publishes zero heads and QEMU warns. Consoles
added after start-up (hot-plugged) are not published.

Enabling it
===========

.. list-table::
   :header-rows: 1

   * - Property
     - Meaning
   * - ``display-shm=NAME``
     - Publish the heads in the segment ``NAME``. 1 to 30 characters from
       ``A-Z a-z 0-9 . _ -``. The 30-character limit is macOS's (a POSIX
       shared-memory name there, including its leading slash, holds 31).
   * - ``display-shm-socket=PATH``
     - The socket's path. Default ``NAME.sock`` in ``$XDG_RUNTIME_DIR``
       when it is set and non-empty, else in the host's temporary
       directory as GLib names it (``$TMPDIR``, ``$TMP`` or ``$TEMP``, then
       the system default). macOS sets no ``XDG_RUNTIME_DIR``, so there it
       is the per-user ``$TMPDIR``, whose path can be long. A path must fit
       in a ``sockaddr_un``, about 104 bytes on macOS and 108 on Linux and
       Windows; QEMU refuses a longer one, and ``display-shm-socket`` then
       names a shorter one.

QEMU creates the segment and the socket after the machine is built, and
prints one line naming both. Where each lives:

.. list-table::
   :header-rows: 1

   * - Host
     - Segment
     - Socket
   * - Linux
     - POSIX shared memory ``/NAME`` (``shm_open``; under ``/dev/shm``)
     - ``AF_UNIX`` stream socket at the path above
   * - macOS
     - POSIX shared memory ``/NAME`` (``shm_open``; the name is not a path)
     - ``AF_UNIX`` stream socket at the path above
   * - Windows 10 and later
     - Named file mapping ``Local\NAME``, backed by the paging file
     - ``AF_UNIX`` stream socket at the path above

A reader opens the segment with the same name and read-write access (it
writes one field, ``reading``), maps all of it, and connects to the socket.
QEMU removes the POSIX segment and the socket file when it exits; on
Windows the mapping disappears with its last handle. A segment or socket
file an earlier crashed run left under the same name is replaced. Two live
machines must not share a name. On Windows the second one refuses to start.
On POSIX it replaces the first one's name, and when the first exits it
removes the second's. On Linux and macOS the socket file is
reachable by its owner only; on Windows it has the user's default access.

The writer needs a little-endian host and refuses the property on a
big-endian one. Every integer below is little endian.

Segment layout
==============

The segment is ``total_bytes`` long: ``data_offset`` (4096) plus, for each
head, three slots of ``slot_bytes`` (16 MiB). The size follows the number of
heads, so a one-head machine takes 48 MiB and a little over, which QEMU
reserves without touching on POSIX and commits on Windows.

Header, at offset 0
-------------------

.. list-table::
   :header-rows: 1

   * - Offset
     - Type
     - Field
     - Value
   * - 0
     - u32
     - ``magic``
     - ``0x48535347`` (the bytes ``47 53 53 48``). QEMU writes it last. A
       reader that opens the segment early waits while it is zero.
   * - 4
     - u32
     - ``version``
     - 2
   * - 8
     - u32
     - ``nheads``
     - Heads published, 0 to 8
   * - 12
     - u32
     - ``slot_bytes``
     - 16777216
   * - 16
     - u64
     - ``data_offset``
     - 4096: where head 0 slot 0 starts
   * - 24
     - u64
     - ``total_bytes``
     - Size of the segment
   * - 32
     - 32 bytes
     - reserved
     - Zero
   * - 64
     - 8 x 128 bytes
     - ``head[8]``
     - Head state; only the first ``nheads`` are used

Head ``h`` starts at ``64 + 128 * h``.

Head state, 128 bytes
---------------------

.. list-table::
   :header-rows: 1

   * - Offset
     - Type
     - Field
     - Meaning
   * - 0
     - u32
     - ``active``
     - 1 while the head has a surface QEMU can publish, else 0. It is 0
       when the surface is larger than 2048 in either direction or its
       format is not x8r8g8b8 or a8r8g8b8.
   * - 4
     - u32
     - ``latest``
     - Index (0 to 2) of the slot holding the newest frame
   * - 8
     - u32
     - ``reading``
     - Slot index plus one that the reader holds, or 0. The reader writes
       it. QEMU reads it once per frame and avoids that slot; a claim made
       after that read is caught by the generation check.
   * - 12
     - u32
     - ``cursor_on``
     - 1 when the machine last reported its pointer shown
   * - 16
     - i32
     - ``cursor_x``
     - Pointer position in head pixels, as last reported
   * - 20
     - i32
     - ``cursor_y``
     -
   * - 24
     - u64
     - ``seq``
     - Sequence number of the newest frame; 0 until the first one. It
       rises by one for every published frame.
   * - 32
     - 3 x 24 bytes
     - ``slot[3]``
     - Per-slot state, below
   * - 104
     - 24 bytes
     - reserved
     - Zero

The cursor is a position only. Its shape is not published. A machine that
never reports a pointer leaves ``cursor_on`` at 0.

Slot state, 24 bytes
--------------------

.. list-table::
   :header-rows: 1

   * - Offset
     - Type
     - Field
     - Meaning
   * - 0
     - u64
     - ``seq``
     - Sequence of the frame this slot holds
   * - 8
     - u32
     - ``gen``
     - Generation counter. Odd while QEMU writes the slot, even when it is
       stable. It rises by two for each frame written.
   * - 12
     - u32
     - ``width``
     - Width in pixels of the frame in the slot
   * - 16
     - u32
     - ``height``
     - Height in pixels
   * - 20
     - u32
     - reserved
     - Zero

``width``, ``height`` and ``seq`` are valid only while ``gen`` is even and
does not change across the read.

Pixels
------

Head ``h`` slot ``s`` starts at
``data_offset + (3 * h + s) * slot_bytes``. Rows follow each other with no
padding, so the stride is ``4 * width``. A frame is at most 2048 by 2048.
Each pixel is a 32-bit little-endian word ``0x00RRGGBB``: the bytes in
memory are blue, green, red, unused. This is pixman's ``x8r8g8b8``. QEMU
also accepts a surface in ``a8r8g8b8`` and passes its alpha byte through,
so a reader must ignore the fourth byte.

The writer
==========

QEMU keeps each head's newest frame in the slot ``latest`` names. For a new
frame it picks a slot that is neither ``latest`` nor the one ``reading``
holds, makes ``gen`` odd, copies the rows that differ from the newest
frame (plus any rows that slot is behind by), stores the slot's ``width``,
``height`` and ``seq``, makes ``gen`` even, then stores ``latest`` and
sends the frame message. A head's size can change from one frame to the
next; the new size is in that frame's slot. The slot a reader holds is
never reused, so a copy can be taken without racing the writer unless the
reader is slower than two frames, which the generation check catches.

QEMU asks for a refresh every 13 ms, so a head can publish about 75 frames a
second. It compares each damaged row against the newest frame, so a frame is
published only when pixels changed.

The reader
==========

Copy a frame like this, for a head ``h``:

.. code-block:: none

   loop:
       s  = load_acquire(head.latest)
       store_seq_cst(head.reading, s + 1)
       g0 = load_acquire(head.slot[s].gen)
       if g0 is odd: retry
       w, h = head.slot[s].width, head.slot[s].height
       copy 4 * w * h bytes from the slot       (or only changed rows)
       fence
       g1 = load(head.slot[s].gen)
       if g1 != g0: retry
       store_release(head.reading, 0)
       use the copy

The ``reading`` store must be visible before the ``gen`` read; use a
sequentially consistent store or a full fence. A retry is rare; do not loop
without limit on one head. Do not write any field of the segment except
``reading``.

A message's ``slot`` is a hint. By the time a reader acts on a message the
slot may have been rewritten, so read ``latest`` rather than trusting it.
Take a frame once per wake-up, not once per message: the messages can queue
faster than the reader draws.

Socket messages
===============

QEMU listens on the socket and accepts one reader at a time. A new
connection closes the previous reader. When a reader connects, QEMU
repaints each active head, so a full-frame message for each follows at the next
refresh, and every frame message carries the cursor state. QEMU
never reads what the reader sends, beyond noticing that it closed; a reader
should send nothing.

Each message is 56 bytes, no padding, little endian.

.. list-table::
   :header-rows: 1

   * - Offset
     - Type
     - Field
     - Meaning
   * - 0
     - u32
     - ``type``
     - 1 frame, 2 cursor
   * - 4
     - u32
     - ``head``
     - Head index
   * - 8
     - u64
     - ``seq``
     - The head's newest frame sequence at send time
   * - 16
     - u32
     - ``slot``
     - Slot of that frame (a hint)
   * - 20
     - u32
     - ``y0``
     - First changed row
   * - 24
     - u32
     - ``y1``
     - One past the last changed row
   * - 28
     - u32
     - ``width``
     - Frame width, 0 for an inactive head
   * - 32
     - u32
     - ``height``
     - Frame height, 0 for an inactive head
   * - 36
     - u32
     - ``flags``
     - Bit 0 size changed, bit 1 full repaint, bit 2 head inactive
   * - 40
     - i32
     - ``cursor_x``
     - Pointer position
   * - 44
     - i32
     - ``cursor_y``
     -
   * - 48
     - u32
     - ``cursor_on``
     - Pointer shown
   * - 52
     - u32
     - reserved
     - Zero

A frame message gives the rows ``y0`` up to, not including, ``y1`` that
changed since the previous message for that head. With bit 1 set
every row counts as changed, and ``y0`` is 0 and ``y1`` the height. QEMU
sets bits 0 and 1 together when the size changes (a mode switch) and for the
repaint after a reader connects, and bit 1 alone when it resends the state
after it dropped a message.
With bit 2 set the head lost its usable surface: ``active`` in the segment is 0 and
the reader should stop showing the head until a later frame arrives.
A cursor message carries the cursor fields; ``seq``, ``slot``, ``width`` and
``height`` repeat the head's current state, and ``y0``, ``y1`` and ``flags``
are zero.

A reader that sees ``seq`` skip a number, or that missed a message, must treat
the whole frame as changed. The socket is non-blocking on QEMU's side. When
it is full, QEMU drops the message, and on a later refresh sends the state
of every head again: a full-frame message for an active head, an inactive
message for one without a surface, and a cursor message for an active head
that has no frame yet, so a reader that falls behind still
ends on the final picture. If QEMU can send only part of a message it closes
the connection; the reader reconnects, repeats the handshake above and
repaints.

A reader that wants no copying can draw directly from the slot it holds, as
long as it keeps ``reading`` set while it draws and obeys the generation
check on release.

.. _sgi-display-shm-input:

Input
=====

Keyboard and mouse events go to the machine over QMP, not through this
socket. A viewer sends ``input-send-event`` with ``key``, ``btn``,
``rel`` or ``abs`` events, for example:

.. code-block:: json

   {"execute": "input-send-event",
    "arguments": {"events": [
      {"type": "key", "data": {"down": true,
                               "key": {"type": "qcode", "data": "a"}}}]}}

An ``abs`` event for a head is in the range 0 to 32767 in each axis and is
scaled by QEMU. Which head's console an event reaches is set by the
``device`` and ``head`` arguments of ``input-send-event``; with neither, it
goes to the machine's first input handler. The SGI models' keyboard and
pointer take these events as they take any other QEMU input.

Versions
========

The ``version`` field is 2. A reader checks ``magic`` and ``version`` and
refuses any other version, rather than guess. Any change to an offset, a
size or the meaning of a field raises the version.

Limits
======

- One reader. A second connection replaces the first.
- At most 8 heads and 2048 by 2048 pixels each; larger surfaces are not
  published.
- Heads are fixed when the machine starts.
- The cursor shape and any audio are not published.
- Windows and macOS builds are checked by the project's release builds,
  not by the qtest below, which runs on hosts with POSIX shared memory.

.. _sgi-display-shm-tests:

Tests
=====

``tests/qtest/sgi-display-shm-test.c`` starts an Octane2 with a graphics
head and ``display-shm``, maps the segment, checks the header, connects to
the socket, reads a frame message and checks the published slot against
the message and against ``screendump``.
