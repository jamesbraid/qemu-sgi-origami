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

``pwr d <sec>`` / ``pwr c`` / ``pwr r``
    The remaining power control, none boot-driven. ``pwr d <sec>``
    schedules a delayed power-down and ``pwr c`` power-cycles, both
    answered a bare ``ok``; both are destructive and supervisor-gated
    like ``rst`` (``err perm`` without supervisor mode). The power-down
    delay is hex seconds: 0 is immediate, otherwise the guide's range
    0x5 to 0x258 (5 to 600 seconds); an out-of-range or non-hex delay is
    ``err arg``. ``pwr r`` queries the redundant supplies, is read-only
    and ungated, and answers a per-module record for each supply tagged
    ``tag:present:health`` -- ``m:y:ok`` / ``m:y:fl`` / ``m:n`` for the
    master and ``s:y:ok`` / ``s:y:fl`` / ``s:n`` for the slave. The present
    flag (``y``/``n``) separates an absent supply (``m:n``) from a failed one
    (``m:y:fl``), and health is ``ok`` or ``fl``. Both supplies are modelled
    present and healthy, so ``ok m:y:ok s:y:ok``. The response payload is
    identical on the operator and wire interfaces -- only the command framing
    differs -- so this is the format the Origin 200 diagnostics guide
    documents (datasheet PR #602); it supersedes an earlier one-colon
    ``ok m:ok s:ok`` that came from the driver's test-fixture mock rather than
    the firmware wire format. ``pwr d`` and ``pwr c`` then act, like
    ``rst`` deferred until the caller has read the ``ok``: ``pwr d`` shuts
    the machine down, ``pwr c`` resets it (a power-cycle). The scheduled
    delay is not modelled -- the shutdown follows the ack. Both supplies
    are modelled healthy. An unknown subcommand letter is ``err cmd``, but
    a known letter with a bad or extra argument is ``err arg``.

``hbt [<interval> <delay>]``
    Heartbeat / NMI-watchdog control, which the kernel enables during
    controller init. ``hbt <interval> <delay>`` enables monitoring so a
    missed heartbeat raises a non-maskable interrupt after the delay,
    ``hbt 0`` disables it, and ``hbt`` alone sends one heartbeat; all
    answer a plain ``ok``. The only valid one-argument form is ``hbt 0``;
    a single non-zero argument is ``err arg``. The two arguments are
    decimal, unlike the hex arguments elsewhere, and a malformed one is
    ``err arg``. The model validates the request and arms no timer -- the
    kernel only enables the watchdog, so no missed-heartbeat interrupt is
    modelled.

``rst``
    The module reset, reached by a panic reboot or fatal-error
    recovery, never a normal boot. Supervisor-gated: without supervisor
    mode it answers ``err perm``. Once authenticated it answers ``ok``
    and requests an immediate machine reset, deferred until the caller
    has read the ``ok`` so the ack is never lost. There is no
    delayed-reset form -- the delay belongs to ``pwr d``. A caller
    treats ``ok`` as proof the reset happened and does not fall back to
    its own hardware reset, so the reset is performed rather than
    logged. The datasheet's reset is module-scoped; on the single-module
    Origin 200 the module is the whole machine, so this is exact. On a
    multi-node machine it resets the whole VM, broader than the hardware's
    per-module reset -- QEMU provides no per-module reset and the model
    keeps the whole-machine reset rather than refuse.

    ``pwr c`` performs that same whole-machine reset, and inherits one
    further multi-node limitation. A power-cycle is a cold restart, so the
    initiating controller re-arms its cold-power latch and its own post-cycle
    ``pwr q`` then answers ``ok 1``. On a two-chassis machine the reset
    re-arms only the initiating controller's latch, so the non-initiating
    module's post-reset ``pwr q`` answers ``ok 0`` -- warm, not cold.
    Single-module (the default) is correct; a machine-wide cold-latch re-arm
    needs cross-device machinery and is a follow-up for the multi-node
    machine model.

``tas <ch>`` / ``tas 0``
    The test-and-set lock cell, which the 6.5.30 firmware drives in its
    arbitration path. ``tas <ch>`` takes the lock if it is free; ``tas 0``
    releases it. Classic test-and-set: ``ok <held>`` reports the value
    the cell held *before* the request, so acquiring a free cell reads
    ``ok 0`` and a caller polls until it reads 0 (was free) or its own
    ``<ch>`` (already holds it). ``<ch>`` is a *literal* character, not a
    hex value: the client stores and compares the owner byte for byte, so
    ``tas A`` reads back ``ok A`` and any single non-``0`` character is a
    valid channel -- including NUL (``0x00``), so occupancy is tracked apart
    from the owner byte and a NUL owner round-trips in the response frame
    rather than being read as the free cell. ASCII ``0`` is reserved as the
    release channel. This is
    a distinct byte, not the bus arbitration below -- a service carried
    over the bus that itself passes through the arbitration wait like any
    command. The literal-byte round-trip is the test-and-set semantic the
    client needs; the datasheet's "hex" wording is flagged for the source
    owner, and the pre-write value is 6.5.7-derived and unverified on
    6.5.30.

``nic <p> <s> <d>``
    One-wire identity passthrough, an interactive-monitor command, not
    boot-driven. Its wire contract is from the controller datasheet's
    firmware-facing command section: three hex arguments (pulse, sample,
    delay) bit-bang the midplane one-wire line, and it answers ``ok
    <hex>`` with the sampled bit, over which a one-wire ROM read is
    layered by repeated calls. No one-wire device is modelled, so the
    open-drain line idles high and every sample reads ``1`` -- a
    modelling choice, not a datasheet value. A malformed argument list
    is ``err arg``; a bare ``nic`` or junk with no space delimiter
    (``nicx``) is ``err cmd``.

Anything else is answered ``err cmd``. That reply is load-bearing:
the firmware's console-selection routine issues a DIP-switch read
before it ever probes the controller as a console, and it tolerates
a negative reply but not a stalled bus.

Deferred destructive actions
----------------------------

``rst``, ``pwr d`` and ``pwr c`` do not act when they answer ``ok``.
Each arms a deferred action that the model carries out only once the
caller has read the whole ``ok``, through a two-phase state machine:

ARMED
    The command was accepted and its ``ok`` framed, but the ack has not
    been fully delivered. A later request on the bus supersedes the
    pending response and cancels the armed action -- a caller that walks
    away without reading its ``ok`` gets nothing.

COMMITTED
    The guest has read the ack to its end. The action is past the point
    of no return: it survives a superseding request and a live migration,
    and it fires.

The transition is the *fetch* of the response's last byte -- the read
that hands the guest everything through the last text byte of the ack --
not a bus STOP or NACK. Committing there is what keeps the action when a
master NACKs the last text byte and then issues a same-address repeated
START with no STOP: neither terminal event sees the whole ack delivered,
yet the guest has it. The committed action then fires from a bottom half,
which releases the I2C bus -- so the controller is not left matched and
misrouting the next transaction -- before requesting the reset,
power-cycle or shutdown.

The bottom half is scheduled at the transaction's next boundary -- a NACK,
a STOP, or the next request's START -- rather than at the commit, so the
reset never runs while the guest is still reading the frame. One path
therefore never schedules at all: a guest that reads through the last text
byte and then abandons the bus -- taking no checksum, sending no STOP and
no repeated START -- commits the action but reaches no boundary, so it
never fires. The IRIX driver cannot leave a transaction that way: every
ELSC exchange ends with a STOP, after the datasheet's closing
invalid-address resync step, so a committed action always reaches a
boundary. A stream migrated in this window is handled the same way -- a
committed action is re-armed on load only once its frame was consumed
(txlen zero); one still open waits for the destination's boundary.

A destructive command that arrives while another is already *committed*
cannot change it: the committed action wins and fires. The new command
still answers ``ok``, deliberately -- refusing would make the IRIX caller
fall back to its own hardware reset and race the committed action, which
is worse than a stale ``ok``. But that ``ok`` then describes a command the
machine will not perform. The action carried out is always the committed
one; the nine combinations resolve as:

.. list-table::
   :header-rows: 1

   * - committed vs requested
     - ``rst`` (reset)
     - ``pwr c`` (cycle)
     - ``pwr d`` (shutdown)
   * - reset
     - exact
     - restarts; name only
     - restart, not off
   * - power-cycle
     - restarts; name only
     - exact
     - restart, not off
   * - shutdown
     - off, not restart
     - off, not restart
     - exact

On the diagonal the ``ok`` is exact. Between ``reset`` and ``power-cycle``
the machine restarts either way, so the ``ok`` names the wrong action but
not the wrong outcome. In the four cells where a committed ``shutdown``
meets a requested restart, or a committed restart meets a requested
``shutdown``, the outcome itself differs -- the machine ends up off when
the caller asked it to come back, or the reverse. Every absorbed command
is logged (``LOG_GUEST_ERROR``) naming the committed action and the new
command, so the mismatch is never silent.

**This table is model behaviour that no hardware capture has confirmed.**
The window in which the collision arises is host-scheduling latency
between the fetch-seam commit and the bottom half -- not a sub-instruction
gap -- and no firmware this project runs issues two destructive commands
back to back, so no guest reaches it. What would settle whether real
hardware resolves the collision the same way is a bus trace of two
destructive commands issued back to back, which nobody is likely to take.
It is recorded as what the model does, held distinct from what the
hardware is known to do.

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
