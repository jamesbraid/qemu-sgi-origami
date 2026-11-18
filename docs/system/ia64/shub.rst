.. SPDX-License-Identifier: BSD-3-Clause

.. _sgi-shub:

SHub 1: SN2 node controller
===========================

Used by the ``x-shub1-test`` fixture machine; the Altix 350 machine arrives in a later change. The SHub is the node controller of the Altix 3000
family and the Altix 350: one chip per node, joining the Itanium 2
processors and memory to NUMAlink and I/O. The ``sgi-shub`` device models
the registers that reviewed public sources define and that the stock PROM's
reset path touches. It is not a model of the chip. Everything else is
undocumented and is refused when touched.

Lowercase register names on this page, such as ``lb_lock_status`` or
``md_reg_0003_0400``, are this tree's own labels. Each is the register's public
name in lower case or a name built from its address and role. None is a symbol
from a firmware image. Uppercase ``SH_`` names are SGI's public Linux names.

Facts are rated *established*, *observed*, *software expectation*,
*inferred*, *approximation* or *unknown*, as in the SGI pages of the manual.
``devices/shub/registers.md`` in the chips repository grades the Linux platform
headers and the PROM readings *established*. Here they are *software expectation* for hardware behavior: a
definition or a PROM requirement says what software computes or needs, not
what the chip does. No SHub reference manual is held, so no register has an
established reset value.

Sources
-------

- The reviewed SHub description (chips ``devices/shub/README.md``).
- The PROM's I/O interface setup (chips ``devices/shub_ii/README.md``,
  independently reviewed).
- The interrupt delivery page (chips ``devices/shub/interrupts.md``,
  independently reviewed).
- The memory bring-up and memory-test engine page (chips
  ``devices/shub/memory-init.md``, independently reviewed) and the I/O
  interface header for the block transfer engines.
- The reviewed PROM reset-path register list (chips
  ``devices/shub/prom-reset.md``) for SAL 5.04: each access, the value the code needs back, and its grade.
  It cites the stock ``shub1snprom.bin`` image by address.
- Its generated register header, ``generated/c/shub.h``, copied to
  ``include/hw/ia64/sgi-shub-regs.h`` with a source notice. The header
  records Linux v5.3 ``arch/ia64/include/asm/sn/shub_mmr.h`` and the SN2
  code that uses it, plus the PROM readings.
- The reviewed SN2 address map (chips ``platforms/sn2/address-map.md``).
- SGI 007-4660-001 pp. 100--105 for the Altix 350 node that carries one
  SHub.

Address map
-----------

The device exposes two 32 GiB regions with the same registers. A register's
address is the window base plus the offset in the table.

``0x80_0000_0000``
    The local MMR window: node field value 2 at NASID shift 38
    (*software expectation*, the kernel's fallback layout; the PROM's
    accesses at ``0x81_xxxx_xxxx`` fit it, *inferred*).

``0x8_0000_0000 + (NASID << 38)``
    The global MMR window of this node (*software expectation*). The device
    moves it when the firmware writes a new NASID to ``shub_id``. A reset
    returns it to the reset NASID.

Register map
------------

Offsets are the generated header's ``SHUB_`` names. Every register is 64 bits
wide except the LED, which is written with 16 bits. An access of another
width up to 8 bytes is refused. A wider access is rejected by the memory
core as a bus error. Access is little endian.

.. list-table::
   :header-rows: 1
   :widths: 24 16 60

   * - Register
     - Offset
     - Model behavior
   * - ``shub_id``
     - ``0x1_1006_0580``
     - Reads and writes the stored word. Reset is the ``shub-id`` property,
       default NASID 0, revision 3. A write may change only bits 42:32
       (NASID), 45:44 and 52:48; a write that changes other bits is refused.
   * - Identification word
     - ``0x1_0000_0000``
     - Read-only: part number ``0x4363`` in bits 27:12 and the ``shub_id``
       revision in bits 31:28. Other bits read 0.
   * - ``rtc_ctl``
     - ``0x1_1006_0280``
     - Stores the written word, reset ``0x1_0000_0000`` (the header's initial
       value: internal source, counter disabled). Bit 0 enables the counter and
       bits 13:4 give the tick period, ``10 * (field + 1)`` ns.
   * - ``rtc``
     - ``0x1_101c_0000``
     - 55-bit counter running at the ``rtc_ctl`` period while its enable bit
       is set, frozen otherwise. Writes set the counter.
   * - ``SH_RTC0_INT_CONFIG`` and ``rtc_int_config`` (4, stride 0x100)
     - ``0x1_1000_1380``
     - Stores the value, reset 0. Bits outside the defined fields are refused.
   * - ``SH_RTC0_INT_ENABLE`` and ``rtc_int_enable`` (4, stride 0x100)
     - ``0x1_1000_1400``
     - Stores the enable bit, reset 0. Enabling arms the comparator. Other
       bits are refused.
   * - ``SH_INT_CMPA`` and ``int_cmp`` (4, stride 0x80)
     - ``0x1_101b_0000``
     - Stores a 55-bit value, reset 0. Wider values are refused.
   * - ``SH_SCRATCH1`` / ``SH_SCRATCH1_ALIAS``
     - ``0x1_101d_0080`` / ``0x1_101d_0088``
     - One stored word, reset 0. A write to ``SH_SCRATCH1`` sets it. A write to
       the alias sets it only while it reads 0. Only ``SH_SCRATCH1`` reads; a
       read of ``SH_SCRATCH1_ALIAS`` is refused.
   * - ``lb_lock_status`` / ``lb_lock``
     - ``0x1_101d_0208`` / ``0x1_101d_0200``
     - A read of the status returns the lock bit and sets it, so a second read
       sees the lock held until a write of 0 to the lock register frees it.
       The lock register refuses any other value or a read.
   * - ``lb_lock2_status`` / ``lb_lock2``
     - ``0x1_101d_0188`` / ``0x1_101d_0180``
     - The second lock pair, used by the firmware's method tables around the
       ``0x1_5003_1540`` flag word. It behaves like the first.
   * - ``md_reg_0003_0400``, ``md_reg_0005_0400``
     - ``0x1_0003_0400``, ``0x1_0005_0400``
     - Write-only: any write is accepted and ignored. Reads are refused.
   * - ``SH_PIO_RQ_CRD_CTL``
     - ``0x1_0001_0068``
     - Stored word, reset 0. The PROM compares it with a credit count.
   * - ``SH_X_DIMM_CFG``, ``SH_Y_DIMM_CFG``, ``SH_JNR_DIMM_CFG``
     - ``0x1_0001_0000``, ``0008``, ``0010``
     - Stored words, reset 0. They must read back what the PROM wrote: the
       memory test reads them to find the banks and their sizes.
   * - ``SH_Y_CFG``, ``SH_Y_DQCT_CFG``, ``SH_Y_REFRESH_CONTROL``
     - ``0x1_0001_0040``, ``0048``, ``0050``
     - Stored words, reset 0. The PROM reads them first and merges.
   * - ``SH_X_CFG``, ``SH_X_DQCT_CFG``, ``SH_X_REFRESH_CONTROL``, ``SH_MEM_RED_BLACK``,
       ``SH_MISC_MEM_CFG``, ``SH_PI_MD_RP_CRD_CTL``
     - ``0x1_0001_0020``, ``0028``, ``0030``, ``0058``, ``0060``, ``0078``
     - Write-only: writes are accepted and ignored. Reads are refused.
   * - ``md_reg_0003_0000``, ``SH_MD_DQRP_MMR_DIR_CONFIG``
     - ``0x1_0003_0000``, ``0x1_0005_0000``
     - Stored words, reset 0 (read-modify-write).
   * - ``md_reg_0003_0200`` and ``0005_0200`` (8 each, stride 0x10), the
       pattern registers ``md_bist_data_*`` (4 blocks, stride 0x1_0000), the
       directory access words
     - ``0x1_0003_0200``, ``0x1_0005_0200``, ``0x1_0003_8000``,
       ``0x1_0003_1000``, ``0x1_0005_1000``
     - Write-only, accepted and ignored.
   * - ``SH_MMRBIST_BASE``, ``SH_MMRBIST_ERR``, ``SH_MEM_ERROR_SUMMARY_ALIAS``
     - ``0x1_0002_0080``, ``0x1_0000_0080``, ``0x1_0000_00c0``
     - Stored words, reset 0. Nothing sets the failure bit (36) of the status.
   * - ``SH_MMRBIST_CTL``
     - ``0x1_0002_0088``
     - A write with go (bit 40) set and an operation in bits 38:32 of 0, 1, 2,
       7, 16, 48, 64 or 80 completes at once: the value is stored with go and fail
       (bit 41) clear. Another operation is refused. A write with go clear
       is stored. Every run passes. Operation 80 reads a directory entry; no
       directory is modelled, so the result registers below read 0, an
       *approximation* that holds because the PROM never tests them.
   * - the directory read results
     - ``0x1_0003_2000``, ``0x1_0003_2800``, ``0x1_0005_2000``,
       ``0x1_0005_2800``
     - Read 0. Writes are refused. They have no public name.
   * - ``SH_JTAG_CONFIG``
     - ``0x1_1006_0500``
     - Reads 0 (200 MHz). Writes are refused.
   * - the I/O interface registers (``iio_*``, 49 rows of the header: widget
       control, access protection, graphics map, interrupt and error words, PIO
       read entries, CRB words, performance counters)
     - ``0x180_0000`` plus the II offset ``0x40_0000`` and up (global form)
     - Stored cells, reset 0, read back what was written, except the next two.
       The local window reaches the same block at ``0x100_0000`` plus the
       offset.
   * - ``llp_control_status``
     - ``0x1c0_0128``
     - Stored, but bit 13 always reads 1: the link is up and the PROM finds an
       I/O brick attached.
   * - ``error_clear``, and the unnamed word at ``0x1_1007_1080``
     - ``0x1c0_01f8``, ``0x1_1007_1080``
     - Write-only, accepted and ignored. Reads are refused.
   * - ``bte_length_status``, ``bte_source``, ``bte_destination``,
       ``bte_control``, ``bte_notify``, ``bte_interrupt`` (2 engines, stride
       0x1_0000)
     - ``0x1c1_0000`` + ``0``, ``8``, ``0x10``, ``0x18``, ``0x20``, ``0x28``
     - Stored words except as follows. A length write sets busy. A control
       write of zero-fill (bit 0), with or without notify (bit 4), writes
       zeros over length * 128 bytes at the destination when one has been
       set, then finishes the transfer at once: busy and length clear. A control write of
       ``0x10`` (notify, a plain copy) copies length * 128 bytes from the
       source to the destination, both node-physical addresses in this
       node's RAM, and then clears busy and length. With the notify bit set
       either transfer then writes 0 to the notification address. Any other control value
       is refused. A control read returns the stored value.
   * - ``SH_INT_NODE_ID_CONFIG``, ``ipi_int_enable``, ``local_int_config`` and
       ``SH_LOCAL_INT0_ENABLE`` (6 each, stride 0x100), ``event_overflow``
     - ``0x1_1000_0300``, ``0400``, ``0480``, ``0500``, ``0x1_1001_0080``
     - Stored words, reset 0. They deliver nothing by themselves.
   * - ``ptc_parms``
     - ``0x1_101a_0100``
     - Stored word, reset ``0x7_ffff_ffff`` (*inferred* from the chips
       notes). It delivers nothing by itself.
   * - ``SH_EVENT_OVERFLOW_ALIAS``
     - ``0x1_1001_0088``
     - A write of ones clears bits in ``event_overflow``. Reads are refused.
   * - ``SH_CRB_MESSAGE_CONTROL``
     - ``0x1_2000_0000``
     - Stored whole, reset 6. At boot the PROM reads it and ORs in
       ``0x3b03f7`` (shub_id revision above 2) or ``0x3f1``, inside a set and
       restore of bit 24 of ``pi_reg_2001_0100``. A write of
       ``0x1_0001_0000_0000`` is refused: that is SAL_A's fatal-error exit,
       taken when PAL reported a fault. It clears bits 11:0 and sets the
       invalidate-acknowledge stall count and throttle to 1; that effect is
       not modelled.
   * - ``crb_nack_limit``, ``crb_timeout_prescale``, ``crb_timeout_skid``
     - ``0x1_2000_0080``, ``0100``, ``0180``
     - Stored words. They reset to 0, 0 and 7 (*inferred* from the 2.4
       header: the PROM reads them first, so a read cannot be refused). The PROM sets
       bits of the skid word outside the header's mask, so every bit is kept.
   * - ``SH_II_INT0`` / ``SH_II_INT1`` (2, stride 0x100)
     - ``0x1_1000_0000``
     - Write-only. The PIC writes its interrupt packet here: ``0x100`` plus the
       vector, at the first target for slice 0 and the second for others. The
       model raises the vector on the processor the pid in bits 19:4 of the
       matching ``ii_int_config`` names, or on processor id 0 (the first
       processor) until the PROM has written that register, which resets to 0
       (an *approximation*: the packet names no processor). Any other layout is refused.
   * - ``ii_int_config``, ``ii_int_enable`` (2 each)
     - ``0x1_1000_0080`` and ``0180``; ``0200`` and ``0280``
     - Stored words, reset 0.
   * - ``SH_SCRATCH2``
     - ``0x1_101d_0100``
     - Stored word. It resets to the board's ``scratch2-reset`` property, the node's
       identity word, and to 0 (unwritten) when the board gives none.
   * - ``SH_JUNK_BUS_LED0`` (4, stride 0x80)
     - ``0x1_1003_0000``
     - Reads 0. Writes are refused.
   * - ``SH_JUNK_BUS_TIME``
     - ``0x1_1002_0000``
     - Write-only: any 64-bit write is accepted and ignored. Reads are refused.
   * - ``pi_reg_2001_0100``
     - ``0x1_2001_0100``
     - Stored word, reset 0.
   * - ``fsb_vga_remap``
     - ``0x1_2001_0180``
     - Stored word, reset 0 (the 2.4 header's INIT, *inferred* from the chips
       notes). The PROM
       reads it, deposits bits 60:49 and writes it back; nothing reads it
       after, so the header's mask is not applied.
   * - ``SH_FSB_SYMMETRIC_AGENT_STATUS``
     - ``0x1_2002_0080``
     - Reads one bit per front-side bus that has a processor: bit 0 for
       slice 0, bit 1 for slice 2 (CPU id ``0x0100``), so ``0x3`` with two
       processors. Writes are refused. The PROM counts a processor only if its
       bit is set; what the register means in hardware is *inferred* from
       that use.
   * - ``cpu_hist`` (4, stride 0x10)
     - ``0x1_5003_1500``
     - Stored word, reset 0.
   * - ``fw_word_1540``, ``cpu_bits``, ``fw_debug_word``
     - ``0x1_5003_1540``, ``1550``, ``1560``
     - Stored words, reset 0.
   * - the XN compare words the PROM zeroes
     - ``0x1_5003_1220``, ``1230``, ``13c0``, ``13e0``, ``15b0``, ``15c0``, ``15a0``, ``1360``, ``1370`` and 39 more listed in the reviewed page (``1270``, ``1110`` to ``1210``, ``1240``, ``1260``, ``1300`` to ``1340``, ``1570`` to ``1600``, ``1610`` to ``1670``, ``13d0``, ``13f0``)
     - Stored words, reset 0.
   * - ``SH_XNNI0_VCSWITCH_FLOW`` / ``SH_XNNI1_VCSWITCH_FLOW`` and ``SH_XNNI0_INJECT_AGE`` / ``SH_XNNI1_INJECT_AGE`` (2 ports, stride 0x200)
     - ``0x1_5003_03a0``, ``0x1_5003_0400``
     - Stored words, reset 0 (*inferred*, the header's INIT). The PROM's
       route set-up reads each and writes it back with a bit XORed or ORed in.
       The header's masks (``0x7_0101_0101`` and ``0xffff``) are not applied
       and the words act on nothing.
   * - ``llp_reset_en`` (2 ports, stride 0x2000)
     - ``0x1_5000_0010``, ``0x1_5000_2010``
     - One-bit stored words, reset 1 (the header's INIT, *inferred*). The
       PROM's partition fence writes 0 to both on a lone node. A warm reset
       is never delivered, so the word acts on nothing.
   * - PI-to-memory credit adjust
     - ``0x1_2005_0d80``, ``0x1_2006_0980``
     - The first is write-only: the PROM writes ``0x88c``, and the effect is
       *unknown*. The second reads 8 and takes no writes. The PROM tops up
       credits with a block transfer only when bits 5:0 read below 8.
   * - ``pio_write_status`` (2, stride 0x80)
     - ``0x1_2007_0200``
     - Reads ``0x3f00_0000_0000_0000`` (the idle count the kernel waits for)
       plus the error bits: bit 2 write error, bit 0 more than one, and the
       first error's address in bits 49:3.
   * - ``SH_PIO_WRITE_STATUS_0_ALIAS`` / ``SH_PIO_WRITE_STATUS_1_ALIAS`` (2, stride 0x80)
     - ``0x1_2007_0208``
     - Accepts 7 and ``0x3_ffff_ffff_fffd`` and clears those bits of the
       status, the address included for the second. Other values are refused.
   * - local block hole
     - ``0x1_1005_0608``
     - Write-only, any value. No register is named here; the write raises a
       write error in the status of the issuing processor's slice pair and
       latches the error in ``lb_error_detail_1`` (valid, command ``0xea``),
       ``lb_error_detail_2`` (the address >> 3) and ``pi_error_detail_1``
       (address bits 32:3), as the header's fields say.
   * - ``pi_cam_control``
     - ``0x1_2005_0300``
     - Accepts only ``0x8000_0000_0000_0010`` and ignores it.
   * - ``md_dqlp_dir_privilege_vector``, ``md_dqrp_dir_privilege_vector``,
       ``SH_IPI_ACCESS``
     - ``0x1_0003_0300``, ``0x1_0005_0300``, ``0x1_1006_0480``
     - Stored words. A read before the first write is refused.
   * - ``SH_JUNK_BUS_LED0``..``SH_JUNK_BUS_LED3`` (4, stride 0x1_0000)
     - ``0x7_fed0_0000``
     - 16-bit writes are stored. Reads are refused.
   * - ``ipi_int``
     - ``0x1_1000_0380``
     - Write-only. With the send bit set and delivery type 0, it latches the
       vector in bits 59:52 on the processor and sets event bit 28. Other
       types, reserved vectors and a clear send bit are refused.
   * - ``event_occurred`` / ``event_occurred_alias``
     - ``0x1_1001_0000`` / ``0x1_1001_0008``
     - The register reads the pending bits, reset 0. A write to it ORs the
       bits in, as the PROM does to raise an event. A write of ones to the
       alias clears them. The alias is write-only. Bit 20 latches the
       rising edge of the junk-bus 16550's interrupt line. The alias clears it even while the line is high, an *approximation*: the chips notes' re-latch rule (the Linux 3.2 console section of ``software/firmware/snprom/l1-requests.md`` in the chips repository, revision 0ad2069 or later) was tried and hangs a RHEL 5.11 guest before ``SGI Altix RTC Timer``, so the model does not re-latch. A message is sent only when the bit goes from clear to set, as
       its first rule says. When the line rises with the bit clear and ``SH_LOCAL_INT5_ENABLE``
       has bit 13 set, the message in ``SH_LOCAL_INT5_CONFIG`` is delivered, which
       is vector ``0xe9`` for the PROM's value. That enable bit names the
       UART's event is only a guess: the PROM writes it, and nothing names it.
   * - ``ptc``
     - ``0x1_101a_0000``
     - Refused. See Model limits.

Every other offset is refused, including the rest of the memory and
directory configuration the PROM writes.

Ratings
-------

- The offsets, the PIO idle value, the PI CAM value, the alias clear value and
  the register widths are *software expectation*: the kernel's definitions,
  or what the PROM's code uses, read in the stock image.
- The election pair is stored as one word. A write to ``SH_SCRATCH1_ALIAS`` is
  visible at ``SH_SCRATCH1`` only if that reads 0, so the first writer wins.
  How the two registers relate in hardware is *unknown*; the first-writer
  latch is an *approximation* that lets exactly one processor win the PROM's
  election. It would be settled by a read of both registers after two
  processors have written the alias.
- The locks, the part number ``0x4363`` and the single
  slice-present bit are *software expectation*: the PROM requires them to
  proceed. A part number outside the four its method tables know leaves a null
  method table, and the PROM calls through low memory with translation off.
  The model has no other part number to offer. Whether the hardware behaves
  this way is *unknown*.
- The block transfer copy moves its data and finishes inside the control
  write (*approximation*: the hardware's timing is *unknown*). With the notify bit set
  the engine ends by writing the final status word to the notification
  address, a *software expectation* from Linux's ``bte_copy``
  (``arch/ia64/sn/kernel/bte.c``) and ``devices/shub_ii/README.md`` in the chips
  repository. The word written is 0, no length, busy or error, which is
  *inferred*: the kernel waits for anything but its busy marker (every bit
  except 21) with bit 16 clear, and the PROM wants not all ones and bit 16 clear.
  A failed transfer reaches the kernel through the CRB interrupt, which then
  writes bit 16 itself; that is not modelled. Without the notify bit nothing is
  written. The zero fill
  writes zeros to the destination, and the second window form of the
  engines at ``0x141_0000`` is not decoded (*unknown* whether it is the same
  block).
- The credit count of 8 at ``0x1_2006_0980`` is *inferred*: the PROM needs
  8 to skip its top-up, and no source gives the register's reset value or how
  the ``0x88c`` write changes it (*approximation*). Neither register has a
  public name.
- That a write to the unnamed local block address ``0x1_1005_0608`` raises a
  PIO write error is *inferred*: the PROM's self-test polls for bit 0 or bit 2
  after it and spins forever without one. The latched address and detail
  words follow the header's fields (*established* for the layout, *inferred*
  for the command code ``0xea``, which the PROM tests as a software
  expectation). The PROM then runs its error clear lists, whose writes land in
  stored cells. The error summary and first-error registers are not set
  because no source maps this error to their bits (*unknown*). Bit 0 on a
  second error follows the header's field text. The alias clear is
  write-one-to-clear (*inferred*).
- The second ``SH_PIO_WRITE_STATUS_0_ALIAS`` / ``SH_PIO_WRITE_STATUS_1_ALIAS`` instance, ``rtc_ctl`` bits other than
  the period field, and the meaning of bits in ``cpu_bits`` are *inferred*.
- The tick period formula is *software expectation* (the PROM and its runtime
  compute it) and the 50 ns rate it gives is *inferred*: no document says the
  chip counts at that rate.
- The 48 XN compare words in the table are stored words with reset 0
  (*inferred*: the SN2 2.4 header gives 64-bit masks and an initial value of
  0 for the words checked, and the page that lists them is reviewed). The
  PROM writes 0 to them in register lists, mostly through the global
  window. The three flag words ``15a0``, ``1360`` and ``1370`` it reads back
  and ORs a mask into, and ``1580`` is a counter it reads and increments,
  so all keep what is stored. The PROM runs this once, on the node's own
  chip. No remote-node path was found, and a call through a computed table
  is not excluded.
- The memory set-up writes the directory vectors of both memory ports: the
  four presence words at ``0x1_0003_0100`` and ``0x1_0005_0100`` (``0110``
  to ``0130``), the local vectors 1 to 7 at ``0200`` and the privilege
  vectors 1 to 7 at ``0300`` (step 0x10), and the four present-SHub words at
  ``0x1_1006_0600``, ``0680``, ``0700`` and ``0780``. Each is a stored cell
  that resets to 0 and reads back what was written (*approximation*: the
  PROM only writes them, the offsets come from a trace of the node set-up
  that has no review on record, and the reset values are *unknown*).
- The PROM also read-modify-writes four link-buffer words, ``lb_liq_ctl``
  (``0x1_1004_0000``, which it reads, ORs ``0x40000`` into and writes back 63
  times, never testing the value), the request and reply credit counts at
  ``0x1_1004_0100`` and ``0x1_1004_0180``, and ``0x1_1004_0300``. Each is a
  stored cell, in the same table as the words below. The two credit counts
  reset to ``0x1f`` and the others to 0 (*approximation*: the reset values
  are *inferred* from the 2.4 header, and the page that lists them is not
  yet reviewed). The PROM then writes the CPU and I/O permission words
  (``0x1_1006_0000``, ``0100``, ``0180``, ``0380`` and ``0400``) from a
  table in memory. They are stored cells. ``0000``, ``0180`` and ``0400``
  reset to all ones and ``0100`` and ``0380`` to 0, from the 2.4 header
  (*inferred*). The mask rule and the fields are *unknown*.
- The PROM's node set-up writes 100 error, mask and alias words outside the XN
  block: the event, error summary, overflow, first-error, time-stamp, detail
  and mask words of the processor, memory, network, crossbar and local-block
  units, listed in ``devices/shub/registers.md`` in the chips repository. Each is a stored 64-bit cell that reads back what was written
  (*approximation*) and resets to 0, except the three processor interface
  error masks (``pi_afi_error_mask``, ``pi_crbp_error_mask`` and
  ``pi_error_mask``), whose valid bits all reset set as the header's initial
  value says (*inferred*, ``devices/shub/registers.md`` in the chips repository; the other masks' initial
  values are *unknown*).
  The PROM only writes them, with 0, masks or
  all valid bits. Write-one-to-clear and mask effects are *unknown* and not
  modelled, and the offsets come from the ``devices/shub/prom-reset.md`` in the chips repository. The event alias words keep their own rows. Other widths are
  refused.
- The PROM reads the two network interface port status words at ``0x1_5000_0000``
  and ``0x1_5000_2000`` and counts a port as down unless the low nibble is 9
  (*software expectation*, ``devices/shub/memory-init.md`` in the chips repository). They are stored cells that
  reset to 0, so both ports report no link and the PROM skips neighbour
  discovery (*approximation*: no network interface is modelled and the other
  bits are *unknown*).
- Before the memory test the PROM sets up each network port's link: it
  read-modify-writes the link configuration (``0x1_5000_0020`` plus
  ``0x2000`` per port, reset ``0x7fc6_ffd00``) and three flow-control words
  (``0x1_5003_03c0``, ``03d0`` and ``03f0``, ``0x200`` apart per port), reads
  the channel mode (``0x18``), and writes the link reset (``0x08``) and test
  control (``0x28``) while the port reads down. All are stored cells. The
  reset pulse does not change the status word, so the port stays down. The
  offsets and accesses are a *software expectation* from the reviewed
  ``devices/shub/prom-reset.md`` in the chips repository. The reset value comes from the 2.4 header
  (*inferred*). Link training and the flow-control fields are *unknown* and
  not modelled.
- The NASID-change step walks 21 interrupt CONFIG and ENABLE pairs and, for
  each whose ENABLE is nonzero, rewrites the CONFIG processor field. The
  processor error and advisory pairs and the profile pair (``0x1_1000_0a80``
  to ``0x1_1000_1300``) are stored cells that reset to 0, so the walk skips
  them. The other pairs already have rows. The offsets are a *software
  expectation* from the PROM's own pair table, read in the listing. The
  reset values come from the 2.4 header for two of the registers and are
  *inferred* for the rest. The page that lists them has no review on record.
- The PROM's routing walk writes the network, memory, I/O interface and
  processor routing tables for the node (``0x1_5002_0000`` to
  ``0x1_5002_4480``: 128 local and 16 global words per table, a per-port copy
  of the network pair, and an override word for each) and
  read-modify-writes two flow words per port (``0x1_5003_03a0`` and
  ``0400``, ``0x200`` apart). All 729 words are stored 64-bit cells that
  keep every bit, including the direction bits above the header's 6-bit
  mask. They reset to 0, except the five override words, which reset to the
  header's values (``0x8000_0000_0000_0000``, and ``0x8000_0000_0000_2000``
  for the memory and processor tables; *inferred*, as ``devices/shub/registers.md`` grades header
  initial values). No route is used because both ports are down, so the
  contents do nothing. The offsets and the access order are a *software
  expectation* from ``devices/shub/prom-reset.md`` in the chips repository (the topology walk). The values
  the PROM writes for a lone node (local entry 0 with the valid bit, then
  ``0xf`` in the network words and ``0xf0f`` in the others) are not checked
  (*approximation*).
- ``SH_IPI_ACCESS``, the privilege vectors, ``SH_SCRATCH2``, ``pi_reg_2001_0100``,
  the PROM's firmware words and the memory controller words are stored and
  read back as plain registers.
  Their layouts, write behavior and reset values are *unknown*.
- Every register the PROM reads before it writes and tolerates any value from
  (``pi_reg_2001_0100``, ``cpu_hist``, ``cpu_bits``, ``SH_SCRATCH2``, the
  election word, ``fw_word_1540``, ``fw_debug_word``, the bank, timing and
  refresh words, ``SH_PIO_RQ_CRD_CTL``) resets to 0. That is an
  *approximation*: the hardware's reset value is *unknown*, and the PROM
  accepts any. Registers it never reads before writing are refused on a read before
  the first write instead.
- ``shub_id`` resetting to NASID 0 and revision 3 is an *approximation*. The
  PROM accepts any NASID and either revision constant, and 0 is the default
  a single node gets. Revision 3 selects the PROM's newer constants.
- That a read of a lock status takes the lock is *inferred*: the header only
  names the registers (one bit wide, reset 0), the PROM polls the status for 0
  and releases by writing 0, and two processors could not share a lock any
  other way. Whether the status read sets bits beyond bit 0 is *unknown*.
- The I/O interface registers the PROM sets up are stored and read back, and
  ``llp_control_status`` always reports the link up. All of that is an
  *approximation*: the PROM tolerates any other value, and no source gives
  the cells' reset values or side effects. The local-window form of the II
  block is *inferred* to be the same registers.
- The junk bus decodes 64 KiB per device, from the LED stride of 0x1_0000
  (*inferred*). The board may map one device, a 16550, at slot 4
  (``0x7fed4_0000``) in both windows with ``sgi_shub_map_junk_bus``. Every
  other junk bus offset, including offsets past the mapped device's own
  registers, is refused. The device's behaviour is the board's to qualify.
- The PROM flash is 8 MiB at ``0x7ff800000`` in both windows
  (``sgi_shub_add_prom_flash``). It is an AMD-command-set flash on 16-bit
  words: reset ``0xf0``, autoselect, CFI query, program, sector erase, 128
  sectors of 64 KiB. That the PROM drives such a flash there, with those
  command sequences and a CFI query it checks (``QRY``, command set 2, size
  at least 8 MiB), is a *software expectation* from the ``software/firmware/snprom/images.md`` in the chips repository, as is the PROM-log header layout. The firmware image at flash offset
  ``0x200000`` is *inferred*: the image's pointer to itself and the 8 MiB
  extent agree, and nothing reads it through the flash window. The
  manufacturer and device ID words (``0x0001``, ``0x227e``) are *inferred*:
  the PROM keeps them and never compares them. Contents start erased
  (``0xff``) with the firmware image given with ``-bios`` copied to the top
  6 MiB, and are migrated. The
  kernel's read of the firmware table pointer at ``0x6_ffff_ffe0`` is
  answered from the same flash: both windows also answer at
  ``0x6_ff80_0000`` to ``0x6_ffff_ffff``, with the same contents for reads. A
  write to the alias is refused. That is *inferred*: a 2.6.5 kernel (``lookup_fit`` in
  ``prominfo_proc.c``) reads the pointer at the end of the PROM space, keeps its
  low 36 bits, adds ``0x7_0000_0000`` and reads the table there, and the two
  addresses differ only in bit 32, so the decode is *inferred* to ignore it. How
  many of bits 35:32 it ignores and what a write to the alias does are
  *unknown*. Later kernels get the table through a SAL call, so only a
  2.6.5-era kernel reads this. Stores take 16 bits and reads 16 or aligned 64
  bits. Anything else is refused.
- Small windows other than 1 forward to a widget the board maps into the
  device (``sgi_shub_widget_window``). A widget register is one 64-bit
  little-endian value, so a narrow access at ``+0`` is the low bits. The
  adapter hands narrow accesses to big-endian models at the mirrored offset
  with the value unchanged. That ``+0`` is bits 31:0 is a *software expectation* (the
  PROM's part and status tests depend on it) and that PIO data is not swapped
  is a *software expectation* from what the PROM's tests depend on
  (``devices/shub_ii/README.md`` in the chips repository). The ``+4`` half is *inferred*: no code reads an ``8n+4`` widget word.
  The PIC's PCI configuration windows (offsets ``0x2_0000`` to ``0x2_9000``
  of each 8 MiB head), its device windows (``0x20_0000`` and up) and its PCI
  memory aliases (``0x4000_0000`` and up) are the exception: they are byte
  addressed, so the ID dword is at ``+0``, a narrow access reaches the model
  at ``o ^ (4 - N)`` only, and an 8-byte access moves as two little-endian
  dwords. The configuration rule is a *software expectation* (the PROM's
  accessors apply no swap there). For the PCI memory aliases the lane rule
  comes from the sibling XBridge's memory swap bit, which is PIC control bit
  22 (*inferred*): with it clear, a processor access of any width sees a
  little-endian device's registers at the same PCI address, which is what
  the model does. Setting the bit would reverse the bytes within the access
  width; the PIC does not accept the bit yet, so the byte-stream case is
  *unknown*. The PIC register words keep the rule above.
- The seven big windows, 512 MiB each at ``0x2000_0000 * n`` of the global
  window (node offset bits 31:29 = n), go through entry n of the translation
  table at ``ii + 0x400160`` (stored words, reset 0): widget in bits 11:8,
  bit 12 the I/O space flag (not modelled; a window with it set is refused), and
  bits 4:0 the Crosstalk address above the window's 29 offset bits. The
  PROM maps its PIC's PCI memory alias with ``0xf02`` (widget 15, Crosstalk
  address ``0x4000_0000``, head 0). The decode and the table are
  a *software expectation*, from the PROM's use.
  An access no widget answers is refused.
- A PIC bus master's request to memory arrives at the packet face with bit 47
  clear. The NASID in bits 46:36 must be the node's and the offset in bits
  35:0 reaches its RAM as the cacheable view, so a read returns the bytes and
  a write stores them, one to eight bytes at a time (*inferred*: no SHub
  specification gives the II's address decode). A request from a widget whose
  bit in ``IIO_IIWA`` is clear is refused, because what the
  requester sees from a refused request is *unknown*. ``IIO_IIDEM``,
  the request buffers and the completion order are not modelled; every
  inbound write is treated as coherent and completes before the next packet.
- The device implements the XIO port interface, so a PIC can link to it. Interrupt
  packets go to the node's own ``SH_II_INT0`` / ``SH_II_INT1`` registers and must be
  8 bytes wide; other widths are refused. The packet address carries the target node in bits 46:36, two
  bits below the processor's ``nasid << 38`` form, and bit 47 for a register
  write. Both are *inferred* from the Linux code that builds the address,
  not from a hardware description, and the node must match the SHub's NASID
  or the packet is refused. A write to the interrupt register with bit 8 clear (a
  clear packet) is also refused: the layout of that half is *unknown*.
  A packet that targets memory is a PIC bus master's request. It reaches node
  RAM through the ``dma-memory`` link when it names the SHub's NASID and the
  widget's inbound access bit is set, and is refused otherwise; the
  crosstalk address translation has no reviewed source.
- The PIC behind window 15 is the shared PIC model. Its head registers that
  model does not implement (offsets such as ``+0x4000`` and ``+0x100000``)
  read 0 and accept writes, logging the access under the mirrored offset. The
  SHub window does not refuse them (*approximation*).
- The PIC interrupt packet's layout (``0x100`` plus the vector) is *inferred*
  from the register layout, and that the write sets event bit 29 or 30 is
  *unknown*, so no event bit is set. Each slice's target reaches the processor the
  matching ``ii_int_config`` pid names: an *approximation*.
- ``SH_CRB_MESSAGE_CONTROL`` resets to 6 (*software expectation*: the 2.4 header's
  init value, reviewed). Its fields are five attribute-mismatch checks, the
  coherence and speculation enables, a bogus-write suppression bit and the
  invalidate-acknowledge consolidation, stall count and throttle. The boot OR
  ``0x3b03f7`` also sets bits 21 to 16, which the header lists as reserved,
  so the model stores the whole word and applies no write mask: masking to
  ``0xffffffff00000fff`` would drop what the PROM writes. What clearing the
  enables does in hardware is *inferred*, since no source states it. The
  quiesce store stays refused on purpose, because accepting it would hide the PAL
  fault behind a silent spin. Only SAL_A's store of it is confirmed to be a
  plain 64-bit store; the SAL_B park path hands it to a write helper that was
  not read.
- ``SH_JUNK_BUS_LED0`` reading 0 is an *approximation*: the PROM accepts any
  value and turns its low byte into a progress code.
- ``SH_JUNK_BUS_TIME``, ``md_reg_0003_0400`` and ``md_reg_0005_0400`` accept
  and ignore any write. That is an *approximation*: their effects and roles
  are *unknown*, and the PROM never reads them.
- The counter runs while ``rtc_ctl`` bit 0 is set (the header's
  ``ENABLE_COUNTER``) and the mode and source bits change nothing
  (*inferred*: the PROM writes bit 0 only on its second write, and no source
  says what the source select does).
- The identification word at ``0x1_0000_0000`` takes its revision from
  ``shub_id``. That is an *approximation*:
  no source ties the two, and the PROM stores both separately.
- After a NASID write the global window sits only at the new NASID. The PROM
  writes the ID through the old window, which works, but any later access
  formed from a stale NASID is refused. Decoding both is an alternative the
  review noted, not needed on a single node.
- The local window answers for the node; the PROM also writes the ID through
  the node's own global window, which the device follows.

Model limits
------------

- **Unsupported accesses.** An access the model does not implement is logged
  with its offset, direction, width and value (``-d unimp``) and fails:
  ``MEMTX_DECODE_ERROR`` where no register is modelled and ``MEMTX_ERROR``
  where the register is but the value or effect is not. Nothing is stored and
  a read returns no made-up value. A processor access that fails is a
  checkstop of the CPU, and ``-action panic`` decides what the VM does (see
  the machine page). This is instrumentation, not a
  claim about the chip's error response.
- **Interrupts.** The comparator at ``SH_RTC0_INT_CONFIG`` is the firmware's: it
  sets event bit 23, the others 24 to 26. Delivery follows what the PROM
  and kernel code do, which is *software expectation*: an IPI message or a comparator reaching its
  compare value raises the configured vector and sets its event bit. The
  rest is an *approximation*. A message goes to the processor whose id
  equals its pid, and is refused if none does; with one processor every
  pid names it. Event bits other than the comparators' (23 to 26),
  the IPI's (28) and the UART's (20) are set only by a write, so the error
  bits stay 0. The IPI enable register and
  the local interrupt destinations are stored but not acted on, because
  how an enable bit maps to an event bit is *unknown*. The other-node
  paths are not modelled and are refused. A comparator fires
  only while the counter is enabled (*inferred*: the counter stands still
  otherwise) and when the counter reaches its value, at once if it already
  has when it is programmed (a later write of the counter, or a state
  load, does not fire it again), which is
  the reading that satisfies every site the review read; whether
  hardware fires on a passed compare is *unknown*. A compare so far ahead
  it would take years (the driver's all-ones) never fires. The IPI is not
  gated on the IPI enable register, because nothing settles whether it
  gates (*unknown*). Delivery types other than 0
  (PMI, NMI, INIT) are refused, because the processor model exports only
  the vector latch.
- **Purge.** ``ptc`` is refused. What the hardware does on a purge store is
  *unknown*, and a single node never needs it.
- **PIO write status.** PIO writes complete synchronously in QEMU, so the
  pending count always reads idle. This is an *approximation*, sound for a
  model without posted writes and wrong for one that adds them.
- **Real-time clock.** The counter's value at reset is *unknown*, and the
  model starts it at 0. The PROM zeroes it before reading, so the start value
  is not observable by the firmware.
- **Processors.** One or two processors are supported, in slices 0 and 2. The
  slice-present word and the PIO write-error latch follow the CPU ids; the
  lock and election behavior are modelled for the processors present.
- **Memory test.** Every engine operation completes at once and passes. No
  DRAM is modelled, so a compare cannot fail, and no operation writes memory.
  RAM starts zeroed, which is what the PROM's zero fills want. The processor
  reaches bank 0 through the cacheable alias at ``0x30_0000_0000`` for NASID 0
  only.
- **Block transfers.** A zero fill writes zeros over the destination and a
  plain copy moves its data; both write 0 to the notification address when the notify bit is set.
  The accesses come from ``devices/shub_ii/README.md`` in the chips repository. A control
  read returns the stored value and a length write of 0 leaves the engine
  idle. The header gives no reset values, so the reset of 0 is *inferred*.
- **Not modelled.** Directory contents, the network
  interface, the I/O interface, memory sizing through the L1 controller,
  SHub 2.

Migration
---------

The device state is saved with a ``VMStateDescription``: the stored ID, the
clock base and the stored register words with their written flags, the
pending event bits, the UART line level, the blind cells, the comparator and
LED values. Restore moves the global window to the saved NASID.
Migration is supported between identical builds only.

Tests
-----

``tests/qtest/sgi-shub-test.c`` runs on the ``x-shub1-test`` fixture machine and covers the junk-bus UART and its event bit, the comparator gate, the PROM flash, the XN and blind cells and two processors. The rest of the register behaviour is tested in a later change, through the Altix 350 machine.