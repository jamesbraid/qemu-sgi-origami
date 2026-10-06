.. SPDX-License-Identifier: BSD-3-Clause

.. _sgi-xbow:

====================================
SGI Crossbow XIO crossbar (XBow)
====================================

Crossbow is the eight-link crossbar of the Origin 2000 and Onyx2
modules and of the Origin 200 GIGAchannel cabinet. It routes packets
between the host links and the I/O widgets by destination widget number
and answers, itself, as logical widget 0. QEMU models it in
``hw/misc/sgi-xbow.c``. This page is the contract the machine models in
``hw/mips/sgi/models/`` cite for the chip: what the register block does,
what the model chose where the sources are silent, and where each
statement comes from.

Ratings follow :ref:`sgi-ip30`: established, inferred, approximation or
unknown. The sources are the *Crossbow ASIC Specification*, revision
12.0, 18 November 1996 (``web:crossbow-asic-spec-12.0``; printed and PDF
page numbers agree, and the page given is the PDF page), the *Origin2000
Theory of Operations* (007-3439-002), SGI's *Hardware Quick-reference
Booklet* (``web:origin-onyx2-quick-reference-booklet``) and public
OpenBSD sgi sources.

.. _sgi-xbow-role:

Role and ports
==============

.. list-table::
   :header-rows: 1
   :widths: 40 12 48

   * - Fact
     - Rating
     - Source
   * - The chip has eight external crosstalk links, decoded as widgets
       8 to 15, and a register and error endpoint that answers as
       logical widget 0. Each link works at 8 or 16 bits.
     - established
     - Crossbow specification, PDF pages 63 and 73 (link register sets
       named for links 8 to F).
   * - One or two hosts connect to the chip; the six remaining ports
       serve I/O widgets, statically divided between the hosts, and a
       surviving host can take over all six.
     - established
     - 007-3439-002, Figures 2-14, 3-4 and 3-5 and their text, printed
       pages 53-54 and 84-85.
   * - In an Origin 2000 module the BaseIO is at widget 8, the two node
       boards at widgets 9 and 10 (0xa), and the XIO slots at 11 to 15
       (0xb to 0xf). The ports ``xbstat`` prints run 0 to 7 for
       widgets 8 to 15.
     - established
     - Quick-reference booklet, "Using the xbstat Command", printed
       pages 249-250.
   * - The mapping between the Theory of Operations' physical port
       numbers (host ports 1 and 3) and these decoded widget numbers.
     - unknown
     - No cited source states it. A photograph or a board netlist would
       settle it.
   * - The base Origin 200 has no Crossbow: its direct-attached Bridge
       answers every widget address and widget 8 is a firmware
       convention. A GIGAchannel cabinet adds an external Crossbow.
     - established
     - 007-3708-002, Figures 1-4 and 1-5, printed pages 6-7; OpenBSD
       sgi ``sys/arch/sgi/sgi/ip27_machdep.c`` probes widget 8 alone for
       the Origin 200 (``openbsd-sgi:2df91d80c104636deeef8124252ddd5951b55fbf``).
   * - On a GIGAchannel, link A reaches the attached module's Hub, link
       9 its IP29 Bridge, link 8 the cabinet Bridge, and links B to F
       the optional positions.
     - approximation
     - Emulator choice. It reconciles the owner's-guide figures with
       the widget numbers guests probe; the physical cable-to-link
       mapping is not stated by any source.

.. _sgi-xbow-registers:

Register block
==============

Every register is 32 bits wide or narrower, sits on an eight-byte
boundary and is accessed as a crosstalk double-word packet. An access to
a reserved address, by another packet size, or a write to a read-only or
read-and-clear register is an error, not an ignored access (Crossbow
specification, PDF pages 70 and 73). The public headers
(OpenBSD ``sys/arch/sgi/xbow/xbow.h`` and Linux
``arch/mips/include/asm/xtalk/xwidget.h``) name each register at its
offset plus four, because the data is the low half of the eight-byte
slot on a big-endian host; this page uses the aligned offset.

.. list-table::
   :header-rows: 1
   :widths: 14 24 38 24

   * - Offset
     - Register
     - Behaviour and default
     - Source and rating
   * - 0x0000
     - Identification
     - Revision in bits 31:28, part number zero, manufacturer zero, bit
       0 reads one.
     - established, spec PDF page 85; OpenBSD lists the Crossbow under
       vendor 0, product 0.
   * - 0x0008
     - Widget 0 status
     - Error summary; all bits default to zero. Reading the clearing
       alias at 0x0050 returns the value and negates the register
       access, crosstalk and connection time-out bits.
     - established, spec PDF pages 73 and 88
   * - 0x0010, 0x0018
     - Error address upper, lower
     - Hold bits 47:32 and 31:0 of the address of the first failed
       packet; later errors are not logged until the error command
       word is written.
     - established, spec PDF page 86
   * - 0x0020
     - Widget 0 control
     - Bit 5 enables the register-access-error interrupt, bit 2 the
       crosstalk-error interrupt and bit 1 the connection-timeout
       interrupt. Default zero.
     - established, spec PDF page 90
   * - 0x0028
     - Packet timeout
     - Bits 19:0, 1.28 microseconds per tick, default 0xfffff.
     - established, spec PDF page 88
   * - 0x0030, 0x0038
     - Interrupt destination upper, lower
     - Upper holds the vector (31:24), target id (19:16, default 8)
       and address bits 47:32; lower holds address bits 31:0.
     - established, spec PDF page 87
   * - 0x0040
     - Error command word
     - Writing it clears the word and both error address registers;
       the data written is discarded.
     - established, spec PDF page 86
   * - 0x0048
     - Link-level protocol control
     - Maximum micropacket retry (25:16, default 0x3ff), null timeout
       (15:10, default 0x06), maximum burst (9:0, default 0x010).
     - established, spec PDF pages 90-91
   * - 0x0058
     - Arbitration reload
     - Bits 5:0, 1.28 microseconds per tick, default 2; must be above
       zero for the chip to arbitrate.
     - established, spec PDF pages 87-88
   * - 0x0060, 0x0068
     - Performance counters A, B
     - Bits 22:20 select the link counted, with 000 meaning link 8 and
       a stated default of link 8; bits 19:0 count.
     - established, spec PDF pages 91-92. The model resets the
       register to 0x00800000 (bit 23 set); the specification marks
       bit 23 reserved and gives no such value, so that reset word is
       unknown and counting itself is not modelled.
   * - 0x0070
     - NIC
     - Readable and writable; the MicroLAN controller behind it is
       listed as not functional, so the model keeps the register but
       drives no one-wire bus.
     - established, spec PDF pages 92-93

The identification revision is a property of the fitted chip and not a
resettable value; reset and migration rebuild it. The model's
four-node Origin 2000 reports revision code 2. The specification says
only that revisions start at 1 (PDF page 85); which revision SGI fitted
to which module is unknown, so the code is an emulator choice made so a
guest that rejects the earliest silicon still attaches.

.. _sgi-xbow-link-registers:

Per-link registers
==================

The set for widget *w* begins at ``0x0100 + (w - 8) * 0x40``
(spec PDF pages 73-74). Within a set:

.. list-table::
   :header-rows: 1
   :widths: 14 86

   * - Offset
     - Behaviour (established, spec PDF pages 77-84 unless noted)
   * - +0x00
     - Input buffer flush. A read returns zero once the port's buffer
       has drained; if a flush is already in progress anywhere on the
       chip it returns one and does nothing (PDF pages 71 and 77).
   * - +0x08
     - Link control: per-error interrupt enables, the widget credit
       field (default 2) and the come-up-in-8-bit-mode bit.
   * - +0x10, +0x28
     - Link status and its read-and-clear alias. Bit 31 is link
       alive; low bits record retry, receive-error and timeout
       conditions; bit 17 illegal destination. The read-and-clear is
       not atomic: a two-cycle window can lose an error (errata,
       PDF page 93).
   * - +0x18, +0x20
     - Arbitration upper and lower: guaranteed-bandwidth and
       remainder-ring weights per source.
   * - +0x30
     - Link reset. A write is a one-shot link reset; a write with
       response returns no response (errata, PDF page 93), so drivers
       use ordinary writes.
   * - +0x38
     - Auxiliary status: retry counters, the timed-out-source map, a
       widget-present bit and the link width.

The port state model is: coming out of reset (alive, max-retry and
failure-mode clear); failed to come out of reset (max-retry and
failure-mode set, which is where a port with no widget attached sits);
up (alive set); failed after coming up (max-retry set). This is
established by the specification's link error description (PDF
pages 65-69). It is what a guest reads to decide whether anything is
plugged in.

.. _sgi-xbow-model-choices:

Emulator choices and unknowns
=============================

.. list-table::
   :header-rows: 1
   :widths: 50 14 36

   * - Statement
     - Rating
     - Basis
   * - The crossbar performs no address translation or byte swapping;
       the destination widget selects the output port and the address
       travels unchanged.
     - established
     - Spec PDF pages 63 and 99 (switch description); byte swapping
       belongs to the Bridge's per-device control (:doc:`sgi-bridge`).
   * - A destination other than widgets 0 and 8 to 15 is an
       illegal-destination error on the receiving port (status bit
       17, repeat sets bit 18); a legal port whose link is down drops
       the request and records nothing at the Crossbow.
     - approximation
     - The specification names the illegal-destination flag (PDF page
       69) but not which port latches it nor what a down link does; the
       model's attribution is an emulator choice.
   * - Training time, software-disabled link transitions, auxiliary
       live-state resampling and the widget 0 connection-timeout
       report.
     - unknown
     - The specification gives flags but no timing; a capture of a
       real chip's link bring-up would settle them.
   * - The 48-bit error address splits between 0x0010 (bits 47:32) and
       0x0018 (bits 31:0).
     - established
     - Spec PDF page 86; the model follows it.
