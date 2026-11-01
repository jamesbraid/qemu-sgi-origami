.. SPDX-License-Identifier: BSD-3-Clause

.. _sgi-xbow:

======================
Crossbow: XIO crossbar
======================

Used in :ref:`sgi-ip27` (the Origin 2000 and Onyx2 modules and the
Origin 200 GIGAchannel cabinet) and :ref:`sgi-ip30` (the Octane and
Octane2 system boards).

.. seealso::

   - Bridge (:doc:`bridge`), XBridge (:doc:`xbridge`) and PIC (:doc:`pic`):
     the XIO-to-PCI and PCI-X bridges that appear as widgets on XIO.

Crossbow is SGI's eight-link XIO crossbar. It routes packets
between the host links and the I/O widgets by destination widget number
and answers, itself, as logical widget 0. QEMU models it in
``hw/misc/sgi-xbow.c``. This page is the contract the machine models in
``hw/mips/sgi/models/`` cite for the chip: what the register block does,
what the model chose where the sources are silent, and where each
statement comes from.

Ratings follow :ref:`sgi-ratings`: established, observed, software expectation,
inferred, approximation or unknown. The sources are the *Crossbow ASIC
Specification*, revision 12.0, 18 November 1996
(:src:`crossbow-asic-spec-12.0`; printed and PDF page numbers agree, and the
page given is the PDF page), the *Origin2000 Theory of Operations*
(007-3439-002), SGI's *Hardware Quick-reference Booklet*
(:src:`origin-onyx2-quick-reference-booklet`) and public OpenBSD sgi
sources.

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
       access, crosstalk and connection time-out bits. On revision 5,
       bits 9:6 report the port the read arrived on. The model fills
       them for the PIC's internal Crossbow and for the Octane's
       Crossbow at revision 5 and later, so ``octane2`` reads ``0x200``
       from HEART, as the measured Octane2 does. On revision 4 they read
       zero.
     - established, spec PDF pages 73 and 88; bits 9:6 *inferred* for
       revision 5 from :src:`xbridge-asic-spec-0.5 p. 75`, a different
       chip's specification, and one read of ``0x200`` from widget 8 on
       the measured Octane2 (:ref:`sgi-xbow-measurements`)
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
       register to zero, the specification's value, keeps only bits
       22:20 of the select and drops a write to reserved bit 23. The
       measured Octane2 reads 0x00800000 in both counters after PROM
       start-up, bit 23 set, which the model does not reproduce
       (:ref:`sgi-xbow-model-choices`). Counting itself is not
       modelled.
   * - 0x0070
     - NIC
     - Readable and writable; the MicroLAN controller behind it is
       listed as not functional, so the model keeps the register but
       drives no one-wire bus.
     - established, spec PDF pages 92-93. The model resets it to 2,
       which the measured Octane2 reads after PROM start-up.
   * - 0x0078
     - Widget 0 reset fence
     - Revision 5 and later. Eight-bit mask: bit *n* lets a warm reset
       from port 8+\ *n* reach the widget 0 core. Each fence decides for
       its own destination. The model resets it to ``0xff``, passing every
       source, and an accepted warm reset leaves a programmed fence as it
       was.
     - Bit meaning from :src:`xbridge-asic-spec-0.5 p. 81-82`, the XBridge's
       Crossbow 2.0 half, so *inferred* for Crossbow revision 5. Existence
       *established* on the measured Octane2 (:ref:`sgi-xbow-measurements`).
       The reset value and retention across a warm reset are
       each an *approximation*; the specification is silent on both.
   * - 0x0080 + 8 * link
     - Per-link reset fence
     - Revision 5 and later. One eight-bit mask per link, with the same
       bit meaning for that link as destination. These are separate from
       the per-link register sets below.
     - *established* for all eight links on the measured Octane2,
       where links 8 and 15 read ``0xf7`` and links 9 to 14 ``0xff``
       after PROM start-up (:ref:`sgi-xbow-measurements`); bit meaning
       from :src:`xbridge-asic-spec-0.5 p. 57`, *inferred* for
       revision 5
   * - 0x00c0, 0x00c8
     - Resource lock, lock clear
     - Revision 5 and later. A write to ``0x00c0`` is stored only while
       the lock reads zero; any write to ``0x00c8`` clears it. Both
       offsets read the stored value with no side effect. Eight bits are
       stored. Reset zero.
     - Existence and zero after power-on *established* on the measured
       Octane2. The acquire and release rule is an *approximation*: the
       model copies its own XBridge model's rule, and the Crossbow
       behaviour is unknown.

Below revision 5 the fence and lock offsets are reserved, and an access
is the reserved-address error described above.

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
       widget-present bit and the link width. A link comes up in the
       narrower of its two ends' widths (PDF page 23).

The port state model is: coming out of reset (alive, max-retry and
failure-mode clear); failed to come out of reset (max-retry and
failure-mode set, which is where a port with no widget attached sits);
up (alive set); failed after coming up (max-retry set). This is
established by the specification's link error description (PDF
pages 65-69). It is what a guest reads to decide whether anything is
plugged in. The measured Octane2's five empty ports sit in the second
state and its three fitted ports in the third
(:ref:`sgi-xbow-octane2-links`).

The model negotiates link width as the link comes up: 8-bit when link
control sets the force-8-bit bit or when the board names the widget in
the ``eight-bit-links`` property, 16-bit otherwise. The Octane machines
name the Bridge, which has only an 8-bit link
(:src:`bridge-asic-spec-4.0 p. 4`), so its auxiliary status reads
``0x30`` as on the measured Octane2. On the Octane at revision 5 and
later, an empty port reports the 8-bit width bit beside failure mode and
reads ``0x50``, as the measured Octane2's empty ports do. This is a
per-instance opt-in because only that standalone Crossbow was measured.
Other crossbars, the PIC's internal revision 5 crossbar among them, read
``0x40`` at an empty port. Four of the measured empty ports also show
receive errors and retry counts, which the model does not reproduce.

.. _sgi-xbow-measurements:

Measurements
============

These reads come from the :ref:`sgi-measured-octane2`, in the PROM Command
Monitor after the firmware had initialised the machine. The chip reports
revision code 5; the inventory prints ASIC revision 1.4. Each read is one
32-bit load at the register's offset plus four, physical ``0x10000000``
plus the offset, issued by the processor through HEART at widget 8, for
example:

.. code-block:: text

   >> dump -w -x 0xffffffffb000000c#1
   0xffffffffb000000c:        200
   >> dump -w -x 0xffffffffb0000114#1
   0xffffffffb0000114:   80000000

None of these is a cold-reset value of the chip.

.. _sgi-xbow-octane2-id:

Widget 0 registers
------------------

.. list-table::
   :header-rows: 1

   * - Offset
     - Width
     - Value
     - Register
     - When
     - What it shows
   * - ``0x0000``
     - 32
     - ``0x50000001``
     - Identification
     - PROM monitor
     - Revision code 5 in bits 31:28.
   * - ``0x0008``
     - 32
     - ``0x00000200``
     - Widget 0 status
     - PROM monitor
     - Bits 9:6 hold 8, the number of the port the read came from;
       no error bit set.
   * - ``0x0010``, ``0x0018``
     - 32
     - ``0``, ``0``
     - Error address upper, lower
     - PROM monitor
     - Nothing logged.
   * - ``0x0020``
     - 32
     - ``0x00000024``
     - Widget 0 control
     - PROM monitor
     - Register-access (5) and crosstalk (2) error interrupts enabled,
       connection time-out (1) not.
   * - ``0x0028``
     - 32
     - ``0x000fffff``
     - Packet timeout
     - PROM monitor
     - The specification default.
   * - ``0x0030``, ``0x0038``
     - 32
     - ``0x3a080000``, ``0x00000080``
     - Interrupt destination upper, lower
     - PROM monitor
     - Vector ``0x3a`` (58) to widget 8, address ``0x80``.
   * - ``0x0040``
     - 32
     - ``0``
     - Error command word
     - PROM monitor
     - Nothing logged.
   * - ``0x0048``
     - 32
     - ``0x03ff1810``
     - LLP control
     - PROM monitor
     - The specification defaults.
   * - ``0x0058``
     - 32
     - ``0x00000002``
     - Arbitration reload
     - PROM monitor
     - The specification default.
   * - ``0x0060``, ``0x0068``
     - 32
     - ``0x00800000``, ``0x00800000``
     - Performance counters A, B
     - PROM monitor
     - Bit 23 set, select and count zero.
   * - ``0x0070``
     - 32
     - ``0x00000002``
     - NIC
     - PROM monitor
     -
   * - ``0x0078``
     - 32
     - ``0xf7``
     - Widget 0 reset fence
     - PROM monitor
     - Bit 3, port 11, clear.
   * - ``0x0080`` + 8 × (*w* − 8)
     - 32
     - ``0xf7`` for links 8 and 15, ``0xff`` for links 9 to 14
     - Per-link reset fences
     - PROM monitor
     - All eight decode on revision 5.
   * - ``0x00c0``
     - 32
     - ``0``
     - Resource lock
     - PROM monitor after power-on
     - The lock exists on revision 5 and starts released.

Bits 9:6 of the status word are reserved in the revision 12.0
specification (PDF page 89). The XBridge specification's Crossbow 2.0
half defines them as the port a read of the register arrived on
(:src:`xbridge-asic-spec-0.5 p. 75`), and the value 8 is HEART's port.
That the field means this on revision 5 is *inferred*: it is one read,
which a constant 8 would also explain, and the definition is from a
different chip's specification. The
interrupt destination sends Crossbow errors to HEART bit 58, the bit
OpenBSD and Linux name the Crossbow error. The fences read as blocking a
warm reset from port 11, the V12, at widget 0, at HEART (link 8) and at
the Bridge (link 15), and leave open the fences of the five empty ports (9, 10, 12, 13 and 14)
and of link 11.

.. _sgi-xbow-octane2-links:

Per-link registers
------------------

Link status at +0x10, auxiliary status at +0x38 and link control at
+0x08, each read as 32 bits:

.. list-table::
   :header-rows: 1

   * - Link
     - Fitted
     - Status
     - Auxiliary status
     - Control
   * - 8
     - HEART
     - ``0x80000000``
     - ``0x00000020``
     - ``0x001301e2``
   * - 9
     - none
     - ``0x00000020``
     - ``0x00000050``
     - ``0x80080000``
   * - 10 (``0xa``)
     - none
     - ``0x000000b0``
     - ``0xd9000050``
     - ``0x80080000``
   * - 11 (``0xb``)
     - V12
     - ``0x80000000``
     - ``0x00000020``
     - ``0x001301e2``
   * - 12 (``0xc``)
     - none
     - ``0x000000b0``
     - ``0x22000050``
     - ``0x80080000``
   * - 13 (``0xd``)
     - none
     - ``0x000000b0``
     - ``0x31000050``
     - ``0x80080000``
   * - 14 (``0xe``)
     - none
     - ``0x000000b0``
     - ``0x6c000050``
     - ``0x80080000``
   * - 15 (``0xf``)
     - Bridge
     - ``0x80000000``
     - ``0x00000030``
     - ``0x000f01e2``

Decoded against the specification's link tables (PDF pages 77-81):

- Links 8, 11 and 15 are alive with no error bit set. Their auxiliary
  status sets widget present (bit 5). HEART and the V12 run 16-bit links
  (bit 4 clear) and the Bridge an 8-bit link (bit 4 set), though no link
  control word sets the force-8-bit bit (24).
- Links 9, 10 and 12 to 14 have no widget: widget present is clear and
  link failure mode (bit 6) is set, with maximum transmitter retry
  (status bit 5), the state the specification gives a port with no
  widget. Their auxiliary bit 4 reads 1. Links 10 and 12 to 14 also show
  LLP receiver errors (status bit 4), a receive error counter overflow
  (bit 7) and nonzero receive retry counts in auxiliary bits 31:24; link
  9 shows neither.
- The three fitted links read widget credit 4, 4 and 3
  (bits 22:18) and have the illegal-destination, overallocation,
  bandwidth, counter-overflow, maximum-retry and maximum-request-timeout
  interrupts. The empty links read the default credit of 2 with
  only the link-alive interrupt (bit 31) enabled. That the firmware set
  the fitted links' values is *inferred* from the reads being taken after
  initialisation.

The ``octane2`` model, which fits no V12 by default, reads widget 0
status ``0x200``, auxiliary status ``0x20`` at link 8, ``0x30`` at
link 15 and ``0x50`` at each empty port, matching these reads across a
machine reset. It reads zero receive retry counts and no receiver error
at the empty ports.

These establish, for this board at PROM time, that one revision 5
Crossbow uses widgets 8, 11 and 15 and no other port, and that no
expansion Bridge is fitted. They do not identify which port an expansion
Bridge would use.

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
       belongs to the Bridge's per-device control (:doc:`/system/mips/sgi/hardware/bridge`).
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
   * - A link comes up 8-bit when its widget has only an 8-bit link
       (``eight-bit-links``) or link control forces it.
     - established
     - Spec PDF page 23; the Bridge's 8-bit link,
       :src:`bridge-asic-spec-4.0 p. 4`; the measured Octane2's link 15.
   * - An empty port on the Octane's revision 5 Crossbow reads the 8-bit
       width bit beside failure mode; other crossbars do not.
     - approximation
     - Measured on the Octane2's standalone Crossbow only
       (:ref:`sgi-xbow-octane2-links`). Whether other revision 5
       crossbars report it is *unknown*.
   * - Empty ports record no receive errors or retry counts.
     - approximation
     - Four of the measured Octane2's empty ports show both; what drives
       them is *unknown*.
   * - A store to the read-only error address registers fails and
       records a register-access error, except on the PIC's
       integrated crossbar, where it completes and still records the
       error (``complete-posted-error-address-writes``).
     - approximation
     - The specification makes the store an error (PDF pages 70 and
       73). The stock IP35 PROM expects it to complete on the IP53
       crossbar, a *software expectation* (:ref:`sgi-pic-pxbow`).
   * - Performance counters reset to zero, with bit 23 reserved and
       dropped on a write.
     - approximation
     - The specification gives zero and marks bit 23 reserved (PDF
       pages 91-92). The measured Octane2 reads bit 23 set after PROM
       start-up (:ref:`sgi-xbow-measurements`); whether it is set at
       reset or by firmware, and whether it reads back, is *unknown*.
   * - The 48-bit error address splits between 0x0010 (bits 47:32) and
       0x0018 (bits 31:0).
     - established
     - Spec PDF page 86; the model follows it.
