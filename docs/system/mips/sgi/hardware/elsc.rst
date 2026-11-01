.. SPDX-License-Identifier: BSD-3-Clause

.. _sgi-elsc:

==============================
ELSC: module system controller
==============================

Used in :ref:`sgi-ip27`: one per Origin 200, Origin 2000 and Onyx2
module.

.. seealso::

   Platform pages: :ref:`sgi-ip27`.

.. seealso::

   - L1 (:doc:`l1`): the system controller of the :term:`SN1` machines.

The module system controller (MSC, also called the ELSC, the entry-level
system controller) is a microcontroller that manages power, reset, the
front panel and an alternate console for one module. The Origin 200 has
one per module; an Origin 2000 or Onyx2 module has one shared by its four
node boards. Each node's :term:`Hub` reaches it over an I2C bus through a Philips
PCF8584 I2C-bus controller, and the controller keeps a 2048-byte
configuration NVRAM on the same bus. QEMU models the three parts
separately:

- ``sgi-elsc`` in ``hw/misc/sgi-elsc.c``, the controller. Its ``build``
  property selects the Origin 200 build (``origin200``, the default) or
  the Origin 2000 build (``origin2000``). The builds differ in the
  commands they answer, their version text and their display.
- ``pcf8584`` in ``hw/i2c/pcf8584.c``, the node-side I2C controller.
- ``sgi-elsc-nvram`` in ``hw/nvram/sgi-elsc-nvram.c``, the configuration
  NVRAM.

An Origin 200 Hub builds its own controller, adapter and NVRAM. An Origin
2000 module builds them in ``sgi-sn0-module-management``
(``hw/misc/sgi-sn0-module-management.c``), which links four adapters to
one bus and adds the module's grant scheduler, one-wire net and power,
reset and NMI outputs. The module's topology is on
:doc:`/system/mips/sgi/ip27`.

No specification of the controller's I2C protocol, its firmware or its
microcontroller is public, and no controller has been measured for this
model. Its command behaviour on this page is therefore the model's, an
*approximation* unless a statement carries another rating. The PCF8584 is
modelled against the Philips product specification; no chip revision is
distinguished. Facts are rated as described in :ref:`sgi-ratings`.

.. _sgi-elsc-sources:

Sources
=======

- The Philips *PCF8584 I2C-bus controller* product specification of
  1997-10-21, :src:`philips-pcf8584-1997`: the register file, the
  register-select bits, the receive pipeline, bus timing and reset.
- NXP *I2C-bus specification and user manual* UM10204 revision 7.0,
  :src:`nxp-um10204-rev7`: clock synchronisation between masters.
- SGI's *Origin 200 and Origin 200 GIGAchannel Owner's Guide*,
  :src:`007-3708-002`: the AUX connector and its line settings, the
  operator's Ctrl+T command line, supervisor mode and the password, the
  command reference and later controller firmware versions.
- SGI's *Origin200/2000 IP27 PROM Technical Reference Manual*,
  :src:`108-0170-001`: the controller's role, the module number's
  storage and the NVRAM's address range.
- SGI's *Origin2000 and Onyx2 Diagnostic Roadmap*,
  :src:`108-0164-002 p. B-1`: one MSC and one NVRAM on one I2C bus shared
  by the four nodes of an Origin 2000 module.
- SGI's *Origin2000 Deskside Owner's Guide*, :src:`007-3453-002 p. 83`,
  for the controller NVRAM's stated size.
- SGI's GPL SN code in the Linux history tree at tag ``v2.5.0``,
  ``arch/ia64/sn/io/l1_command.c`` and
  ``include/asm-ia64/sn/ksys/elsc.h``: the layout of the controller's
  NVRAM block.
- Linux ``drivers/i2c/algos/i2c-algo-pcf.c`` and
  ``arch/mips/include/asm/sn/sn0/addrs.h`` at ``db2ddb871435``, as
  software expectations: the PCF8584 own-address convention and the ELSC
  name.
- The IP27 :term:`PROM` 6.156 (:src:`ip27-prom-6.156`), the IO6 PROM and the
  ``unix.IP27`` kernel shipped with IRIX 6.5.30, read as binaries, as
  software expectations: which controller commands they send, when, and
  what they do with the reply, and the PCF8584 register sequences they
  issue.

Identity and placement
======================

The controller and its NVRAM sit on an I2C bus that reaches each node's
Hub; on an Origin 2000 module one controller and one NVRAM share one bus
with all four nodes (:src:`108-0170-001 section 1.6`,
:src:`108-0164-002 p. B-1`), *established*. A two-module Origin 200 has
one controller per module.

.. list-table::
   :header-rows: 1

   * - Machine
     - Controller build
     - Adapters on the bus
     - I2C addresses
   * - ``origin200``, one module
     - ``origin200``
     - one, the node's Hub
     - controller ``0x08``; NVRAM ``0x50``-``0x57``
   * - ``origin200``, two modules
     - ``origin200`` on each module; the first node's is the master
     - one per module, each on its own bus
     - as above, on each bus
   * - ``origin2000`` and Onyx2
     - ``origin2000``
     - four, one per node
     - controller ``0x08``; NVRAM ``0x50``-``0x57``; UST control ``0x20``

The controller answers at seven-bit address ``0x08``, where the IP27
PROM 6.156 addresses it, a *software expectation*; no SGI document states
the address. The NVRAM's addresses are
described under :ref:`sgi-elsc-nvram`.

The PCF8584 is mapped in the Hub's local register space, data register
first and control/status eight bytes above it (:doc:`hub`). The model
takes 1- to 8-byte big-endian accesses and handles an 8-byte access as
one register access: the stock PROM reads the data register with 8-byte
loads, and a split access would advance the receive pipeline twice. The
chip's interrupt output exists in the model but no Hub connects it;
software polls, a *software expectation*.

Register map
============

The PCF8584 has two addresses, selected by its A0 pin: S0 and the
registers that share it at A0 low, and S1, control on write and status on
read, at A0 high :src:`philips-pcf8584-1997`, *established*. The register
behind A0 low is chosen by control bits ESO, ES1 and ES2. The access
masks and the reset values other than status are the model's
(*approximation*); every register is 8 bits.

.. list-table::
   :header-rows: 1

   * - Offset
     - Name
     - Selected by
     - Reset
     - Access
     - Notes
   * - ``0x0``
     - S0', own address
     - ESO, ES1, ES2 clear
     - ``0``
     - R/W, ``0x7f``
     - Seven-bit address, right-justified; the chip puts it on the wire
       shifted left by one.
   * - ``0x0``
     - S2, clock
     - ESO clear, ES1 set
     - ``0``
     - R/W, ``0x1f``
     - Oscillator divider and SCL rate select. Stored only.
   * - ``0x0``
     - S3, interrupt vector
     - ES2 set and ES1 clear, or ESO and ES2 set
     - ``0``
     - R/W
     - Stored only.
   * - ``0x0``
     - S0, data
     - ESO set, ES2 clear
     - ``0``
     - R/W
     - The byte shifted in or out. Reads and writes have side effects.
   * - ``0x8``
     - S1, control
     - write
     - ``0``
     - W
     - PIN ``0x80``, ESO ``0x40``, ES1 ``0x20``, ES2 ``0x10``, ENI
       ``0x08``, STA ``0x04``, STO ``0x02``, ACK ``0x01``.
   * - ``0x8``
     - S1, status
     - read
     - ``0x81``
     - R
     - PIN ``0x80``, STS ``0x20``, BER ``0x10``, LRB ``0x08``, AAS
       ``0x04``, LAB ``0x02``, BB ``0x01`` (set means not busy).

Register selection
------------------

ESO chooses between the data register and the configuration registers,
but ES2 keeps selecting with ESO set: it then chooses between data and
the interrupt vector, and only ES1 stops meaning anything
:src:`philips-pcf8584-1997`, *established*. With ESO clear and both ES1
and ES2 set the model selects the clock register, an *approximation*.

With ESO clear, a read of S1 returns the control bits with the live PIN
bit in bit 7, an *approximation*: the specification describes S1 as
write-only in that state.

Own address
-----------

The own-address register stores what is written and returns it. The
stock PROM writes ``0x7f``, selects the clock register and writes
``0x1c`` (12 MHz input, 90 kHz SCL), reselects the own address and reads
``0x7f`` back, then writes its real address and sets ESO, a *software
expectation*; a register that returned a constant would fail that check.
Linux ``i2c-algo-pcf.c`` treats the address the same way, right-justified
and shifted onto the wire by the chip.

Status
------

Hardware reset sets PIN and BB, so status reads ``0x81``
:src:`philips-pcf8584-1997`, *established*. A START clears BB and a STOP
sets it, and a match on the own address sets AAS
:src:`philips-pcf8584-1997`, *established*. In the model a control write
with PIN set sets PIN and clears BER, LAB and AAS without disturbing a
transfer in progress, and the general call does not set AAS, each
an *approximation*.

Transfers
=========

Master transfers
----------------

A first START is the address byte written to S0 while the bus is free,
then a control write with STA. A repeated START reverses that order: STA
first, then the address byte. A STOP is a control write with STO. PIN
clears when each byte, its acknowledgement clock included, is done, and
LRB records whether the far end acknowledged
:src:`philips-pcf8584-1997 p. 12, 16`, *established*. On a private bus,
the Origin 200 wiring, a byte completes within the host register access
that starts it and no virtual time passes.

A repeated START to a different address ends the old match before the
new one, so a slave in the middle of a command never sees a spurious end
of transfer. A refused address leaves no open transfer behind: the next
data byte reports NAK rather than a false acknowledgement.

The receive pipeline
--------------------

The transfer attaches to the read of the data register, not to the
START. After a read-direction address the first read returns what S0
already holds, the address byte, and that read moves the first data byte
in; each later read returns the byte fetched by the one before
:src:`philips-pcf8584-1997`, *established*. A model that fetched at the
START would shift every response by one byte. Clearing ACK before a read
makes that read's fetch the last, NAKed byte.

The stock PROM follows this: it discards the first read, clears ACK
before reading the last byte, and ends every exchange that reached its
data phase with a control write ``0x45`` (ESO, STA and ACK), the address
byte ``0xfe`` and a STOP, a repeated START to address ``0x7f`` that no
device answers, a *software expectation*. In the model that closing START
reads back NAK.

The shared module wire
----------------------

On an Origin 2000 module four adapters and the controller share one bus.
Each adapter there has a participant number (``wire-master``) and drives
SCL and SDA as staged transitions on the virtual clock, so bitwise
arbitration, clock stretching and a slave's hold of SCL happen on the
wire rather than in one indivisible call. The model uses a 90 kHz clock
split into two 5556 ns halves and a 4.7 us bus-free time. The
specification calls the rates approximate and fixes no duty cycle, so the
even split is an *approximation*; both halves clear the 4.7 us low and
4.0 us high minimums :src:`philips-pcf8584-1997 p. 8, 24`. Masters
synchronise as the I2C specification describes: the shared low period
follows the longest low, the high period the shortest high
:src:`nxp-um10204-rev7 p. 10-11`, *established*.

- **Arbitration loss.** A master that sends high and samples low sets LAB
  at that bit and releases SDA. PIN goes active when the winning byte
  finishes :src:`philips-pcf8584-1997`, *established*. The loser stops clocking at once
  rather than clocking out the winning byte, an *approximation*: the
  sources do not distinguish the two.
- **Addressed service.** An adapter addressed as a slave sets PIN after
  the ninth clock and holds SCL low until its own host reads (as a
  receiver) or writes (as a transmitter) S0. Servicing one adapter never
  releases another's hold.
- **Grant observation.** A nonzero-address adapter with ESO set copies an
  otherwise unmatched address byte in the range ``0xe0``-``0xef`` into S0
  once its STOP completes, and keeps it until a later address phase
  replaces it. It does not acknowledge, set AAS or PIN, or hold SCL. The
  specification guarantees passive buffering only with own address zero,
  so this is an *approximation* that the grant protocol requires
  (:ref:`sgi-elsc-arbitration`).
- **START on a busy bus.** The specification does not say what happens to
  a STA written while another master holds the bus. The model keeps one
  such request and issues it after the next STOP and bus-free time, an
  *approximation*. Clearing ESO withdraws it.
- **STOP inside a byte.** A STOP written while a byte is on the wire is
  placed after that byte, an *approximation*: the specification names no
  byte boundary for it.
- **Reset.** A reset adapter releases both lines. A peer still contending
  carries its transfer on, so a reset non-owner never ends the winner's
  transfer. What an owning adapter's reset does to the wire is *unknown*;
  the model releases the lines and, when nothing else holds them, ends
  the transfer as if at a STOP.

Controller protocol
===================

Framing
-------

Every command is a short ASCII request carried in one combined
write-then-read I2C transaction to the controller. The write half carries
the request. Switching the bus direction to read completes it and
produces the response, framed the same way:

- one length byte, counting only the text;
- that many bytes of ASCII text, at most 94;
- one checksum byte that makes the whole packet, length through
  checksum, sum to zero modulo 256.

A success with no value is the three bytes ``ok`` and a space; "a plain
``ok``" below means that reply. Values follow ``ok`` and a space, and hex
values are lowercase with no leading zero. Errors are ``err csum``,
``err cmd``, ``err arg`` and, on the serial line only, ``err perm``.

A short, long or badly summed request is answered ``err csum`` and does
not run. A response belongs to the processor whose request produced it
and stays pending until it is read to its end or a new request replaces
it. A requester may end its transaction with the response unread and
fetch the frame later with a read-only transaction. On the Origin 200 the
requester's grant shows signal 2 while its response waits
(:ref:`sgi-elsc-arbitration`); the Origin 2000 scheduler gives no such
signal. A request from a context with no
processor, a monitor or qtest access, is attributed to processor code 0.

Request grammar
---------------

The model keeps the request text and puts a carriage return where the
checksum was, so every command ends in one.

- The first four bytes are a three-letter lowercase mnemonic and a
  separator, a space or the end of the text. Anything else, including an
  empty request or an uppercase mnemonic, is ``err cmd``.
- Most single-letter arguments come from a character reader: it skips
  spaces and takes the next character. The end of the text or a line
  feed is no argument. ``pwr  q`` and ``pwr q junk`` are both ``pwr q``.
- Numeric arguments come from a hex reader: up to four digits, either
  case, with no leading spaces skipped. A number shorter than four digits
  consumes the character that ends it, so a second number must follow
  exactly one space. A four-digit number consumes nothing, so the next
  number follows with no space and a fifth digit starts it. No digits at
  all is no value. ``hbt 1e 14`` and ``hbt 025814`` carry two values;
  ``hbt 1e  14`` carries one.
- Whatever a command does not read is discarded.

The Owner's Guide gives the delayed power commands a hexadecimal delay
from 5 to 258 :src:`007-3708-002 p. 131`, *established*, which agrees
with hex arguments.

Commands
--------

The two builds answer these commands over I2C. The serial-only commands
(``cfg``, ``dbg``, ``ech``, ``fre``, ``mod``, ``nvm``, ``pas``) and any
mnemonic a build lacks answer ``err cmd`` with no other effect, as does
any other request. Supervisor mode, which the Owner's Guide describes for
the operator's serial line :src:`007-3708-002 p. 124`, controls nothing that
arrives over I2C: the IRIX 6.5.30 kernel sends ``pwr d`` without ever
sending ``pas``, a *software expectation*.

``ver``
    ``ok`` and the build's eight-character version field: ``V 3.1S`` and
    two spaces for the Origin 200 build, ``VER 3.2`` and one space for
    the Origin 2000 build. These texts are the model's. The Owner's Guide
    names later Origin 200 controller firmware 3.2S and 3.3S
    :src:`007-3708-002 p. 26, 97`, *established*, so no one version
    describes every machine.

``rsw``
    The front-panel DIP switches, active-low, as ``ok <hex>``. The
    switches come from the ``dip-switches`` property (the machine's
    ``elsc-dip-switches``). In the Origin 200 build bit ``0x04`` of the
    byte is the controller's role, set on a master and clear on a slave;
    the model takes the role from the topology, so the configured value
    of that bit has no effect there.

``get``
    The console-input poll. The reply is ``acp``, a space, the input
    selector and a space, then the queued input. The selector is the
    selected processor's digit, ``.`` for automatic selection or ``/``
    with selection disabled. It names the selection mode, not the
    reader: queued input goes to whichever processor asks. When the
    controller has a message for the Hubs, ``get`` returns that message
    instead, with no ``ok`` (`Controller messages`_).

``acp <selector> <characters>``
    Console output. Characters from byte 6 of the request on, at most 86
    per request, go to the AUX serial line, filtered by ``see``. Fewer
    than six bytes of text is ``err arg``. The reply is always
    ``ok <count>`` in hex, never a plain ``ok``: a caller that read a
    missing count as zero characters taken would resend for ever. The
    selector character less the code of ``0`` is recorded as the most
    recent output sender, with no range check, as the request is parsed,
    so a frame with a bad checksum still records it.

``pwr q``
    The cold-power query, which the IP27 PROM sends at power-on, a
    *software expectation*. ``ok 1`` on the first read after a power-up
    and ``ok 0`` afterwards: the model latches a cold power-on when the
    controller starts and at every power-up of its module, and clears it
    on read. No reset re-arms it. Whether hardware clears it on read is
    *unknown*; one exchange with a real controller would settle it. The
    latch is per controller, so each module of a two-module machine has
    its own.

``pwr`` and ``pwr u``
    ``pwr`` reports module power, ``ok u`` or ``ok d``. ``pwr u`` answers
    ``ok`` and, on an unpowered module, starts the power-up sequence once
    the caller has read the ``ok``, cutting short any power-cycle
    countdown. The sequence frees the ``tas`` cell, discards queued serial
    input and holds the module's reset for 4 s (Origin 200 build) or
    4.4 s (Origin 2000 build). Then the module runs and the cold-power
    latch is set. The model's supplies never fail, so its checks always
    pass, and it keeps answering commands during the sequence. A powered
    module ignores ``pwr u``. See `Module power`_.

``pwr d [<sec>]``
    Delayed or immediate power-down, answered ``ok``. Exactly one
    character after the ``d`` is skipped and the delay comes from the hex
    reader. 1 to ``0x258`` (re)loads a countdown from this request, so a
    repeated request moves the deadline either way. 0 powers down at
    once, as does a request with no delay while a countdown is pending; a
    request with no delay otherwise starts a three-second countdown. Above
    ``0x258`` is ``err arg`` and changes nothing. On an unpowered module
    every ``pwr d`` answers ``ok`` and does nothing, with no range check.
    A timed request queues ``pd remote`` for the Hubs. A countdown
    survives ``rst``. At power-off the IRIX 6.5.30 kernel sends
    ``dsp PowerOff`` and ``pwr d 1``: on an Origin 2000 to every module,
    through the first node it found in each, and on a two-module Origin
    200 only to the lowest-numbered module (`Master and slave`_). It
    repeats ``pwr d 8`` about once a second while it extends a
    controller-started shutdown. Each is a *software expectation*.

``pwr c [<sec>]``
    Power cycle with an optional off interval, read the same way, from 5
    to ``0x258`` seconds, 5 without one. Anything outside that is
    ``err arg``. Once the caller has read the ``ok``, it queues
    ``pd cycle`` for the Hubs, removes module power and starts the
    power-up countdown, which ``clr`` leaves running. After the interval
    the power-up sequence runs as for ``pwr u``. On a module already off,
    ``pwr c`` only restarts the countdown.

``pwr r``
    The redundant-supply query belongs to controller firmware 3.3S and
    later :src:`007-3708-002 p. 26`, *established*. The model answers it
    ``err arg``, as it does any unknown letter. The IRIX 6.5.30 kernel
    and IO6 PROM send it and skip the supply report on an error, a
    *software expectation*.

``hbt [<interval> [<delay>]]``
    The heartbeat watchdog. ``hbt 0`` stops it. An interval of 5 to
    ``0x258`` seconds arms it: the stage returns to waiting, the timer
    starts at the interval, and the delay from its NMI to its power cycle
    is the second value, zero allowed, or 30 seconds without one. Any
    other interval is ``err arg`` and changes nothing. With no readable
    value the request is a heartbeat, which reloads the timer. When the
    timer runs out the watchdog moves on a stage: from waiting to a
    five-second warning; from the warning to an NMI pulsed at once, then
    the delay; from the delay to a power cycle with a five-second off
    interval, after which it is idle with its interval still set. A
    heartbeat never returns it to an earlier stage, and a delay of zero
    never power-cycles. ``clr`` and ``hbt 0`` stop it; ``rst``, ``nmi``
    and power-off do not, so an armed watchdog brings back a module that
    ``pwr d`` turned off. The IRIX 6.5.30 kernel sends ``hbt 30 20`` only
    when its heartbeat tunable is set, which it is not by default, and
    formats the numbers in decimal, so the controller reads ``0x30`` and
    ``0x20`` seconds, a *software expectation*.

``rst``
    Module reset. An unpowered module answers ``err cmd``. Otherwise it
    answers ``ok``, queues ``rst`` for the Hubs once the caller has read
    the ``ok``, and resets the module one second later; another ``rst``
    inside that second restarts the wait. Processors run through the
    second. An Origin 2000 module drives a 100 ms reset pulse; an Origin
    200 reset in QEMU is instantaneous. The reset reaches the module
    whose controller took the command and no other. Whether it reaches
    the module's Routers, Crossbows and :term:`XIO` boards is *unknown*, and the
    model resets them with the module. IRIX 6.5.30 reboots through the
    Hub's network-interface port reset and sends ``rst`` only when its
    reboot-on-panic tunable is 2; the IO6 PROM sends it to recover from a
    SCSI timeout, a *software expectation*.

``nmi``
    An unpowered module answers ``err cmd``. Otherwise ``ok``, and one
    second later the module's processors get a 100 ms NMI pulse; another
    ``nmi`` inside that second restarts the wait. Unlike a reset, the
    wait starts when the ``ok`` is framed. After the pulse the controller
    frees the ``tas`` cell and reinitialises its bus interface. No
    message goes to the Hubs, and power, countdowns and console selection
    are untouched. The Origin 200 controller drives its own ``nmi``
    output, which the machine wires to every processor on the node. An
    Origin 2000 controller has its module pulse the module's NMI output,
    which reaches every processor in the module, and frees the lock as
    the pulse starts.

``tas [<ch>]``
    A one-byte test-and-set cell, ``0`` when free. The argument is the
    next non-space character, a literal byte. ``tas 0`` frees the cell;
    any other byte, NUL included, takes it only when it is free; no
    argument only reports. Every reply is ``ok`` and the byte the cell
    held before, framed with an explicit length so a NUL owner
    round-trips. No IRIX 6.5.30 binary sends it, a *software
    expectation*.

``sel [<name>|all|none|auto]`` and ``see [<name>|all|none]``
    Console input selection and output filter. Each takes two characters
    from the character reader, folded to uppercase. A processor name is a
    slot digit ``1``-``4`` and ``a`` or ``b``, processor codes 0 to 7
    (``1a`` is 0, ``4b`` is 7). With no argument ``sel`` reports
    ``ok auto``, ``ok none`` or ``ok`` and a name, and ``see`` reports
    ``ok all`` or ``ok`` and a name. ``sel`` with a name selects that
    processor for input; ``all`` and ``none`` (two letters count) disable
    selection; ``auto`` follows the most recent output sender. ``see``
    with a name copies only that processor's ``acp`` output to the serial
    line, still accepting and counting the rest, and also selects it for
    input; ``all`` and ``none`` copy everything. Anything else is
    ``err arg``. Every input selection discards queued input not yet
    fetched. The Owner's Guide names automatic selection as the power-on
    default and output from every processor as the output default
    :src:`007-3708-002 p. 132`, *established*. The model starts there
    with no output sender yet, so queued input is announced to nobody
    until a processor sends output. ``clr``, ``rst``, ``nmi`` and a bus
    reinitialisation leave selection and sender alone.

``nic <pulse> <sample> <delay>``
    One-wire passthrough: the three hex values bit-bang the midplane
    one-wire line and the reply is ``ok`` and the sampled bit. Any value
    missing is ``err arg``. On an Origin 2000 module the line reaches the
    module's identity parts (:ref:`sgi-origin2000-module-identity`). On an
    Origin 200 no device is modelled on it, so every sample reads ``1``,
    an *approximation*.

``aut [1|0]``
    Origin 200 build only. The automatic power-on bit, ``0x02`` of the
    configuration byte at NVRAM ``0x707``. The query answers ``ok 1`` or
    ``ok 0``; ``aut 1`` and ``aut 0`` read, modify and write the byte and
    answer ``ok``; anything else is ``err arg``. The controller reaches
    its NVRAM directly rather than by a nested bus transfer. A fresh store
    answers ``ok 1`` (:ref:`sgi-elsc-nvram`). The bit only matters when
    the controller starts, which the model does not separate from the
    machine's start.

``clr``
    ``ok`` whether the module is on or off. Cancels a pending power-down
    countdown, the heartbeat watchdog and the fan-high request, turns
    serial echo back on and ends serial supervisor mode. The Origin 2000
    build also blanks its display. The ``tas`` cell, margins, ``fsc`` and
    ``tst`` flags and the NVRAM are untouched.

``fan [h|n]``
    ``fan h`` and ``fan n`` set and clear the fan-high request and answer
    ``ok``; any other letter is ``err arg``. The query answers ``ok h`` or
    ``ok n``. The model has no fans or temperature inputs, so the request
    alone decides. Over I2C neither form writes the NVRAM.

``fsc [1|0]``
    Declares a multimodule controller present or absent and answers
    ``ok``. The query form, and any other argument, answers a plain
    ``ok`` and changes nothing. In the Origin 2000 build the flag mirrors
    host display text to the serial line.

``tst <c>``
    ``1`` sets the short factory timeouts and any other character clears
    them, answering ``ok``; no character is ``err arg``. The flag is only
    stored.

``tmp``
    ``ok n``: no temperature inputs. A master that has heard ``sla``
    ``0x07`` from its slave appends a space and the slave's last
    temperature letter.

``dsp <message>``
    Origin 2000 build only. Up to eight printable characters, spaces
    included, written onto the display from its first position, leaving
    the rest. The message stops at the first unprintable character. Empty
    is ``err arg``; otherwise ``ok``.

``dsc <position> <character>``
    Origin 2000 build only. One display character: the position is the
    digit ``1`` to ``8``, for positions 0 to 7, and the character is
    printable and never a space. Anything else is ``err arg``.

``key``
    Origin 2000 build only. ``ok on``: the model's key switch is always
    on.

``vlm <supply> [h|l|n]``
    Origin 2000 build only. The supply is ``3``, ``5`` or ``v``. With no
    second character the reply is that supply's margin, ``ok h``,
    ``ok l`` or ``ok n``; a second character sets it. ``vlm 5h`` and
    ``vlm 5 h`` are the same. Anything else is ``err arg``. The margins
    are stored only.

``sla <c>``
    The report a slave controller sends its master. The Origin 2000 build
    answers ``ok`` and does nothing. In the Origin 200 build a master
    takes ``n``, ``h`` and ``o`` as its slave's temperature and ``0x07``
    as the slave announcing itself, and a slave stores the character. A
    master logs ``LOG_UNIMP`` instead of carrying out the shutdowns and
    fan reports that ``o``, ``0x04``, ``0x05`` and ``0x06`` would start.

Module power
------------

The controller decides whether its module is powered. The Origin 200
controller holds that itself; an Origin 2000 controller drives its
module's power output. A power event, and the reset ``rst`` asks for,
reaches the controller's own module and no other. While a module is
unpowered the controller keeps running: ``pwr`` gives ``ok d``, ``rst``
and ``nmi`` give ``err cmd``, a pulse already under way reaches no
processor, and serial commands still run. An Origin 2000 controller
stops publishing grants while its module is off; an Origin 200 controller
keeps publishing.

Removing power resets and halts that module's processors with their
Hub's processor enable clear, while the virtual clock and the rest of the
machine run on. When power returns the machine sets the cold-power latch
and cold-starts that module and no other. What an unpowered module's
parts do is described on :ref:`sgi-ip27-power-implementation`.

When a module loses power and every module is then off with nothing to
bring one back (no power-cycle countdown, no watchdog that will cycle a
module, no start-up power-up and no master forward pending), the machine
powers down. IRIX 6.5.30's ``shutdown -p`` relies on that. The machine
decides again when a pending power-up is cancelled on an unpowered module
(``hbt 0`` or ``clr``) and when a master's forward is delivered or
dropped. A module that is off while others run stays stopped.

.. _sgi-elsc-master-slave:

Master and slave
----------------

On a two-module Origin 200 the master controller passes what it does to
its module on to the slave, which takes each as an ordinary command:

- ``pwr d 0`` whenever the master removes its own module's power: by
  ``pwr d`` (a countdown's expiry included), by ``pwr c`` and by the
  watchdog's power cycle;
- ``pwr u`` whenever the master has powered its own module up, which for
  ``pwr c`` is after its own off interval and power-up sequence;
- ``nmi`` for every ``nmi`` it accepts, and for the watchdog's NMI only
  once the slave has announced itself (``sla`` ``0x07``);
- ``fan h`` and ``fan n`` while that link is up.

``rst`` is never forwarded. A slave's own countdown, or a ``pwr c`` sent
to it directly, acts on the slave alone.

Only one forward is pending at a time; a later one replaces an earlier
one not yet sent. The model tries it once per revolution of the
five-position grant ring, every 5 ms, at most five times, then drops it.
An attempt fails only while the slave waits out its power-up sequence.
The master has done its own part either way. Whether the two controllers
have a link of their own, and what it carries, is *unknown*: the forward
is a model mechanism.

The IRIX 6.5.30 kernel sends its power-off ``pwr d 1`` to the
lowest-numbered module's controller, a *software expectation*. The model
gives the master the first node and the lowest module number, so the
master's countdown expires, it forwards ``pwr d 0``, both modules go off
and the emulator ends. That the lowest-numbered module is the master on
hardware is *inferred*, not established.

Start-up power-up
-----------------

By default an Origin 2000 module starts unpowered and its controller
powers it up inside the machine's reset, which stands for the cabinet's
AC cycle, through the same power-up that ends a power cycle, with no
wait. Power is on before any processor runs its first instruction. An
Origin 200 module starts powered. How long a real controller waits
before powering its module up is *unknown* from public sources.

The machine property ``module-start-delay`` (milliseconds) models a wait
instead: every module starts off, each controller that powers its module
up on its own counts the delay on the virtual clock and then runs the
whole power-up sequence, and an Origin 200 slave waits for its master's
forward. The machine does not end while that is pending. A key switch,
over-temperature or a multimodule controller cannot cancel it, because
none is modelled.

Controller state across resets
------------------------------

The controller runs on standby power, so no module reset or power change
names it. The Origin 200 controller sits on its Hub's adapter bus, which
the module's cold start resets, so it rides through that reset; the
Origin 2000 controller's bus belongs to the module-management device,
which the module's start does not touch. Through such a reset it
reinitialises its bus interface and queues, frees the ``tas`` cell and
drops any message queued for the Hubs. It keeps module power, its
countdowns, the waits of an accepted ``rst`` or ``nmi``, the watchdog,
console selection and sender, the fan request, the ``fsc`` and ``tst``
flags, the margins and the display.

Any other QEMU reset, ``system_reset`` included, stands for an AC cycle:
every controller starts over, both of a two-module Origin 200 included,
and its start powers the module up as above.

Controller messages
-------------------

A timed ``pwr d`` queues ``pd remote`` for the Hubs, ``pwr c`` queues
``pd cycle`` and ``rst`` queues ``rst``. A message is announced with
signal 2 in every processor code's position, and ``get`` from a
processor returns its text, with no ``ok``, and stops announcing it to
that processor. The announcement lapses one second after the message is
queued, and a later message replaces an earlier one. On an Origin 2000
module the grant scheduler announces it. The Origin 200 build also
prints each message on its serial line. The IRIX 6.5.30 kernel starts a
controller-initiated shutdown when it reads ``pd remote``, a *software
expectation*. ``pwr c`` and ``rst`` queue their message once the caller
has read the ``ok``, and the restart that follows clears it, so no Hub
reads ``pd cycle``. The model has no thermal, fan, supply or key events,
so the controller's other messages never arise.

Deferred destructive actions
----------------------------

``rst``, an immediate ``pwr d``, ``pwr c`` and ``pwr u`` act only once
the caller has read the whole ``ok``. Each arms an action that passes
through two states:

Armed
    The ``ok`` is framed but not yet read through its last text byte. A
    later request supersedes the response and cancels the action, so a
    caller that never reads its ``ok`` gets nothing.

Committed
    The guest has read the ``ok`` through its last text byte. The action
    survives a superseding request and a migration, and it fires.

Committing on the read of the last text byte, not on a STOP or NACK,
keeps the action when a master NAKs that byte and then sends a
same-address repeated START with no STOP. Firing waits for the
transaction's next boundary, a NACK, a STOP or the next START, so the
action never runs while the guest is still reading the frame. It then
releases the I2C bus before the reset, power cycle or power-down, so the
controller is not left matched. A guest that reads the last text byte and
abandons the bus with no STOP and no repeated START commits the action
but never fires it. The stock PROM and kernel end every exchange with a
STOP, a *software expectation*.

A committed action runs as work queued on the requesting processor, at
its next translation-block boundary, so every processor stops at the same
point on every run under round-robin execution. With multi-threaded
execution only the outcome is guaranteed, not how far a peer runs first.
With no processor to queue on, a qtest access or an action re-armed after
a migration, a bottom half runs it. ``rst`` only starts its one-second
wait there.

Where an Origin 2000 module owns the controller, the module is asked
whether it can carry out the action before the command is answered, and
a command it cannot carry out is answered ``err cmd``: a caller treats
``ok`` as proof that the action happened. With no module owner, the
Origin 200, the controller answers for itself.

A destructive command that arrives while another is committed cannot
change it. It still answers ``ok``, because refusing would make an IRIX
caller fall back to its own reset and race the committed action, and the
mismatch is logged ``LOG_GUEST_ERROR``. The outcome follows the committed
action:

.. list-table::
   :header-rows: 1

   * - Committed, then requested
     - ``rst``
     - ``pwr c``
     - ``pwr d``
     - ``pwr u``
   * - reset
     - exact
     - restarts; wrong name
     - restarts, not off
     - module on: no-op
   * - power cycle
     - restarts; wrong name
     - exact
     - restarts, not off
     - no-op while on; else on after the interval
   * - power-down
     - off, no restart
     - off, no restart
     - exact
     - module on: no-op; ends off
   * - power-up
     - ``err cmd``
     - on, no off interval
     - on, not off
     - exact

A committed reset, cycle or power-down leaves the module on until it
fires, so ``pwr u`` meets a powered module and nothing is logged. A
committed power-up leaves it off, so ``rst`` is refused and ``pwr d`` gets
the unpowered module's plain ``ok``, also unlogged. This table is model
behaviour. No guest sends two destructive commands back to back, and what
hardware does is *unknown*.

Alphanumeric display
--------------------

The Origin 2000 build drives the module's eight-character front-panel
display. The IRIX 6.5.30 IP27 PROM writes each node's boot progress as
two hex digits at positions 2s-1 and 2s for slot s with ``dsc``, and the
IO6 PROM and kernel write short messages with ``dsp``. They send the same
commands to an Origin 200, whose build answers ``err cmd``, and ignore
the reply either way, a *software expectation*. Which physical end of the
display is position 1 is *unknown*. The display starts with the version
text, ``VER 3.2`` and a space.

The controller replaces host text with status messages of its own at a
timed ``pwr d``, ``rst``, ``nmi``, the watchdog's warning and module power
changes. Their texts are *unknown*, so the model invents none:
``info elsc`` shows the last host text and names the status message that
replaced it. A later ``dsp`` or ``dsc`` replaces the message, and ``clr``
blanks the display.

.. _sgi-elsc-arbitration:

Bus arbitration
===============

The controller and the Hubs share the bus with no multi-master scheme in
software. The controller publishes a rotation of per-processor grant
bytes as address-only transactions, and a processor waits for its own
byte in its PCF8584's data register before starting a transaction.

For processor code ``c`` the grant is ``0xe0 + c`` (signal 1, nothing
waiting) or ``0xe8 + c`` (signal 2, something waiting). The code is twice
the node's slot index plus the processor number, so codes run 0 to 7. The
IP27 PROM's wait computes its two bytes this way, and it requires the bus
free and its own byte absent, then present and unchanged for more than
250 of the PROM's converted Hub-counter units, nominally 250 us, a
*software expectation*. The grants of other processors supply the
absence; there is no separate gap transaction. The model presents
signal 2 while something waits for that processor: queued console input
for the processor input is announced to, or a controller message. The
Origin 200 ring also presents it while a response waits unread for its
requester; the Origin 2000 scheduler does not.

.. list-table::
   :header-rows: 1

   * - Machine
     - Positions
     - Tick
     - Busy tick
     - Other rules
   * - Origin 200
     - 5: codes 0-3, then a fifth position published as code 4's byte
     - 1 ms
     - publishes nothing, does not advance
     - starts at code 0 and advances after publishing; a two-module
       master publishes on both modules' buses
   * - Origin 2000
     - 8: codes 0-7
     - 1 ms
     - publishes nothing, does not advance
     - starts before code 0 and advances before publishing; five seconds
       of busy or failed ticks reinitialise the controller's interface

These schedules are the model's. No public source documents the
controller's grant values, ordering or timing, so each is *unknown* as
hardware. The fifth Origin 200 position stands for the slave controller
of a two-module machine; the model keeps it on one module too, an
*approximation*. Each grant stays readable until the next replaces it,
which covers the PROM's hold check. The Origin 200 ring re-arms each tick
from when it ran, so a late host tick lengthens that position rather than
shortening the next; the Origin 2000 scheduler ticks on a fixed
one-millisecond grid of virtual time however late the host runs it. A
STOP never triggers a grant; the controller waits for its next tick. On
the Origin 200 a serial command waiting for the bus also stops the
grants, unless a controller message is pending.

On the Origin 2000 module the scheduler gives its turn to a pending
controller transfer first and a pending serial command second, but to
neither while a controller message is pending; a handoff skips
publication without advancing the code. A busy tick, a tick while the
controller is addressed as a slave and a refused grant count down a
five-second recovery, and a completed grant reloads it. At zero the
controller reinitialises its interface and protocol queues and releases
any line it held. Without that release, a node that writes STOP after
acknowledging a byte of a controller response would find the controller
holding SDA low for the next byte, and the bus would stay busy. The
release is *inferred* from the specification's description of clearing
ESO :src:`philips-pcf8584-1997`; no capture of the lines across a
recovery exists.

The ``arbitration`` property, on by default, enables the Origin 200
rotation. The Origin 2000 module-management device switches it off on its
controller and runs the eight-code scheduler on the shared wire instead.

AUX serial port
===============

The Origin 200's AUX connector is an 8-pin mini-DIN on the rear of the
module carrying RS-232 at 9600 baud, 8 data bits, 1 stop bit and no
parity, with flow control none or XON/XOFF. It reaches the master module
only and cannot be used to log in to IRIX
:src:`007-3708-002 p. 18, 120-122, 194`, *established*. The model's line
has no flow control and never stalls.

``-serial`` lines are node-major with a stride of six: node *n* owns
indices 6n to 6n+5, and its AUX slot is the third. The external AUX
backend attaches to the master, node 0 at index 2; a backend at the
slave's index 8 is not attached. When console output arrives with no
backend attached, the model logs ``LOG_GUEST_ERROR`` once and keeps
serving the protocol. The IRIX console is :term:`IOC3` serial A, not this port.

The operator's command line
---------------------------

Everything typed is console input for the processors except Ctrl+T
(``0x14``), which starts a command for the controller; the operator tests
the line with Ctrl+T and Enter and gets an ``MSC>`` prompt
:src:`007-3708-002 p. 122-123, 132`, *established*. Console input waits
for ``get`` in a 256-byte queue and is announced as ``sel`` describes; a
full queue keeps taking bytes and overwrites the oldest.

- After Ctrl+T, with echo on, the model prints a line end, ``MSC>`` and
  a space. Typed characters are stored and echoed; backspace or delete
  removes the last one. A carriage return, line feed or NUL ends the
  line. A second Ctrl+T abandons the line, erases the prompt and passes
  one Ctrl+T on as input; what was typed starts the next line.
- Console output that arrives while a line is being typed is acknowledged
  over I2C and lost.
- A finished line runs once the bus is free, the controller's I2C slave
  is idle and no Hub message is pending; an I2C command arriving at the
  same moment goes first.
- The line is parsed as an I2C request. The reply is Ctrl+T, the reply
  text (``ok`` with no trailing space when it carries no value), and a
  return and line feed; no prompt follows. ``fsc`` with no setting prints
  ``ok1`` or ``ok0``. Ctrl+T and Enter alone print ``ok`` with echo on
  and ``err cmd`` with it off.
- ``ech`` sets echo (``1`` on, ``0`` off, none toggles); ``clr`` turns it
  back on.
- Supervisor mode starts off. ``pas <word>`` with exactly four characters
  matching the password at NVRAM ``0x701``-``0x704`` sets it, and anything
  else is ``err arg``; ``pas`` alone clears it; ``pas s <word>``, in
  supervisor mode, stores the first four characters of a word at least
  that long. ``clr`` from either channel clears it. Without it, ``clr``,
  ``hbt``, ``nmi``, ``rst``, ``pwr u``/``d``/``c``, the setting forms of
  ``fan``, ``aut``, ``vlm``, ``fre``, ``mod`` and ``dbg`` and ``nvm``
  writes print ``err perm``. The Owner's Guide gives the factory password
  ``none`` and the ``err perm`` reply :src:`007-3708-002 p. 124`,
  *established*.
- ``acp`` and ``get`` print ``err cmd``. ``fan h`` and ``fan n`` also set
  or clear bit ``0x01`` of the NVRAM configuration byte. ``mod`` reads or
  sets the module number at ``0x708``, ``dbg`` the debug-switch bytes at
  ``0x705``-``0x706``, ``fre`` (Origin 2000 build) the clock-select byte
  at ``0x70b``, and ``nvm <addr> [<data>]`` any byte of the NVRAM, the
  address's high byte choosing the I2C address, so addresses above
  ``0x7ff`` alias or reach other devices; a device that does not answer
  prints a bare ``err``. ``cfg`` prints the NVRAM's bytes from 0 through
  the first zero byte, at most ``0x700`` of them.
- A command's power or reset action runs once its reply is printed.
- At its start the controller prints Ctrl+T, ``MSC`` and a space, its
  version field and a line end. The Origin 200 build prints each Hub
  message as Ctrl+T, return, line feed, ``MSC:``, a space, the message,
  Ctrl+T and a return. An Origin 2000 controller with ``fsc 1`` copies
  each host ``dsp`` as Ctrl+T, ``dsp``, a space, the text, the digit 4
  and a line end.
- An Origin 200 slave answers Ctrl+T with the prompt and ``Slave!`` when
  echo is on and takes no command. Its line has no backend.

.. _sgi-elsc-nvram:

Configuration NVRAM
===================

Each module system controller keeps a 2048-byte configuration store on
its module's I2C bus, addressed 0 to 2047 with 1792 to 2047
(``0x700``-``0x7ff``) reserved for the controller
:src:`108-0170-001 p. 4-22`, *established*. It holds the module number,
1 to 255 with 0 unassigned, which each node's PROM log backs up
:src:`108-0170-001 p. 1-4`, *established*. An Origin 2000 module's one
store is shared by its four Hubs (:src:`108-0164-002 p. B-1`). The
Origin 2000 deskside owner's guide gives the controller's NVRAM as 1024
by 8 bits :src:`007-3453-002 p. 83`, which conflicts with the 2048
addresses the PROM uses. The part, its package count and the board that
carries it are *unknown*.

Address ``A`` answers at seven-bit I2C address ``0x50 + (A >> 8)`` with
the one-byte word address ``A & 0xff``, the convention of a 24C16-class
EEPROM. The stock IP27 PROM reads the controller block at ``0x57`` that
way, a *software expectation*. Addresses ``0x000``-``0x6ff`` hold the
board inventory log the PROM writes.

.. list-table::
   :header-rows: 1

   * - Address
     - Field
     - Fresh store, Origin 200
     - Fresh store, Origin 2000
   * - ``0x700``
     - Validity marker, ``0x37``
     - ``0x37``
     - ``0x37``
   * - ``0x701``-``0x704``
     - Password, four characters
     - ``none``
     - ``none``
   * - ``0x705``, ``0x706``
     - Virtual and physical debug-switch values
     - ``0``
     - ``0``
   * - ``0x707``
     - Configuration: bit 0 fans high, bit 1 automatic power-up
     - ``0x02``
     - ``0``
   * - ``0x708``
     - Module number
     - ``0``
     - the module's selected number
   * - ``0x709``
     - Self-test flags, two bits per node board
     - ``0``
     - ``0``
   * - ``0x70a``
     - Partition
     - ``0``
     - ``0``
   * - ``0x70b``
     - Domain; also the controller's clock-select byte
     - ``0x78``
     - ``0``
   * - ``0x70c``, ``0x70d``
     - Cluster, cell
     - ``0``
     - ``0``

The field layout is SGI's, published in the Linux history tree's
``v2.5.0`` SN code, *established*; the factory password ``none`` is in
the Owner's Guide :src:`007-3708-002 p. 124`. Every other byte of a fresh
store is zero.

The fresh-store values, ``0x78`` at ``0x70b`` on the Origin 200 and 0
there on the Origin 2000 included, are the model's choice, an
*approximation*. On the Origin 2000 that 0 is also the domain a guest
reads. Factory contents are *unknown*, and an erased EEPROM usually reads
``0xff``. A two-module Origin
200's slave store starts like the master's; which store a slave module's
Hub reaches on hardware is *unknown*.

The model is one device answering eight addresses, not eight parts. A
transfer is a word-address write, optionally followed by data bytes, or
by a repeated START and a read. A sequential read or write past a block's
last byte wraps to that block's first byte, a write of any length is
accepted, writes complete at once and are never refused as busy, and the
part does not answer a general call. A real 24C16 wraps writes within a
16-byte page; block-end behaviour, the write buffer and the write-cycle
time are *unknown*, so all of this is *approximation*. A twelve-byte
write at ``0x700`` is accepted as one transaction.

The store survives every reset, power change and migration between
identical builds. With a backing file bound through the machine's
``module<N>-nvram`` property it also survives QEMU processes; the file
must be exactly 2048 bytes, and its contents win over the fresh-store
values. A write the backing file refuses is reported on standard error
and the byte stays in the store.

Monitor
=======

``info elsc`` prints, for each controller: module power and any pending
power-up, a pending forward to the slave, the console-input queue and a
preview of it, the Origin 2000 display and any status message replacing
it, a pending controller message, a pending response and its owner, the
input selection and output filter, the next ring position and the grant
byte due for processor code 0.

Migration
=========

The controller, adapters and NVRAM migrate between identical builds
only. The controller's deferred action, its timers on the virtual clock,
power state and cold-power latch travel with it, so a migration or a
record and replay reproduces an off interval. A controller whose module
is off holds that module's processors again after a load, and a module
start owed when the source stopped is made when the destination starts
running. A committed action is re-armed on load only if its firing was
already scheduled at a transaction boundary; otherwise it waits for the
destination's boundary. A master's pending forward travels in a
subsection present only while one waits. Loads reject states the devices
could not have produced.

Measurements
============

No reading has been taken from a physical module system controller,
PCF8584 or controller NVRAM for this model, and no published capture
records the controller's bus traffic. What would settle the open points
above: a ``ver`` and command probe over a real controller's serial port,
a logic capture of a module's I2C bus during boot, and a dump of a real
controller NVRAM (POD ``scr`` over all 2048 bytes).

.. _sgi-elsc-limits:

Model limits
============

- **No public protocol.** The command set, replies, timings, version
  texts and both grant schedules are the model's. Hardware may differ.
- **Corrupted requests.** A request with a bad checksum or length is
  answered ``err csum`` without running.
- **Slave reports.** A slave controller never sends ``sla`` reports to
  its master. A master that receives ``sla o``, ``0x04``, ``0x05`` or
  ``0x06`` logs ``LOG_UNIMP``.
- **No sensors.** There are no temperature, fan, supply or key-switch
  inputs. The module is cool, its fans and supplies never fail and its
  key is on, so none of the shutdowns, messages or replies those inputs
  drive arises. The power-up supply check always passes.
- **No interrupts.** The controller's interrupt to the Hub and the
  PCF8584's interrupt output are not connected.
- **Timer arithmetic.** Countdowns and the watchdog run the nominal number
  of seconds on the virtual clock; a real controller's millisecond
  counting is *unknown*. Removing power takes no time.
- **Display status texts** are not known and are not shown.
- **Midplane clock and one-wire.** The Origin 2000 ``fre`` byte has no
  clock chip behind it. The Origin 200 one-wire line has no device on it.
- **NVRAM missing.** Every machine gives the controller its NVRAM. A
  controller without one answers ``pas``, ``aut`` and the serial NVRAM
  commands as if every byte were zero, loses what they write and logs
  ``LOG_UNIMP``.
- **Unexpected access.** A data-register write during a receive, or while
  a byte is on the shared wire, is ignored with ``LOG_GUEST_ERROR``. A
  repeated START part way through a byte, a STOP during a repeated START
  or STOP, and a line transition the shared wire refuses log
  ``LOG_UNIMP``. On an Origin 2000 module a destructive command the
  module declares no path for, a reset or NMI requested during another
  pulse, a refused grant and a peer's START overtaking a grant log
  ``LOG_UNIMP`` or ``LOG_GUEST_ERROR``; console output or a message for a
  processor code above 7 logs ``LOG_GUEST_ERROR`` and is dropped.

Tests
=====

``tests/qtest/sn0-elsc-test.c`` drives the PCF8584 register sequences the
PROM uses and covers framing and the request grammar, ``ver``, ``rsw``,
errors, the Origin 200 command set, console input and output with and
without a backend, the operator's command line and supervisor mode,
``pwr`` in every form with its countdowns and cold-power latch, ``rst``
and the deferred-action rules (supersession, NACK of the last byte, no
STOP, bus release, migration), ``tas``, the ``hbt`` stages, ``sel`` and
``see``, ``nmi`` timing, ``aut``, ``nic``, ``info elsc``, the Origin 200
grant ring (one-millisecond tick, busy ticks, signal 2 for a pending
response) and two-module behaviour (one AUX, a shared five-tick ring,
slave power-off).

``tests/qtest/sn0-elsc-nvram-test.c`` covers fresh-store values, block
addressing, the twelve-byte block write, survival across
``system_reset``, a caller image winning over the defaults, persistence
across processes, migration and the two-module stores.

``tests/qtest/sn0-module-management-test.c`` covers the Origin 2000
module: four separate adapters, one controller, the shared NVRAM, the
eight-code grant rotation, STOP placement and arbitration loss on the
shared wire, recovery after a STOP following an acknowledged read byte,
the Origin 2000 build's commands and display, messages to every code and
their expiry, module reset, power-down, power cycle and start-up
power-up, the serial line, NMI and migration.

``tests/qtest/sn0-migration-test.c`` checks that loads reject out-of-range
framing lengths and ring positions. The functional test
``tests/functional/mips64/test_sn0_reset.py`` drives ``rst`` through the
PCF8584 from guest code on an Origin 200.
