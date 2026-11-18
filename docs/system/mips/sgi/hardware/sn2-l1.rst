.. SPDX-License-Identifier: BSD-3-Clause

.. _sgi-sn2-l1:

SN2 L1: node serial link
========================

Used in the Altix 350 machine, which attaches the device to the node's 16550
through a Chardev. The device is the
node's controller link: the node :term:`PROM` and the kernel's console driver talk to
it through the node's 16550.

.. seealso::

   :doc:`l1` describes the MIPS controller model.

The ``sgi-l1-sn2`` device (``hw/misc/sgi-l1-sn2.c``) implements an SN2
node's L1 link. It decodes the node's frames, answers the requests listed
under :ref:`Replies <sgi-sn2-l1-replies>`, forwards console text both ways and
serves DIMM serial presence detect and empty chassis and board information
areas. It
does not execute L1 firmware or supply any other board or EEPROM contents.
It shares :doc:`l1`'s frame decoder and typed-argument validator
(``hw/misc/sgi-l1-frame.c``) and nothing else: its requests, queue and
migration state are its own. The MIPS controller's product identities and
replies are not transferred to an Altix 350.

Sources
-------

The source is a reviewed, own-word account of the SN2 PROM-to-L1 link (``software/firmware/l1/sn2-prom-link-1.62.1.md`` in the sysco repository). It
identifies both stock binaries: the L1 1.62.1 Linux L1 executable, SHA-256
``22cb62871e447a394acbd662f4f49f3146580afec93839953b37a5354545bfc5``,
and the SAL 5.04 runtime in snprom 10.54's ``shub1snprom.bin``.
No firmware bytes are included in QEMU.

Each protocol fact below is a *software expectation*, not a measurement
of a physical controller. The L1 image's packet receive and request-header
routines (at ``0x10df80``, ``0xc2c58`` and ``0xc2284``) establish the receive
framing and request header. The node PROM's transmit and receive routines
(at ``0x52fe0`` and ``0x53b80``) independently agree on flags, FCS
polynomial/residue, and the maximum payload. Argument encoding is recorded
in the contract but was not independently re-read in its first review.

The chips notes' L1 firmware page on the 1.62.1 packages (what the ARM
packages serve) leaves the Altix 350's controller CPU/package assignment *unknown*. The
transport does not choose a ColdFire or ARM personality. Sharing the node
protocol does not establish that all their request handlers agree.

Receive behavior
----------------

* Flags are ``7e``. A flag terminates the previous frame and starts the next;
  repeated flags carry no packet. Bytes before the first flag are ignored.
* Escape ``7d`` causes the next byte to be XORed with ``20``. This also
  accepts escaped XON, XOFF and carriage return. As in the existing decoder,
  a second consecutive escape re-arms the first. No ``ff 03`` PPP prefix
  or protocol field is inserted or removed.
* The FCS starts at ``ffff``, uses reflected polynomial ``8408``, and has
  receive residue ``f0b8`` including the two check bytes. The transmitter
  complements the check and sends its low byte first.
* The modeled maximum is 128 payload bytes plus two FCS bytes: the PROM's
  console writer sends frames of that size (a class byte and 127 text
  bytes), observed in a boot. The real receiver's limit is *unknown*. An
  oversized frame, a short check, a bad check,
  or an escape interrupted by a flag is discarded and logged with
  ``LOG_GUEST_ERROR``. Discarding until the next flag after overflow is a
  bounded-buffer implementation policy, not a measured controller reaction.
* Payload byte 0 has a three-bit class and five-bit index. Class 0 requests
  have a big-endian destination at bytes 1--4, a big-endian request code at
  bytes 5--6, and the argument count at byte 7.
* Argument types are ``00`` plus four big-endian integer bytes, ``01`` plus
  a NUL-terminated string, or ``80 | length`` plus that many raw bytes.
  Truncated arguments, unknown types and trailing bytes are rejected with
  ``LOG_GUEST_ERROR``. Exact consumption is the parser's safety policy.
* Codes ``3001``--``3003`` go to a separate assembler in the firmware.
  They are logged as unsupported chunks, not parsed as ordinary arguments.

Valid requests are logged with index, address, code and argument count under
``LOG_UNIMP``. Other classes, including console events, are also logged as
unsupported. A request with no reply below is logged and not answered, and
the VM keeps running (see :ref:`Replies <sgi-sn2-l1-replies>`). The model does not manufacture a
successful EEPROM read from plausible DIMM data.

.. _sgi-sn2-l1-replies:

Replies
-------

A reply is a class-1 frame: byte 0 is ``0x20`` plus the request's index, bytes
1--4 are the status (big-endian, 0 for success, negative for failure), byte 5
is the number of arguments, and the arguments follow in the request
encoding. The frame check is computed as for a request and only ``7e`` and
``7d`` are escaped; the node link escapes neither ``0d`` nor the flow-control
bytes. All of this is *software expectation*: the L1 writer and the node's
reader were read independently and agree.

The bytes go to the node UART's Chardev, so the platform's 16550 sees them as
received characters. The model offers a byte only when the UART reports room
for it and sends the rest when the UART asks for more input, because the
real link runs at a serial rate and a 16-byte receive FIFO could not take a
whole reply at once. That pacing is an implementation policy. A reply that
has not yet been delivered is part of the migrated state and is dropped by
reset. The queue holds 1024 bytes. A reply that does not fit is dropped and
logged under ``LOG_UNIMP``; nothing stops the VM.

.. list-table::
   :header-rows: 1

   * - Request
     - Reply
     - Rating
   * - ``1005`` display text (one string)
     - Status 0, no arguments; -100 if the request has no argument. The text
       is traced, not shown anywhere.
     - Software expectation
   * - ``1007`` event subchannel (one integer: the subchannel slot the PROM
       claimed for events, 19 on the Altix 350)
     - Status 0, no arguments; -100 if the count is not one. The L1 sends no
       events afterwards.
     - Software expectation: the PROM's runtime sends the request and takes
       exactly this reply (a negative status closes the slot and the kernel
       goes on without events). The real L1's own events, class-2 frames with
       header ``0x40`` plus the slot, are not known and are not sent.
   * - ``100b`` PROM version report (three integers)
     - Status 0 and no arguments; -100 if the count is not three. **An
       estimate, not the L1's behaviour**: see below.
     - *approximation*. What the real L1 does is not known.
   * - ``0003`` rack, bay and type (no arguments)
     - Two integers: the module word ``rack << 6 | bay`` from the ``rack`` and
       ``bay`` properties (default rack 1, bay 1: 0x41), then the type letter
       ``O`` (0x4f). The PROM lowercases it to ``o``, the onboard I/O
       letter its :term:`PIC` attach accepts.
     - Layout: software expectation.

       Rack and bay values: *approximation*. Any module word gives the PROM
       a valid location, and rack 1, bay 1 reads as ``001c01`` in its
       console name.

       Type letter ``O``: *inferred*. The PIC attach takes the byte from this
       reply and accepts only ``#``, ``=``, ``m``, ``o`` and ``v``, so ``F``
       or the slab fold to ``c`` would make it reject the base I/O board with
       "Unknown ioboard". ``o`` is the only accepted letter that names an
       Altix 350.
   * - ``000e`` slab
     - One integer, 0.
     - Layout: software expectation.

       Value (one node): *inferred*.
   * - ``0013`` slot
     - Two integers: slot 0, then the type letter ``O``. The reply layout
       equals ``0003``'s. The PROM does not fold this letter by slab.
     - Layout: software expectation.

       Type letter ``O``: *inferred* as above.
   * - ``0009`` partition
     - One integer, the ``partition`` property (default 0).
     - Layout software expectation, value *inferred* (blank settings).
   * - ``0001`` debug switches
     - One integer, 0. With 0 the PROM takes its interconnect-discovery
       path.
     - Software expectation for a blank settings record. A deployed L1's
       word is not known.
   * - ``0015`` sharing mode
     - One integer, 0.
     - Layout: software expectation.

       Value: *inferred*.
   * - ``0012`` maximum nodes
     - One integer, 0. The PROM raises anything below 0x800 to 0x800, so
       only the status and the count matter to it.
     - Layout: software expectation. What the L1 reports is not known.
   * - ``0008`` system serial
     - Status -104 and no arguments, which is the L1's answer when no serial
       is set. The PROM falls back to a default text.
     - Software expectation for an L1 with no serial. A deployed serial is
       not known.
   * - ``0010`` processor presence (no arguments)
     - Four integers: node count 1, the processor count (the ``cpus``
       property, 1 or 2), then the presence word and the enable word of that
       node: ``0x1``, or ``0x5`` with two processors (slices 0 and 2). The
       PROM tests the bit of its own processor in presence AND enable, and
       parks any processor whose bit is clear.
     - Layout: software expectation.

       Values: *inferred*. What a blank L1 reports is not known, because the
       real L1 reads the masks from the board.

   * - ``0000`` reboot, to the controller task 3 (destination ``0x0ffff003``;
       one string, ``* rst`` or ``p <partition> rst``)
     - Status 0 and no arguments, then a machine reset request. Any other
       string is not answered.
     - Software expectation: the PROM's reboot call sends it and accepts
       any valid reply. What the real L1 does with the node is not known.
   * - ``0000`` L2 probe, to ``0x1ffff003`` (the string ``version``)
     - Status ``0xffffffff`` and no arguments: this machine has no L2, so
       the probe fails (the PROM takes only status 0 as success) and the
       PROM goes on to the plain reboot request above.
     - *inferred*. A real L1 with no L2 sends a routing-error reply of a
       documented shape (a status of ``0xffffffff`` and a four-byte code)
       that the model does not reproduce; silence makes the PROM wait about
       20 seconds. The reply shape here is a choice.
   * - ``200e`` get time (no arguments)
     - Status 0 and six integers: year, month, day, hour, minute, second,
       from the machine's RTC clock in UTC (so ``-rtc`` options apply).
     - Layout: software expectation.

       That the L1 keeps UTC: *inferred*.
   * - ``200d`` set time (the same six integers)
     - Status 0, and the clock does not change: the controller's set-time
       helper does nothing and returns 0.
     - Software expectation from the L1 image's helper.
   * - ``0006`` EEPROM read (device id, selector, offset, length)
     - A reply is status 0, an integer with the bytes left after the read,
       and a raw block (omitted when the length is 0). Selector 4 reads a
       DIMM's 256-byte serial-presence-detect record. Ids 8, 16, 9 and 17,
       bank 0, are populated with four 512 MB single-rank modules and serve
       the Micron MT9VDDT6472A table (Table 21 of its datasheet, rev. B)
       byte for byte, 2 GiB in all, with byte 63 the sum of bytes 0 to 62.
       Bytes 72 and up, which the datasheet calls variable data, read as
       zero. Every other slot, including 14, 15, 22 and 23, answers -104.
       An offset of 256 or more, or a selector above 4, is -105. A selector
       other than 4 on a DIMM is -104. Fewer than four arguments is -100.
       A read of 64 bytes is the most the PROM sends and the most the model
       serves. Device 1 (the node) selector 2, the board information area,
       answers status 0, the bytes left and a 64-byte window of a 512-byte
       IPMI FRU: a common header and an empty board area, each summing to
       zero. Selector 1, the chassis area, is the same shape with an empty
       chassis area. For both, an offset of 512 or more is -105. Device 98 (the Prism riser, which
       a 350 asks for only when its type is ``O``) answers -104.
     - Frame and statuses: software expectation.

       DIMM module: *approximation*. The DIMMs of a real Altix 350 are not
       known, so this is a part chosen so that the PROM sizes 2 GiB, with the
       datasheet's bytes.

       Node FRU contents: *approximation*. The format and the window reply
       come from the ColdFire handler's trace, but no source holds the
       contents, so every field is empty.

       Device 98 answering -104: *inferred*. It is faithful for a unit with
       the riser-absent strap, whose polarity is the inferred part.

``100b`` is the first request the PROM sends, with the integers 1, 5 and 4
(the PROM version). The L1 program read sends no reply to it, and the PROM
waits three seconds, runs its fatal-error routine and parks the processor.
Published logs show PROM builds with the same wait and fatal-error code
(4.65 and 4.80) reaching the EFI banner on real machines, so something ended
the wait there, and nothing read says what. The model sends the status-0,
no-argument frame the PROM's receiver accepts, so a boot can go on. That
frame is an *approximation* with its limitation stated: it is a guess at
what ended the wait, not hardware behaviour, and a reading of a real
machine may show that nothing is sent. Each answer is traced
(``sgi_l1_sn2_estimate``) and logged under ``LOG_UNIMP``.

The requests from ``0003`` on take no arguments, except ``0006``. One sent
with arguments is not answered.

Any other valid request is not answered either. That covers a code with no
row above, a request with a shape the row does not describe, a chunk code
(``3001`` to ``3003``) and an EEPROM read of a device or selector whose
contents are unknown. The model logs it under ``LOG_UNIMP`` with the
request code, index, destination and argument bytes, sends no reply and
leaves the VM running. It never answers with a made-up success. Console frames
(class 2) carry the PROM's text. With a ``chardev`` property the model writes
the bytes after the class byte to it, and without one it logs and ignores
them.

A byte read from that chardev goes to the node as a console input frame, one
character per frame: ``7e``, the header, the character, the CRC low then high
byte, ``7e``, with the same escaping and CRC as every other frame. The header
is the console index of the last console output with the class bits clear
(index 0 for the PROM's text, so typing ``a`` sends ``7e 00 61 c8 7d 5d 7e``).
The frame layout, the header and the CRC are *inferred* from a static reading
of the PROM's receiver. The real L1 forwards typed characters only in its
pass-through console mode; the model forwards them always. The PROM drops a
frame unless a console poll is outstanding, so input is only seen once it
reads the console.

Integration and state
---------------------

A platform selects ``CONFIG_SGI_L1``, creates a ``sgi-l1-sn2`` device with
the ``rack``, ``bay``, ``partition`` and ``cpus`` properties and an optional
``chardev`` for the console, realizes it, and connects the node's 16550 to
the chardev the device creates (type ``chardev-sgi-l1-sn2``, returned by
``sgi_l1_sn2_link()``) through that UART's ``chardev`` property. The
controller has no
MMIO registers. The platform owns UART spacing, interrupts and mapping. SAL
5.04 uses an eight-byte register stride at local address
``0x8000000ffed40000``; a separate scan accessor's shift by six is not this
UART's stride.

The receive callback runs synchronously under the UART's BQL. Device reset
clears partial-frame, escape and overflow state, the reply queue and the
operator mode. The device's own migration section (``sgi-l1-sn2``, version
1) carries those fields and the typed-ahead operator line. The shared decoder
checks the FCS at the closing flag, so a frame may continue across migration,
including between the two bytes of an escape.

Node identity
-------------

``sgi_l1_sn2_identity_word(rack, bay, partition)`` in
``include/hw/misc/sgi-l1-sn2.h`` builds the 64-bit word the PROM reads from
SH_SCRATCH2: rack bits 8:0 in bits 34:26 and rack >> 6 in 35:32, bay in
25:20, partition in 15:8 and ``0x6c`` in 7:0. The device does not store the
word anywhere. The platform decides where it goes, and builds it from the
same three numbers it gives the device. The layout follows the shipped L1
firmware; the rack, bay and partition values are a choice, an
*approximation*.

Model limits
------------

The reply header and the replies above follow the reviewed contract. The
console frame class and index values and the event payload are *inferred*.

Firmware revision requests are unsupported rather than borrowing another
product's behavior. A request that needs one is not answered, as above.

The operator's L1 command interface is modelled to the one command that ends
a run. Ctrl-T on the console enters L1 mode; the line ``pwr d`` and a return
then asks for a machine shutdown, and the model prints nothing: no prompt, echo
or error text is sourced. Ctrl-D leaves the mode, and Backspace edits the line.
Any other line is logged under ``LOG_UNIMP`` and the console goes back to
typed input. The mode and the
Ctrl-D exit are an *approximation*: the keys follow the MIPS L1 profile, and
what the SN2 L1 prints is *unknown*. The node's power-down request (SN_SAL
system power-down) sends the L1 nothing (*inferred*), so the guest spins after
the kernel prints ``reboot: Power down`` until the operator types ``pwr d``.
Whether hardware does the same is not known. A guest reboot does work, through the L1's reboot request.

Tests
-----

``tests/unit/test-sgi-sn2-l1.c`` tests the actual chardev, malformed input,
resynchronization, reset and partial-frame migration without guest firmware.
The test checks every reply byte for byte, including the wire vector for a
status-0 reply that the PROM-side contract gives, a receive FIFO that takes
four bytes at a time, a reply held by a full FIFO across reset and
migration, and that a request with no known reply is logged, gets no
reply and does not stop the VM.
