.. SPDX-License-Identifier: BSD-3-Clause

.. _sgi-l1:

=====================================
SGI L1 system controller (Origin 300)
=====================================

The L1 is the per-chassis system controller of the Origin 300, Origin 350
and Fuel. QEMU models the node-facing side in ``hw/misc/sgi-l1.c``: a
16550-style UART in the Bedrock's Hub special space speaking a framed
request and response protocol, which the PROM, IRIX and the other
guests use to read board records, drive the display and carry the
operator console. The controller's board-side interfaces (environmental
monitoring, JTAG scan, USB to an upstream controller, the RS-422
peer links) are not modelled. This page states what public sources
establish, what the model chose, and what has no public source.
Ratings follow :ref:`sgi-ip30`.

Sources: OpenBSD sgi at
``openbsd-sgi:2df91d80c104636deeef8124252ddd5951b55fbf``
(``sys/arch/sgi/sgi/l1.c``, ``l1.h``, ``sys/arch/sgi/xbow/hub.h``); SGI's
Linux SN1 port at ``linux-history:7570df54ef8c`` (tag v2.5.40,
``arch/ia64/sn/io/l1.c``, ``l1_command.c``, ``include/asm-ia64/sn/ksys/l1.h``);
the out-of-tree MIPS port ``linux-ip35:9f613a321ea0``
(``arch/mips/sgi-ip35/ip35-l1.c``); *SGI L1 and L2 Controller Software
User's Guide* (007-3938-001 and -006, printed pages cited below); *Origin
300 User's Guide* (007-4365-002); *Origin 300 System with NUMAlink Module
User's Guide* (007-4498-002); *Origin 3000 System Architecture*
(108-0240-002); *PCI Expansion Module User's Guide* (007-4499-002);
*Fuel Visual Workstation Hardware User's Guide* (007-4480-001).

.. _sgi-l1-role:

Role
====

.. list-table::
   :header-rows: 1
   :widths: 46 12 42

   * - Fact
     - Rating
     - Source
   * - The Origin 300 has IP45 L1 logic for fans, temperature and LEDs, and
       an I/O-8 assembly with a rear L1 console connector and a separate
       management USB port toward an optional upstream controller. The
       display reads "L1 running" on mains power before the rest of the
       chassis is powered.
     - established
     - 007-4365-002 printed pages 10, 12 and 35; 007-3938-001 printed
       page 3 (the USB connection as a slave port).
   * - The Fuel has an L1 as a dedicated diagnostic connection, separate
       from the external serial ports, and two USB ports.
     - established
     - 007-4480-001 printed pages 6 and 127-130.
   * - The PCI expansion module has its own L1: display hardware at the
       front, control logic on the power board; it monitors the module
       and controls power to its PCI slots, runs on 12 V standby, and can
       report that power is insufficient for all fitted cards.
     - established
     - 007-4499-002 printed pages 36, 48 and 50. Its controller protocol,
       identity and the guest-visible effect of slot-power changes are
       unknown.
   * - Three public consumers speak to the L1 over its UART: OpenBSD sends
       four request kinds and never uses it as a console; the Linux SN1
       port sends more and uses it as its early console; the out-of-tree
       MIPS port uses it only as a console.
     - established
     - The three consumers' sources above.
   * - What the stock IP35 PROM and IRIX 6.5.30 send.
     - unknown
     - No public source states it. The model's replies to those requests
       come from tracing the stock PROM's and IRIX's accesses; a
       capture of an L1 session from a real chassis would settle them.

.. _sgi-l1-uart:

UART window
===========

The UART is reached in Hub special space at the node's remote register
window plus ``0x80``, that is the node's Hub special base plus
``0x3000_0080``, one 16550 register per 8 bytes, accessed as 64-bit loads
and stores of which only the low byte is significant (established: OpenBSD
``hub.h`` lines 68-73 and ``l1.c`` lines 97-142; Linux SN1 ``l1.c`` lines
61-77; ``ip35-l1.c`` lines 28-50). All three consumers use the remote form;
none uses the local form. What no public source states is whether the
Bedrock's memory and directory unit decodes this window or another unit
forwards it (unknown; the unit's junk-bus timing register carries setup and
hold fields for the UART, Linux SN1 ``hubmd.h`` lines 1059-1097).

.. list-table::
   :header-rows: 1
   :widths: 14 30 56

   * - Offset
     - 16550 register
     - What the consumers require
   * - ``0x00``
     - receive and transmit data
     - A write hands a byte to the frame parser; a read pops the next
       byte the L1 has for the processor.
   * - ``0x08``
     - interrupt enable
     - Accepts writes; the SN1 port writes 0.
   * - ``0x10``
     - interrupt identification, FIFO control
     - The SN1 port writes ``0x01`` then ``0x07``; read ``0x01`` (none
       pending) in polled operation.
   * - ``0x18``
     - line control
     - Read-write latch; the SN1 port writes ``0x80`` for the divisor then
       ``0x03``.
   * - ``0x20``
     - modem control
     - Accepts writes; the SN1 port writes ``0x22``.
   * - ``0x28``
     - line status
     - Bits 5 and 6 set whenever a byte can be accepted; bit 0 set
       exactly while a received byte waits; bits 2 and 3 clear.
   * - ``0x30``
     - modem status
     - Bit 4 (clear to send) set; the SN1 early path and the MIPS port
       spin forever without it (``l1.c`` line 1973; ``ip35-l1.c`` lines
       56-57).
   * - ``0x38``
     - scratch
     - Unused.

The SN1 port programs a divisor of 13 against a 12 MHz clock for 57,600
baud, and the L1's own ``uart`` command reports the Bedrock-side port at
57,692 baud, exactly that divisor (007-3938-006 printed pages 110-111);
the SN1 port's raw-byte configuration uses a baud base of 124,800
(``kernel/setup.c`` lines 236-259), so the model enforces no divisor. OpenBSD
never programs the UART and polls line status up to a million times per
byte with no wall-clock bound (``l1.c`` lines 100-144), so a silent UART
costs it nine such waits per node (established). The model also decodes the
same eight registers at the local form's ``+0x80`` (node Hub special base
plus ``0x1000_0080``), sharing state: an *approximation* with no public
consumer, kept because the vendor field-diagnostic monitor sMDK writes its
console there when booted headless under the stock PROM (observed on the
model; the diagnostics' own source is not cited). A read of Hub special
local offset ``0x80`` on a physical Origin 300 or 350 would settle whether
real silicon answers there.

.. _sgi-l1-framing:

Framing and packets
===================

.. list-table::
   :header-rows: 1
   :widths: 46 12 42

   * - Fact
     - Rating
     - Source
   * - Frames are HDLC-like, as in PPP: a ``0x7e`` flag on each end, ``0x7d``
       escaping ``0x7e`` and ``0x7d`` by exclusive-or ``0x20``, and a
       16-bit frame check initialised ``0xffff``, complemented and sent low
       byte first over every payload byte from the header to the last
       argument; a receiver's running check over payload plus the two check
       bytes equals ``0xf0b8``. Frames shorter than two bytes are dropped.
     - established
     - All three consumers agree: OpenBSD ``l1.c`` lines 156-306 with
       ``ppp_defs.h``; Linux SN1 ``l1.c`` lines 737-1006 and 1186-1474;
       ``ip35-l1.c`` lines 99-238.
   * - Only the SN1 receiver tolerates repeated flags between frames
       (``l1.c`` lines 1239-1242, 1327); OpenBSD reads to the next flag
       and the MIPS port loses the next frame after a doubled or shared
       flag (``l1.c`` lines 266-293, 525-526; ``ip35-l1.c`` lines
       193-217). The model therefore sends exactly one opening and one
       closing flag per frame and never shares a flag.
     - established
     - The same lines.
   * - Payloads are at most 127 bytes (the SN1 send-side clamp and the
       seven-bit binary length).
     - established
     - Linux SN1 ``l1.c``.
   * - The header byte carries the packet type in bits 7:5 and the
       subchannel in 4:0: request ``0x00``, response ``0x20``, event
       ``0x40``. Subchannels 0 to 3 are per-processor consoles, 4 the
       system console, 16 to 31 operating-system requests (OpenBSD uses
       16 for everything). A response returns on the request's
       subchannel.
     - established
     - OpenBSD ``l1.h``; Linux SN1 ``ksys/l1.h``.
   * - A request payload after the header is a four-byte big-endian
       destination, a two-byte request code, an argument count byte, then
       typed arguments (``0x00`` plus four bytes an integer, ``0x01`` plus
       text and a zero a string, ``0x80`` with the low seven bits the
       length a binary). The SN1 port omits the count byte on
       zero-argument requests, OpenBSD always includes it, and a
       responder must accept both.
     - established
     - OpenBSD ``l1.c``; Linux SN1 ``l1.c``.
   * - A response payload is a four-byte signed code, a field count, then
       typed fields. Codes: 0 success, minus 100 wrong argument count,
       minus 101 unknown request, minus 104 data not available, minus 105
       argument out of range, minus 107 data invalid. OpenBSD decodes the
       whole word; the SN1 port keeps only the low byte and tests for a
       negative result, so it never sees a negative code (``l1.c`` lines
       133-139, 2467-2471).
     - established
     - OpenBSD ``l1.h``; Linux SN1 ``ksys/l1.h`` and ``l1.c``. The
       firmware also sends minus 1 (router error), minus 102, minus 106
       and minus 108: known only from the stock L1 firmware image, not a
       registered source, so unknown here as a public fact.
   * - The 32-bit destination: bits 31:28 controller type (0 L1, 1 L2, 2
       L3, 3 attached C-brick, 4 attached I/O brick), 27:18 rack with
       ``0x3ff`` meaning local, 17:12 bay with ``0x3f`` meaning local,
       4:0 task (3 command interpreter, 4 environmental monitor, 5
       Bedrock console, 6 general). The local general address is
       ``0x0ffff006``, the command interpreter ``0x0ffff003``, the
       attached I/O brick's general task ``0x4ffff006``.
     - established
     - Linux SN1 ``ksys/l1.h``; OpenBSD ``l1.h``;
       linux-ip35 ``sn/l1.h``.
   * - The prompt names the chassis as rack, type letter and slot,
       ``001c01`` for a stock single chassis; the letter is one of c, i,
       p, r, x, n, g, or ``?``. A second chassis is given slot 02 before
       cabling and the pair appears in the PROM inventory as ``Module
       001c01`` and ``Module 001c02``.
     - established
     - 007-3938-006 printed page 60 ("Valid Brick Types");
       007-4365-002 printed page 38; the 2014 two-chassis OpenBSD
       capture ``web:marc-openbsd-sgi-139223952727384``.

.. _sgi-l1-routing:

Routing outcomes
================

How the firmware handles destination forms that are not local (a foreign
rack or bay, a mixed local sentinel, controller types 1 to 4 and above,
unregistered tasks) was read from the stock L1 firmware image, version
1.44.0, by static analysis. That image is not a registered public source,
so these outcomes are *unknown* for citation and are the model's choice,
recorded in the checks of ``hw/misc/sgi-l1.c``: a local key (type 0 with the
own rack and bay, or both sentinels) is delivered to the addressed task; a
foreign or mixed key gets a router error reply (code minus 1 with a
detail number); an unregistered local task gets no reply. What a real
standalone Origin 300 answers to a type-4 (attached I/O brick) request is
open: the IO-8 is not an attached I/O brick to the firmware and real
``cfg`` output lists those positions as ``not connected``. A matching
stock-PROM request, L1 handle status and Bedrock reply capture would settle
it.

.. _sgi-l1-model:

Emulator choices
================

- The device answers reviewed node-facing requests for one chassis; the
  M-Bus environmental monitoring, JTAG scan, flash programming, USB device
  to an L2 and RS-422 peer routing are not modelled
  (approximation: the controller has them, no revision-matched contract is
  public).
- The two-chassis pair relies on the operator procedure of 007-4365-002
  printed pages 34-41 and 007-4498-002 (system ids, cabling with both
  servers off); the model's inter-L1 link is an emulator construction.
- Console multiplexing, the management console and the power and reset
  lifecycle are approximations built from the stock PROM's and IRIX's
  observed requests; no public document describes them beyond the user
  guides' operator-side commands (007-3938-006 printed pages 9-12, 19, 28,
  46-48, 63-64, 77-79, 100-103 and 110-111).
