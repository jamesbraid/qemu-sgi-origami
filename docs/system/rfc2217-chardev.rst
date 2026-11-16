.. SPDX-License-Identifier: BSD-3-Clause

=============================================
RFC 2217: a serial line carried over a socket
=============================================

Status: host convenience. It is a wiring helper, not a hardware model, so it
has no hardware ratings.

The ``rfc2217`` chardev carries a guest serial port over a socket chardev with
the Telnet Com Port Control option (`RFC 2217 <https://www.rfc-editor.org/rfc/rfc2217>`__),
so the process on the other end sees the speed, framing, break and DTR/RTS the
guest sets. That process can be a device emulator, or ``ser2net`` with a real
port.

.. code-block:: shell

   -chardev socket,id=wire,host=127.0.0.1,port=2217,reconnect-ms=1000 \
   -chardev rfc2217,id=line,chardev=wire \
   -serial chardev:line

It can also be one leg of a ``ycable``, next to a built-in model.

Behaviour
=========

- On connection, and again on every reconnection, it sends ``IAC WILL
  COM-PORT-OPTION``, ``IAC WILL TRANSMIT-BINARY`` and ``IAC DO
  TRANSMIT-BINARY`` (so a server on a plain Telnet terminal does not rewrite
  carriage returns), followed by the guest's current line: baud rate, data
  size, parity and stop size (in the order the RFC recommends), then DTR, RTS
  and break. The guest sets its line long before a server connects, so the
  last settings are kept.
- Later changes go out as they happen: ``SET-BAUDRATE``, ``SET-DATASIZE``,
  ``SET-PARITY``, ``SET-STOPSIZE`` and ``SET-CONTROL`` for break, DTR and
  RTS.
- The server's other options are refused once (``DO`` gets ``WONT``,
  ``WILL`` gets ``DONT``); a ``WILL COM-PORT-OPTION`` is answered with ``DO``.
- A 16550 reports its whole line after every LCR or divisor write, so a
  guest that programs a new speed sends the server a series of full settings,
  the last being the one it meant.
- Data bytes of 0xff are doubled in both directions. Telnet commands and
  the server's option traffic are consumed and not shown to the guest.

Approximations and limits
=========================

- Acknowledgements are not waited for. A setting the server ignores is not
  retried, and a server's requests beyond option negotiation are not answered.
- Modem input lines are not carried back. CTS, DSR and DCD read as asserted
  (the line assumes a device that asserts them), and the ring indicator low.
  ``NOTIFY-MODEMSTATE`` is ignored, so a server that drops CTS or DSR is
  not noticed: this is a known limit. Line state, flow control and purge
  commands from the server are parsed and ignored too.
- ``SET-CONTROL`` with DTR and RTS only: the other flow-control values are
  never sent.
- The socket chardev is written without blocking. If it will not take the
  bytes (a server that stopped reading) they are dropped and a warning is
  printed once. A dropped command can leave the server mid-command until the
  next reconnect, which resends the line.
- Settings are one-way: a server cannot change the guest's line.

Tests
=====

``tests/qtest/rfc2217-test.c`` plays the access server on a unix socket
against a 16550 on Malta and checks the whole byte stream the server
receives, literally: the opening negotiation, the series of line commands for two framings, break, DTR and
RTS, doubling of 0xff both ways (including inside a command value, at 255
baud), option replies, a command split across reads, and the line being sent
again after a reconnect. A ``ycable`` of a tablet and two ``rfc2217`` legs
checks that break and DTR reach both servers and that a modem read is
answered by the first leg that can.
