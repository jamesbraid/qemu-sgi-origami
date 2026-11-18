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

Facts are rated *established*, *observed*, *software expectation*,
*inferred*, *approximation* or *unknown*, as in the SGI pages of the manual.
The chips repository grades the Linux platform headers and the PROM readings
*established*. Here they are *software expectation* for hardware behavior: a
definition or a PROM requirement says what software computes or needs, not
what the chip does. No SHub reference manual is held, so no register has an
established reset value.

Sources
-------

- The reviewed `SHub description
  <https://github.com/jamesbraid/chips/blob/94c62dd/devices/shub/README.md>`_.
- The `PROM's I/O interface setup
  <https://github.com/jamesbraid/chips/blob/b4fe973/devices/shub_ii/README.md>`_
  (independently reviewed).
- The `interrupt delivery page
  <https://github.com/jamesbraid/chips/blob/e915ce1/devices/shub/interrupts.md>`_
  (independently reviewed).
- The `memory bring-up and memory-test engine page
  <https://github.com/jamesbraid/chips/blob/f1e8461/devices/shub/memory-init.md>`_
  (independently reviewed) and the I/O interface header for the
  block transfer engines.
- The reviewed `PROM reset-path register list
  <https://github.com/jamesbraid/chips/blob/b5eb2f2/devices/shub/prom-reset.md>`_
  for SAL 5.04: each access, the value the code needs back, and its grade.
  It cites the stock ``shub1snprom.bin`` image by address.
- Its generated register header, ``generated/c/shub.h``, copied to
  ``include/hw/ia64/sgi-shub-regs.h`` with a source notice. The header
  records Linux v5.3 ``arch/ia64/include/asm/sn/shub_mmr.h`` and the SN2
  code that uses it, plus the PROM readings.
- The reviewed `SN2 address map
  <https://github.com/jamesbraid/chips/blob/94c62dd/platforms/sn2/address-map.md>`_.
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
   * - ``chip_id``
     - ``0x1_0000_0000``
     - Read-only: part number ``0x4363`` in bits 27:12 and the ``shub_id``
       revision in bits 31:28. Other bits read 0.
   * - ``rtc_ctl``
     - ``0x1_1006_0280``
     - Stores the written word. A read before the first write is refused. Bits
       13:4 give the tick period, ``10 * (field + 1)`` ns.
   * - ``rtc``
     - ``0x1_101c_0000``
     - 55-bit counter running at the ``rtc_ctl`` period. Writes set the
       counter. A read before ``rtc_ctl`` is written stops.
   * - ``rtc0_int_config`` and ``rtc_int_config`` (4, stride 0x100)
     - ``0x1_1000_1380``
     - Stores the value, reset 0. Bits outside the defined fields are refused.
   * - ``rtc0_int_enable`` and ``rtc_int_enable`` (4, stride 0x100)
     - ``0x1_1000_1400``
     - Stores the enable bit, reset 0. Enabling arms the comparator. Other
       bits are refused.
   * - ``int_cmpa`` and ``int_cmp`` (4, stride 0x80)
     - ``0x1_101b_0000``
     - Stores a 55-bit value, reset 0. Wider values are refused.
   * - ``lb_elect`` / ``lb_elect_set``
     - ``0x1_101d_0080`` / ``0x1_101d_0088``
     - One stored word, reset 0. A write to either sets it. Only ``lb_elect``
       reads; a read of ``lb_elect_set`` stops.
   * - ``lb_lock_status`` / ``lb_lock``
     - ``0x1_101d_0208`` / ``0x1_101d_0200``
     - The status always reads 0. The lock register accepts a write of 0 and
       stops on any other value or a read.
   * - ``lb_lock2_status`` / ``lb_lock2``
     - ``0x1_101d_0188`` / ``0x1_101d_0180``
     - The second lock pair, used by the firmware's method tables around the
       ``0x1_5003_1540`` flag word. The status always reads 0. The lock
       register accepts a write of 0 and stops on any other value or a read.
   * - ``md_reg_0003_0400``, ``md_reg_0005_0400``
     - ``0x1_0003_0400``, ``0x1_0005_0400``
     - Write-only: any write is accepted and ignored. Reads are refused.
   * - ``md_reg_0001_0068``
     - ``0x1_0001_0068``
     - Stored word, reset 0. The PROM compares it with a credit count.
   * - ``md_bank_cfg0``, ``md_bank_cfg1``, ``md_bank_cfg2``
     - ``0x1_0001_0000``, ``0008``, ``0010``
     - Stored words, reset 0. They must read back what the PROM wrote: the
       memory test reads them to find the banks and their sizes.
   * - ``md_timing_2``, ``md_timing_3``, ``md_refresh_1``
     - ``0x1_0001_0040``, ``0048``, ``0050``
     - Stored words, reset 0. The PROM reads them first and merges.
   * - ``md_timing_0``, ``md_timing_1``, ``md_refresh_0``, ``md_const_58``,
       ``md_ctl_60``, ``md_const_78``
     - ``0x1_0001_0020``, ``0028``, ``0030``, ``0058``, ``0060``, ``0078``
     - Write-only: writes are accepted and ignored. Reads are refused.
   * - ``md_reg_0003_0000``, ``md_reg_0005_0000``
     - ``0x1_0003_0000``, ``0x1_0005_0000``
     - Stored words, reset 0 (read-modify-write).
   * - ``md_reg_0003_0200`` and ``0005_0200`` (8 each, stride 0x10), the
       pattern registers ``md_bist_data_*`` (4 blocks, stride 0x1_0000), the
       directory access words
     - ``0x1_0003_0200``, ``0x1_0005_0200``, ``0x1_0003_8000``,
       ``0x1_0003_1000``, ``0x1_0005_1000``
     - Write-only, accepted and ignored.
   * - ``md_mbist_cmd``, ``md_mbist_status``, ``md_mbist_mask``
     - ``0x1_0002_0080``, ``0x1_0000_0080``, ``0x1_0000_00c0``
     - Stored words, reset 0. Nothing sets the failure bit (36) of the status.
   * - ``md_mbist_ctl``
     - ``0x1_0002_0088``
     - A write with go (bit 40) set and an operation in bits 38:32 of 0, 1, 2,
       7, 16, 48 or 64 completes at once: the value is stored with go and fail
       (bit 41) clear. Another operation is refused. A write with go clear
       is stored. Every run passes.
   * - ``md_clk_sel``
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
       write of zero-fill (bit 0), with or without notify (bit 4), finishes
       the transfer at once: busy and length clear. Any other control value
       is refused. A control read returns the stored value.
   * - ``int_node_id_config``, ``ipi_int_enable``, ``local_int_config`` and
       ``local_int_enable`` (6 each, stride 0x100), ``ptc_parms``,
       ``event_overflow``
     - ``0x1_1000_0300``, ``0400``, ``0480``, ``0500``, ``0x1_101a_0100``,
       ``0x1_1001_0080``
     - Stored words, reset 0. They deliver nothing by themselves.
   * - ``event_overflow_alias``
     - ``0x1_1001_0088``
     - A write of ones clears bits in ``event_overflow``. Reads are refused.
   * - ``crb_message_control``
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
     - Stored words. A read before the first write stops: no reset value is
       known.
   * - ``ii_int`` (2, stride 0x100)
     - ``0x1_1000_0000``
     - Write-only. The PIC writes its interrupt packet here: ``0x100`` plus the
       vector, at the first target for slice 0 and the second for others. The
       model raises the vector on the one processor. Any other layout is refused.
   * - ``ii_int_config``, ``ii_int_enable`` (2 each)
     - ``0x1_1000_0080`` and ``0180``; ``0200`` and ``0280``
     - Stored words, reset 0.
   * - ``lb_l1_state``
     - ``0x1_101d_0100``
     - Stored word, reset 0.
   * - ``lb_slice_rec`` (4, stride 0x80)
     - ``0x1_1003_0000``
     - Reads 0. Writes are refused.
   * - ``lb_reg_1002_0000``
     - ``0x1_1002_0000``
     - Write-only: any 64-bit write is accepted and ignored. Reads are refused.
   * - ``pi_reg_2001_0100``
     - ``0x1_2001_0100``
     - Stored word, reset 0.
   * - ``pi_slice_present``
     - ``0x1_2002_0080``
     - Reads 1: the one processor, in slice 0. Writes are refused.
   * - ``cpu_hist`` (4, stride 0x10)
     - ``0x1_5003_1500``
     - Stored word, reset 0.
   * - ``fw_word_1540``, ``cpu_bits``, ``fw_debug_word``
     - ``0x1_5003_1540``, ``1550``, ``1560``
     - Stored words, reset 0.
   * - ``pio_write_status`` (2, stride 0x80)
     - ``0x1_2007_0200``
     - Reads ``0x3f00_0000_0000_0000``, the idle value the kernel waits for.
   * - ``pio_write_status_alias`` (2, stride 0x80)
     - ``0x1_2007_0208``
     - Accepts a write of 7. Other values are refused.
   * - ``pi_cam_control``
     - ``0x1_2005_0300``
     - Accepts only ``0x8000_0000_0000_0010`` and ignores it.
   * - ``md_dqlp_dir_privilege_vector``, ``md_dqrp_dir_privilege_vector``,
       ``ipi_access``
     - ``0x1_0003_0300``, ``0x1_0005_0300``, ``0x1_1006_0480``
     - Stored words. A read before the first write stops.
   * - ``junk_bus_led`` (4, stride 0x1_0000)
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
       alias clears them. The alias is write-only.
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
- The election pair is stored as one word, so a write to ``lb_elect_set`` is
  visible at ``lb_elect``. How the two registers relate in hardware (alias,
  latch or separate stores) is *unknown*; the single word is an
  *approximation* that satisfies the PROM's write-then-read test.
- The locks, the part number ``0x4363`` and the single
  slice-present bit are *software expectation*: the PROM requires them to
  proceed. A part number outside the four its method tables know leaves a null
  method table, and the PROM calls through low memory with translation off.
  The model has no other part number to offer. Whether the hardware behaves
  this way is *unknown*.
- The second ``pio_write_status_alias`` instance, ``rtc_ctl`` bits other than
  the period field, and the meaning of bits in ``cpu_bits`` are *inferred*.
- The tick period formula is *software expectation* (the PROM and its runtime
  compute it) and the 50 ns rate it gives is *inferred*: no document says the
  chip counts at that rate.
- ``ipi_access``, the privilege vectors, ``lb_l1_state``, ``pi_reg_2001_0100``,
  the PROM's firmware words and the memory controller words are stored and
  read back as plain registers.
  Their layouts, write behavior and reset values are *unknown*.
- Every register the PROM reads before it writes and tolerates any value from
  (``pi_reg_2001_0100``, ``cpu_hist``, ``cpu_bits``, ``lb_l1_state``, the
  election word, ``fw_word_1540``, ``fw_debug_word``, the bank, timing and
  refresh words, ``md_reg_0001_0068``) resets to 0. That is an
  *approximation*: the hardware's reset value is *unknown*, and the PROM
  accepts any. Registers it never reads before writing are refused on a read before
  the first write instead.
- ``shub_id`` resetting to NASID 0 and revision 3 is an *approximation*. The
  PROM accepts any NASID and either revision constant, and 0 is the default
  a single node gets. Revision 3 selects the PROM's newer constants.
- Both locks always reading free is an *approximation*: it holds only while one
  processor uses it.
- The I/O interface registers the PROM sets up are stored and read back, and
  ``llp_control_status`` always reports the link up. All of that is an
  *approximation*: the PROM tolerates any other value, and no source gives
  the cells' reset values or side effects. The widget behind small window 0,
  the crossbow, is not modelled in the device yet, so reads from the widget
  windows stop the VM. The local-window form of the II block is *inferred* to
  be the same registers.
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
  at least 8 MiB), is a *software expectation* from the reviewed chips
  page, as is the PROM-log header layout. The firmware image at flash offset
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
- The PIC interrupt packet's layout (``0x100`` plus the vector) is *inferred*
  from the register layout, and that the write sets event bit 29 or 30 is
  *unknown*, so no event bit is set. Both slice targets reach the one
  processor: an *approximation*.
- ``crb_message_control`` resets to 6 (*software expectation*: the 2.4 header's
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
- ``lb_slice_rec`` reading 0 is an *approximation*: the PROM accepts any
  value and turns its low byte into a progress code.
- ``lb_reg_1002_0000``, ``md_reg_0003_0400`` and ``md_reg_0005_0400`` accept
  and ignore any write. That is an *approximation*: their effects and roles
  are *unknown*, and the PROM never reads them.
- The counter starts running at the first ``rtc_ctl`` write, whatever bit 0
  and the mode bits say. That is an *approximation*: bit 0 may be a run
  control, which is *inferred* only.
- ``chip_id`` takes its revision from ``shub_id``. That is an *approximation*:
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
  a read returns no made-up value. The VM keeps running and the processor
  model decides what the failed access does. This is instrumentation, not a
  claim about the chip's error response.
- **Interrupts.** The comparator at ``rtc0_int_config`` is the firmware's: it
  sets event bit 23, the others 24 to 26. Delivery follows what the PROM
  and kernel code do, which is *software expectation*: an IPI message or a comparator reaching its
  compare value raises the configured vector and sets its event bit. The
  rest is an *approximation*. The processor id is not decoded: with one
  processor every id names it. Event bits other than the written ones are
  never set, so the error and UART bits stay 0. The IPI enable register and
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
- **One processor.** The slice-present, lock and election behavior assume a
  single processor in slice 0.
- **Memory test.** Every engine operation completes at once and passes. No
  DRAM is modelled, so a compare cannot fail, and no operation writes memory.
  RAM starts zeroed, which is what the PROM's zero fills want. The processor
  reaches bank 0 through the cacheable alias at ``0x30_0000_0000`` for NASID 0
  only.
- **Block transfers.** Only a zero fill is accepted. It moves no data and
  writes no notification word. The PROM never reads the notification word.
- **Not modelled.** Directory contents, the network
  interface, the I/O interface, memory sizing through the L1 controller,
  SHub 2.

Migration
---------

The device state is saved with a ``VMStateDescription``: the stored ID, the
clock base and the stored register words with their written flags, the
comparator and LED values. Restore moves the global window to the saved NASID.
Migration is supported between identical builds only.

Tests
-----

``tests/qtest/sgi-shub-test.c`` runs on the ``x-shub1-test`` fixture machine and covers the junk-bus UART windows and the PROM flash. The rest of the register behaviour is tested in a later change, through the Altix 350 machine.