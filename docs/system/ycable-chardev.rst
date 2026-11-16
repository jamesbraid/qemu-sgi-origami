.. SPDX-License-Identifier: BSD-3-Clause

======================================
Ycable: a serial splitter for chardevs
======================================

Status: host convenience. It is a wiring helper, not a hardware model, so it
has no hardware ratings.

A ``ycable`` chardev joins up to four other chardevs, its legs, to one
frontend such as a guest serial port. It is for a guest port that must reach
several things at once: a built-in model, a process emulating a serial device
over a socket, a log file.

.. code-block:: shell

   -chardev wacom-iv,id=tablet \
   -chardev socket,id=resp,path=/run/device.sock \
   -chardev ycable,id=line,legs.0=tablet,legs.1=resp \
   -serial chardev:line

Behaviour
=========

- Every byte the guest writes goes to every leg, so every leg sees all of
  the guest's traffic, whatever it is meant for.
- Each leg's output reaches the guest. A read from one leg is held as a chunk
  and delivered before the next leg's chunk starts, so two legs never
  interleave inside one read.
- The guest side sets the pace. Bytes wait in the splitter while the guest
  port is full, up to 4096, after which the legs are told to wait.
- Serial ioctls go to every leg: line parameters, break and modem lines.
  The result is success if any leg accepted. A modem-line read comes from
  the first leg that answers.
- The line counts as open while any leg is open, so a leg that connects later
  (a socket server with no client yet) does not close it.
- A leg can be any chardev except ``hub`` or one with ``mux=on``. Another
  ``ycable`` works.

Approximations and limits
=========================

- A chunk is one read from a leg. A leg that writes a message in several
  pieces can be interleaved with another leg between the pieces. For a leg
  that is a socket, a piece is what one ``read`` returned.
- The legs do not agree on line settings. A leg that ignores them (a plain
  socket) hears the guest whatever the line is, while one that checks them
  (``wacom-iv``) goes quiet on a mismatch.
- Guest output is written to each leg without blocking. If a leg cannot take
  it (a peer that stopped reading) the bytes for that leg are dropped and a
  warning is printed once per leg; the other legs and the VM carry on.
- ``legs.N`` must be consecutive from ``legs.0``. At most four legs. Legs cannot be added or removed after start.
- Replies from different legs are not ordered against each other in time:
  chunks go out in the order they arrived.

Tests
=====

``tests/qtest/ycable-test.c`` runs a 16550 on Malta over ``wacom-iv`` and unix
socket legs: line settings reach every tablet leg, every leg hears the guest,
a leg's reply reaches the guest, replies from two legs at once arrive whole
one after the other (short and longer than the FIFO), more than 4096 bytes
arrive in order, a break on a leg reaches the guest, and a leg can drop and
reconnect while the line stays open. Modem-line fan-out is tested with the
``rfc2217`` chardev as a leg, which is the only leg that carries those lines.
