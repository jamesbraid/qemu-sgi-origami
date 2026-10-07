.. _sgi-elsc:

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

The same controller program, built a second way, is the module
controller of an Origin 2000 module. The ``build`` property selects
which: ``origin200`` (the default) or ``origin2000``, which the Origin
2000 module-management device sets on its own controller. The builds
differ in the commands they answer and in their version token; this
page names each difference where it applies.

This page describes the register-level contract the model implements.

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

A success with no value is framed as the firmware sends it, the three
bytes ``ok`` and a space; "a plain ``ok``" below means that reply. IRIX
reads ``ok`` followed by a space or the end of the text the same way.

A malformed packet -- short, long, or failing the checksum -- is
answered ``err csum``. The protocol names no separate framing error:
a malformed packet fails the same sums-to-zero test a corrupted
checksum does.

Request grammar
---------------

The controller parses every command the same way. It keeps the
request's text and puts a carriage return where the checksum was, so
each command ends in one.

* The first four bytes are a three-letter lowercase mnemonic and a
  separator, which must be a space or the end of the text. Anything
  else, including an empty request or an uppercase mnemonic, is
  ``err cmd``.
* Most single-letter arguments come from a character reader: it skips
  spaces and takes the next character, and the end of the text or a
  line feed is no argument. ``pwr  q`` and ``pwr q junk`` are both
  ``pwr q``.
* Numeric arguments come from a hex reader: up to four digits, either
  case, with no leading spaces skipped. A number shorter than four
  digits consumes the character that ends it, so a second number must
  follow exactly one space; a four-digit number consumes nothing, so
  the next number follows with no space at all and a fifth digit
  starts it. No digits at all is no value. ``hbt 1e 14`` and
  ``hbt 025814`` carry two values; ``hbt 1e  14`` carries one.
* Whatever a command does not read is discarded.
* Hex values in replies are lowercase with no leading zero.

The firmware runs a recognised command whose checksum or length is bad
and then answers the checksum error. The model answers ``err csum``
without running it, which is safer and differs from the firmware only
for a corrupted request.

Command vocabulary
------------------

``ver``
    Answered ``ok`` plus the build's eight-character version field:
    ``V 3.1S`` and two spaces for the Origin 200 build, ``VER 3.2`` and
    one space for the Origin 2000 build. The firmware requires the
    ``ok`` but does not interpret the token; the first token
    including its terminator must fit in eight bytes.

``rsw``
    The front-panel DIP switches, reported active-low, as
    ``ok <hex>`` with no leading zero.

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

``pas``
    Not a command on this channel: answered ``err cmd``. The controller
    has a supervisor mode, entered with ``pas`` and a password, but it
    exists only on the operator's serial line and gates nothing that
    arrives over I2C. The controller datasheet records this from the
    controller firmware; IRIX sends ``pwr d`` and ``rst`` without ever
    sending ``pas``.

``dsp <message>``
    Origin 2000 build only; the Origin 200 build answers ``err cmd``.
    Writes a message onto the alphanumeric display: up to eight
    printable characters, spaces included, overwriting from the
    first position and leaving the rest unaltered. The message stops
    at the first unprintable character and anything past eight is
    dropped. An empty message is ``err arg``; otherwise a plain
    ``ok``.

``dsc <position> <character>``
    Origin 2000 build only; the Origin 200 build answers ``err cmd``.
    Writes one display character. Spaces before each argument are
    skipped, so the position is the digit ``1`` to ``8``, for display
    positions 0 to 7, and the character is printable and never a
    space. Anything else is ``err arg``; otherwise a plain ``ok``.

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

``pwr d <sec>`` / ``pwr c``
    The remaining power control, none boot-driven. ``pwr d <sec>``
    schedules a delayed power-down and ``pwr c`` power-cycles, both
    answered a bare ``ok``; both are destructive and, like ``rst``, need
    no supervisor mode over I2C. The power-down delay is up to four hex
    digits after one separator character, read until the first non-hex
    character. 1 to 0x258 (re)loads a countdown measured from that
    request, so a repeated request moves the deadline either way; IRIX
    sends ``pwr d 1`` at halt and repeats ``pwr d 8`` while extending a
    shutdown. 0 powers down at once, as does a request with no delay
    while a countdown is pending; a request with no delay otherwise
    starts a three-second countdown. A delay above 0x258 is ``err arg``
    and changes nothing. ``pwr r``, the redundant-supply query, belongs
    to controller firmware 3.3S and later; neither 6.5.7-era build has
    it, so it is ``err arg`` like any unknown letter. The IRIX kernel
    and IO6 PROM query it and skip the supply report on an error. An
    immediate ``pwr d`` and ``pwr c`` act, like ``rst``,
    once the caller has read the ``ok``: ``pwr d`` shuts the machine
    down, ``pwr c`` resets it (a power-cycle). A countdown shuts the
    machine down when it expires, on the virtual clock. Five
    approximations remain. A system reset cancels a pending countdown,
    although the controller keeps running through a reset on hardware,
    so a countdown followed by ``rst`` never powers down. The countdown
    is the nominal number of seconds, although the datasheet infers that
    the firmware's 16-bit millisecond count wraps above 0x41; the ``pd
    remote`` message the controller queues for the Hub on each timed
    request is not sent; and expiry shuts down the whole machine, where
    a two-module Origin 200's slave controller would power off only its
    own module. The controller does not track module power, so it never
    gives the firmware's bare ``ok`` to a ``pwr d`` sent while its
    module is off. Both supplies are modelled healthy. The letter comes
    from the character reader, and a letter the controller does not
    know is ``err arg``. ``pwr c`` takes an optional off interval, read
    like the ``pwr d`` delay, from 5 to 0x258 seconds; anything outside
    that is ``err arg``, and the model restarts at once whatever the
    interval.

``hbt [<interval> [<cycle>]]``
    Heartbeat / NMI-watchdog control. ``hbt`` with no readable value
    sends one heartbeat and ``hbt 0`` disables monitoring. Both
    values come from the hex reader: the interval is ``5`` to ``258``
    seconds, and the optional second value is the wait from the NMI to
    the controller's power cycle. An interval out of range is
    ``err arg``; everything else answers a plain ``ok``. IRIX 6.5.30 sends ``hbt 30 20`` only when its
    heartbeat tunable is set, which it is not by default, so the model
    validates the request and arms no timer: no missed-heartbeat NMI
    or power cycle is modelled.

``rst``
    The module reset, reached by a panic reboot or fatal-error
    recovery, never a normal boot. It answers ``ok`` and requests an
    immediate machine reset, deferred until the caller has read the
    ``ok`` so the ack is never lost. There is no
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

``tas [<ch>]``
    The test-and-set lock cell: one byte, ``0`` when free. The argument
    is the next non-space character, a literal byte rather than a hex
    value, so ``tas  A`` is ``tas A`` and a space is never an owner.
    ``tas 0`` frees the cell; any other byte, NUL included, takes it only
    when it is free and otherwise leaves the holder in place; no
    argument only reports. Every reply is ``ok`` and the byte the cell
    held before the request, so a caller polls until it reads ``0`` or
    its own byte. The reply is framed with an explicit length so a NUL
    owner round-trips. This is a service carried over the bus, not the
    bus arbitration below. No IRIX 6.5.30 binary is known to send it.

``nic <p> <s> <d>``
    One-wire identity passthrough, an interactive-monitor command, not
    boot-driven. Its wire contract takes three hex arguments (pulse,
    sample, delay) that bit-bang the midplane one-wire line, and it answers ``ok
    <hex>`` with the sampled bit, over which a one-wire ROM read is
    layered by repeated calls. On an Origin 2000 module the line
    reaches the module's ``MODULEID`` and midplane parts
    (:ref:`sgi-origin2000-module-identity`). On an Origin 200 no
    one-wire device is modelled on it, so the open-drain line idles high
    and every sample reads ``1`` -- a modelling choice, not a datasheet
    value. The three values come from the hex reader, and any of them
    missing is ``err arg``.

``clr``
    Answers ``ok`` with the module on or off. It cancels a pending
    power-down countdown and the fan-high request, and on the Origin
    2000 build blanks the display. It leaves the ``tas`` byte, the
    margins, the ``fsc`` and ``tst`` flags and the NVRAM alone. The
    serial echo and supervisor mode it also resets belong to the
    operator's serial line, which the model does not have.

``fan [h|n]``
    ``fan h`` and ``fan n`` set and clear the fan-high request and
    answer ``ok``; any other letter, uppercase included, is ``err
    arg``. The query answers ``ok h`` while the request stands and
    ``ok n`` otherwise. The model has no temperature inputs and no fan
    monitor, so the request alone decides, and the firmware's failed-fan
    reply never arises. Over I2C neither form writes the NVRAM.

``fsc [1|0]``
    Declares a multimodule controller present or absent and answers
    ``ok``. The query form loses its answer over I2C, as the firmware's
    does, and answers a bare ``ok``; any other argument is that query
    too and leaves the flag alone. The flag's effects belong to an
    Origin 2000 module's key switch and display mirroring, neither of
    which the model drives, so it is only stored.

``tst <c>``
    The factory's short timeouts: ``1`` sets them and any other
    character clears them, answering ``ok``; no character is ``err
    arg``. They shorten waits the model does not have, so the flag is
    only stored.

``tmp``
    ``ok n``: the model has no temperature inputs. A master controller
    that has heard ``sla`` 0x07 from a slave appends a space and the
    slave's last temperature letter.

``key``
    Origin 2000 build only. ``ok on``: the model's key switch is on.

``vlm <supply> [h|l|n]``
    Origin 2000 build only. The supply is ``3``, ``5`` or ``v``; with no
    second character the reply is that supply's margin, ``ok h``, ``ok
    l`` or ``ok n``, and a second character sets it. ``vlm 5h`` and
    ``vlm 5 h`` are the same request. Anything else is ``err arg``. The
    margins are stored; nothing electrical follows.

``sla <c>``
    The report a slave controller sends its master. The Origin 2000
    build answers ``ok`` and does nothing. In the Origin 200 build a
    master takes ``n``, ``h`` and ``o`` as its slave's temperature and
    0x07 as the slave announcing itself, and a slave stores the
    character. The model has no controller-to-controller link, so a
    slave never forwards its report, and a master logs rather than
    carries out the shutdowns and fan reports that ``o``, 0x04, 0x05
    and 0x06 start.

Anything else is answered ``err cmd``. That reply is load-bearing:
the firmware's console-selection routine issues a DIP-switch read
before it ever probes the controller as a console, and it tolerates
a negative reply but not a stalled bus.

The serial-only commands (``cfg``, ``dbg``, ``ech``, ``fre``, ``mod``,
``nvm``, ``pas``) and any mnemonic a build lacks answer ``err cmd`` with
no other effect. Some commands the controller firmware answers over I2C
are not modelled yet and get that ``err cmd`` too: ``nmi``, ``see`` and
``sel`` in both builds, and ``pwr u``; ``aut`` in the Origin 200 build.
Neither the IRIX 6.5.30 PROMs nor its kernel send any of them.

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
yet the guest has it. The committed action then fires, releasing the I2C
bus -- so the controller is not left matched and misrouting the next
transaction -- before requesting the reset, power-cycle or shutdown.

Firing is scheduled at the transaction's next boundary -- a NACK, a STOP,
or the next request's START -- rather than at the commit, so the reset
never runs while the guest is still reading the frame. One path therefore
never schedules at all: a guest that reads through the last text byte and
then abandons the bus -- taking no checksum, sending no STOP and no
repeated START -- commits the action but reaches no boundary, so it never
fires. The IRIX driver cannot leave a transaction that way: every ELSC
exchange ends with a STOP, after the datasheet's closing invalid-address
resync step, so a committed action always reaches a boundary. A stream
migrated in this window is handled the same way -- a committed action is
re-armed on load only once its frame was consumed (txlen zero); one still
open waits for the destination's boundary.

Firing queues the action on the requesting processor's own work, at the
processor's next translation-block boundary, rather than a main-loop
bottom half: round-robin execution reaches queued work before any peer is
scheduled again, so a bottom half's real defect -- every other processor
free to keep executing until the main loop next runs the reset, a
different number of instructions on every run -- does not arise. Where no
module owns the controller (the Origin 200; see below), every processor
is then stopped, mirroring the Hub's port-reset write
(``sgi_hub_request_machine_reset()`` in :file:`hw/misc/sgi-hub.c`).
Multi-threaded execution cannot promise a peer stops before its own next
instruction -- it runs on its own host thread -- so there only the
resulting reset, power-cycle or shutdown itself is guaranteed, not a
bound on what a peer executed first. With no processor to queue the work
on -- the bottom half re-armed after a migration, or a qtest access with
no processor running guest code -- firing still uses a bottom half, which
does not stop any processor; nothing here has settled what real hardware
does with a peer processor's execution in that window, only that this
model must not leave it to host scheduling.

Where an Origin 2000 module complex owns this controller, the module
decides what a reset or a power-off actually reaches, and it is asked
before the command is answered rather than after. A command the module
cannot carry out is answered ``err cmd``. The ``ok`` is the caller's
proof that the action happened -- it will not fall back to its own
hardware reset afterwards -- so acknowledging an action that will not
happen is a lie with nothing in the protocol to expose it. With no module
owner, which is the Origin 200, the controller is the whole module and
answers for itself exactly as it always did.

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

An Origin 2000 module controller drives the module's front-panel
eight-character alphanumeric display. The IRIX 6.5.30 IP27 PROM
writes each node's boot progress there as two hex digits, at
positions 2s-1 and 2s for slot s, and the IO6 PROM and the kernel
write short messages with ``dsp``. They send the same commands on an
Origin 200, whose controller build has no display and answers ``err
cmd``; every sender ignores the reply and none retries, so the
refusal costs nothing. The power-on content is unstated, and the
model blanks it to spaces. ``info elsc`` shows the current content
of an Origin 2000 build's display.

.. _sgi-elsc-arbitration:

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
of two processors each. The Origin 200 controller's ring is narrower.
The datasheet states it for the two-module machine: processor codes 0
to 3, then the slave-controller position. The model uses the same
five-position ring on a single module too. That is an inference, not
an established fact: the datasheet describes one controller counter
covering those five positions, and nothing found describes a
different count without a second module. The model publishes the
fifth position as code 4's signal byte, which no processor waits
for.

Two properties of the rotation are contracts, not conveniences:

* A processor's own grant must be absent before it trusts a later
  appearance of it. The other positions' grants supply that absence;
  there is no separate gap broadcast.
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

The controller attempts one address-only grant on each
one-millisecond periodic tick, so every position stands for one tick
and a ring of *n* positions turns in *n* milliseconds. A grant stays
readable in every PCF8584 data register until the next one replaces
it, which covers the firmware's hold check of 250 converted Hub
counter units (250 microseconds at the model's counter rate). Each
publication recomputes the signal 2 half. Each tick re-arms the next
one tick from when it actually ran, so a tick the host runs late
stretches that position rather than shortening the next one below
the hold check. The model publishes code 0 at reset and advances
after each publication, where the controller firmware advances
before each eligible attempt; the resulting difference of about one
tick in the ring's starting phase after reset is a model choice. A
tick that
finds either module's bus busy publishes nothing and does not
advance, and the controller waits for its next tick rather than
publishing as the bus frees, so a STOP never triggers a grant. The
datasheet takes this pacing from the controller firmware and forbids
lengthening a position or inserting a gap to compensate for emulator
scheduling; the model does neither. The rotation is purely
timer-driven: no bus traffic ends a position early, and no
acknowledgement consumes a grant. Busy ticks are detected from the
I2C core's bus state rather than through its pending-master queue,
because the PCF8584 model drives transactions directly and never
calls ``i2c_bus_release()``.

The rotation is this model's synthesis of a mechanism the Origin 200
midplane provides some other way, so the ``arbitration`` property (on by
default) switches it off. An Origin 2000 module needs that: its grants
are real address-only bus traffic published by the module controller
itself, onto a bus whose participants the I2C core identifies -- and the
broadcast drives the core's anonymous transfer calls, which assert that no
participant ids are configured. Left on, the rotation would abort such a
machine rather than quietly do the wrong thing.

AUX serial port
===============

The master controller is the far end of the Origin 200's external AUX
serial port. The ``-serial`` lines remain node-major with a fixed stride
of four: node *n* owns the block ``[4n, 4n+4)``, and its nominal AUX slot
is the third line in that block. The external AUX backend attaches only
to the master, node 0 at index 2. A backend provisioned at the slave's
nominal index 6 remains unattached. Node 0's index 3 is the bring-up UART,
which exists on node 0 only.

The external AUX port's one multiplexing rule (Origin 200 Owner's Guide,
page 132) is that a byte sequence beginning 0x14 is a command to the
controller itself and is never queued for ``get``. Every other byte is
console input. Nothing read for the datasheet states what ends an escaped
command. The model discards through the next line terminator as a model
choice.

QEMU continues to model the slave controller and its internal I2C command
path. That preserves existing model behavior. It is not a hardware claim,
because the available Origin 200 evidence does not establish slave-local
I2C or arbitration behavior.

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

.. _sgi-elsc-nvram:

Module controller configuration NVRAM
=====================================

Each module system controller keeps a 2048-byte configuration NVRAM on
its module's management I2C bus: the Origin 200 Hub's embedded ELSC, and
the Origin 2000 or Onyx2 module controller, whose one store its four Hubs
share. It holds the board inventory log from address 0 and, at 0x700,
the controller's block: validity marker, password, debug-switch values,
configuration byte, module number, self-test flags and partition, domain,
cluster and cell numbers. ``hw/nvram/sgi-elsc-nvram.c`` models it as one
device. Ratings follow :ref:`sgi-ip30`.

.. list-table::
   :header-rows: 1
   :widths: 46 12 42

   * - Fact
     - Rating
     - Source
   * - The store is addressed 0 to 2047 and addresses 1792 to 2047 are
       reserved for the system controller's use.
     - established
     - 108-0170-001 ``scr`` and ``scw`` command
       text (PDF page 76, printed 4-22).
   * - The module number (1 to 255, 0 unassigned) lives in this NVRAM
       and is backed up in each node's PROM log.
     - established
     - 108-0170-001 section 1.5, printed page 1-4 (PDF 18).
   * - Address ``A`` answers at I2C address ``0x50 + (A >> 8)`` with the
       one-byte word address ``A & 0xff``.
     - established
     - The Origami project's PCF8584 and ELSC datasheet, configuration
       NVRAM section. Which part, and how many packages, is unknown; one
       24C16-class part fits the addressing.
   * - The controller block's layout: 0x700 marker ``0x37``, 0x701 to
       0x704 the password, 0x705 and 0x706 the debug-switch values, 0x707
       the configuration byte, 0x708 the module number, 0x709 the
       self-test flags, 0x70a to 0x70d partition, domain, cluster and
       cell.
     - established
     - SGI's GPL SN code in Linux history (``v2.5.0``,
       ``arch/ia64/sn/io/l1_command.c``), as the datasheet records it.
   * - A store with no backing file starts as a master controller leaves
       it after its first power-up: zeroes, then at 0x700 the marker,
       ``none``, zero switch values, the configuration byte (``0x02``,
       automatic power-up, on the Origin 200 build; 0 on the Origin 2000
       build), the module number (0 on Origin 200; the module's selected
       number on Origin 2000), zero self-test flags and partition, and the
       clock-select byte at 0x70b: ``0x78`` on Origin 200, 0 on Origin 2000.
     - approximation
     - The controller's initialization, the Origin 200 build's ``0x78``
       included, is established by the datasheet. The zero fill elsewhere
       is not, since factory contents are unknown and an erased EEPROM
       usually reads ``0xff``. The Origin 2000 controller writes ``0x78``
       only on the full-clock midplane type, and this module being that
       type is inferred. Its PROMs read 0x70b as the domain, so the model
       keeps 0 there rather than change what a guest sees on an inference.
   * - A two-module Origin 200's slave-module store starts like the
       master's.
     - approximation
     - The slave controller does not initialize its store, so its
       contents are unknown (an erased part usually reads ``0xff``), and
       which store the slave module's Hub reaches is unknown. In the model
       the slave node's processors reach that store through their own Hub,
       so its seed is guest-visible; keeping the master's seed leaves that
       behaviour unchanged.
   * - A sequential read or write past a block's last byte wraps to that
       block's first byte, as the previous model's eight 256-byte parts
       did. A write of any length is accepted. Writes complete at once and
       are never refused as busy. The part does not accept a general call.
     - approximation
     - Block-end behaviour, the write buffer and the write-cycle time are
       unknown on hardware, and a real 24C16 wraps a write within a
       16-byte page; the firmware's longest write is 12 bytes from a
       block's start. The firmware's log writer does not retry, so a busy
       NAK would cut its records short.
   * - Persistence: the store survives reset, migration between identical
       builds and, with a ``module<N>-nvram`` file bound, QEMU processes.
       A bound file's contents win over the product's seed.
     - established
     - Datasheet persistence paragraph; the file is an emulator feature.
