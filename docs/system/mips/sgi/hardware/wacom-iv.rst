.. SPDX-License-Identifier: BSD-3-Clause

.. _sgi-wacom-iv:

====================================
Wacom IV: serial graphics tablet
====================================

Used in :ref:`sgi-ip27`, where an Onyx2 guest running a digitiser-aware
application reads the tablet on a serial port. It is a host-side serial
backend, so any machine with a serial port can carry it.

.. seealso::

   Platform pages: :ref:`sgi-ip27`.

A Wacom IV UD-series tablet is a serial digitiser that reports the pen
position, a switch number and a pressure. QEMU models a UD-1212-R at ROM
version 1.3 as ``hw/input/wacom-iv.c``, a character device backend named
``wacom-iv``. The backend takes no options:

.. code-block:: shell

   qemu-system-mips64 ... -chardev wacom-iv,id=tablet -serial chardev:tablet

It is separate from the Penpartner backend ``wctablet``, which waits for an
``ST`` command before it transmits and answers ``RE`` with the Penpartner's
configuration line. The ``wacom-iv`` backend serves both drivers that talk to
a UD tablet: one that sends ``ST`` and one that never does.

Facts are rated as described in :ref:`sgi-ratings`.

.. _sgi-wacom-iv-sources:

Sources
=======

- The Wacom software interface manual for the UD, KT and SD series,
  :src:`wtc-1002`. It gives the command set, the packet formats and the
  factory settings of appendix C.
- The Linux ``wacom_serial4`` driver, mainline commit ``bf4afc53b77a``,
  ``drivers/input/tablet/wacom_serial4.c``, as a software expectation. It
  asks ``~#``, ``~R`` and ``~C``, then sends a setup ending in ``ST``.
- The Discreet tablet driver object, version 3.03, :src:`dlwacom-3.03`, as a
  software expectation. It resets with ``RE``, asks ``~#`` and ``~C``, then
  sets ``OC1``, ``IT2``, ``SU1`` and ``NR1270``, and sends ``ST`` only for
  one of its own controls and for another tablet family
  (:src:`dlwacom-3.03 0x2c6c, 0x3268, 0x3a58`).
- The stock IRIX 6.5.30 ``wacom`` STREAMS module for IP27,
  :src:`irix-wacom-6.5.30`, as a software expectation. Its command strings
  include ``~*F203C000``, ``AS1``, ``DE0``, ``PH1`` and ``NR%d``
  (:src:`irix-wacom-6.5.30 0xde68, 0xdf38, 0xdf40, 0xddf8, 0xdb80`). The
  order below is a model-run trace of the commands it sent to this model
  under IRIX 6.5.30 on the Onyx2, not a capture of a real tablet. At 9600
  baud it sends ``SP``, ``~#``, ``#``, ``SP``, ``~C``, ``SP`` and
  ``~*F203C000``, which moves the tablet to 19200 baud. It then sets the
  host line to 19200 baud and sends ``SP``, ``AS1``, ``PH1``, ``DE0``,
  ``HC0``, ``SP``, ``SP``, ``~M1``, ``~M5``, ``NR2540``, ``SU5`` and
  ``ST``. When the X server reopens the port it first sends ``$`` at 19200
  baud. ``AS``, ``DE`` and ``PH`` are listed for WACOM II-S only
  :src:`wtc-1002 p. 44`, yet the module sends them after selecting
  WACOM IV; the model ignores them.
- The X.org serial Wacom driver was not read, so nothing here is rated
  from it.

Identity
========

A UD-1212-R answers the model query with a string of the form
``~#Tablet_Model ROM_Version<cr>`` and the coordinate query with
``~Cxxxxx,yyyyy<cr>``, whose example is ``~C15240,15240<cr>``
:src:`wtc-1002 p. 20-21`, *established*. The model answers ``~#`` with
``~#UD-1212-R00 V1.3-5<cr>`` and ``~C`` with ``~C15240,15240<cr>``. The driver
accepts both answers, *software expectation*. The manual's own example
prints ``~#UD1212-R00 V1.1-0``, with no hyphen between ``UD`` and ``1212``
and ROM version 1.1 :src:`wtc-1002 p. 20`, *established*. The model's string
differs in both, an *approximation* chosen because the driver accepts it and
reads the ROM level as 13.

The UD-1212-R is one model of the series. The model presents it for every
guest, whatever the driver detects from the string, and does not model the
other sizes or the menu strip, each an *approximation*.

Line
====

The factory line is 9600 baud, 8 data bits, no parity, 1 stop bit
:src:`wtc-1002 p. 50`, *established*. The Setting body carries the tablet's
baud rate, parity, data length and stop bits :src:`wtc-1002 p. 47`,
*established*, so ``~*`` changes the line the tablet uses, and ``RE``, which
restores the factory defaults :src:`wtc-1002 p. 13`, returns it to 9600
baud 8N1 :src:`wtc-1002 p. 50`, *established*. A Setting
written with ``~*`` takes effect immediately, and the host must then set its
own port to match :src:`wtc-1002 p. 43`, *established*. The model takes its
line from the current Setting and applies a new one as soon as the ``~*``
command ends, as the manual says. It drops any bytes after that command in
the same write, *approximation*. The model ignores bytes written to it and
sends nothing while the host side of the line differs from the tablet's,
*approximation*: the manual requires the match but does not say what a
mismatched tablet does. It does not reset when the
host side changes, *approximation*.

Commands
========

A command ends in a carriage return :src:`wtc-1002 p. 37`, *established*. The
model also accepts a line feed, and answers ``~#`` without waiting for the
carriage return, because the Discreet driver sends it as a separate write
after ``~#``, *software expectation*. Both are *approximation*. ``#`` and
``$`` are the global resets to the WACOM IV and WACOM II-S factory defaults
:src:`wtc-1002 p. 19`, *established*, and both factory lines are 9600 baud 8N1
:src:`wtc-1002 p. 50, 52`, *established*. The model resets its Setting on
either, to ``E203C000,000,02,1270,1270`` for ``#`` and to C.4's
``A21BC800,000,00,1270,1270`` for ``$``, which returns the line to 9600 baud
8N1 :src:`wtc-1002 p. 50, 52`, *established*. The WACOM II-S command set
itself is not modelled, *approximation*. Its default is point mode, ASCII
output :src:`wtc-1002 p. 9, 52`, *established*, and point mode sends one
coordinate when a switch goes from off to on :src:`wtc-1002 p. 12`,
*established*; the model instead sends no coordinates at all after ``$`` until
a Setting selects WACOM IV, though it answers queries. The stock IRIX module
depends on ``$``: it sends it at 19200 baud when it reopens the port,
*software expectation*. The model parses the commands below. Every other
command is accepted and ignored without a reply, *approximation*, and counted
in a trace event.

.. list-table::
   :header-rows: 1

   * - Command
     - What the manual says
     - What the model does
   * - ``~#``
     - Returns the model and ROM version :src:`wtc-1002 p. 20`,
       *established*.
     - Replies with the model string, *approximation*.
   * - ``~C``
     - Returns the maximum coordinates :src:`wtc-1002 p. 21`, *established*.
     - Replies ``~C15240,15240<cr>``.
   * - ``~R``
     - Returns the current Setting as
       ``~Rdddddddd,ddd,dd,dddd,dddd<cr>`` :src:`wtc-1002 p. 24`,
       *established*.
     - Replies with the current Setting, which starts as the power-up one.
       That the starting value is the power-up Setting is *inferred* (see
       below); the reply's form is *established*.
   * - ``~R1``, ``~R2``
     - Return the Setting stored in M1 or M2 :src:`wtc-1002 p. 24`,
       *established*. The manual's example returns
       ``E202A000,010,02,1000,1000``, not the factory Setting.
     - Reply ``~R`` and the digit, then the power-up Setting, as no
       stored Setting is modelled, *approximation*.
   * - ``~*``
     - Rewrites the current Setting :src:`wtc-1002 p. 21`, *established*. The
       manual is silent on any reply, so whether there is one is *unknown*; a
       capture would settle it.
     - Rewrites the Setting from eight hexadecimal digits, optionally
       followed by ``,iii,tt,xxxx,yyyy``, and sends no reply. The longer form
       also sets the increment, interval and resolutions, *approximation*.
       A resolution field is taken only where ``NR`` would take the value,
       and otherwise the axis keeps its resolution, a model choice.
   * - ``RE``
     - Resets to the factory defaults, flushes the command buffer and loses
       any byte that arrives in the next 10 ms :src:`wtc-1002 p. 13`,
       *established*. The manual is silent on any reply, so whether there is
       one is *unknown*; a capture would settle it.
     - Resets the Setting and the mode and sends no reply, *approximation*.
       It discards the bytes after it in the same write as a stand-in for
       the loss window, *approximation*.
   * - ``SP``
     - Stops transmitting until ``ST`` or ``RQ`` :src:`wtc-1002 p. 14`,
       *established*.
     - Stops transmitting, and lets go of the console pointer,
       *approximation*.
   * - ``ST``
     - Begins transmitting :src:`wtc-1002 p. 15`, *established*. In remote
       request mode it cancels the mode :src:`wtc-1002 p. 13`,
       *established*.
     - Resumes transmission and leaves remote request mode, and takes the
       console pointer again, *approximation*.
   * - ``RQ1``, ``@``
     - Sends one coordinate pair regardless of the operation mode and switch
       status, and enters remote request mode, where the tablet sends only
       on further requests :src:`wtc-1002 p. 13`, *established*.
     - The same. It also ends an ``SP`` and takes the console pointer
       again, *approximation*.
   * - ``SUd1d2``
     - Sets suppressed mode with an increment :src:`wtc-1002 p. 16`,
       *established*.
     - Sets the increment, in counts at the logical resolution,
       *approximation*: the manual does not give the unit.
   * - ``ITd1d2``
     - Sets the interval in units of 5 ms; 0 is the maximum for the line
       :src:`wtc-1002 p. 10`, *established*.
     - Sets the interval.
   * - ``NRd1d2d3d4``
     - Sets the logical resolution of both axes :src:`wtc-1002 p. 11`,
       *established*.
     - Shows it in the ``xxxx,yyyy`` fields of the Setting extension that
       ``~R`` returns :src:`wtc-1002 p. 42`, *established*, and leaves the
       body's resolution bits 16 and 17, which encode only 1270, 1000, 508
       or 500 lpi :src:`wtc-1002 p. 47`, unchanged, *approximation*. It
       scales the coordinates, below. The manual's maximum for ROM 1.2 and
       later is 2540 :src:`wtc-1002 p. 11`, and the model ignores a value
       above it, *approximation*. It also ignores 0 and below, a model
       choice: the manual gives a minimum of 1 only for ROM versions below
       1.2.

The power-up Setting is ``E203C000,000,02,1270,1270``, *inferred*. That is
the one ``~R`` response Appendix C.2 gives for the UD series
:src:`wtc-1002 p. 50`, but C.2 does not mark it as the power-up value. The
ArtPad table in C.3 gives a different response on power up and after ``#``
:src:`wtc-1002 p. 51`, and the appendix notes that the defaults after a
global command "may vary slightly from the power up defaults"
:src:`wtc-1002 p. 49`. A ``~R`` captured from a real UD tablet after power up
would settle it.
The ``02`` field of ``~R`` is not the interval the tablet applies: the same
appendix lists the power-up transfer rate as the maximum, so the model sends
at the line's maximum rate until an interval is set, *approximation*. A
reset also clears the Setting to this value.

Transmission
============

The manual's factory defaults for a UD tablet are suppressed mode with
increment 0, binary absolute output and the maximum transfer rate
:src:`wtc-1002 p. 50`, *established*. ``RE`` resets to those defaults
(:src:`wtc-1002 p. 13`) and ``SP`` makes the tablet "cease to send data
until it receives an ST or an RQ command" (:src:`wtc-1002 p. 14`),
*established*. Taken together, a UD tablet transmits at power up and after
``RE`` without being sent ``ST``, *inferred*. Two steps are assumptions. The
manual never says that the state at power up is the factory defaults: it
heads Appendix C "Factory (Power Up) default settings" and nothing more,
and its own footnote says the defaults after a global command "may vary
slightly from the power up defaults" :src:`wtc-1002 p. 49`.
And ``ST`` "begin[s] transmitting", which a reader could take to mean the
tablet does not transmit before it, although ``SP`` is the only command that
the manual says stops it. No sentence of the manual says a tablet starts
sending by itself. A capture of a real UD tablet after power up, or a
sentence in the manual that says so, would settle it. The model transmits
from creation and after ``RE``, and ``ST`` or ``RQ`` after ``SP`` resumes it,
*approximation*. A driver that sends ``ST`` first sees no difference: the
Linux driver and the stock IRIX module both do, *software expectation*, and
the Discreet driver does not, *software expectation*.

Suppressed mode "transmits coordinate data only if" a switch changes state
in either direction or the increment given with ``SU`` is exceeded
:src:`wtc-1002 p. 16`, *established*. The model sends one packet for each of
those and nothing else: it sends no packet for a pointer that is still, and
none for proximity, which the manual does not list under ``SU``,
*approximation*. With increment 0 any change of a coordinate counts. The
manual says the sent position is the last one that met the increment; the
model sends the position at the time of sending, *approximation*. A driver
that moves the pointer therefore gets a packet for each movement, which is how
the Discreet driver's cursor follows the pointer, *software expectation*.
Stream, switch stream and point mode (``SR``, ``SW``, ``PO``) are accepted
and ignored, so the model stays in suppressed mode whatever the driver asks
for, *approximation*. The Discreet driver sets none of them while it sets up
the tablet and while its cursor tracks the pointer, *software expectation*;
what its later controls send was not captured.

At the maximum rate the line carries one 7-byte packet per 70 bit times,
136 packets per second at 9600 baud :src:`wtc-1002 p. 10`, *established*.
The model spaces packets by that or by the ``IT`` interval, whichever is
longer, on the virtual clock. A packet that is due while the guest has not
read the previous one waits, and a position that changes while one waits is
sent in its place.

The model never leaves proximity: the console pointer is always in range,
*approximation*.

Packet
======

The packet is the WACOM IV binary format of ROM version 1.2 and later, 7
bytes :src:`wtc-1002 p. 26, 30`, *established*:

.. list-table::
   :header-rows: 1

   * - Byte
     - Bits 6 to 0 (bit 7 is set in byte 1 and clear in the rest)
   * - 1
     - Proximity, pointer, button flag, X15 and X14
   * - 2
     - X13 to X7
   * - 3
     - X6 to X0
   * - 4
     - B3 to B0 (the switch number), P0, Y15 and Y14
   * - 5
     - Y13 to Y7
   * - 6
     - Y6 to Y0
   * - 7
     - Sp, then P6 to P1

The model sets proximity and the stylus pointer bit, and the button flag
whenever the switch number is not zero. ``~C`` gives the maximum
coordinate at 0.02 mm, 15240 on a UD-1212-R :src:`wtc-1002 p. 21`,
*established*. The factory resolution is 1270 lpi :src:`wtc-1002 p. 50`,
*established*, and 0.02 mm per count is the same 1270 lpi by conversion
(25.4 mm / 1270). ``NR`` sets the logical resolution in lines per inch
without changing the physical one :src:`wtc-1002 p. 11`, *established*.
The model reads packet coordinates as counts at the logical resolution, so
the far edge is the ``~C`` value times the resolution over 1270,
*inferred*: 15240 at the factory 1270 and 30480 at ``NR2540``, the same
304.8 mm either way. The manual conflicts with this. It calls the ``~C``
value the highest possible at 0.02 mm and the most "that will ever be
transmitted by the tablet" :src:`wtc-1002 p. 21`, while the model sends up
to 30480 at ``NR2540``. The manual does not say how a resolution finer than
0.02 mm reports. A capture of a real tablet at ``NR2540`` would settle it.

The stock IRIX 6.5.30 module sends ``NR`` from its ``NR%d`` format
(:src:`irix-wacom-6.5.30 0xdb80`), with the value capped at 2540 or 1270
by its UD set-up routine (:src:`irix-wacom-6.5.30 0xbde4, 0xbe5c`),
*software expectation*. On this model under IRIX 6.5.30 on the Onyx2 it
sent ``NR2540``, and its record held 0 to 30480 (0x7710) as the range of
both pointer axes, read from the running kernel, *software expectation*
observed on the model. The offset that writes 30480 was not traced. With
the model sending at most 15240 there, the IRIX cursor reached only the
top-left quarter of the screen, *inferred* from a model run. The model
scales the console's 0 to 32767 range onto the far edge on both axes,
*approximation*: a real tablet's active area is what the stylus can reach,
not the console's.

The pressure is an 8-bit two's complement value spread over P0 and Sp with
P6 to P1: P0 is bit 2 of byte 4, and byte 7 holds Sp and P6 to P1
:src:`wtc-1002 p. 30-31`, *established*. The manual says the no-pressure
value is about -120. The model reports -120 with the pen up, which puts
byte 7 at ``0x44`` and P0 at 0, *approximation*: it is the manual's
approximate figure, and the driver reads it as 7 of 255, *software
expectation*. The left button reports switch number 1 and +120, which sets
byte 7 to ``0x3c`` and reads as 247 of 255 in the driver, *software
expectation*.

The manual defines switch numbers 0 to 5 for a stylus and the button flag
that goes with them :src:`wtc-1002 p. 31-32`, *established*. It does not say
which pen button gives which number. The model gives the left button switch
1 with pressure +120. The middle and right buttons give the switch numbers 2
and 4, or 3 and 5 together with the left button, at the pen-up pressure
unless the left button is down, and the right one wins when both are down. The
mapping is a model choice, *approximation*; no driver expectation was found
for it.

Console pointer
===============

The model registers as an absolute pointer with the console. It takes the
pointer only once the guest has set the host side of the line to the
tablet's line and sent a command, and it lets go on ``SP`` or when the two
lines differ,
*approximation*. A tablet that held the pointer from creation would starve
the PS/2 mouse whenever no guest driver had the serial port open. The manual
does not describe the console side. ``RE`` and ``ST`` take the pointer
again.

Model limits
============

- The model is a UD-1212-R only; there is no tilt, multi mode, menu strip,
  ASCII or relative format, EEPROM settings or out-of-proximity data.
- ``AS``, ``DE``, ``FM``, ``IN``, ``OC``, ``PH``, ``PO``, ``SR``, ``SW`` and
  every other command not listed under Commands is accepted without effect.
- The tablet keeps its Setting and state only in the host process; there is
  no migration state.
- It cannot be created with QMP ``chardev-add``, because it has no schema
  entry. Use ``-chardev``.

.. _sgi-wacom-iv-tests:

Tests
=====

``tests/qtest/wacom-iv-test.c`` drives the backend through a 16550 on the
Malta machine. It covers the ``~#``, ``~C`` and ``~R`` replies; silence after
``RE`` and after ``~*``; ``~*`` and ``RE`` against ``~R``; transmission from
power up with no ``ST``; the pen-up pressure and byte 7; the left button's
switch and pressure; no packet for a still pointer; coordinates at
``NR2540`` and ``NR1270``, out-of-range resolutions from ``NR`` and ``~*``
and ``#`` restoring 1270; the ``SU`` increment;
the ``IT`` interval; ``SP``, ``ST`` and ``RE`` with the pointer ownership
shown by ``query-mice``; ``RQ1`` and ``@``; the command sequences of the Discreet driver, the
stock IRIX module (including its move to 19200 baud and ``RE``'s return to
9600) and the Linux driver; and silence on a wrong baud rate or parity.
