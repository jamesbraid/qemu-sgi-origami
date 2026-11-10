.. SPDX-License-Identifier: BSD-3-Clause

.. _sgi-l1-power-bay-link:

=====================================
L1 power-bay link: second serial port
=====================================

Used in :ref:`sgi-ip35` (Origin 300 and Fuel), :ref:`sgi-ip53` (Origin
350, Onyx 350, Tezro and the Onyx4 host), the Origin 300 PCI expansion
module (:doc:`pci-expansion`) and :term:`NUMAlink` module
(:ref:`sgi-bricks-numalink-module`), and the Origin 3000 and Onyx 3000
C-, I-, N-, V-, R- and CX-bricks (:ref:`sgi-bricks`), which the
``origin3000`` machine's topologies build.

.. seealso::

   Platform pages: :ref:`sgi-ip35`, :ref:`sgi-ip53`.

.. seealso::

   - :doc:`The L1 page <l1>`: the controller's identity, UART and
     console (:ref:`sgi-l1-console`).
   - :doc:`pbay`: the power bay.

This page is the Origin 300 L1's second serial port, which reads a power
bay: the receive path, the initialisation and polling task, and the
``pbay`` command. The bay itself is described on :doc:`pbay`, and the rest
of the L1 on :doc:`the L1 page <l1>`.

.. _sgi-l1-pbay:

Power-bay link
==============

The Origin 300 L1 package reads a power bay over its second serial port
(software port 1, the processor's on-chip UART 1, *inferred* from the
register block's address), at 19200 baud, 8 data bits, no parity and no
flow control, *software expectation*. The port's interrupt handler frames
the traffic in a mode the power-bay task selects: a transmission is a
start byte, the body with its checksum and the three framing bytes
stuffed, and a terminator; the transaction code above the handler works
on unframed bodies, *software expectation*. The link, its commands and the
bay's replies are described on :doc:`pbay`. The L1 starts the power-bay
task by the brick's type letter. In the package the Origin 3000 compute
brick uses it does not start it for ``0``, ``4``, ``7``, ``8``, ``9``,
``A``, ``E``, ``G``, ``O``, ``Q`` or ``V``, and the port is closed on all of
them but ``G`` and ``O``; on ``G`` the second port is a console at 38400
baud (setting word ``0x9600``, where 19200 is ``0x4b00``) instead of the bay
link, *software expectation*. Two other packages carry no letter table
and start the task for every letter except ``O``, ``G`` included although
port 1 holds the console there; neither lists ``G`` as a supported brick, so
the case does not arise, *inferred*. The model follows the first set.

The controller has an opt-in ``pbay`` character device property carrying the
framed bytes; ``pbay-address`` (default ``0x0001``) is the L1's own address
and ``pbay-poll-ms`` (default 0) overrides the poll period for tests. The
default period is the source material's, 6000 + 100 * bay ticks (:ref:`below
<sgi-l1-pbay-poll>`). ``pbay-tick-us`` is described under the receive path.
With the property unset, the controller behaves as it did before and the
link does not exist. With it set, the controller refuses to
start on a brick type that does not run the task, and blocks migration, because
the bay's state lies outside the machine.

Receive path
------------

The model follows the handler as read: it ignores every byte before a start
byte, drops a start byte inside a frame without restarting the frame,
unstuffs ``0xaa`` followed by ``0xb0``, ``0xb5`` or ``0xba`` and drops any
other byte after ``0xaa``, ends the frame at the terminator and discards
an empty one, *software expectation*. A frame whose first two body bytes
(little-endian) are neither ``0x0000`` nor the L1's address is dropped to
its terminator, and the checksum is not checked in this path but by the
transaction layer, *software expectation*. A stray ``0xaa`` before the
start byte puts the receiver in its escape state, which drops the next
start byte and everything up to a terminator, *software expectation*. A
terminator that ends a frame while an escape is pending clears the escape
as well, the same reset a terminator gives outside a frame; the handler's
treatment of that case is not read, so this is an *approximation*. The
handler does not act on UART line errors in this mode, *software
expectation*. Every request allows three transmissions with up to three
reads each. A read that times out goes to the next transmission, which waits
20 ticks first; a frame that fails the reply checks costs a read and the next
read is made without resending, *software expectation*. A read lasts 1 to 2
periods of 100 ticks, *software expectation*; the model gives each read 150
ticks, an *approximation* because the phase is *unknown*. A tick is 10 ms
(*inferred* from the 100 Hz timer); the ``pbay-tick-us`` property (default
10000, at least 1) sets it in microseconds, so tests can shorten every tick
timing at once. The first start-up request, command ``0x10``, has no reply
and is sent once: the source material does not say whether it is sent three
times, and no reply could be accepted for it, an *approximation*. A write that
finds a byte waiting in the UART is retried after a tick in the L1; the model's
receiver is never busy, so that retry is not modelled. The timers run on the
host's real-time clock, because the bay is a host peer that answers in real
time and the virtual clock is stopped while the guest is halted, so under
``-icount`` or record and replay the link's timing is not deterministic.

Initialisation and polling
--------------------------

The model starts the link when the controller starts, when it is reset and
when ``reboot_l1`` restarts it (a C-brick's L1 is not reset with the
machine, so only that command restarts its link). The task
sets the status to "busy initialising" and waits ``100 * bay + 300`` ticks,
``bay`` being the controller's ``bay`` property (L1 setting 2), before its
first request: 3 s plus 1 s per bay number at 10 ms a tick, *software
expectation* for the formula and *inferred* for the tick. It sends command ``0x10``
and then ``0x02`` to address 0 and takes the replied address, with zero or no
reply meaning a communication failure, then ``0x12`` with that address,
``0x13``, the ``0x00,0x00`` pair and the record loop from id 0, reading five
bytes of each record to learn its id and then the whole record, until the
next id is ``0xffff``, *software expectation*. It sets the status to
"busy initialising" while it runs, to "ready" when it finishes and to
"communication failure" when a transaction fails after its attempts. A reply
is accepted when its checksum holds, its control byte has the reply bit and
the request's tag, its command matches and its completion code is zero, and
for a tunnelled request when the inner command and completion code
match and the network function is the request's plus the response bit,
*software expectation*.

Of the three capability bytes, byte 1 when nonzero is stored as the fallback
FRU device of slot 0, used until a FRU locator for entity ``0xb0`` replaces
it; byte 2 when nonzero runs the record loop, *software expectation*. The
model reads the loop's records for what the L1 takes from them,
*software expectation* for the use and *inferred* for the field offsets in
the record (they follow the public IPMI sensor and FRU-locator record
layouts): a sensor record of entity ``0x0a`` and sensor type 8 maps its
sensors to supplies 1 to 6, one of entity ``0x15`` and type 9 to DC ports 1 to
8, and a FRU locator of entity ``0x0a`` (instances 1 to 6), ``0xb0`` or
``0x15`` gives the device of FRU slot 1 to 6, 0 or 7. The records do not say
how many sensors a record covers, so the model takes six and eight, an
*approximation*. The count of records read is exposed as ``pbay-records`` and slot 0's
device as ``pbay-fru-slot0``.

.. _sgi-l1-pbay-poll:

The polling task waits on its queue for ``6000 + 100 * bay`` ticks (60 s plus
1 s per bay number at 10 ms a tick, *software expectation* for the formula,
*inferred* for the tick). A request to the bay restarts the wait and a
timeout runs the poll. The model arms the wait when the link becomes ready
and after every command or pass that uses the bay; a change of the
monitoring setting is not a request to the bay. The poll does nothing, and
the wait starts again, unless the environment-monitoring setting (L1
setting 85, four bits) is nonzero, the status is ready and no other request
holds the bay; it sets the status to "busy" while it runs. The setting is
zero at power-on, so a controller with a bay polls nothing until ``pbay env
on`` stores ``0x0f`` (*software expectation* for the store, *inferred* that
nothing else sets it). Bit 0 of the setting enables the presence and AC-fail
tests, bit 1 the supply-fail test, bit 2 the predictive-fail test and bit 3
the DC ports, *software expectation*. ``pbay-poll-ms``, when nonzero,
replaces the period for tests; ``pbay-tick-us`` scales it otherwise.

A pass reads the supplies' sensors (when any of bits 0 to 2 is set) and the
ports' (bit 3), and each message goes to the L1 log, *software expectation*.
The line texts begin with ``Power Bay DPS n`` or ``Power Bay DCport n`` and
carry the words the polling task logs. The model keeps the last value of each
sensor, and nothing is stored at start, so a fault already present on the
first pass counts as new. For sensors 1 to 6 (``DPS`` 1 to 6) bit 0 set means
not inserted and clears the stored value, so a returning supply's faults are
new. Per supply the first matching test wins, so a pass logs at most one
fault for it: AC fail (bit 3 newly set), then fail (bit 1), then predictive
fail (bit 2). Fail and predictive fail are logged at once. An AC fail is
logged on the fourth consecutive sighting, with three immediate re-reads of
all six supplies between; a supply whose bit is gone at any re-read is not
logged. For sensors 7 to 14 (``DCport`` 1 to 8) a port whose bit 0 is set is
skipped and keeps its stored value, and bit 1 newly set on a port is logged
on the third consecutive sighting, with a re-read of the ports after 100
ticks. The source material does not say whether the two port re-reads are one
wait each or one wait in all, nor whether a supply with an AC candidate that
fails the debounce still lets the fail test run; the model waits 100 ticks
before each of two re-reads and lets the AC candidate take the supply's turn,
an *approximation*. A consequence of that choice: a supply with a new AC-fail
bit that then fails the debounce, and whose fail bit is also set, logs
neither fault on that pass, and the fail is new again only if its bit goes
from 0 to 1 later. The fail and predictive tests use the last reading.

The set of supplies with bit 0 clear is compared with L1 setting 105, the
expected-supply mask (bit n-1 for supply n; zero means no presence messages).
A supply that leaves it is logged as removed and one that arrives as
inserted, then the present set is written back when it differs. So the first
pass on a fresh record logs nothing and stores the baseline, and a pass that
finds no supply present is a failed read that skips the step, *software
expectation*. A supply whose single read fails (no answer after the retries)
is treated like one that is not present for that pass, so with the other
supplies answering it is logged as removed and as inserted when it answers
again; how the task treats one failed read is not in the source material, so
this is an *approximation*. Settings 85 and 105 are the L1's, so a restart of the
link keeps them; the previous readings are RAM and are cleared. The model does
not migrate any of this, because a configured link blocks migration.

Each message goes through the L1's event formatter, which adds two numbers
and sends an event; the wrapper's text and the numbers' meaning are *unknown*.
The model logs the bare message to the L1 log only, and logs ``LOG_UNIMP``
for the wrapper.

The ``pbay`` command
--------------------

The operator command is accepted on the brick types ``C356P%#=IXYRN&!``
(the link itself opens on more), *software expectation*; on another type the
model prints ``ERROR: command not found.``, because the refusal's text is
*unknown*. Its fourteen forms share one handler, *software expectation*.
The texts below paraphrase the source material, so the column heads and widths
are an *approximation*; the rows' words and their order are *software
expectation*.

- Whatever the form, a link that is not ready prints its state text
  (``PBay not initialized``, ``PBay busy initializing``, ``PBay busy try
  again later``, ``PBay busy flashing``, ``PBay communication failure`` or
  ``PBay state unknown``) and the command fails. A request that finds the
  bay held by a polling pass or another command prints the busy text. In
  state 5 ``pbay init`` runs the initialisation order again and prints ``PBay
  ready`` or the state text; in any other state it prints ``PBay already
  initialized``. The model does not repeat the task's start delay, an
  *approximation*.
- ``pbay version`` sends link command ``0xc0`` and prints ``PBay FW Version
  00.18, Built 09/29/2000 14:00:00``: the version bytes second then first,
  and the build time read as a Unix time in UTC (*inferred*). With no reply
  it prints ``PBay version unavailable``, an *approximation*: the source
  material gives no text.
- ``pbay dps [digits]`` and ``pbay dcport [digits]`` read the mapped sensors
  of the supplies 1 to 6 or the ports 1 to 8 (or the digits given) and print
  a header and one row each. A supply row is ``Ok`` or ``Low`` (bit 3),
  ``220Vac`` or ``110Vac`` (bit 4), ``Ok`` or ``Failing`` (bit 2) and ``Ok`` or
  ``Failed`` (bit 1), or ``Not_Present`` alone (bit 0). A port row is ``48V
  Off`` (bit 0) or ``48V On, `` with ``Ok`` or ``Failed`` (bit 1). An
  unmapped supply or port prints nothing and a failed read prints ``**read
  error**``. The string that follows a port's row when link command ``0x0b``
  returns the port's number is *unknown*: the model does not ask and logs
  ``LOG_UNIMP``.
- ``pbay fru [n]`` reads the FRU device of each slot (slot 0 the bay, 1 to 6
  the supplies; a negative ``n`` selects the seventh) with the streamed read
  and prints manufacturer, product name, part number, version, serial and
  the manufacturing date from the FRU's common header, product area and
  board area. A bad checksum prints ``**FRU checksum error**``. The
  source material does not say how the L1 addresses or sizes the reads,
  which area each column comes from, or the labels of the first column; the
  model reads 128 bytes from address 0, takes the columns from the product
  area and the date from the board area, labels slot 0 ``Bay`` and the
  seventh slot ``Card``, and prints the date as month, day and year. All of
  that is an *approximation*. The bay model returns zeros, so against it the rows read ``**FRU
  checksum error**``.
- ``pbay reset`` sends the pair ``0x18,0x03`` and prints ``Ok`` or
  ``Timeout``.
- ``pbay env`` prints ``Power Bay env monitoring enabled`` or ``disabled``, a
  blank line, and the ``dps`` and ``dcport`` tables; ``pbay env on`` and
  ``off`` print nothing and store ``0x0f`` or 0 in setting 85.
- ``pbay serial`` prints the serial in slot 0's product area (*inferred* to
  be the fifth field), nothing when there is no slot 0. The L1 caches it at
  start-up; the model reads it on first use, then keeps it until the link
  starts again, an *approximation* with the same output.
- Bare ``pbay`` prints a line per FRU device with a current in amps. Its
  arithmetic and record selection are *unknown*: the model prints nothing and
  logs ``LOG_UNIMP``.

A command that needs the bay finishes when the bay has answered, and the
prompt follows. Over task 3 the reply carries as many output lines as fit its
121 bytes of fields (each line takes its text plus two bytes), whole lines
only, and blank lines are dropped. A command relayed from another
controller (``ctc``) cannot wait for the bay and prints an error, logging
``LOG_UNIMP``. A request restarts the polling task's wait.

The flash task and its ``0x92`` to ``0x94`` requests are not modelled. The
read-only properties ``pbay-status``,
``pbay-records``, ``pbay-passes`` and ``pbay-fru-slot0`` expose the state for
tests.
