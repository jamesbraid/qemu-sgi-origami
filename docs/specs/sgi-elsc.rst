==========================================
SGI Origin 200 module system controller
==========================================

The module system controller (MSC, historically "ELSC") is a
microcontroller on the Origin 200 midplane, reached over the module's
I2C bus. Guests use it for console input before any real serial
driver is running, for reading the front-panel DIP switches, and as
the console of last resort after a failed power-on diagnostic. QEMU
models it as an I2C slave (``hw/misc/sgi-elsc.c``) on the same bus as
each node's PCF8584 controller.

The register-level sources behind every value here are collected in
the Origami project tree's controller datasheet; this page describes
the contract the model implements.

ASCII command protocol
======================

Every command is a short ASCII mnemonic carried in one combined
write-then-read I2C transaction. The write half frames the request;
switching the bus direction to read marks it complete and produces
the response, framed the same way:

* one length byte, counting only the text;
* that many bytes of ASCII text;
* one checksum byte, chosen so that the whole packet -- length
  through checksum inclusive -- sums to zero modulo 256.

The text length may not exceed the protocol maximum. The firmware's
transfer-length tracking is unclamped, so a longer frame makes it
read past its own buffer before any check runs; the model asserts
the bound on everything it emits.

A malformed packet -- short, long, or failing the checksum -- is
answered ``err csum``. The protocol names no separate framing error:
a malformed packet fails the same sums-to-zero test a corrupted
checksum does.

Command vocabulary
------------------

``ver``
    Answered ``ok`` plus a version token. The firmware requires the
    ``ok`` but does not interpret the token; the first token
    including its terminator must fit in eight bytes.

``rsw``
    The front-panel DIP switches, reported active-low, as
    ``ok <two hex digits>``.

``get``
    The console-input poll. The response payload begins with a fixed
    six-byte ``acp <selector> `` header; the firmware checks only
    the first four bytes and copies the payload starting at byte
    six, so the shape must be exact. The model reaches only
    processor 0, so the selector is always ``0``.

``acp <selector> <characters>``
    The console-output flush, at most 86 characters per transaction.
    Always answered in the ``ok <count>`` form with the count in
    hexadecimal: a bare ``ok`` parses as zero characters accepted
    and livelocks the firmware's output queue.

``pas <4 characters>``
    Supervisor-mode gate for protected commands. The factory default
    password is ``none``; the model implements no way to change it.

``dsp <message>``
    Writes a message onto the alphanumeric display: up to eight
    ASCII characters other than nul, overwriting from position 0 and
    leaving the remaining positions unaltered. Answered a plain
    ``ok``.

``dsc <position> <character>``
    Writes one display character; the position is a digit from 0 to
    7, and the character is any ASCII character other than nul -- a
    space included, so the argument bytes are read by position
    rather than split on whitespace. Answered a plain ``ok``.

``pwr q``
    The power-on query, which the PROM issues once at power-on to
    detect a cold start. Answered ``ok 1`` on the first read in a
    power cycle and ``ok 0`` afterwards. The model latches a cold
    power-on and clears the latch on read, so only the first caller
    after power-on sees the 1; the latch is set when the controller
    powers up and is not re-armed by a warm reset. The controller
    datasheet records this clear-on-read behaviour as 6.5.7-derived and
    unconfirmed for the 6.5.30 firmware; one measured exchange would
    settle it. The latch is per-controller: on the single-module Origin
    200 that is the whole machine, and a multi-module machine has one
    controller per module (each genuinely cold-powered), with the boot
    master reading its own module's latch once and propagating the
    result. Whether a multi-module cold-power latch is per-module or
    machine-wide is not settled by the datasheet.

Anything else is answered ``err cmd``. That reply is load-bearing:
the firmware's console-selection routine issues a DIP-switch read
before it ever probes the controller as a console, and it tolerates
a negative reply but not a stalled bus.

Alphanumeric display
--------------------

The controller drives the module's front-panel eight-character
alphanumeric display. The SGI Origin2000 and Onyx2 hardware guide
documents both display commands, neither in its
supervisor-restricted set, and shows what firmware writes there:
per-node hex status codes, placed a character at a time -- which is
how a launching node's slave processor uses ``dsc`` during
bring-up, so answering ``err cmd`` wedged a two-node boot with the
slave retrying its status write forever. Malformed arguments are
answered ``err arg``. The manual does not state the power-on
content (the model blanks it to spaces) or what an over-length
``dsp`` message does (the model rejects it); ``info elsc`` shows
the current content.

Bus arbitration
===============

The controller and every node's Hub share one I2C bus with no real
multi-master mechanism. The controller arbitrates by broadcasting a
rotation of per-processor signal bytes; a processor polls its
PCF8584's data register for its own signal before starting a
transaction.

The signal pair for a processor code ``c`` is ``0xE0 + c``
(signal 1, "your slot, nothing waiting") and ``0xE0 + c + 8``
(signal 2, "your slot, something waiting for you"). The code must fit in
three bits: signal 2 is signal 1 with bit 3 set, so a code of eight
or more would collide with another processor's signal 1. Eight codes
is also exactly the protocol's addressable space -- four node slots
of two processors each. All eight codes are offered in turn whether
or not the machine populates them, as a real controller doling out
time slices does.

Two properties of the rotation are contracts, not conveniences:

* A gap -- a broadcast of neither signal value -- precedes every
  signal. The firmware's wait loop requires its signal to be absent
  before it trusts a subsequent appearance of it.
* Signal 2 is presented only while something waits for the slot's
  processor: console input, for the console processor, or a pending
  response, for its requester. The console case is what tells the
  driver a ``get`` will find something -- the driver never issues
  ``get`` without having seen it -- and it is the special case of
  the general rule, observed from the 6.5.30 firmware's slave
  bring-up, which parks a launching processor on its own signal 2
  until the controller announces the response it has yet to read.

A response belongs to its requester -- the processor whose request
the direction switch completed -- and it outlives the transaction
that produced it: a requester may end its transaction with the
response unread, poll for its own signal 2, and fetch the frame with
a read-only transaction. The response stays pending, and its owner's
slot keeps presenting signal 2, until it is read to its end or a new
request replaces it. A request from a context that carries no
processor -- a monitor or qtest access -- is attributed to the
console processor.

Each signal is held for a slot of at least ten milliseconds,
re-broadcast throughout so that a poll at any cadence finds it, then
withdrawn behind a fresh gap as the rotation moves to the next code.
Each re-broadcast recomputes the signal 2 half, so a slot whose
promise has been kept mid-slot -- the response consumed, the queue
drained -- downgrades to signal 1 rather than announcing something
no longer there.
The rotation is purely timer-driven: no bus traffic ends a hold
early. The model deliberately does not model a processor answering
its slot, because on the bus a waiting processor's periodic command
retries are indistinguishable from its answer -- ending the hold on
them withdrew a signal microseconds after each presentation, before
the waiting processor's own poll could sample it.

Slot lengths vary, from ten to eleven point six milliseconds, as a
fixed function of the code and a revolution counter. Both the hold
and the variation exist to remove races the model itself created:
guest polling loops and the rotation run on the one deterministic
virtual clock, so a signal presented once on a fixed beat could
phase-lock with a poll and leave a processor never sampling its own
slot, and even held slots on a metronomic ring let a polling loop
whose period is commensurate with the ring read the same slot at
the same phase on every revolution -- observed as a poll pinned to
one code while the rotation ran beneath it. On hardware the
controller's and the processors' oscillators are unsynchronized and
drift; the varying slot length models that non-synchronization with
a pure function of model state, so record/replay and migration stay
exact where real drift would not.

The model's timing values are choices, not measurements: the gap
need only be observable by a polling guest, a held signal stands
far past the 250 microsecond threshold the datasheet specifies, and
the roughly ten millisecond slots only need to be impossible for a
plausible polling cadence to miss while keeping the full eight-code
round well inside the deadlines the polling firmware applies.
Whether the real controller holds a signal until acknowledged, and
how it paces its rotation, is unobserved; settling it would take
bus traces from a real machine. When the bus is busy with a real transaction the
rotation defers by short retry rather than through the I2C core's
pending-master queue, because the PCF8584 model drives transactions
directly and never calls ``i2c_bus_release()``.

AUX serial port
===============

The controller is the far end of its module's AUX serial port. Every
module carries the same serial connectors, so the ``-serial`` lines
are node-major with a fixed stride of four: node *n* owns the block
``[4n, 4n+4)``, and the AUX port is the third line in each block.
Node 0's AUX is index 2, node 1's is index 6 (node 0's index 3 is the
bring-up UART, which exists on node 0 only). Its one multiplexing
rule (Origin 200 Owner's Guide, page 132): a byte sequence beginning
0x14 is a command to the controller itself and is never queued for
``get``; every other byte is console input. Nothing read for the
datasheet states what ends an escaped command; the model discards
through the next line terminator as a model choice.

The firmware redirects its console to this port after a failed
power-on diagnostic. A guest flushing console output here with no
character device attached is therefore losing diagnostic output; the
model warns once and keeps serving the protocol.

The PCF8584 controller
======================

Each node reaches this bus through a Philips PCF8584 embedded in its
Hub (``hw/i2c/pcf8584.c``), which QEMU models against the Philips
product specification. Two addresses, selected by the A0 pin, sit
eight bytes apart the way the Hub wires them. Register selection is
asymmetric: ESO chooses between the data register and the
configuration registers, but ES2 keeps selecting either way -- with
the serial interface enabled it still chooses between data and the
interrupt vector, and only ES1 stops meaning anything. Reads of the
data register advance the receive pipeline, and the firmware issues
eight-byte accesses, so the register's implementation width is eight
or the core would split one access into two side effects.

The model's interaction with QEMU's I2C core carries three
contracts. The core rescans for a slave only when none is matched,
so a repeated start to a *different* address (the protocol's closing
invalid-address step) must end the old match first; ending it
unconditionally would deliver a finish to a slave still mid-command.
A refused same-address repeated start is left open by the core for
the caller to end, and held open it keeps the bus busy forever
against the arbitration broadcast. And a send over the core's empty
matched list reports success, so a refused start is remembered
rather than asked again -- the firmware's closing step, addressed to
0x7f with nothing there, must read back NAK.

Monitor access
==============

``info elsc`` prints each controller's console-input queue, the
alphanumeric display content, any pending response and its owner,
and the arbitration signal currently presented for the console
processor, without requiring a trace backend or diagnostic sink.
