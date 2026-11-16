.. SPDX-License-Identifier: BSD-3-Clause

=========================================
Display protocol version 4 (display-shm)
=========================================

``-device display-shm`` publishes the picture of each graphic console to
local viewer processes over a socket, and takes their keyboard and pointer
back. The picture travels through shared memory: the producer fills a slot,
lends it to a viewer, and takes it back when the viewer says it is finished.
Nothing is sampled for a head unless a viewer is waiting for it.

The wire is ``include/ui/display-shm-proto.h``, whose structure sizes and
offsets are checked at build time, and ``tests/qtest/display-shm-test.c``
runs the rules below against a producer. Version 3 (the machine property
``display-shm``, :ref:`sgi-display-shm`) stays beside it and is not changed.
A version 3 viewer that connects here is closed.

All integers are little endian. The producer refuses to start on a
big-endian host.

Starting it
===========

.. code-block:: shell

   qemu-system-mips64 ... -device display-shm,path=/run/user/1000/qemu/d.sock

.. list-table::
   :header-rows: 1

   * - Property
     - Meaning
   * - ``fd``
     - An inherited listening socket, preferred: the launcher owns the path.
   * - ``path``
     - The socket's path. Its directory is made ``0700`` if absent and must
       belong to the user and be writable by no one else. A path that
       answers a connection is in use and is refused.
   * - ``max-viewers``
     - Viewers at once, 1 to 8 (default 4).
   * - ``heads``
     - Rooms for heads in the state region (default: the consoles and
       registered sources, plus a spare, and four more with ``x-test``).
   * - ``allow-uid``
     - Comma separated user ids, besides the user's own, that may connect.
   * - ``steering``
     - ``auto`` (default), ``on`` or ``off``: offer
       ``qemu.pointer-steering``.
   * - ``max-head-bytes``, ``max-sets-per-head``, ``max-retained-bytes``
     - Largest slot (256 MiB), slot sets per head (3), and memory held for
       slots (1 GiB).
   * - ``x-test``
     - Offer ``qemu.test`` and add a test head.

``vulkan``, ``udmabuf`` (unless ``off``) and ``retire-on-disconnect`` (unless
``held``) are refused until the extensions exist; ``shm-prefix`` names
shared memory where the host names it (macOS).

Heads
=====

A head is one graphic console (at most 16384 pixels each way; a larger mode
shows nothing), numbered from 0 in console order and never
reused. The SGI scanout is a head whose reference frame is the console's
surface. A console of a 2D device that completes a hardware update before
``qemu_console_hw_update()`` returns (the VGA family, Cirrus, Bochs, ramfb,
virtio-gpu without GL) is a surface head. Every other graphic console is
announced as ``UNSUPPORTED``, because a listener cannot see when a deferred
update, qxl's, finishes. With ``x-test`` a test head follows the consoles.

Frames
======

The producer keeps a private reference frame for each head, always equal to
the newest picture ``N``. It never reads a slot. A slot set holds 1 plus the
credits of its viewers slots (2 to 16); a viewer asks for a credit of 1 to 4
in ``HELLO``, the number of frames it may hold at once.

``FRAME`` lends a slot under a lease: an index below the credit and a
never-repeating ``lease_seq``. The viewer ends the lease by storing that
``lease_seq`` into ``released[lease]`` in its feedback region, then sends
``REQUEST`` again if one is outstanding. The producer writes a slot only
while no lease names it.

* A viewer with a request recorded and a free lease is *due* when ``N`` is
  greater than its ``frame_seen`` and than the newest serial lent to it. It
  is served at once, from the reference frame, copying the slot's stale
  lines.
* ``REQUEST`` with ``SAMPLE_NOW`` is answered by a sample that began after
  it, or one that began within a quarter of the head's refresh period before
  it. A request is not due while its sample is incomplete.
* A plain ``REQUEST`` on a head with unsampled changes schedules a sample;
  with none recorded, a source samples nothing. ``N`` rises only if a line
  differs from the reference frame, and on a head's or a mode's first sample.
* At most one ``FRAME`` per viewer and head is unsent: a newer one replaces
  it. Cancelling an unsent ``REGION`` cancels the unsent ``HEAD`` and
  ``FRAME`` messages that name it, and undoes each ``FRAME``.
* After a re-home or a mode change ``frame_seen`` and the last serial lent
  read as 0 until the first byte of a frame from the new set is sent.
* A viewer that disconnects has the slots it was sent retired and replaced.

Damage is the union of changed lines since the viewer's previous ``FRAME``:
line runs as rectangles, a line bitmap past 64 runs, or the whole frame.
Rectangles span the head's width; the producer does not track columns.

Storage
=======

Only class ``shm`` exists. On Linux a slot is a memfd sealed against growing
and shrinking and against further seals; a set the viewer may not write is
also sealed against new writable mappings. ``RW_PIXELS`` gives the viewer a
set of its own that it may write. ``IMPORT_FAILED`` re-homes the viewer:
device lost (once per connection) and representation unsupported re-create
its set, needs-write moves it to a writable set, too-large marks the head
``TOO_LARGE`` for it, and the rest end the session because no other class
exists.

On Windows a slot is an unnamed, pagefile-backed section. The producer
copies the handle into the viewer's process (the socket's peer, found with
``SIO_AF_UNIX_GETPEERPID`` and required to run as the producer's user) just
before the first byte of the ``REGION`` goes out, with only the access the
region allows, and writes the value into the message's last eight bytes. A
message that never started takes its copy back. A closed connection keeps the
handles it started after the viewer's acknowledged count for 60 seconds (at
most 8 connections), and a ``HELLO`` that names ``prev_viewer_id``,
``prev_session_id`` and its taken count is answered with ``ORPHANS``. Only
``path`` listens; an inherited ``fd`` is refused. The socket is not given an
owner-only ACL, so keep it in a directory only the user can write.
A read-only handle stops the viewer mapping a region for write by mistake.
It is not a barrier: a viewer running as the producer's user can open the
producer's process, so it is trusted not to write what it was given
read-only. A viewer closed with input unread sees its connection reset, so
an ``ERROR`` sent just before the close, such as ``LIMIT`` for a full output
queue, can be lost.

Input
=====

The first viewer with ``INPUT`` owns input, and ``INPUT_CLAIM`` moves it.
Others' events are dropped. Events apply in order and ``SYNC`` ends a report.
Each button change is a report of its own. Consecutive ``REL`` events of a
head are summed in 64 bits and clamped to +-32767 per axis. A report holds
256 events at most. Motion past 4096 counts per axis, and wheel notches past
64, in 10 ms of guest time are dropped, so a flood cannot overflow the PS/2
model's accumulator quickly. It can still, in about 87 minutes of
continuous maximum flooding: closing that needs a change in the PS/2 model.

Pointer steering
================

``qemu.pointer-steering`` (version 1) gives a relative-only pointer an
absolute one: ``ABS`` closes a loop on the cursor position the guest
reports, one relative packet in flight at a time. A button during a move
waits, and everything after it waits behind it, until the move ends or the
first entry has waited 150 ms of guest time. ``ABS`` is never queued; it
moves the target. ``REL`` or ``ABS`` for another head ends the move
(``STEER_DONE`` reason 4) after the queue is flushed in order; a mode
change does the same; a reset, an owner change or a disconnect drops the
queue and releases held keys and buttons.

``qemu.test``
=============

Offered only with ``x-test`` and only to a viewer that sets the ``TEST``
feature. ``STIMULUS`` changes the test head's picture to a pure function of
a pattern, a seed and a step, so a viewer can compute every pixel it should
see; it also moves and defines the cursor, changes the mode and adds or
removes heads. ``CONTROL`` holds the next sample of a head (1, 2), retires
the slot of a target's unsent ``FRAME`` (3), makes the next stimulus mark
its lines from a second thread with a pause between the bit and the flag
(4), and holds a target's socket writes (5, 6). ``INPUT_ECHO`` reports each
event as it reaches the guest's input layer, and ``STATS`` reports the
producer's counters: ten u64 from offset 8 (samples, fills, lines produced,
stored and copied, bytes copied, publishes, frames sent, wake-ups, retained
bytes), then the u32 ``sets`` at 88 and ``max_queued`` at 92.

Not yet
=======

macOS (not built or run), the ``vk.external-memory``,
``linux.dmabuf`` and ``sgi.raw-scanout`` extensions, leaving the cursor out
of the picture, horizontal damage extents, a console created after a viewer
connected (it becomes a head, or an unsupported one, at the next connection), and the held-sample ``CONTROL``
for a producer that samples elsewhere than its own thread.
