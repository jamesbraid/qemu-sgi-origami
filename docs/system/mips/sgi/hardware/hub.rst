.. SPDX-License-Identifier: BSD-3-Clause

.. _sgi-hub:

=====================================================
Hub node controller (Origin 200, Origin 2000, Onyx2)
=====================================================

The Hub is the node controller of the SN0 machines: the Origin 200
(IP29 system board) and the Origin 2000 and Onyx2 (IP27 node board).
One Hub per node joins the node's two R10000-family processors, its
memory and directory, one XIO port and one CrayLink network port
(:src:`007-3439-002 Figure 2-1`). QEMU models it as ``sgi-hub`` in
``hw/misc/sgi-hub.c``, with the graphics flow-control block in
``hw/mips/sgi/pi-gfx.c``. The ``origin200`` machines and the plain
``origin2000`` reference report Hub revision code 3 (Hub 2.1); the
Origin 2000 rack and deskside and the Onyx2 models report code 5
(Hub 2.3). The model is a functional profile: it gives the stock IP27
PROM 6.156, IRIX 6.5.30, Linux and OpenBSD the registers they use, and
it is not a revision-matched description of the chip. Facts are rated
as described in :ref:`sgi-ratings`. No Hub specification is public, so
most register behaviour here is a *software expectation* or an
*approximation*.

.. _sgi-hub-sources:

Sources
=======

- SGI's *Origin and Onyx2 Theory of Operations Manual*,
  :src:`007-3439-002 Figure 2-1`: the Hub's four interfaces and its
  single XIO port. It gives no register values.
- The *Origin 200 and Origin 200 GIGAchannel Owner's Guide*,
  :src:`007-3708-002 p. 6`: the direct Hub-to-Hub CrayLink cable of a
  two-module Origin 200.
- The *Origin200/2000 IP27 PROM Technical Reference Manual*,
  :src:`108-0170-001`: node numbering and the address spaces that reach
  the Hub.
- Linux ``arch/mips/include/asm/sn/sn0/hubpi.h``, ``hubmd.h``,
  ``hubio.h``, ``hubni.h``, ``hub.h`` and ``addrs.h``,
  ``arch/mips/include/asm/sn/io.h`` and
  ``arch/mips/include/asm/xtalk/xwidget.h``, and
  ``arch/mips/sgi-ip27/ip27-init.c``, ``ip27-timer.c``, ``ip27-irq.c``,
  ``ip27-berr.c`` and ``ip27-reset.c``, at ``db2ddb871435``, as software
  expectations: every register offset on this page, most field layouts,
  the revision code table and the two composite reset constants they
  carry. The headers say they derive from SGI's own, so they are one
  account of the chip, not two.
- OpenBSD ``sys/arch/sgi/xbow/hub.h``, ``xbowdevs.h`` and
  ``sys/arch/sgi/sgi/ip27_machdep.c`` and ``sginode.c`` at
  ``2df91d80c104``, as software expectations: the widget identity, the
  counter rate, the mode bit and the reboot path. This is the one source
  written independently of SGI's headers.
- The *Crossbow ASIC Specification*, revision 12.0,
  :src:`crossbow-asic-spec-12.0 p. 132-133`, for what a warm reset does
  at the far end of the XIO link.
- Mike Galles, *Spider: A High-Speed Network Interconnect*,
  :src:`galles-spider-1997`, for the link layer's width negotiation.
- The DS2505 datasheet, :src:`ds2505-datasheet`, for the part on the
  Hub's one-wire bus.
- The stock IP27 PROM, :src:`ip27-prom-6.156`, and the IP27 kernel and
  graphics objects shipped with IRIX 6.5.30, read as binaries, as
  software expectations.
- Published inventories reporting the Hub revision,
  :src:`nekonomicon-16726704`, :src:`nekonomicon-16723250`,
  :src:`nekonomicon-16727716` and :src:`nekonomicon-16729560`, and
  published inventories of node board records,
  :src:`ebbes-parallelrechner-dissertation p. 93`,
  :src:`usenet-6kd8bj` and :src:`irixnet-thread-4337`.

.. _sgi-hub-identity:

Identity and placement
======================

Each node's Hub registers fill one 8 MiB window. A processor reaches it
two ways inside the uncached I/O space: at offset ``0x0100_0000`` with a
node field of zero, which every Hub answers for its own processors, and
at offset ``0x0180_0000`` of a named node's I/O space (Linux ``addrs.h``,
``LOCAL_HUB_ADDR`` and ``REMOTE_HUB_ADDR``), a *software expectation*.
The model maps the register region at both places for each node.

The Hub carries two separate revision fields, and the model sets them
separately.

.. list-table::
   :header-rows: 1

   * - Field
     - Model value
     - Basis
   * - Network-interface status bits 7:4 (chip revision)
     - 3 on ``origin200`` and plain ``origin2000``; 5 on the Origin 2000
       rack and deskside and the Onyx2 models
     - Linux ``hub.h`` maps codes 1 to 6 to Hub 1.0, 2.0, 2.1, 2.2, 2.3
       and 2.4, a *software expectation*. Published inventories show
       code 5 on a two-node Origin 2200, an Origin 2000 deskside, an
       Onyx2 and a two-module Origin 200 with InfiniteReality2 graphics,
       *observed* (:src:`nekonomicon-16729560 board list`,
       :src:`nekonomicon-16727716 hinv`,
       :src:`nekonomicon-16723250 hinv`,
       :src:`nekonomicon-16726704 hinv`). The IRIX 6.5.30 kernel warns
       about a code below 3, so 3 is the earliest it accepts, a
       *software expectation*. Which code a given Origin 200 shipped
       with is *unknown*.
   * - Widget identification bits 31:28
     - 1
     - *Approximation*. No published capture of this field is known.

The widget identification word at ``0x40_0000`` reads ``0x1c10106d``:
revision 1, part ``0xc101``, manufacturer ``0x036`` and bit 0 set.
OpenBSD ``xbowdevs.h`` lists the Hub as part ``0xc101`` of vendor
``0x0036``, and Linux ``xwidget.h`` gives the Hub manufacturer as
``0x036``, a *software expectation*. Bit 0 is the bit every XIO widget
identification register sets
(:src:`crossbow-asic-spec-12.0 widget identification register`); no Hub
source declares it, so it is *inferred*. A machine may override the
manufacturer with the ``manufacturer`` property.

The widget control register's number field resets to 1, the Hub's own
widget number, and its crossbar credit to 2. The reset value is an
*approximation*; that the Hub is widget 1 is a *software expectation*
(Linux ``addrs.h`` places the Hub register alias at widget 1's small
window).

.. _sgi-hub-registers:

Register map
============

Offsets are from the base of the Hub window. Every register is 64 bits,
big-endian. Names follow Linux. Reset values are the model's, an
*approximation*, except the two composite constants Linux carries; no
other Hub reset value is published. Access codes: R read, RW read and
write, WO write-only, T a write that acts and stores nothing, RC a read
that clears, W1C writing ones clears.

.. list-table::
   :header-rows: 1

   * - Offset
     - Name
     - Width
     - Reset
     - Access
     - Notes
   * - ``0x00_0018``
     - ``PI_REGION_PRESENT``
     - 64
     - all ones
     - RW
     - Stored, not consulted.
   * - ``0x00_0020``
     - ``PI_CPU_NUM``
     - 1
     - per reader
     - R
     - 0 for processor A, 1 for B. Writes log ``LOG_UNIMP``.
   * - ``0x00_0028``
     - ``PI_CALIAS_SIZE``
     - 4
     - 15
     - RW
     - Sizes the cached alias (:ref:`sgi-hub-calias`).
   * - ``0x00_0040``, ``0x00_0048``
     - ``PI_CPU_PRESENT_A``, ``_B``
     - 1
     - per node
     - R
     - One for each populated socket.
   * - ``0x00_0050``, ``0x00_0058``
     - ``PI_CPU_ENABLE_A``, ``_B``
     - 1
     - 0
     - RW
     - Clearing a bit stops the processor.
   * - ``0x00_0070``, ``0x00_0078``
     - ``PI_NMI_A``, ``_B``
     - 64
     - none
     - T
     - Non-maskable interrupt to one processor.
   * - ``0x00_0080``
     - ``PI_SOFTRESET``
     - 64
     - none
     - T
     - Soft reset to the node's enabled processors.
   * - ``0x00_0090``
     - ``PI_INT_PEND_MOD``
     - 9
     - none
     - WO
     - Sets or clears one pending bit. A read logs
       ``LOG_GUEST_ERROR``.
   * - ``0x00_0098``, ``0x00_00a0``
     - ``PI_INT_PEND0``, ``1``
     - 64
     - 0
     - R
     - Writes log ``LOG_GUEST_ERROR``.
   * - ``0x00_00a8`` to ``0x00_00c0``
     - ``PI_INT_MASK0_A``, ``1_A``, ``0_B``, ``1_B``
     - 64
     - 0
     - RW
     -
   * - ``0x00_0108``, ``0x00_0110``
     - ``PI_RT_COMPARE_A``, ``_B``
     - 52
     - 0
     - RW
     -
   * - ``0x00_0120``, ``0x00_0128``
     - ``PI_RT_PEND_A``, ``_B``
     - 1
     - 0
     - RW
     - Writing zero acknowledges.
   * - ``0x00_0140``, ``0x00_0148``
     - ``PI_RT_EN_A``, ``_B``
     - 1
     - 0
     - RW
     -
   * - ``0x00_0160``
     - ``PI_RT_LOCAL_CTRL``
     - 17
     - ``0x10000``
     - RW
     - Bits 16 and 7:0 store; the rest read zero.
   * - ``0x00_0300`` to ``0x00_0348``
     - ``PI_GFX_*_A``, ``PI_GFX_*_B``
     - see :ref:`sgi-hub-graphics`
     - 0
     - RW
     - Graphics flow control, five registers per processor slot.
   * - ``0x00_0400``
     - ``PI_ERR_INT_PEND``
     - 25
     - 0
     - R
     - Reads zero; a write does nothing.
   * - ``0x00_0408``, ``0x00_0410``
     - ``PI_ERR_INT_MASK_A``, ``_B``
     - 25
     - 0
     - RW
     - Stored, not consulted.
   * - ``0x00_0418``, ``0x00_0420``
     - ``PI_ERR_STACK_ADDR_A``, ``_B``
     - bits 31:3
     - 0 at power-on
     - RW
     - Survive reset.
   * - ``0x00_0428``
     - ``PI_ERR_STACK_SIZE``
     - 4
     - 0
     - RW
     - Stored; nothing is spooled.
   * - ``0x00_0430``, ``0x00_0450``
     - ``PI_ERR_STATUS0_A``, ``_B``
     - 64
     - 0
     - R
     - Valid, overrun and address of a failed processor read.
   * - ``0x00_0438``, ``0x00_0458``
     - ``PI_ERR_STATUS0_A_RCLR``, ``_B_RCLR``
     - 64
     - 0
     - RC
     - A 64-bit store of zero also clears.
   * - ``0x00_0440``, ``0x00_0460``
     - ``PI_ERR_STATUS1_A``, ``_B``
     - 64
     - 0
     - R
     - Always zero.
   * - ``0x00_0448``, ``0x00_0468``
     - ``PI_ERR_STATUS1_A_RCLR``, ``_B_RCLR``
     - 64
     - 0
     - R
     - Reads zero. A 64-bit store of zero clears that processor's
       status 0.
   * - ``0x00_0480``, ``0x00_0488``
     - timed-out request buffers A, B
     - 12
     - 0
     - R
     - Read zero; writes log ``LOG_UNIMP``.
   * - ``0x02_0068``
     - ``PI_HARDRESET_BIT``
     - 1
     - none
     - WO
     - A zero store is taken; any other value logs ``LOG_UNIMP``.
   * - ``0x03_0100``
     - ``PI_RT_COUNT``
     - 52
     - 0
     - RW
     - Free-running counter.
   * - ``0x20_0018``
     - ``MD_MEMORY_CONFIG``
     - 54
     - ``0x001e7fbc3fffffff``
     - RW
     - Linux ``MMC_RESET_DEFAULTS``. Stored only.
   * - ``0x20_0050`` to ``0x20_0088``
     - ``MD_DIR_ERROR`` to ``MD_MISC_ERROR_CLR``
     - 64
     - 0
     - R
     - Eight registers, all read zero. Writes log ``LOG_UNIMP``.
   * - ``0x20_0090``, ``0x20_0098``
     - ``MD_MEM_DIMM_INIT``, ``MD_DIR_DIMM_INIT``
     - 64
     - none
     - WO
     - Writes log ``LOG_UNIMP``.
   * - ``0x20_00a8``
     - ``MD_MLAN_CTL``
     - 34
     - ``0x18b100000``
     - RW
     - One-wire master. Linux ``MLAN_RESET_DEFAULTS``.
   * - ``0x21_0010``
     - ``MD_PERF_CNT0``
     - 20
     - 0
     - RW
     - Stored; never counts.
   * - ``0x22_0000``, ``0x22_0008``
     - PCF8584 S0, S1
     - 8
     - see :doc:`elsc`
     - RW
     - The node's I2C controller, where fitted.
   * - ``0x22_0048``
     - ``MD_SLOTID_USTAT``
     - 8
     - per machine
     - R
     - Slot identity and boot-flash ready.
   * - ``0x22_0050``, ``0x22_0058``
     - ``MD_LED0``, ``MD_LED1``
     - 8
     - none
     - WO
     - Diagnostic indicator for processor A, B.
   * - ``0x22_0080``, ``0x22_00a0``
     - Origin 200 indicators A, B
     - 8
     - none
     - WO
     - Traced like ``MD_LED0`` and ``MD_LED1``.
   * - ``0x22_00c0`` to ``0x22_00f8``
     - bring-up UART
     - 8
     - 16550
     - RW
     - Only with ``junk-uart=on``.
   * - ``0x40_0000``
     - ``IIO_WID``
     - 32
     - ``0x1c10106d``
     - R
     - Writes log ``LOG_GUEST_ERROR``.
   * - ``0x40_0008``
     - ``IIO_WSTAT``
     - 33
     - 0
     - R
     - Always zero. Writes log ``LOG_UNIMP``.
   * - ``0x40_0020``
     - ``IIO_WCR``
     - 23
     - ``0x4001``
     - RW
     - Bit 4 (tag mode) reads zero.
   * - ``0x40_0100``
     - ``IIO_ILAPR``
     - 64
     - all ones
     - RW
     - Stored, not enforced.
   * - ``0x40_0108``
     - ``IIO_ILAPO``
     - 64
     - none
     - WO
     - Accepted; reads zero.
   * - ``0x40_0110``, ``0x40_0118``
     - ``IIO_IOWA``, ``IIO_IIWA``
     - 16
     - 0
     - RW
     - Bits 15:8 and 0 store. Not enforced.
   * - ``0x40_0120``
     - ``IIO_IIDEM``
     - 64
     - all ones
     - RW
     - Stored, not enforced.
   * - ``0x40_0128``
     - ``IIO_ILCSR``
     - 42
     - ``0x3fc03ff200a``
     - RW
     - Link state reads up. Bit 8 sends a warm reset.
   * - ``0x40_0130``
     - ``IIO_ILLR``
     - 32
     - 0
     - RW
     - Reads zero; writes are dropped.
   * - ``0x40_0138``
     - ``IIO_IIDSR``
     - 29
     - 0
     - RW
     - Mask ``0x1101ff7f``. Stored; the Hub never sends it.
   * - ``0x40_0140``, ``0x40_0148``
     - ``IIO_IGFX_0``, ``_1``
     - 21
     - 0
     - RW
     - Graphics node-widget map.
   * - ``0x40_0150``, ``0x40_0158``
     - ``IIO_SCRATCH_REG0``, ``1``
     - 36
     - 0
     - RW
     - Mask ``0xf00f11fff`` (Linux ``IIO_SCRATCH_MASK``).
   * - ``0x40_0160`` to ``0x40_0190``
     - ``IIO_ITTE(0)`` to ``(6)``
     - 13
     - 0
     - RW
     - Big-window translation entries.
   * - ``0x40_01e8``
     - ``IIO_IMEM``
     - 64
     - none
     - RW
     - Reads zero. Bits 12, 8, 4 and 0 written as one log ``LOG_UNIMP``.
   * - ``0x40_01f8``
     - ``IIO_IECLR``
     - 64
     - none
     - WO
     - Bits 17 and 18 clear an engine's error. Other bits log
       ``LOG_UNIMP``.
   * - ``0x40_0388``, ``0x40_0390``
     - ``IIO_IPDR``, ``IIO_ICDR``
     - 64
     - 0
     - R
     - Read zero. Writes log ``LOG_UNIMP``.
   * - ``0x40_0398``
     - ``IIO_IFDR``
     - 64
     - 0
     - R
     - Reads zero. Writes log ``LOG_UNIMP``.
   * - ``0x40_03a8``, ``0x40_03b0``
     - ``IIO_ICMR``, ``IIO_ICCR``
     - 64
     - 0
     - R
     - Read zero. Writes log ``LOG_UNIMP``.
   * - ``0x40_03b8``
     - ``IIO_ICTO``
     - 8
     - 0
     - RW
     - Request time-out. Stored only.
   * - ``0x40_03c0``
     - ``IIO_ICTP``
     - 24
     - 0
     - RW
     - Request time-out prescaler. Stored only.
   * - ``0x40_04f0`` to ``0x40_05d0``, stride ``0x20``
     - ``IIO_ICRB_C(7)`` to ``(14)``
     - 64
     - 0
     - R
     - Read zero. Writes log ``LOG_UNIMP``.
   * - ``0x41_0000`` to ``0x41_0028``
     - ``IIO_IBLS_0`` to ``IIO_IBIA_0``
     - see :ref:`sgi-hub-bte`
     - 0
     - RW
     - Block transfer engine 0.
   * - ``0x42_0000`` to ``0x42_0028``
     - ``IIO_IBLS_1`` to ``IIO_IBIA_1``
     - see :ref:`sgi-hub-bte`
     - 0
     - RW
     - Block transfer engine 1.
   * - ``0x60_0000``
     - ``NI_STATUS_REV_ID``
     - 31
     - see :ref:`sgi-hub-network`
     - RW
     - Node number, mode, region size, link state, revision.
   * - ``0x60_0008``
     - ``NI_PORT_RESET``
     - 8
     - none
     - T
     - Bits 7, 1 and 0.
   * - ``0x60_0010``
     - ``NI_PROTECTION``
     - 1
     - 1
     - RW
     - Stored, not consulted.
   * - ``0x60_0100``, ``0x60_0108``
     - ``NI_SCRATCH_REG0``, ``1``
     - 64
     - 0
     - RW
     -
   * - ``0x60_0110``
     - ``NI_DIAG_PARMS``
     - 19
     - 0
     - RW
     - Bits 0 and 12 act; the rest are stored.
   * - ``0x60_0200``
     - ``NI_VECTOR_PARMS``
     - 64
     - none
     - T
     - Launches a vector operation.
   * - ``0x60_0208``, ``0x60_0210``
     - ``NI_VECTOR``, ``NI_VECTOR_DATA``
     - 64
     - 0
     - RW
     -
   * - ``0x60_0300``
     - ``NI_VECTOR_STATUS``
     - 64
     - 0
     - R
     - A write of zero clears it.
   * - ``0x60_0308``, ``0x60_0310``
     - ``NI_RETURN_VECTOR``, ``NI_VECTOR_READ_DATA``
     - 64
     - 0
     - RW
     -
   * - ``0x60_0380``
     - ``NI_VECTOR_CLEAR``
     - 64
     - 0
     - RC
     - Reads zero and clears the status. A write also clears.
   * - ``0x60_0400``
     - ``NI_IO_PROTECT``
     - 64
     - all ones
     - RW
     - Stored, not consulted.
   * - ``0x60_0408``
     - ``NI_IO_PROT_OVRRD``
     - 64
     - none
     - WO
     - Anything but the password logs ``LOG_UNIMP``.
   * - ``0x60_0500`` to ``0x60_0538``
     - ``NI_AGE_*``
     - 12
     - 0
     - RW
     - Eight ageing controls, stored only.
   * - ``0x60_8000``
     - ``NI_PORT_PARMS``
     - 16
     - ``0x1bf0``
     - RW
     - Stored only.
   * - ``0x60_8008``
     - ``NI_PORT_ERROR``
     - 38
     - 0
     - W1C
     - Bit 37 link reset; bits 15:8 a check-bit count.
   * - ``0x60_8088``
     - ``NI_PORT_ERROR_CLEAR``
     - 38
     - 0
     - RC
     - Returns and clears the port error.
   * - ``0x63_8000`` to ``0x63_80f8``
     - ``NI_META_TABLE(0)`` to ``(31)``
     - 4
     - 0
     - RW
     - Routing tables (:ref:`sgi-hub-network`).
   * - ``0x63_8100`` to ``0x63_8178``
     - ``NI_LOCAL_TABLE(0)`` to ``(15)``
     - 4
     - 0
     - RW
     -

Every other offset reads zero and logs ``LOG_UNIMP`` on a read or a
write, as does a read of a write-only register above. The unimplemented
set includes the profiling compare and its pending and enable bits, the
cross-call registers, the processor-bus checking and
negative-acknowledge registers, the refresh and page-migration
registers, the other five performance counters and their selector, the
processor and memory protection registers, the global link parameters,
the I/O request-buffer tables and the I/O performance counter.

.. _sgi-hub-access-width:

Access width
============

Outside the I2C controller and the bring-up UART, which are mapped over
it and take narrower accesses, the register region takes 4- and 8-byte
accesses; narrower ones are refused by QEMU's memory core. A 32-bit access names half of the aligned
doubleword: offset 0 carries bits 63:32 and offset 4 bits 31:0, as on a
big-endian bus. The model composes the halves itself. A 32-bit write
replaces its half of the stored value and then takes the ordinary 64-bit
write path. For the interrupt-modify register, the port error register
and any register that stores nothing, the other half contributes zero.

A 32-bit read of a read-to-clear register returns its half without
clearing. Only a full-width read clears, and only a full-width store of
zero clears a status 0 record. What hardware does for the narrower
access is *unknown*; this is an *approximation* that keeps a latch until
software reads all of it.

.. _sgi-hub-processor-interface:

Processor interface
===================

Real-time counter
-----------------

``PI_RT_COUNT`` is a 52-bit up-counter shared by the node's two
processors. The machine drives it at 1,250,000 Hz. OpenBSD registers its
Hub time source at exactly that rate (``ip27_machdep.c``), and the
stock PROM's configuration block states it
(:src:`ip27-prom-6.156 configuration block`), *software expectations*.
Linux hard-codes an 800 ns tick in ``ip27-timer.c`` beside a comment
saying the right value is not known, so it does not corroborate the
rate. The counter wraps after about 114 years.

The model derives the count from QEMU's virtual clock, so it advances
between two reads a few instructions apart. Linux depends on that: it
arms a comparator by reading the counter, adding a delta, writing the
comparator and re-reading the counter to check the deadline has not
passed (``ip27-timer.c``), a *software expectation*. A write moves the
counter's origin, so the counter keeps running from the written value
and every armed comparator re-arms against it. ``PI_RT_LOCAL_CTRL``
resets with bit 16 set and stores bits 16 and 7:0; the network-clock
observation bits read zero. Nothing written there changes the rate, an
*approximation*.

Comparators
-----------

Each processor has a comparator, a pending bit and an enable bit. When
the counter reaches the comparator the pending bit sets, whether or not
the enable bit is set; the enable bit only gates the pending bit onto
the processor's interrupt line (Linux ``hubpi.h``). Writing zero to the
pending bit acknowledges it, which Linux's handler does first
(``ip27-timer.c``), a *software expectation*.

A comparator written at or behind the counter does not match until the
counter wraps round to it, up to 114 years later. The IRIX 6.5.30
kernel parks a secondary processor's tick by writing zero to its
comparator with the counter high and depends on no interrupt following,
a *software expectation*; matching at once turns that write into a
continuous interrupt. Linux's re-read guard handles the same case from
the other side. A match fires once; nothing re-arms the comparator until
software writes it again. Moving the counter does not make a comparator
pend. A socket with no processor never arms a timer.

.. _sgi-hub-interrupts:

Interrupts
==========

Two pending banks of 64 bits each, ``PI_INT_PEND0`` and
``PI_INT_PEND1``, are node-wide. Interrupt numbers 0 to 63 are bank 0
and 64 to 127 bank 1. The only way to change a pending bit is
``PI_INT_PEND_MOD``: bits 6:0 name the interrupt, and bit 8 set means set
and clear means clear (Linux ``hubpi.h`` and ``intr.h``; OpenBSD
``hub.h``), a *software expectation*. A device interrupt arrives the
same way, as an XIO write from a Bridge to the node's interrupt-modify
register (:ref:`sgi-hub-inbound`).

Each processor has one mask per bank. A processor's line for a bank is
raised while any bit is set in both the bank and its mask. The lines are
levels, recomputed on every change to a pending bit, a mask, a counter
pending bit or an enable. The model wires three lines into each
processor's Cause register:

.. list-table::
   :header-rows: 1

   * - Hardware interrupt
     - Source
   * - 0
     - Bank 0 under the processor's mask 0.
   * - 1
     - Bank 1 under the processor's mask 1.
   * - 2
     - The processor's counter comparator, when enabled.

That assignment is Linux's (``ip27-irq.c``) and OpenBSD's, a *software
expectation*. The profiling interrupt (line 3) and the Hub error
interrupt (line 4) are not modelled. Bank 0 bits 1 and 2 carry the
graphics flow-control interrupts (Linux ``intr.h``); bank 1 bit 62
would be the network-interface error and never sets.

.. _sgi-hub-processor-control:

Processor enable, NMI and reset
===============================

``PI_CPU_PRESENT_A`` and ``_B`` read one for each populated socket.
Linux reads them to decide whether a node has a processor
(``ip27-init.c``), a *software expectation*. ``PI_CPU_ENABLE_A`` and
``_B`` reset to zero. Clearing a bit halts that processor and setting it
releases it; the change is queued on the processor's own thread. The
stock PROM halts a processor by clearing its enable, a *software
expectation*. On a node with one processor, ``_B`` stores its value and
reaches no processor.

A store to ``PI_NMI_A`` or ``_B`` raises the non-maskable interrupt of
that processor and drops the value. A store to ``PI_SOFTRESET`` gives
each enabled processor of the node the soft-reset entry the CPU model
provides: Status.SR set, its other state kept. Linux describes the
register as "Softreset (to both CPUs)" (``hubpi.h``); the model takes
that literally, an *approximation*. Neither store changes Hub state.

``PI_CPU_NUM`` returns the reading processor's slice. Writes to it log
``LOG_UNIMP`` and change nothing, an *approximation*; what a store does
on hardware is *unknown*.

The error stack address registers keep bits 31:3, start at zero and
survive a reset. OpenBSD parks a PROM-operation word in processor A's
before it reboots, and the stock PROM parks a marker in processor B's
to tell a warm start from a cold one, both *software expectations*.

``NI_PORT_RESET`` carries three resets, each acting when written,
nothing stored (Linux ``hubni.h``: bit 7 sends a warm reset, bit 1 a
link reset, bit 0 resets the whole Hub). In the model:

- Bit 0 restarts the machine. Every processor stops at the write and
  the machine cold-starts every powered module from the main loop,
  whichever Hub was written. On hardware bit 0 resets the Hub's own node
  and bit 7 carries a warm reset to the nodes the link reaches; the
  model does not separate them, an *approximation*. Linux writes bits 7
  and 0 together (``ip27-reset.c``); OpenBSD sets ``NI_PROTECTION`` and
  writes bits 1 and 0 to each node in turn (``ip27_machdep.c``), both
  *software expectations*. In the model either ends in a whole-machine
  restart. The
  standby system controllers keep their state, so the PROM sees a warm
  start (:doc:`/system/mips/sgi/sn`, "Module power and reset").
- Bit 1 alone resets the network link (:ref:`sgi-hub-link-reset`). A
  restart supersedes it.
- Bit 7 without bit 0 logs ``LOG_UNIMP`` and does nothing. No supported
  software writes it, and its effect alone is *unknown*.

``NI_PROTECTION`` and the Routers' reset masks are stored and do not
steer a reset.

.. _sgi-hub-pi-errors:

Processor errors and the bus-error probe
========================================

Every uncached I/O-space request a processor makes passes through its
Hub: the machine places a Hub region over each processor's view of the
I/O space, which forwards the request on. When a processor read fails,
at the Hub or downstream at a Crossbow or Bridge, the processor takes a
bus error and the Hub records it in that slice's status 0: bit 63
(valid) and the processor physical address bits 39:3 in bits 61:25
(Linux ``hubpi.h``). A failed read while the record is valid sets bit 62
(overrun) and keeps the first record. Recording every failed read is
*inferred* from the register's purpose; keeping the first record is
*inferred* from Linux's description of overrun. The type, request,
supplemental and command fields stay zero, an *approximation*. A
debugger's access, a device's DMA and a qtest access do not take this
path and record nothing.

The Hub fails one address itself: a doubleword read at node offset
``0x0240_0208``. Linux ``hubio.h`` names it ``IIO_IIBUSERR``, "Reads here
cause a bus error", and ``addrs.h`` makes it the error-status workaround
address, a *software expectation*. The model ignores the node number in
the match, so the issuing Hub fails the read whichever node it names
(*inferred*), and nothing goes out on the XIO link. A narrower read
there goes on down the link.

The stock PROM 6.156 clears processor status 0 through its ``_RCLR``
alias with a 64-bit zero store, then reads ``0x0240_0208`` inside a
fault guard, leaving a valid record naming that address. The IRIX
6.5.30 kernel checks that each booting processor's status 0 is valid
with bits 61:25 equal to ``0x480041``, and selects a conservative mode
and warns about old firmware if not. It later stores 64-bit zero to the
status 1 ``_RCLR`` alias and walks the error stack until status 0 reads
not valid. All three are *software expectations* read from the shipped
binaries. That a zero store to status 1's alias ends the status 0
record is *inferred* from that loop; Linux says only that the alias
clears on read. The model honours a 64-bit zero store at either alias
and nothing narrower or nonzero.

``PI_ERR_INT_PEND`` reads zero because no error source sets it. The
masks and stack size are stored, and the two timed-out request buffer
registers read zero. Linux zeroes them during setup and does not depend
on them otherwise (``ip27-berr.c``).

.. _sgi-hub-rendezvous:

Processor rendezvous assist
===========================

The stock PROM makes a node's processors meet through Hub registers:
pending-bank bits as barrier and lock mailboxes, the other processor's
comparator and scratch registers as progress mailboxes. It times its
polls in retired instructions, a *software expectation*. Under TCG a
poller can spend that budget before the processor it waits for has run,
and report a live partner dead. The model adds scheduling help that
changes no register value; only when a waiting processor runs moves.

In every execution mode these writes wake ("kick") a partner:

- an interrupt-modify write that changes a pending bit wakes the node's
  processors (under single-threaded round-robin TCG every such write
  does);
- a write into another node's network scratch registers wakes that
  node's processors;
- a comparator write while that processor's counter interrupt is off
  wakes the other processor.

Under single-threaded round-robin TCG, where one host thread runs every
processor, reads also kick: re-reading a pending bank unchanged, the
second processor reading bank 1 with bit 113 set, and reading the other
processor's comparator.

Under multi-threaded TCG a descheduled host thread can miss a short
post. The comparator rendezvous shows it: each processor posts the same
value to its own comparator with its counter interrupt off and polls the
other's, and the second to arrive withdraws its post soon after the
match. The first processor then sets bit 23 of I/O scratch register 0
and raises interrupt 113, an alive signal the second processor tests
next. The model holds a processor, at its next block boundary, in three
cases:

- the second processor, reading its own posted value from the first
  processor's comparator before the alive signal since reset, waits for
  the signal;
- a processor writing zero over its own post that the other has not
  read waits until the other reads it, and the zero lands on that read;
- a processor that has posted and re-reads anything but its own value,
  with no partner write since, waits for the partner's next write.

A hold also ends when the partner stops, when the held processor has a
stop request or a deliverable interrupt, or after two seconds of host
time. Work queued on the held processor runs first and the hold resumes
behind it. A Hub holds at most one processor at a time, and round-robin
execution holds nobody. Whether the alive signal has been given is kept
from reset and migrated. This is host scheduling, not hardware
behaviour; its cost is listed under :ref:`sgi-hub-limits`.

.. _sgi-hub-graphics:

Graphics flow control
=====================

Each processor slot has five registers that pace its uncached stores to
an attached graphics pipe (Linux ``hubpi.h``, ``PI_GFX_*``): a page at
``0x300`` (``0x328`` for slot B), a credit counter, a bias, an interrupt
counter and an interrupt compare. The field widths follow the later
chip's header and are *inferred* for this one: page enable in bit 40
(Linux ``PI_GFX_PAGE_ENABLE``) and address in bits 39:17, credit and
bias twelve bits, the two interrupt registers 26 bits. All reset to
zero, an *approximation*.

An uncached I/O store of up to eight bytes by the processor in slot S,
to an address whose bits 39:17 match slot S's enabled page, costs slot S
one credit. The store goes on to its target marked as counted. A
graphics head returns credits by widget number, and ``IIO_IGFX_0`` and
``_1`` (Linux ``IIO_IGFX_INIT``: widget 3:0, node 12:4, slot 16, valid
20) send them to a slot on the named node's Hub. The first valid entry
naming the widget wins; a return no valid entry names logs
``LOG_GUEST_ERROR``. A head returns credit only for stores that were
counted. The counter wraps at either end and logs ``LOG_GUEST_ERROR``. A
guest write replaces the count.

When a counted store leaves the counter below the bias, the model sets
bank 0 bit 1 plus the slot once, and re-arms when the counter rises
above the bias again. The processor stall that goes with it on the chip
is zero length here and the interrupt counter and compare are stored
only, both *approximations*, logged once per slot with ``LOG_UNIMP``.
The IRIX 6.5.30 graphics objects attach a pipe by writing its page with
bit 40 set, the saved count and then the map entry, and detach in the
reverse order, a *software expectation*.

``PI_HARDRESET_BIT`` at ``0x02_0068`` is a hard-reset bit in Linux and
the graphics credit mode in the later chip's header. IRIX 6.5.30 stores
zero there on Hubs of revision code 4 and later, selecting doubleword
credits, the only mode modelled, a *software expectation*. The model
takes a zero store; any other value logs ``LOG_UNIMP``. Its read value is
*unknown* and a read logs as unimplemented.

.. _sgi-hub-memory:

Memory and directory
====================

``MD_MEMORY_CONFIG`` is masked storage over 54 bits. It resets to
``0x001e7fbc3fffffff``, Linux's ``MMC_RESET_DEFAULTS``, a *software
expectation*: every bank descriptor 7, the reply-guarantee count full,
the directory-premium and ignore-ECC bits set, and the boot-flash and
microcontroller timing pairs at their defaults. No write resizes memory
or changes error reporting.

The eight directory, protocol, memory and miscellaneous error registers
and their clear aliases read zero; the model never latches a memory
error. Writes log ``LOG_UNIMP``. The two DIMM mode-initialisation
registers are write-only and log their writes. ``MD_PERF_CNT0`` stores
20 bits and never counts, since the model tracks no event to count; its
event selector is not decoded.

Slot identity and indicators
----------------------------

``MD_SLOTID_USTAT`` samples board signals. Linux ``hubmd.h`` names bit
4 flash ready, bits 2:0 the Origin 2000 slot identity and bit 7 the
Origin 200 slot identity, a *software expectation*. The model reads:

- on the Origin 200, ``0x95`` on the master module and ``0x15`` on the
  slave module of a two-module machine: bit 7 is the master sense, bits
  0 and 4 both read flash ready and bit 2 is set;
- on the Origin 2000 and Onyx2, ``0x90`` with bits 2:0 replaced by the
  slot code the module wiring supplies for the node's slot
  (:ref:`sgi-origin2000-module-identity`).

Bits 0 and 2 on the Origin 200, bit 7 on the Origin 2000 and the bits
the model leaves clear are *approximations*: no public source gives their
values on these boards.

``MD_LED0`` and ``MD_LED1`` are write-only diagnostic indicators, one
per processor (Linux ``hubmd.h``). The model also treats ``0x22_0080``
and ``0x22_00a0`` as the Origin 200 indicator pair, an *approximation*:
Linux names those offsets microcontroller/UART registers. Every write
to the four is traced (``sgi_hub_led``) and decoded against the stock
PROM's progress and failure codes when it matches one; any other value
is traced as undecoded, which is not a fault. Nothing is stored and a
read logs as unimplemented.

I2C, system controller and bring-up UART
----------------------------------------

Where a node has one, its PCF8584 I2C controller sits at ``0x22_0000``,
mapped over the Hub window at higher priority so both mappings of the
window reach it; it has no interrupt line. On the Origin 200 the module
system controller answers on that bus at I2C address ``0x08`` and its
2 KiB configuration NVRAM at addresses ``0x50`` to ``0x57``
(:doc:`elsc`). On the Origin
2000 the controller and its NVRAM sit on the module bus instead.

With the machine's ``junk-uart=on``, node 0's Hub fits a 16550 at
``0x22_00c0`` with an eight-byte register stride, the port SGI's
operator booklet calls the junk bus UART
(:src:`origin-onyx2-quick-reference-booklet`). The stock PROM probes it
as a first console. No shipped Origin 200 has the part, so it is off by
default. Its clock is the model's choice.

Serial lines are assigned per node in blocks of six: IOC3 ports A and
B, the system controller's AUX port, the bring-up UART, and the IOC3
transmit records for ports A and B. Node *n*'s block starts at
``-serial`` index 6\ *n*. The IRIX console is IOC3 serial A, not the AUX
port.

.. _sgi-hub-onewire:

One-wire master and node record
===============================

``MD_MLAN_CTL`` is the node's one-wire bus master (Linux ``hubmd.h``):
bit 0 received data, bit 1 done, bits 9:2 sample time, bits 19:10 pulse
width, and bits 26:20 and 33:27 two timing phases. The register is 34
bits wide; truncating it to 32 bits would rewrite the upper phase. It
resets to ``0x31`` in both phases, Linux's ``MLAN_RESET_DEFAULTS``, a
*software expectation*. Linux's ``MLAN_PHI0_MASK`` repeats the upper
phase's shift, a typo; the model uses bits 26:20.

A write stores the timing fields and starts a slot. Starting clears
done, and a write cannot set done or received data. The slot completes
one microsecond of virtual time later, and only a read after that sees
done and the sampled level. How long a slot takes on hardware is
*unknown*; the model needs only that a slot does not complete inside
the write that starts it. A reset cancels a slot in progress. The pulse
widths that classify a slot as reset, write-zero or write-one are the
same model choices as the Bridge's one-wire master (:doc:`bridge`).

The bus carries one DS2505-compatible part with family code ``0x0b``
(:src:`ds2505-datasheet family code`). It answers reset with a presence
pulse and implements the ROM search, match and skip commands and the
memory, status and extended reads, with CRC-8 and CRC-16 framing as the
datasheet defines. The 48-bit serial is a synthetic per-node value the
machine wires; a real machine's serial is never used. Unprogrammed
bytes read ``0xff``. Programming commands are not answered.

The part holds the node's board record, in the fixed-offset layout of
the Bridge's board records (:doc:`bridge`):

.. list-table::
   :header-rows: 1

   * - Node
     - Name
     - Part
     - Revision
     - Basis
   * - Origin 200, one processor
     - ``PIMM_1XT5_1MB``
     - ``013-1895-001``
     - C
     - *Observed*, a published 1998 sale listing of a dual-module
       machine (:src:`usenet-6kd8bj`).
   * - Origin 200, two processors
     - ``PIMM_2XT5_1MB``
     - ``013-1896-001``
     - D
     - *Observed*, a published inventory
       (:src:`ebbes-parallelrechner-dissertation p. 93`).
   * - Origin 2000 and Onyx2
     - ``IP27``
     - ``030-0733-003``
     - G
     - *Observed* for a 195 MHz, 4 MB node
       (:src:`irixnet-thread-4337 hinv -mvvv`); *inferred* for the
       model's 180 MHz, 1 MB node.

Every record carries group, capability and variety bytes of ``0xff``,
as published records do, and a barcode marked synthetic (``EMUU`` or
``EMUN`` and the instance number). Revision letters vary between units
of one part, so each is one shipped revision. The IRIX 6.5.30 kernel
formats page A only when its first byte is ``0x01``, a *software
expectation*; a processor module with no known record leaves the part
unprogrammed and IRIX reports missing page data. On an Origin 2000 or
Onyx2 the n1 node's line also reaches its module's midplane one-wire
net, so a search from that Hub finds the module identity and midplane
parts as well (:ref:`sgi-origin2000-module-identity`).

.. _sgi-hub-io:

I/O interface
=============

The Hub has one XIO port (:src:`007-3439-002 p. 53`), cabled to a Bridge
on the Origin 200 and to a Crossbow on the Origin 2000.

Widget registers
----------------

``IIO_WSTAT`` always reads zero: the model completes every request
before it returns, retries nothing and never fails its link. The stock
PROM writes it once at power-on; the write logs ``LOG_UNIMP``. Linux
gives the retry count as eight bits in its structure and seven in its
mask; nothing reads it nonzero here.

``IIO_WCR`` stores bits 22:0 except bit 4, which reads zero. The stock
PROM sets bit 17 (direct connect) with a crossbar credit of 3 or 4 when
it finds a Bridge on the link, and IRIX 6.5.30 reads bit 17 to size its
request credits, a *software expectation*. The model gives the bit and
the credit no effect, since it models no credits.

``IIO_ICTO`` and ``IIO_ICTP`` are the first registers Linux writes
(``ip27-init.c``, ``0xff`` and ``0x800``) and accept a write at reset
with no unlock step. The two scratch registers store the 36 bits of
``IIO_SCRATCH_MASK``; the stock PROM uses register 0 as boot state.

Access protection
-----------------

``IIO_ILAPR`` resets to all ones, the widget access pair to zero and
``IIO_IIDEM`` to all ones. The stock PROM writes all ones to
``IIO_ILAPR`` and ``0xff01`` to both widget access registers, opening
widget 0 and widgets 8 to 15. IRIX 6.5.30 saves, zeroes and restores the
outbound register while it programs its request buffers, and clears and
sets a device's ``IIO_IIDEM`` bit when it removes and re-enables the
device. All are *software expectations*. The model stores the registers
and enforces none, an *approximation*: enforcement and what a refused
request does are *unknown*. ``IIO_IIDSR`` stores its 29 bits; the model
raises none of the conditions that send the Hub's own error interrupt.
The ``SGIrules`` password writes to ``IIO_ILAPO`` and
``NI_IO_PROT_OVRRD`` are accepted (Linux ``IIO_PROTECT_OVRRD_KEY``);
there is nothing to unlock.

``IIO_IMEM`` reads zero. Linux names bits 0, 4 and 8 as widget 0, BTE 0
and BTE 1 shut down after an error; nothing here shuts down, so they
read clear and a write of ones logs ``LOG_UNIMP``. Bit 12 clears a
precise-error latch the model does not have.

Link control
------------

``IIO_ILCSR`` (Linux ``hubii_ilcsr_t``) resets with maximum burst
``0x3fc``, maximum retry ``0x3ff``, null timeout ``0xa`` and the link
state field (bits 13:12) at 2. In that field 0 means failed, 1 never
came up, and 2 and 3 working, so bit 13 alone answers "is the link up",
the test Linux makes (``IIO_LLP_CSR_IS_UP``), a *software expectation*.
The link reads up from reset because a link that reads down makes Linux
abandon its XIO probe; the field's real reset value is not published, so
this is an *approximation*. The state field and bit 11 (eight-bit mode)
are read-only. Burst, retry, null timeout and bit 10 (link enable) are
writable; bit 10 resets clear because firmware sets it itself with a
read-modify-write.

Bit 8 (Linux ``ILCSR_WARM_RESET``) sends a warm-reset pulse to the far
end of the XIO link on every write with it set, and reads zero. A
Crossbow treats an arriving warm reset as a reset of its own core and
resets every link (:src:`crossbow-asic-spec-12.0 p. 132-133`). That the
Hub bit sends that pattern is *inferred* from its name; that it reads
zero is *inferred* from the stock PROM, which sets it from its reset
vector and later sets bit 10 with a read-modify-write that would
otherwise send a second reset. With nothing on the link the write logs
``LOG_UNIMP``.

A link reset arriving from the Crossbow changes nothing in the model and
logs ``LOG_UNIMP``. The Crossbow specification says it clears the
transmit buffer and link protocol state and restarts width negotiation;
the model has neither, and the value the state field reads during
negotiation is *unknown*.

``IIO_ILLR`` counts link check-bit and sequence errors. The model's link
commits none, so it reads zero and writes are dropped. The IRIX 6.5.30
kernel reads it about once a second and evaluates the counts, a
*software expectation*, so the model never serves an error the link did
not make.

Big windows
-----------

Seven translation entries, ``IIO_ITTE(0)`` to ``(6)``, aim seven 512 MiB
big windows (Linux ``asm/sn/io.h``): bits 4:0 supply XIO address bits
33:29, bits 11:8 the widget, and bit 12 selects the widget's I/O space,
a *software expectation*. Bits 7:5 are stored as written. There is no
valid bit: guests disable a window by aiming it at a widget that is not
there. Every entry resets to zero, which names widget 0 in memory space.
OpenBSD reaches widget 0 only through the last big window and never
programs that entry, so it relies on the zero reset, a *software
expectation*.

A window aimed at an absent widget reads zero and drops writes, logging
``LOG_UNIMP`` with the window number. That is an *approximation*: what a
real Hub does there is *unknown*, and guests disable windows on purpose.
A Crossbow port with no widget is not a disabled window and still
fails. An entry with bit 12 set is modelled only for entry 0 at slice
0, which the stock PROM uses to read a peer Hub's identity; it turns the
access into an XIO I/O-space packet. Any other I/O-space entry logs
``LOG_UNIMP`` and reaches nothing.

.. _sgi-hub-inbound:

Inbound XIO
-----------

A packet arriving on the XIO link is decoded by address. A memory
address below 2\ :sup:`40` goes to the node's memory view, resolved by
programmed node number as a processor's access is. A doubleword write
to a node's interrupt-modify register, with bit 47 set and the node in
bits 46:38, is a device interrupt and lands on that node's Hub. A
doubleword read at I/O-view offset ``0x0160_0000`` with node field zero
is a peer's identity probe and returns this Hub's ``NI_STATUS_REV_ID``.
Anything else logs ``LOG_GUEST_ERROR`` and fails.

A peer's programmed I/O to the Hub's own widget number answers only a
4-byte read at offset 4, from the widget identification register, as
IRIX 6.5.30 issues it. This alias is an *approximation*; every other
access fails with ``LOG_GUEST_ERROR``. What the short widget offsets do
on a Hub is *unknown*.

``IIO_IPDR``, ``IIO_ICDR``, ``IIO_IFDR``, ``IIO_ICMR``, ``IIO_ICCR`` and
the C register of request buffers 7 to 14 read zero because nothing is
ever in flight; a guest that polls a written pending bit to clear needs
that zero. Writes log ``LOG_UNIMP``.

.. _sgi-hub-bte:

Block transfer engines
======================

The I/O interface has two block transfer engines, at ``0x41_0000`` and
``0x42_0000``, each with six registers (Linux ``hubio.h``):

.. list-table::
   :header-rows: 1

   * - Offset
     - Name
     - Fields the model keeps
   * - ``+0x00``
     - ``IBLS`` length and status
     - Bit 20 busy, bit 16 error, bits 15:0 length in 128-byte lines.
   * - ``+0x08``, ``+0x10``
     - ``IBSA``, ``IBDA`` source, destination
     - Bits 39:7.
   * - ``+0x18``
     - ``IBCT`` control
     - Bit 8 poison, bit 4 notify, bit 0 zero-fill.
   * - ``+0x20``
     - ``IBNA`` notification address
     - Bits 39:7.
   * - ``+0x28``
     - ``IBIA`` interrupt address
     - Bits 22:16 level, bits 8:0 node.

Addresses keep bits 39:7 and higher bits drop silently, an
*approximation*; the stock PROM's engine diagnostic passes when they do.
Bits 6:0 set log ``LOG_GUEST_ERROR``. A
store to ``IBLS`` sets busy and length but never clears a latched error.

A store to ``IBCT`` while busy is set runs the whole transfer inside
that store. The engines work from reset with no enable written, an
*approximation*; the sense of the ``IIO_IMEM`` engine bits is *unknown*.
Source, destination and
notification addresses resolve through the node's memory view by
programmed node number, as a processor's do, so a node field of zero
names whichever Hub holds number zero. A store with busy clear stores
the control bits and does nothing.

On completion busy and the count clear. With notify set the engine
writes the 8-byte ``IBLS`` value, big-endian, to the notification
address. Otherwise it sets interrupt *level* through the
interrupt-modify register of the Hub programmed with *node*, this one
included; a node no reachable Hub holds logs ``LOG_GUEST_ERROR``.

A line that cannot be read or written fails the transfer: busy and
error stay set with the count of lines not moved, neither completion
runs, and the model logs ``LOG_GUEST_ERROR``. Only writing bit 17 or 18
of ``IIO_IECLR`` (Linux ``IECLR_BTE0`` and ``IECLR_BTE1``) clears that
engine's error. The Hub error interrupt is not modelled.

The model moves 4096 lines (512 KiB) per call through one buffer. A
failed chunk is retried a line at a time so the remaining count is
exact, and an overlapping source and destination run a line at a time,
keeping the engine's forward order. Other choices, each an
*approximation*: a zero-length launch completes at once and logs
``LOG_GUEST_ERROR``; zero-fill does not read the source; poison mode
copies and logs ``LOG_UNIMP``, since the directory is not modelled; a
read of ``IBCT`` returns the stored bits and stops nothing, because no
transfer is ever in progress; the notification writes eight bytes and
no more.

.. _sgi-hub-network:

Network interface
=================

Status, node number and mode
----------------------------

``NI_STATUS_REV_ID`` (Linux ``hubni.h``) holds bit 30 eight-bit link,
bit 29 link up, bit 28 down reason (0 failed, 1 never came out of
reset), bit 18 more nodes (M mode when clear), bit 17 region size, bits
16:8 the node number, bits 7:4 the chip revision and bits 3:0 the chip
identity. The node number, region size and more-nodes bits are
writable; the rest are not. Linux reads the node number before anything
node-relative, and OpenBSD reads bit 18 to choose M or N mode
(``sginode.c``), a *software expectation*.

At reset the node number is zero (or the ``nasid`` a synthesized
firmware supplies), the mode M, region size set, the chip identity zero
and bit 28 set. The link-up and eight-bit bits follow the cable: a Hub
with a powered peer reads link up; a Hub with none, as on the Origin 200
and the one-node Origin 2000, reads down with bit 28 set. The IRIX
6.5.30 kernel treats down with bit 28 clear as a lost CrayLink on every
bus error, a *software expectation*. The reset values are
*approximations*. The model stays in M mode; node numbers 0 to 255 are
routed.

Node numbering and the claim layer
----------------------------------

The node number a Hub answers to is programmed, not its board position.
Every Hub starts at zero, so each node's early accesses to node zero
stay on its own Hub until the boot master numbers the others. For each
attached processor's view and for its own engines' view, the Hub places
one alias per Hub its link reaches, at that Hub's programmed number,
over a floor that fails every number no reachable Hub holds with a
decode error and ``LOG_GUEST_ERROR``. The machine wires the views and
the address geometry; the Hub moves its aliases whenever any Hub's
number or routing changes, at reset exit and after a migration load.
Failing rather than timing out an unclaimed number is an
*approximation*.

Vector PIO
----------

A write to ``NI_VECTOR_PARMS`` launches a vector operation: bits 19:3
address a register in the target's network-interface block, bits 1:0
give the type (read, write, exchange), and ``NI_VECTOR`` holds the
route, four bits per hop (Linux ``hubni.h``). Firmware uses it to reach
a Hub before node numbers exist. The fabric walks the route from this
Hub's port. On the direct cable of a two-module Origin 200
(:src:`007-3708-002 p. 6`) a zero route names the other Hub; a nonzero
route would need a Router and completes with the address-error type. On
a Router fabric each Router forwards by the route (:doc:`router`).

The target answers from its own register store with no local side
effects: the status, protection, scratch, diagnostic, I/O protection,
ageing and port registers and both routing tables. An exchange writes
only over a zero. A register outside that set completes with type 4,
address error. On completion ``NI_VECTOR_STATUS`` holds bit 63 valid,
the answering node in bits 60:51, the request's identifiers, address
bits 31:3 and the type, and a read's data lands in
``NI_VECTOR_READ_DATA`` (Linux ``NVS_*``). A route nothing answers
leaves the status untouched and logs ``LOG_UNIMP``, so the requester's
poll times out, an *approximation*. Overrun (bit 62) never sets.

Routing tables
--------------

32 meta entries at ``0x63_8000`` and 16 local entries at ``0x63_8100``
hold the four-bit direction the first Router takes
(:ref:`sgi-router-routing`; offsets from Linux ``hubni.h``). A destination in this
Hub's own group of sixteen node numbers uses the local entry for its
low four bits; any other uses the meta entry for its high bits. An entry
not written since reset routes nothing. The tables drive the claim
layer and vector routing.

.. _sgi-hub-link-reset:

Link reset
----------

A link reset, ``NI_PORT_RESET`` bit 1, completes when it is written.
With a peer across the cable the ends settle the width from both ends'
force-eight-bit bits (``NI_DIAG_PARMS`` bit 12 here, the Router's
per-port bit at the other end): eight bits if either end forces it,
sixteen otherwise. The link layer negotiates its width at reset
(:src:`galles-spider-1997`). Link up stays set, bit 30 reports the
width at both ends, and both Hub ends latch ``NI_PORT_ERROR`` bit 37
(Linux ``NPE_LINKRESET``); a Router end latches nothing. With no peer
the link stays down with bit 28 set and only this Hub latches bit 37.
Node number, mode, routing, vector, protection and diagnostic state
keep their values. Writing bit 12 alone changes nothing until the next
link reset.

The IRIX 6.5.30 power-off path reads ``NI_PORT_ERROR_CLEAR`` until bit
37 reads clear, a *software expectation*; completing at once satisfies
it. A single clearing read sees the bit set once and then clear. The reset's duration, the link-down
interval and the latch on a Hub with no peer are *unknown*. Because both
ends latch, a link reset written after a node has done its clearing
read leaves that node's bit 37 set. The IRIX 6.5.30 power-off path
resets the link from one module only, so it does not meet this.

Port registers and protection
-----------------------------

``NI_PORT_PARMS`` stores 16 bits and resets to ``0x1bf0``.
``NI_PORT_ERROR`` holds 38 bits; writing ones clears them, and
``NI_PORT_ERROR_CLEAR`` returns and clears them. On a Router fabric
``NI_DIAG_PARMS`` bit 0 makes this Hub's vector writes count a check-bit
error at the first Router, and a Router counting one against this Hub
increments bits 15:8, saturating at 255. The other diagnostic bits, the
protection and I/O protection registers and the ageing controls are
stored and never consulted; enforcing protection a firmware forgot to
open would leave a dead machine with no diagnostic. The stock PROM's
link diagnostic vector-writes a peer's ``NI_IO_PROTECT`` and
vector-reads its ``NI_PORT_ERROR``, and IRIX 6.5.30 vector-reads and
writes ``NI_DIAG_PARMS`` at boot, *software expectations*; all three
answer the vector decode.

.. _sgi-hub-power:

Module power
------------

When the machine powers a module down its Hubs answer nothing: register
accesses fail with a decode error, the big windows switch off, inbound
XIO fails and the network port reads disabled, so the peer sees its link
fail. The I2C controller keeps answering, so the standby system
controller stays reachable over its own bus, an *approximation*.

.. _sgi-hub-calias:

Cached alias
============

``PI_CALIAS_SIZE`` bounds the reader-local low window of the cached
memory space (Linux ``addrs.h``: CALIAS "points to the first n bytes of
memory" on the reader's node). Code 0 disables it, code 1 is 4 KiB and
each further code doubles it, to 64 MiB at 15 (Linux ``hubpi.h``,
``PI_CALIAS_SIZE_*``). Within the window a memory access with node field
zero stays on the reader's node; beyond it the access goes to the claim
layer, which routes number zero to whichever Hub holds it. The register
resets to 15, an *approximation*; every guest writes it first. The
stock PROM writes code 1, OpenBSD writes 0, and Linux writes 0 on the
boot node and 2 (8 KiB) on the others (``ip27-init.c``), all *software
expectations*. The machine creates each window at 64 MiB and the Hub
resizes and gates it on a store, at reset exit and after a load. The
other spaces' reader-local windows do not depend on the register.

.. _sgi-hub-migration:

Reset and migration
===================

Reset sets every register to the values above, stops every comparator
timer, ends every rendezvous hold, cancels a one-wire slot in progress
and clears both processor enables in the hold phase. The error stack
addresses, memory and the one-wire part's contents survive. The node
numbers, claim aliases and cached-alias windows re-derive in the exit
phase, once every Hub's enter phase has run.

The device's state saves as ``sgi-hub`` version 31. The big windows,
claim aliases and cached-alias windows re-derive from the restored
registers, and the interrupt lines are re-driven from the restored
state, which is sound because each drives a level input on a processor
whose Cause bits migrate with it. A withdrawal the rendezvous assist is
holding back lands before the state is saved, so the destination sees
what a machine with no hold would have had; the holds themselves are
not carried. The one-wire part migrates in its own section. Migration
works only between identical builds.

.. _sgi-hub-measurements:

Measurements
============

No register of this part has been read on a physical machine for this
project; neither machine the project measures carries a Hub. The only
hardware evidence is the published inventories cited under
:ref:`sgi-hub-identity` and :ref:`sgi-hub-onewire`.

.. _sgi-hub-limits:

Model limits
============

- Every reset value is the model's choice except the two Linux
  composite constants. The real power-on values are *unknown*.
- Protection, access and error-mask registers are stored, never
  enforced. No refusal path exists.
- No error latch other than the processor status 0 record and the
  engines' error bit is modelled. The Hub error interrupt, the profiling
  interrupt, cross-calls and the network-interface error interrupt never
  fire.
- Only the first doubleword of each processor error record is kept;
  its type, request, supplemental and command fields stay zero.
- Bit 0 of ``NI_PORT_RESET`` restarts the whole machine, not one node;
  bit 7 alone is not implemented.
- The I/O link never retries, times out or renegotiates; the link log
  reads zero and an arriving link reset changes nothing.
- I/O-space big windows work only for entry 0 at slice 0. A peer's
  programmed I/O answers only the 4-byte identity read.
- The graphics stall and the graphics interrupt counter are not
  modelled; the interrupt is raised at once when the credit counter
  falls below the bias.
- The block transfer engines complete inside the launching store; the
  directory, poison and engine enable mask are not modelled.
- The network interface stays in M mode. Vector overrun, the warm reset
  across CrayLink, the port-disable and port-timeout diagnostics and
  in-flight traffic are not modelled.
- ``MD_SLOTID_USTAT`` bits 0 and 2 read set on the Origin 200, and bit
  7 reads set on the Origin 2000, where Linux names it a core-clock test
  bit; their hardware values are *unknown*.
- The rendezvous assist can delay a multi-threaded guest that polls the
  comparators with their interrupts off by up to two seconds per partner
  write, two seconds when it clears its own post, and two seconds at its
  match on a second processor whose alive signal never comes.
- Unimplemented offsets read zero and log ``LOG_UNIMP``. A powered-down
  Hub fails every access with a decode error.

.. _sgi-hub-tests:

Tests
=====

``tests/qtest/sn0-hub-test.c`` (``/sn0/hub/*``) covers reset values,
the 32-bit halves, processor number, presence and enable, the error
stack address mask, the counter's rate, width and writes, comparator
matching and wrap, pending banks, masks and lines, the rendezvous holds
and their migration, the indicators, the bring-up UART, the slot
identity on every machine, widget identity and control, the access and
protection registers, the link register and link log, the inbound link
reset, the scratch registers, vector PIO and its decode, the port
registers and link reset, routing tables and node numbering, the claim
floor, the cached alias, memory configuration, the error and DIMM
registers, the one-wire master, the DS2505 part and the node records,
the big windows, the engines' registers, modes, completion, errors and
routing, and the graphics flow-control registers.

``tests/qtest/sn0-hub-inbound-test.c`` (``/sn0/hub-inbound/*``) covers
inbound XIO DMA from a Bridge and directly, the decoded address ranges,
node-qualified routing, reset and migration.
``tests/qtest/sn0-pi-error-test.c`` runs processors through the stock
PROM's bus-error probe and checks the status 0 record.
``tests/qtest/sn0-pi-gfx-test.c`` charges and returns graphics credits
from running processors, and ``tests/unit/test-sgi-pi-gfx.c`` checks the
flow-control block alone. ``tests/qtest/sn0-pio-forward-test.c`` checks
that a processor store follows a re-aimed big window.
``tests/qtest/sgi-migration-roundtrip-test.c`` saves and restores a
running Hub.

The functional tests ``tests/functional/mips64/test_sn0_smp.py`` and
``test_sn0_smp_rendezvous.py`` run processor rendezvous through the Hub
on one and two nodes; ``test_sn0_reset.py`` and
``test_sn0_warm_reset.py`` check the port reset and the guest-requested
machine restart.
