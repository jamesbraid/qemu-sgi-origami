.. SPDX-License-Identifier: BSD-3-Clause

.. _sgi-qlogic-isp:

========================================================
QLogic ISP1040, ISP12160 and ISP2200 adapters, SCSI tape
========================================================

The QLogic ISP parts are intelligent host adapters. The host builds whole
commands in memory rings, a mailbox interface carries everything else, and
vendor microcode on an on-chip RISC processor does the bus work. This page
covers three of them and the tape drive that sits behind them.

- The ISP1040, a single-channel SCSI adapter, is the disk and CD-ROM
  controller of the Octane and Octane2 (IP30 board), the Origin 200 and the
  Origin 2000 BaseIO and MSCSI boards. QEMU models it as
  ``qlogic-isp1040``, PCI revision 5, the ISP1040C.
- The ISP12160, a dual-channel SCSI adapter, is the controller of the
  Origin 300 and Fuel IO8 boards, the Origin 350 IO9 board and the PCI
  carriers. QEMU models it as ``qlogic-isp12160``, PCI revision 6.
- The ISP2200, a Fibre Channel adapter, is modelled as ``qlogic-isp2200``.
  No machine fits one by default. It has its own page,
  :doc:`qlogic-isp2200`.
- ``scsi-tape`` is a generic SCSI sequential-access target, not a QLogic
  part. It attaches to any of the adapters above.

The two SCSI adapters share ``hw/scsi/qlogic-isp.c`` and its header
``include/hw/scsi/qlogic-isp.h``. They are two parts of one abstract type,
``qlogic-isp``. Each part supplies a table of values, and a part differs
from its sibling only where its table says so. The ISP2200 is
``hw/scsi/qlogic-isp2200.c`` and the tape is ``hw/scsi/scsi-tape.c``.

None of the adapters executes vendor microcode. A guest downloads
microcode at every attach. The model accepts and keeps the download,
answers the checksum question over the words actually sent, and implements
the mailbox commands and rings that the microcode provides. No vendor
firmware is present or required. A successful download shows that the model
took the words, not that a guest's microcode would run on the real part.

Facts are rated as described in :ref:`sgi-ratings`. No vendor programming
manual for any of the three chips is public, so every register, mailbox and
ring fact below is a *software expectation* from public drivers unless it
carries another rating. A model run shows what the model does, not what
hardware does.

.. _sgi-qlogic-isp-sources:

Sources
=======

- The QLogic ISP1020B/ISP1040B product brief,
  :src:`qlogic-isp1040b-product-brief-1996`, and the ISP12160A data sheet,
  :src:`qlogic-isp12160a-datasheet-1999`. Both are short product
  descriptions. They name register groups and give no offsets, bit fields
  or reset values. The ISP12160A sheet states a 66 MHz, 64-bit PCI
  interface and PCI power management revision 1.0 with the D0, D3hot and
  D3cold states.
- The QLogic *Hardware Installation Guide for the QLA2200 Board Family*,
  :src:`860-0228-001`: board RAM and NVRAM sizes and interrupt line A.
- The PCI Local Bus Specification revision 2.2,
  :src:`pci-local-bus-2.2`, for the layout of the Status
  register and the capabilities list.
- Linux ``drivers/scsi/qla1280.c`` and ``qla1280.h`` (ISP1040, ISP12160)
  and ``drivers/scsi/qla2xxx/`` (ISP2200) at ``db2ddb871435``.
- OpenBSD ``sys/dev/ic/qlw.c``, ``qlwreg.h``, ``qlwvar.h`` and
  ``sys/dev/pci/qlw_pci.c`` (ISP1040), and ``sys/dev/ic/qla.c``,
  ``qlareg.h``, ``qlavar.h`` and ``sys/dev/pci/qla_pci.c`` (ISP2200), at
  ``2df91d80c104`` on the ``sgi-is-alive-at-7.9`` branch.
- NetBSD ``sys/dev/ic/isp.c``, ``ispreg.h``, ``ispmbox.h`` and
  ``sys/dev/pci/isp_pci.c`` at ``07004b2a9dac``. For the ISP1040 register
  map this is the second, unrelated implementation next to Linux. The
  OpenBSD driver is a third lineage, written separately.
- The 86Box ISP1020/ISP1040 model, as a second implementation of the reset
  signature and of the firmware-as-memory approach.
- Linux ``drivers/scsi/st.c``, OpenBSD ``sys/scsi/st.c`` with
  ``scsi_tape.h``, and NetBSD ``sys/dev/scsipi/st.c``, for the tape command
  set and mount sequence.
- Captures of real machines: an OpenBSD boot of a real Origin 200,
  :src:`dmesgd-8994`, and the PROM hardware inventories
  :src:`nekonomicon-16727716` and :src:`nekonomicon-17194`. Two mailing
  list posts hold raw configuration-space bytes of ISP12160 functions on
  other systems, :src:`mail-isp12160-config-capture-2005` and
  :src:`mail-isp12160-config-capture-2006`.
- The reference Octane2 (:ref:`sgi-measured-octane2`).

.. _sgi-qlogic-isp-identity:

Identity and placement
======================

.. list-table::
   :header-rows: 1

   * - Item
     - ISP1040 (``qlogic-isp1040``)
     - ISP12160 (``qlogic-isp12160``)
   * - PCI vendor and device
     - ``0x1077:0x1020``
     - ``0x1077:0x1216``
   * - PCI class
     - SCSI bus controller
     - SCSI bus controller
   * - PCI revision byte
     - ``0x05``
     - ``0x06``
   * - Bus configuration 0 (``0x04``)
     - PCI revision plus one, ``0x06``
     - ``0``
   * - SCSI buses
     - 1
     - 2
   * - Interrupt
     - one level INTx, pin A
     - one level INTx, pin A
   * - Bank window at ``0x80``
     - two banks, select at bus configuration 1 bit 3
     - four banks, select at bus configuration 1 bits 9:8
   * - Flat DMA blocks at ``0x20`` and ``0x40``
     - decoded
     - not present
   * - RAM commands past the end of RAM
     - fold back to word 0
     - refused with ``0x4006``
   * - Mailbox submitted while a result is held
     - overwrites the result
     - refused, result kept
   * - Load RAM with a 64-bit address, ``0x0009``
     - not in the command set
     - in the command set
   * - Abort commands ``0x0015`` to ``0x0017``
     - not in the command set
     - in the command set
   * - About-firmware version
     - 1.0.0
     - 10.4.0
   * - PCI Status at reset
     - ``0x0200``
     - ``0x02b0``
   * - PCI power management capability
     - none
     - version 1 at ``0x44``
   * - PCI data path
     - 32-bit agent
     - 64-bit agent
   * - Command bits writable beyond the PCI core's
     - none
     - parity error response, memory write and invalidate
   * - Latency Timer bits writable
     - none
     - all eight

The ``octane`` and ``octane2`` machines build two ISP1040 functions at
onboard PCI devices 0 and 1 of the Bridge. The Origin 200 and Origin 2000
BaseIO boards build two at devices 0 and 1, and the MSCSI board four at
devices 0 to 3. The Origin 300 IO8 and Fuel boards each place one ISP12160
at device 1, the Origin 350 IO9 board places one on its PCI head, and PCI
carriers accept either part with ``-device``. The Origin board models set
the PCI revision byte after they create the device. The Octane models keep
the type's default of 5. No machine instantiates
``qlogic-isp2200``. It is a PCI device for ``-device``.

Two adapters per BaseIO is *established* by :src:`007-3439-002 Figure 2-25,
printed page 69`, which draws two controllers on the BaseIO PCI bus. The
PROM inventory lists two ``PCI-SCSI Rev 5`` adapters at PCI ids 0 and 1
(:src:`nekonomicon-16727716`), *observed*. The Origin 200 drawings put
channel 0 on a 50-pin connector for the 5.25-inch bay and channel 1 on a
68-pin connector for the 3.5-inch bays (:src:`007-3708-002`),
*established*. Which of the two PCI devices is the drawing's channel 0 is
*unknown*. The inventory places the system disk at ID 1 on the first
adapter and the tape and CD-ROM on the second, and a read of the board's
connector wiring would settle the mapping.

PCI revision and the two revision fields
----------------------------------------

An OpenBSD boot of a real Origin 200 prints the adapter as an ISP1020 at
``rev 0x05`` (:src:`dmesgd-8994`), *observed*. The PROM inventory calls the
same adapters ``PCI-SCSI Rev 5`` (:src:`nekonomicon-16727716`), *observed*,
and IRIX prints ``QL1040B (rev. 2)`` for adapters of the class
(:src:`nekonomicon-17194`), *observed*. The ``rev. 2`` is IRIX's
sub-version name, not the PCI revision byte. That all three describe one
part is *inferred* from the shared machine family.

One PCI device identification, ``0x1020``, answers for every member from
the ISP1020 to the ISP1040C, and the revision byte selects the part.
OpenBSD reads the byte and programs the adapter clock from it: 40 MHz
below revision 2 and 60 MHz from revision 2 up (*software expectation*). A
model that reported revision 2 would be driven at the right clock and
misnamed to every guest. The model reports 5 for the ISP1040C.

Linux reads a second revision, the low four bits of bus configuration 0.
The family numbers it one above the PCI byte: 1 for the ISP1020, 2 for
the ISP1020A, 3 for the ISP1040, 4 for the ISP1040A, 5 for the ISP1040B and
6 for the ISP1040C (*software expectation*, Linux ``qla1280.h``). The model
derives bus configuration 0 from the PCI revision byte, so a board that
overrides the byte moves both. Linux compares the field with the ISP1040A
value once, to suppress the FIFO threshold write as a workaround for that
part's broken FIFO, and stores nothing else. Any value but 4 takes the
ordinary path. That the real ISP1040C reports 6 is *inferred* from the
numbering and has not been read from hardware.

No consumer reads a revision from bus configuration 0 on the ISP12160, and
the QLogic sheet names no such field, so the model reads 0, an
*approximation*. The ISP12160 PCI revision ``0x06`` is *observed* in two
mailing-list posts of raw configuration bytes from ISP12160 functions on
other systems (:src:`mail-isp12160-config-capture-2005`,
:src:`mail-isp12160-config-capture-2006`). Neither is an SGI machine.

PCI configuration
-----------------

BAR 0 is a 0x100-byte I/O window and BAR 1 a 0x100-byte 32-bit memory
window. BAR 1 is an alias of BAR 0, so one register file answers behind
both. OpenBSD tries the memory window first and falls back to I/O, so both
must decode (*software expectation*). Interrupt pin is A.

The Status word at reset reads medium DEVSEL timing, ``0x0200``, on the
ISP1040. The reset value matters. The stock IRIX single-CPU miniroot reads
Command and Status together for each PCI device in its I/O configuration
check and compares all 32 bits with an expected value. For this adapter it
expects ``0x02000000``: Command 0 and Status ``0x0200``. A model that
presents a zero Status half fails the I/O slot (*software expectation*,
observed on the model). No public QLogic document gives a reset
value for this register. DEVSEL timing as the meaning of Status bits 10:9
is *established* by :src:`pci-local-bus-2.2`. That the real
chip resets to ``0x0200`` is *unknown*, and a raw configuration read of the
real part would settle it. The error latches in Status are the PCI core's
write-one-to-clear bits, and nothing in the model sets them.

The ISP12160 differs in these ways.

- Status resets to ``0x02b0``: medium DEVSEL, fast back-to-back capable,
  66 MHz capable and a capabilities list. The two configuration dumps in
  the mailing-list posts show the same Status, a capability pointer of
  ``0x44`` and a power management version 1 capability at that address,
  *observed* on other systems. The value at an Origin 300 cold reset is
  *unknown*.
- The capability at ``0x44`` has ID 1, power management version 1 and no
  next pointer. The rest of its bytes are placeholders. The power
  management control word at ``0x48`` holds the PME enable bit (``0x0100``)
  and nothing else.
- The stock Origin 300 IO8 board test writes ``0x156`` to Command and
  requires the whole word back. It writes ``0xf8ff`` to the cache line size
  and Latency Timer word and requires it back. It writes ``0x100`` to the
  power management control word and requires it back (*software
  expectation*, observed on the model). Reading all five Command bits back
  says the part implements them. Memory write and invalidate and parity
  error response are corroborated by the ISP12160A sheet's claims of the
  PCI cache commands and of PCI 2.1 compliance. The model leaves the other
  Command bits as the PCI core sets them: I/O space, memory space, bus
  master, SERR enable and INTx disable are writable, and special cycles,
  VGA palette snoop, wait cycle control and fast back-to-back are
  read-only. Whether the real part implements them is *unknown*. The width of the Latency Timer is *unknown*,
  because the test writes zeros into its low three bits. The model makes the
  whole byte writable. Whether the part advertises PME event support, which
  would decide if the enable bit holds, is *unknown*.
- The part declares itself a 64-bit PCI agent, from the ISP12160A sheet's
  "66-MHz, 64-bit PCI host bus interface". In a slot the board wires to the
  64-bit data path it runs 64-bit data phases and the Bridge places its
  bytes by that width. Whether the IO8 board's on-board bus carries the
  64-bit lines is *unknown*. The machine leaves that slot on a 32-bit path,
  an *approximation* that makes no byte difference under IRIX, which sets
  the adapter's swap bits so that placement is the same at either width
  (:doc:`bridge`).

.. _sgi-qlogic-isp-registers:

Register map
============

The register window is 0x100 bytes of 16-bit registers, little-endian on
the bus, *software expectation*. The memory region accepts 16-bit accesses
only, and an access of another width is refused by the memory core. The
byte swap belongs to the Bridge, not to this device (see
`Byte order and DMA`_).

Every offset the model does not decode answers 0 and logs ``LOG_UNIMP``. A
write to a read-only register logs ``LOG_GUEST_ERROR``.

.. list-table::
   :header-rows: 1

   * - Offset
     - Name
     - Reset
     - Access
     - Notes
   * - ``0x00``
     - Chip identity, low
     - ``0x1077``
     - RO
     - Vendor.
   * - ``0x02``
     - Chip identity, high
     - ``0x1020`` or ``0x1216``
     - RO
     - Device.
   * - ``0x04``
     - Bus configuration 0
     - PCI revision plus one, or 0
     - RO
     - See `Identity and placement`_.
   * - ``0x06``
     - Bus configuration 1
     - 0
     - RW
     - ISP1040 keeps bits ``0x007c``, ISP12160 keeps ``0x0300``. Other
       written bits log ``LOG_UNIMP`` and read 0.
   * - ``0x08``
     - Bus control
     - 0
     - RW
     - Bit 0 soft reset, bit 1 global interrupt enable, bit 2 RISC
       interrupt enable. Bit 0 never reads back set.
   * - ``0x0a``
     - Bus status
     - 0
     - RO
     - Bit 1 interrupt request, bit 2 RISC interrupt request.
   * - ``0x0c``
     - Semaphore
     - 0
     - RW
     - Bits 1:0 stored. Bit 0 is the mailbox-result lock.
   * - ``0x0e``
     - NVRAM
     - 0
     - RW
     - Bit 0 clock, bit 1 chip select and bit 2 data out are stored. Bit 3,
       data in, always reads 0.
   * - ``0x20``, ``0x40``
     - DMA block configuration
     - 0
     - RW
     - ISP1040 only. Bit 1 (burst enable) is stored and other bits read 0.
   * - ``0x22``, ``0x42``
     - DMA block control
     - none
     - WO
     - ISP1040 only. Writes are accepted and do nothing. Reads log
       ``LOG_UNIMP`` and return 0.
   * - ``0x70`` to ``0x7e``
     - Mailboxes 0 to 7
     - signature
     - RW
     - See `Mailboxes and queue pointers`_.
   * - ``0x80`` to ``0xbe``
     - Bank window
     - 0
     - RW
     - 32 words per bank of plain storage. See below.
   * - ``0xc0``
     - Host command, RISC flags
     - 0
     - RW
     - A command in the top four bits on write, state flags on read.

Everything else answers 0 with ``LOG_UNIMP``. That includes the flash BIOS
registers at ``0x10`` and ``0x12``, the GPIO data and enable registers at
``0xcc`` and ``0xce``, the rest of each ISP1040 DMA block (status, FIFO,
count and address) and, on the ISP12160, all four flat DMA offsets. Linux
and OpenBSD name the flash and GPIO registers, and no boot path reads them.
The SGI PROM's SCSI power-on diagnostic writes ``0x000a`` to the two DMA
control registers once per adapter and reads neither back (*software
expectation*, observed on the model), so the model keeps no value there.

Bus configuration 1 and the bank window
---------------------------------------

The window from ``0x80`` to ``0xbf`` shows one of several register banks.
The model keeps each bank as plain 32-word storage, so the only observable
is that the same offset holds different values under different selections.

- ISP1040: bit 3 (``0x0008``) selects between the RISC bank (clear) and the
  SXP bank (set). This is bit 3 and not bit 0. NetBSD uses its bit-3
  constant on every SXP access, while a bit-0 constant in the Linux header
  is defined and never used. The two drivers disagree, and the evidence
  favours bit 3 (*software expectation*). The other bits of the register
  are the FIFO threshold (``0x0070``) and burst enable (``0x0004``).
- ISP12160: bits 9:8 select the RISC (0), SXP0 (1), SXP1 (2) or DMA (3)
  bank. NetBSD uses the 1080-family banked accessors for this part and
  writes ``0x0100``, ``0x0200`` and ``0x0300`` for the last three
  (*software expectation*). That value 0 is the RISC bank follows the
  ISP1040's bank 0 and is an *approximation*. The model keeps only bits 9:8
  of the register, and the full writable mask and reset value are
  *unknown*. The banked layout replaces the flat DMA registers.

Bank accesses are traced, not logged as unimplemented.

Bus control, bus status and the interrupt line
----------------------------------------------

Writing bus control bit 0 resets the adapter to the state in
`Reset and buses`_. The reset runs after the enable bits are stored, so
both enables read 0 afterwards.

The interrupt output is a level. It is high while bus status bit 2 and bus
control bit 2 are both set (*software expectation*). Bus status bit 1 and
the global enable in bus control bit 1 are stored but have no effect on the
line. A second independent implementation also says they do nothing.

A posted mailbox result sets the semaphore to exactly ``0x0001`` and sets
bus status bits 1 and 2. A posted I/O completion sets the same two status
bits and leaves the semaphore alone. The driver reads the semaphore first,
and a set lock bit means that a mailbox result or asynchronous event is
waiting. Only when it is clear does the driver look at bus status for
response entries. So the model takes the lock for a mailbox result and not
for a completion (*software expectation*, Linux and OpenBSD).

The result and the pending interrupt persist until the guest acknowledges,
in either order. Linux writes the clear-RISC-interrupt host command and
then zero to the semaphore, while NetBSD writes the semaphore first. The
host command clears bus status and the zero write releases the semaphore. A
masked interrupt keeps its pending state.

The semaphore's second bit is plain storage. The stock Origin 300 IO8 board
test writes the semaphore while it is already set and expects bits 1:0 back
as ``0x3``, reading bit 1 as "in use by someone". The model reads back what
was written. No published QLogic source states the rule, so the meaning of
bit 1 is *unknown*. A register read on a real ISP12160, or a QLogic
technical manual, would settle it.

NVRAM
-----

The adapter carries a 128-byte serial EEPROM, read by bit-banging the NVRAM
register (*software expectation*, OpenBSD ``qlw``). The model never asserts data in, so every read returns the same
fixed pattern, which fails the identifier and checksum validation. OpenBSD
then prints that the NVRAM is corrupt and proceeds on its built-in defaults
(*software expectation*). The contents of a real Origin 200 EEPROM are
*unknown*. OpenBSD hard-codes SGI initiator ID 0 where the standard default
is 7, so the fallback is correct for these machines (*software
expectation*, OpenBSD ``qlw``).

Host command register
---------------------

A write takes the command from bits 15:12. A read returns the RISC state
flags: ``0x0080`` host interrupt set, ``0x0040`` RISC in reset, ``0x0020``
RISC paused and ``0x0010`` external, which the model never sets.

.. list-table::
   :header-rows: 1

   * - Command
     - Name
     - Effect in the model
   * - ``0x0``
     - No-op
     - None.
   * - ``0x1``
     - Reset RISC
     - Replaces the flags with ``0x0040``. Mailboxes, rings and RAM stay.
   * - ``0x2``
     - Pause RISC
     - Sets ``0x0020``.
   * - ``0x3``
     - Release RISC
     - Clears ``0x0020`` and ``0x0040``.
   * - ``0x5``
     - Set host interrupt
     - Sets ``0x0080`` and runs the mailbox command synchronously. On the
       ISP12160, if a previous result still holds the semaphore, nothing
       changes and ``LOG_GUEST_ERROR`` logs the refusal.
   * - ``0x6``
     - Clear host interrupt
     - None.
   * - ``0x7``
     - Clear RISC interrupt
     - Clears ``0x0080``, clears bus status and re-evaluates the line.
   * - ``0x4``, ``0x9``, ``0xa``, ``0xe``
     - Mask parity, BIOS enable, enable parity, parity error
     - Accepted. ``LOG_UNIMP`` logs "accepted, not modelled".
   * - others
     - Unknown
     - ``LOG_GUEST_ERROR``.

Pause and reset change only the flags. The model runs no RISC, so it has
nothing to stop, and commands keep working while the flags say paused.

.. _sgi-qlogic-isp-mailboxes:

Mailboxes and queue pointers
============================

Eight 16-bit mailboxes sit at ``0x70`` to ``0x7e``. Mailbox 0 carries the
command in and the result out, and the others are per-command arguments.
The host writes the arguments, then the host-interrupt command. The model
runs the command inside that write and leaves the result in the mailboxes.

After reset mailboxes 1 to 4 read ``0x4953``, ``0x5020``, ``0x2020`` and
``0x0001``: "IS", "P ", two spaces, and a plain word. Linux reads all four
right after reset and aborts the attach on a mismatch. OpenBSD reads the
first three and carries on if they differ. The 86Box model sets the same
four values. That all four are a hardware fact and not one driver's habit
is *inferred* from two independent implementations. The signature also
proves that reset ran, since most registers reset to zero.

Queue pointers
--------------

Mailboxes 4 and 5 double as the queue pointers once a queue exists. The
request side is at ``0x78`` and the response side at ``0x7a``.

- Before the request queue is initialised ``0x78`` is mailbox 4. Once the
  queue exists a write stores the producer index and walks the ring, and a
  read returns the adapter's own consumer index. Linux computes its free
  ring space from that read, so echoing the guest's write would break it.
- Before the response queue is initialised ``0x7a`` is mailbox 5. Once the
  queue exists a read returns the adapter's producer index, and a write
  records the guest's consumer index, wrapped to the ring length. The write
  never touches the producer index. OpenBSD reads the producer once at the
  top of its handler and writes that value back as its acknowledgement. A
  completion that ran inside the handler would otherwise be erased by the
  stale acknowledgement.

Gating on the queue's own length keeps the firmware register test working.
That test walks a pattern through mailboxes 1 to 7 before any queue exists
and reads each back, so both queue-pointer mailboxes must answer as
storage then. The guest's producer value is wrapped on use, so a value at
or past the ring length cannot make the ring walk loop.

Mailbox command set
-------------------

A command outside the set answers ``0x4001`` and logs ``LOG_UNIMP``. A
part-limited opcode on the part that lacks it answers the same way.

.. list-table::
   :header-rows: 1

   * - Opcode
     - Command
     - Parts
     - Model
   * - ``0x0000``
     - No-op
     - both
     - ``0x4000``.
   * - ``0x0001``
     - Load RAM
     - both
     - mb1 word offset, mb4 word count, host address in mb2:mb3.
   * - ``0x0002``
     - Execute firmware
     - both
     - ``0x4000``. Runs nothing.
   * - ``0x0003``
     - Dump RAM
     - both
     - The other direction, same arguments.
   * - ``0x0004``
     - Write RAM word
     - both
     - mb1 word address, mb2 data.
   * - ``0x0005``
     - Read RAM word
     - both
     - mb1 word address. mb2 gets the word and mb3 is cleared.
   * - ``0x0006``
     - Mailbox register test
     - both
     - ``0x4000``. Mailboxes 1 to 7 are already unchanged.
   * - ``0x0007``
     - Verify checksum
     - both
     - See below.
   * - ``0x0008``
     - About firmware
     - both
     - mb1, mb2 and mb3 hold 1.0.0 (ISP1040) or 10.4.0 (ISP12160). mb6 is
       0.
   * - ``0x0009``
     - Load RAM, 64-bit address
     - ISP12160
     - As ``0x0001`` with mb6 (address bits 63:48) and mb7 (47:32).
   * - ``0x0010``, ``0x0052``
     - Initialise request queue
     - both
     - See below.
   * - ``0x0011``, ``0x0053``
     - Initialise response queue
     - both
     - See below.
   * - ``0x0015``
     - Abort command
     - ISP12160
     - See `Recovery commands`_.
   * - ``0x0016``
     - Abort device
     - ISP12160
     - See `Recovery commands`_.
   * - ``0x0017``
     - Abort target
     - ISP12160
     - See `Recovery commands`_.
   * - ``0x0018``
     - Bus reset
     - both
     - See `Recovery commands`_.
   * - ``0x001f``
     - Get firmware status
     - both
     - mb1 and mb3 are 0, mb2 is 512 (the request budget).
   * - ``0x0028``
     - Get target parameters
     - both
     - mb2 and mb3 are 0, asynchronous and narrow. A two-channel part
       also clears mb6.
   * - ``0x0030`` to ``0x0039``
     - Initiator ID, selection timeout, retry count, tag age limit, clock
       rate, active negation, asynchronous data setup, PCI control, target
       parameters, device queue
     - both
     - ``0x4000``. Nothing reads the values back.
   * - ``0x003a``
     - Set reset delay parameters
     - both
     - Stores mb1 for channel 0 and, on the ISP12160, mb2 for channel 1.
   * - ``0x0045``, ``0x004a``, ``0x005a``
     - System parameter, firmware features, data-overrun recovery
     - both
     - ``0x4000``.

The setters that complete and do nothing are not decoration. OpenBSD aborts
its attach when any of them answers invalid, before it creates the SCSI
bus, and the only visible symptom is that no ``scsibus`` appears
(*software expectation*). OpenBSD issues them in this order: firmware
features only when its feature flags are non-zero, then clock rate, retry
count, asynchronous data setup, active negation, tag age limit, selection
timeout, initiator ID once per bus, target parameters once per target and
device queue once per target and LUN. Linux issues ``0x0028`` once per
target. IRIX 6.5.30 issues ``0x003a`` during adapter initialisation before
its bus scan, observed on the model.

The firmware status answer sizes the guest's queues and command budget.
OpenBSD caps what it reads at 512, sizes the response queue at half of it
with a floor of 64, and allows one command per three request entries. No
source gives a real adapter's number, so 512 is an *approximation* that
exercises the ring arithmetic at the driver's own ceiling. The About
firmware versions are model constants, also an *approximation*. Guests
print them and never branch on them.

Controller RAM and the download
-------------------------------

Controller RAM is 65,536 16-bit words, cleared at reset. Offsets and
counts are in words, and a host-side step of one word is two bytes
(*software expectation*). The model retains every word so that the
checksum is real.

A load reads the whole transfer from host memory before it keeps any word.
A refused read answers ``0x4002`` and leaves RAM as it was, because a
committed prefix would make the guest's own checksum blame the image for
bytes the model made up. A zero count completes. A dump that the fabric
refuses answers ``0x4002`` for the same reason: a guest told that it
succeeded would read whatever the buffer already held. The words cross the
DMA interface little-endian, whatever the fabric does to byte lanes.

The two parts move the block differently. The ISP1040 has a 16-bit data
path and runs one cycle per word. The ISP12160 moves the whole block in one
transfer. At the alignments a RAM transfer uses, the two were measured on
the model to deliver the same bytes, so no guest can tell them apart. The
distinction is kept because the width belongs to the part.

A RAM range that runs past word 65,535 behaves differently on the two
parts.

- ISP1040: the offset folds back to word 0. This is the model's choice.
  86Box answers a parameter error instead, and no source says which the
  silicon does (*unknown*). The firmware transfers are small and low, so
  nothing on a boot path tells the two apart.
- ISP12160: a load, dump or checksum whose start plus count exceeds 65,536
  answers parameter error ``0x4006`` and writes nothing. No source gives
  the chip's failure code, so ``0x4006`` is an *approximation*.
  The 65,536-word array is also an *approximation*, since the physical RAM
  size and its reset contents are *unknown*.

Plain load and dump (``0x0001``, ``0x0003``) take the host address from mb2
and mb3 only. The 64-bit load (``0x0009``) adds mb6 and mb7. Linux selects
input mask ``0x1f`` for the plain forms and ``0xdf`` for the 64-bit forms,
and NetBSD agrees (*software expectation*). Stale values left in mb6 and
mb7 by an earlier command therefore never change a plain transfer. Linux
also uses ``0x000a`` for a 64-bit dump. The model does not implement it and
answers ``0x4001``.

Verify checksum takes the word offset in mb1, reads the image length from
the fourth word of the image (word offset 3), and sums that many 16-bit
words modulo 65,536. A sum of zero answers ``0x4000`` and anything else
``0x4003``. The sum goes to both mb1 and mb2, because the SGI firmware reads
mb2 and Linux reads mb1. On the ISP12160 a header word or a span that would
pass the end of RAM answers ``0x4006`` and leaves the sum mailboxes alone.
The exact checksum algorithm of the chip is *unknown*. This follows the
loaders that all three drivers share.

The SCSI diagnostic of the stock IP35 PROM (release 6.210, the BaseIO
image) issues load RAM ``0x0001`` and dump RAM ``0x0003`` against the
ISP12160 on the Origin 300 IO8 board (*software expectation*, observed on
the model). It writes a
pattern through the adapter and reads it back.

Queue initialisation
--------------------

The request and response queues take an entry count in mb1 and a base
address from mb2 and mb3, plus mb6 and mb7 for the 64-bit opcodes. The
address assembles as ``(mb6 << 48) | (mb7 << 32) | (mb2 << 16) | mb3``,
with each word widened before the shift. The plain opcodes (``0x0010`` and
``0x0011``) ignore mb6 and mb7. The 64-bit opcodes (``0x0052`` and
``0x0053``) use them. NetBSD gives the plain commands input masks that
exclude those two mailboxes and the 64-bit ones masks that include them
(*software expectation*). OpenBSD issues the plain opcodes while setting
the wider masks. That shows extra mailbox writes, not that the firmware
consumes them for the plain command.

A zero base or a zero count answers ``0x4006``. The queues can be
initialised in either order, and the two drivers use opposite orders.
Initialising the request queue clears the adapter's consumer index and
mailbox 4. Initialising the response queue clears the adapter's producer
index and the guest's consumer index, and discards any completion held for
the ring. Nothing resumes consumption there, so a request ring stalled
behind that backlog stays stalled until the guest writes the producer
pointer or acknowledges a response. That discard is model behaviour, not a
documented hardware answer.

.. _sgi-qlogic-isp-rings:

Rings
=====

Both rings hold 64-byte entries with little-endian fields. One request ring
and one response ring serve the whole adapter, including both channels of
the ISP12160, and the channel bit in each entry routes it. The
queue-initialise commands carry no channel selector, so a second
initialisation replaces the first.

Request entries
---------------

Four entry types are read. The first byte is the type and the second is the
number of ring slots the command takes, continuations included.

.. list-table::
   :header-rows: 1

   * - Type
     - Meaning
     - Layout
   * - ``0x01``
     - Command, 32-bit segments
     - Four inline segments at byte 32, each a 32-bit address and a 32-bit
       length.
   * - ``0x02``
     - Continuation of ``0x01``
     - Seven segments from byte 8.
   * - ``0x09``
     - Command, 64-bit segments
     - Two inline segments at byte 40, each 12 bytes: low address word,
       high address word, length.
   * - ``0x0a``
     - Continuation of ``0x09``
     - Five 12-byte segments from byte 4. There is no handle: offset 4 is
       the low half of a DMA address.
   * - ``0x04``
     - Marker
     - Consumed, no answer.

The command header is the same in both formats through byte 31
(*software expectation*, Linux and OpenBSD agree on every offset). The
handle is a 32-bit field at 4. The device word at 8 carries the LUN in bits
7:0, the target in bits 14:8 and the channel in bit 15, so byte 9 holds the
channel in bit 7 and the target in bits 6:0. The CDB length is at 10,
control flags at 12, the timeout at 16, the segment count at 18 and twelve
bytes of CDB from 20. The model reads the handle, device word, CDB length
(capped at 12), segment count and CDB. It does not read the control flags,
because QEMU's SCSI layer derives the transfer direction from the CDB, and
it ignores the timeout.

Linux uses only the 64-bit pair, because its DMA addresses are 64 bits on
every 64-bit kernel. OpenBSD uses only type ``0x01``. A continuation is
never dispatched on its own. The ring walker steps past it using the owning
command's slot count, which also keeps the walk from reading a DMA address
as a handle.

The walk is driven by the number of published entries, decremented by each
entry's slot count with the count clamped to what remains. A walk that
compared cursor and producer for equality would never end for a stride that
shares a factor with the ring length. A zero slot count is read as 1. An
entry type the model does not read is answered incomplete (completion
``0x0001``, both flag words zero) and logs ``LOG_GUEST_ERROR``.

The descriptors name the host's buffer and the command names how many bytes
the target fills, so only that many are offered to the SCSI layer. The
whole described length is still counted, because the residual is measured
against it. A segment of length zero adds nothing. A descriptor read that
the fabric refuses reads 0. A CDB read that the fabric refuses drops the
request, leaves the consume index on the entry and logs the entry address.

The SCSI bus carries targets 0 to 15 and LUNs 0 to 7. An absent device and
an address past the bus both take the missing-target answer below. A
channel bit past the part's channel count (bit 15 on the ISP1040) is
answered incomplete with both flag words zero.

Status entries
--------------

A status entry (type ``0x03``, count 1) has these fields.

.. list-table::
   :header-rows: 1

   * - Offset
     - Field
     - Model
   * - 4
     - Handle
     - The command's handle.
   * - 8
     - SCSI status
     - Target status in the low byte. Bit 9 (``0x0200``) is set when sense
       data is valid.
   * - 10
     - Completion
     - ``0x0000`` complete, ``0x0001`` incomplete, ``0x0004`` reset,
       ``0x0005`` aborted, ``0x0015`` data underrun.
   * - 12
     - State flags
     - See below.
   * - 14
     - Status flags
     - 0, except ``0x0040`` in a missing-target answer.
   * - 16
     - Response length or time
     - 0. Neither driver reads it.
   * - 18
     - Sense length
     - Up to 32.
   * - 20
     - Residual
     - Bytes left unmoved.
   * - 32
     - Sense data
     - Up to 32 bytes.

All 64 bytes are written, so a reused ring slot never carries a previous
completion's bytes. Offsets 4 to 20 are *software expectation*, identical in
Linux and OpenBSD. The two disagree only on the name of offset 16.

Completion ``0x0000`` means that the transport finished, and the target's
own status stays in the low byte. A target that answers CHECK CONDITION
still completes ``0x0000``. Underrun ``0x0015`` is reported only when the
target status is GOOD and bytes remain unmoved, counting descriptor bytes
beyond the command's length. The residual is that unmoved count.

Linux reads the state flags only on an incomplete completion, to choose
among its failure reasons. NetBSD reads them for its got-target decision.
OpenBSD tests a header byte instead and collapses every incomplete
completion to a selection timeout. The flags occupy bits 8 to 14: bus
acquired ``0x0100``, target selected ``0x0200``, CDB sent ``0x0400``, data
transferred ``0x0800``, status received ``0x1000``, sense received
``0x2000`` and transfer complete ``0x4000``.

The two parts compute a completed command's flags differently.

- ISP1040: every flag except sense is set, and sense is set only when the
  completion carries sense. Applying the all-phases mask to a command with
  no data phase, such as TEST UNIT READY, raises data bits for work that did
  not run. That is an *approximation* of the part, and no source
  contradicts it.
- ISP12160: a flag for work the command skipped would misreport it, and
  Linux checks the bus and target flags before the later phases. The model
  therefore reports a flag only for work done, an *approximation*. It always reports bus, target, CDB and status. It adds data transferred
  once a byte has moved, transfer complete when every described byte moved,
  and sense only when sense was collected. A command with no data therefore
  reports neither data bit.

The sense bit must stay clear on a GOOD completion. The sense phase means
that the adapter ran auto-sense, which a target triggers only by returning
CHECK CONDITION. With the bit raised on every completion, IRIX 6.5.30
re-issues a CD-ROM's MODE SELECT without bound and never reaches the login
(*software expectation*, observed on the model). Why IRIX reacts so is
*unknown*; a read of its completion path would settle it.

Missing targets
---------------

A command for a target with no device completes incomplete (``0x0001``)
with bus acquired set and target selected clear in the state flags, and
the selection-timeout bit ``0x0040`` in the status flags. The offsets and
the bit value come from the public layouts, and ``0x0040`` is NetBSD's
``RQSTF_TIMEOUT``. Linux branches on the target-selected bit and NetBSD on
the same condition, and neither public parallel driver reads the status
flags there.

IRIX 6.5.30 does. On the Origin 200 model with the bit clear, the kernel
declares a hard error for every absent target on the bus during its scan.
With the bit set it scans silently, on both channels of an ISP12160
plugged into the same machine (*software expectation*, observed on the
model). The PROM SCSI walk completes cleanly with the bit set. Both parts
set the bit. What real silicon reports in this word is *unknown*. For the
ISP1040 the answer is an *approximation* that satisfies all four consumers.
For the ISP12160 no source gives the word, and the IRIX behaviour settles
the choice.

Response ring pressure
----------------------

The response ring reserves one slot, so equal producer and consumer always
mean empty. A real adapter completes one command at a time with bus latency
and cannot fill the ring before the driver services it. The model has no
such latency. A completion that finds the ring full is held in the order it
finished and posted as the guest's acknowledgement frees room. Each hold
logs ``LOG_GUEST_ERROR``. A completion posted before the response queue
exists is dropped with the same log.

The announced budget bounds the backlog. The model consumes no request
entry while any completion is held or 512 commands are outstanding, and
resumes when one is posted or retires. A held backlog can reach at most
512, and a migration stream claiming more is refused. This is an
*approximation* of hardware that never reaches the full-ring case.

A status write that the fabric refuses is dropped: the producer index does
not advance and no interrupt is raised, and the drop logs the entry
address. Advancing or interrupting would point the guest at a slot holding
a previous entry.

.. _sgi-qlogic-isp-recovery:

Recovery commands
=================

The recovery commands cancel outstanding requests and retire each through
the response ring. Retirement completes the backend cancellation first, so
no callback for a retired handle can arrive after the guest reuses the
handle. The mailbox result and the retirements are separate events, and
the host waits for both.

Bus reset ``0x0018`` is on both parts. The other three commands are the
ISP12160's alone, and the ISP1040 answers ``0x4001`` for them.

.. list-table::
   :header-rows: 1

   * - Opcode
     - Inputs
     - Cancels
     - Retired with
   * - ``0x0015`` abort command
     - mb1: channel bit 15, target bits 14:8, LUN bits 7:0. mb2:mb3:
       handle.
     - The one request with that handle on that channel, target and LUN.
     - ``0x0005``
   * - ``0x0016`` abort device
     - mb1 as above. mb2 and mb3 zero.
     - Every request for that LUN on the target and channel.
     - ``0x0005``
   * - ``0x0017`` abort target
     - mb1: channel and target in the upper byte.
     - Every request to the target across its LUNs, on that channel. Also
       resets the target's devices.
     - ``0x0004``
   * - ``0x0018`` bus reset
     - mb1 reset delay, mb2 channel.
     - Every request on the channel. Resets the channel's bus.
     - ``0x0004``

The state flags of a retired request are 0, since no further phase was
reached. Linux supplies the abort-command and target-reset layouts and a
bus-reset layout with the delay in mb1 and the channel in mb2 (*software
expectation*). IRIX 6.5.30 submits the abort-device and bus-reset forms,
with the same bus-reset placement (*software expectation*). The
abort-device scope, an explicit channel, target and LUN selector, is the
model's bounded reading of that request, an *approximation*. The ISP1040
reads neither mailbox in bus reset and resets its one bus.

A natural completion racing an abort is retired once with its own handle. A
transfer already issued to the backend may complete into the guest's buffer
before the abort call returns. The cancellation is synchronous, so nothing
lands after that.

The result codes the commands return are a selected scope, not values read
from a captured device, and all are an *approximation*.

- Abort command answers ``0x4005`` for an unknown handle, or one that does
  not match the request found at the channel, target and LUN, and retires
  nothing.
- Abort device answers ``0x4000`` when nothing is outstanding for the LUN.
- On the ISP12160, bus reset answers ``0x4006`` for a channel number at or
  past the channel count. The three abort commands take the channel from a
  single selector bit and can never name a third channel, so the same check
  cannot fail for them.

The `Tests`_ pin the unknown-handle answer only as not a completion that
retires nothing, and pin the abort-device answer to exactly ``0x4000``.

.. _sgi-qlogic-isp-reset:

Reset and buses
===============

Power-on reset, PCI reset and the bus-control soft reset land on one state.
The mailboxes return to the signature. Bus configuration 1, bus control,
bus status, the semaphore, NVRAM, the host-command flags, the DMA block
configuration, all register banks, controller RAM and the per-channel reset
delays clear. Both rings and every held completion are discarded, and the
resume of a stalled ring is cancelled. Values other than the signature reset
to zero under the rule that an unconfirmed value is zero, and no driver
source contradicts them. That is an *approximation*.

A soft reset also cold-resets every realised SCSI bus, which cancels
in-flight commands silently through the SCSI core, because the same reset
clears the rings a completion would have been posted to. A device reset
reaches the buses through the device tree. The bus-reset mailbox command is
different: it resets one channel's bus, leaves chip state standing and
retires its outstanding requests first.

The ISP12160 owns two SCSI buses. Each bus stamps its channel number into
every device plugged onto it. A target number repeated on the two channels
is therefore two addresses and not a collision, and the two channels'
devices have distinct migration paths. Target and LUN numbers are local to a
channel. The helper that attaches ``-drive if=scsi`` disks offers the first
channel only. A device removed from a bus unplugs through the adapter,
which holds the ring's resume until the purge finishes.

Byte order and DMA
==================

.. _sgi-qlogic-isp-dma:

The device does plain little-endian DMA and holds no byte-order rule of its
own. Every ring, descriptor, command block, data segment and RAM transfer
goes through the card's own PCI address space, so the Bridge's address
translation applies and no path skips the host bridge. The Bridge places the
bytes from the requesting slot's swap bits and the window the address falls
in (:doc:`bridge`). If the Bridge's swap were removed this device would need
a correction, and a correction added here while the Bridge keeps its own
would cancel it.

IRIX 6.5.30 asks for byte-stream order and 64-bit addressing on every
mapping for this adapter. On the Origin 200 model both ISP slots read
``0x12099082`` and ``0x12099084`` in the Bridge device register once IRIX is
up, so bit 19 is set and bits 20 and 13 are clear (*software expectation*,
observed on the model). IRIX writes no byte-order setting to the adapter
itself. The kernel keeps queue entries as big-endian structures and swaps
each 32-bit word in software on the way in and out (*software
expectation*), so the entries sit in host memory in the little-endian
layout the public drivers use. Data
blocks pass through the same mapping unchanged. That the adapter therefore
sees host-memory order is *inferred* from that behaviour and the Bridge's
swap-set tables.
Whether the chip itself puts a byte stream or word values on the bus is
*unknown*, and an ISP1040 manual that states the bus order would settle it.
The ISP1040 is a 32-bit PCI agent and is not declared 64-bit.

Migration
=========

The ``qlogic-isp`` stream is version 10 and loads only that version. It
carries the registers, mailboxes, banks, the 65,536-word RAM, queue state
and the held completions as a counted array. The count is checked before
any entry is read, and a stream claiming more than 512 is refused.
In-flight requests travel with the SCSI core's request stream, with the
segment count bounded by the entry format and a stream claiming more
failing the load.

A subsection, ``qlogic-isp/isp12160``, carries the SXP1 and DMA banks and
the two channels' reset delays. It is emitted only for a banked or
two-channel part, so an ISP1040 stream never contains it. Migration works
only between identical builds.

.. _sgi-qlogic-isp2200:

The ISP2200 adapter
===================

The ISP2200 Fibre Channel adapter, QEMU's ``qlogic-isp2200``, has its own
page: :doc:`qlogic-isp2200`.

.. _sgi-scsi-tape:

SCSI tape
=========

``scsi-tape`` is a sequential-access SCSI target, peripheral device type 1,
in ``hw/scsi/scsi-tape.c``. It is not memory-mapped. A SCSI initiator reaches
it through the bus of an adapter above, and the contract is a command set.
A tape has no block address to seek to. The medium has a position, records
are read and written in order, filemarks separate data sets, and the end of
recorded data is a different condition from the physical end of tape. The
model tracks that position.

A published Origin 200 inventory lists ``ARCHIVE Python 02779-XXX`` at SCSI
ID 5 on the second adapter, beside the CD-ROM at ID 6, with the system disk
at ID 1 on the first (*observed*). Whether this unit is a DDS (4 mm DAT) drive is *unknown*;
a manufacturer document for the product would settle it. The tape is not
on any boot path. Guests reach it through ``mt``, ``tar``, ``cpio``, ``bru``
and the online tape diagnostic.

Identity
--------

Vendor, product and revision come from the ``vendor``, ``product`` and
``ver`` properties, space-padded to 8, 16 and 4 bytes. Without them the
INQUIRY answer is ``QEMU``, ``QEMU TAPE`` and the QEMU hardware version. The
model does not default to the Origin's drive identity. The functional
diagnostics set the properties to ``ARCHIVE`` and ``Python 25601``, not to
the inventory string. That is a model choice, an *approximation*. Whether
IRIX 6.5.30 treats the two strings differently is *unknown*; running its
online tape test against each would settle it. The real drive's
INQUIRY revision is *unknown*.

INQUIRY returns 36 bytes: type 1, the removable-medium bit set, version 2,
response format 2 and additional length 31. Vital product data page 0x00
lists support for pages 0x00 and 0x80, and page 0x80 returns the ``serial``
property. Any other page, or a non-zero page code without the EVPD bit,
answers ILLEGAL REQUEST (invalid field).

Command set
-----------

The commands are the SSC and SPC commands that the three public drivers
issue (*software expectation*). INQUIRY, REQUEST SENSE, PREVENT/ALLOW MEDIUM
REMOVAL, LOAD/UNLOAD, TEST UNIT READY, READ BLOCK LIMITS, MODE SENSE and
MODE SELECT need no medium. Every other command first checks that a medium
is loaded and answers NOT READY (no medium) if it is not.

.. list-table::
   :header-rows: 1

   * - Opcode
     - Command
     - Model
   * - ``0x12``
     - INQUIRY
     - As above.
   * - ``0x03``
     - REQUEST SENSE
     - The SCSI bus core returns the stored sense. The device's own handler
       returns NO SENSE.
   * - ``0x1e``
     - PREVENT/ALLOW MEDIUM REMOVAL
     - Byte 4 bit 0 sets the lock.
   * - ``0x1b``
     - LOAD/UNLOAD
     - Byte 4 bit 1 (retension) rewinds and needs a medium. Bit 0 loads and
       fails with NO MEDIUM when the drive has no backing image. Unload
       fails with NOT READY, removal prevented, while the lock is set. Load
       and unload both rewind.
   * - ``0x00``
     - TEST UNIT READY
     - GOOD with a medium loaded, otherwise NO MEDIUM.
   * - ``0x05``
     - READ BLOCK LIMITS
     - Granularity 0, maximum ``0x00ffffff``, minimum 1.
   * - ``0x1a``
     - MODE SENSE(6)
     - See below.
   * - ``0x15``
     - MODE SELECT(6)
     - See below.
   * - ``0x01``
     - REWIND
     - Position to the start.
   * - ``0x34``
     - READ POSITION
     - Short form (action 0): 20 bytes, beginning-of-partition flag in byte
       0 bit 7, first and last block both the current record count. Long
       form (action 6): 32 bytes with the block number at offset 8 and the
       file number at 16. Other actions answer invalid field.
   * - ``0x08``
     - READ(6)
     - See below.
   * - ``0x0a``
     - WRITE(6)
     - See below.
   * - ``0x10``
     - WRITE FILEMARKS
     - Writes the counted marks.
   * - ``0x11``
     - SPACE
     - Codes 0 (blocks), 1 (filemarks) and 3 (end of data), with a signed
       24-bit count. Other codes answer invalid field.
   * - ``0x2b``
     - LOCATE(10)
     - Rewinds and walks forward to the logical object in bytes 3 to 6. Past
       the end it answers BLANK CHECK.
   * - ``0x19``
     - ERASE
     - Truncates the image at the current position and seals it with the
       end-of-data marker. The long-erase bit is ignored.

Any other opcode, with a medium loaded, answers ILLEGAL REQUEST (invalid
opcode). A read-only backing image sets the write-protect bit in MODE SENSE
and makes WRITE(6), WRITE FILEMARKS and ERASE fail with DATA PROTECT (write
protected).

OpenBSD's mount sequence runs LOAD, TEST UNIT READY, READ BLOCK LIMITS, MODE
SENSE for page 0, an unconditional MODE SELECT with a block descriptor, and
PREVENT/ALLOW MEDIUM REMOVAL (*software expectation*). A target that rejects
LOAD or MODE SELECT, or returns nonsense block limits or an empty mode
parameter block, is detected and then fails to mount.

Mode pages
----------

MODE SENSE returns a four-byte header and an eight-byte block descriptor.
The header holds the mode data length, medium type 0, a device-specific byte
with write-protect in bit 7 and buffered mode in bit 4, and a
block-descriptor length of 8, or 0 with DBD. The descriptor carries the
density code at byte 4 and the block length at bytes 9 to 11. The page code
selects the pages that follow.

.. list-table::
   :header-rows: 1

   * - Page
     - Content
   * - ``0x00``
     - Header and descriptor only.
   * - ``0x01``
     - Read-write error recovery, all zero.
   * - ``0x0f``
     - Data compression, decompression enabled and nothing else.
   * - ``0x10``
     - Device configuration, byte 10 = ``0x18``.
   * - ``0x11``
     - Medium partition: one partition, not partitionable. A DAT driver reads
       it at open and fails the open if it is missing.
   * - ``0x3f``
     - Pages ``0x01``, ``0x0f``, ``0x10`` and ``0x11``.

Other pages answer invalid field. Page control 3 (saved values) answers
SAVING PARAMETERS NOT SUPPORTED. Page control 1 (changeable) reports every
page body, and the descriptor's density and block length, as changeable.
MODE SELECT accepts a header with no block descriptor or the eight-byte
descriptor, stores the non-zero density and the block length, and keeps the
SCSI core's block size in step so that a fixed-mode transfer is sized
correctly. A zero block length selects variable records. A block length
over ``0x00ffffff`` or another descriptor length answers invalid parameter.
Pages are accepted whole and none is acted on beyond what MODE SENSE
reports. A page other than the four logs ``LOG_UNIMP``.

Reads, writes and sense
-----------------------

READ(6) and WRITE(6) carry a fixed bit. With it set, the length counts
blocks of the mode block length, and a zero block length answers invalid
field. With it clear, the length is one variable-length record. One command
moves at most twice ``0x00ffffff`` bytes. Which mode a guest uses is its own
choice, and the model starts in variable mode at reset.

A boundary is not an error, but it ends the transfer early and reports
through fixed-format sense after any data read has been delivered. Byte 2
carries the key in its low four bits and the flags ``0x80`` filemark,
``0x40`` end-of-medium and ``0x20`` incorrect-length indicator. The
information field (bytes 3 to 6, valid bit set) carries the residue: blocks
not transferred in fixed mode and bytes in variable mode.

.. list-table::
   :header-rows: 1

   * - Condition
     - Key
     - Flag
     - ASC/ASCQ
   * - Filemark read or spaced over
     - NO SENSE
     - filemark
     - ``0x00``/``0x01``
   * - End of data reached
     - BLANK CHECK
     - none
     - ``0x00``/``0x05``
   * - Start of tape reached spacing backwards
     - BLANK CHECK
     - none
     - ``0x00``/``0x04``
   * - Record length differs from the request
     - NO SENSE
     - incorrect length
     - ``0x00``/``0x00``
   * - Unreadable record
     - MEDIUM ERROR
     - none
     - ``0x11``/``0x00``
   * - Write or filemark failed on the host
     - MEDIUM ERROR
     - none
     - ``0x0c``/``0x00``

Where the three drivers diverge, the model emits OpenBSD's form. OpenBSD
recognises the start of tape only as BLANK CHECK ``0x00``/``0x04`` on a
SPACE. NetBSD has no start-of-tape case. Linux reads BLANK CHECK as end of
data. The model does not emit an end-of-medium condition in any form. It
models no early warning, its image grows so a write never overflows, and its
end of recorded data is reported as end of data (*approximation*).

A variable read of a record whose length differs from the request returns
the record up to the requested length and sets the incorrect-length flag.
The information field is the request length minus the record length, which
is negative when the record is longer. SILI suppresses the report only when
the record is shorter. A fixed read of a wrong-length record delivers up to
one block, does not count it as transferred and reports the full remaining
block count. A filemark moves the position past the mark. End of data leaves
the position where it was.

WRITE writes records at the current position and then truncates the image
there, so everything after is gone, as on a real tape. The image grows on
write. A zero-length variable write is GOOD and writes nothing.

Backing image
-------------

The ``drive`` property supplies a host file that holds the medium. It is a
SIMH tape image, readable by SIMH and the tools that share its ``.tap``
convention. A record is a 4-byte little-endian length, the data padded to
an even length, and the length again, so the medium can be walked
backwards. A filemark is a zero length. SIMH's all-ones end-of-medium marker
follows the last entry and is read as end of data. The marker is written
because the block layer reports an image's length rounded up to a 512-byte
sector, and the zero padding past a file's real end would read as filemarks.
A zero length in the last sector followed only by zeros counts as that
padding, so an image another tool wrote without the marker still ends where
its data does. A filemark, or a run of them, ending such an image inside its
last sector looks like padding and is lost. Images the device writes always
carry the marker. SIMH's erase-gap marker and error-flagged records (a
length with the top bit set) read as a medium error. A fresh empty file is a
blank tape.

The device keeps a byte offset for the position and a running count of
records and marks passed since the start. READ POSITION and LOCATE use the
count, and SPACE and the data commands use the offset. A writable backend
gets resize permission so that a write can set the image's exact length. A
host-side resize failure logs a warning, and the guest sees a medium error.

Reset rewinds, selects variable records, clears the density code and the
removal lock, reloads the medium if the backend is present and purges
pending requests with a reset sense. The ``scsi-tape`` migration stream is
version 1 and carries the position, record and file counts, block size,
density, medium-loaded flag and removal lock.

.. _sgi-qlogic-isp-measurements:

Measurements
============

No register of any of these parts has been read on a physical machine. The
reference Octane2 supplied two software reports.

.. list-table::
   :header-rows: 1

   * - Item
     - Value
     - Chip revision
     - When
   * - Hardware inventory SCSI line
     - two integral ``QL1040B`` controllers, revision 2
     - ISP1040, IRIX name ``rev. 2``
     - Running IRIX 6.5.30m, ``hinv``
   * - Hardware graph
     - disk under ``/hw/node/xtalk/15/pci/0/scsi_ctlr/0``, target 1, LUN 0,
       and a second ``scsi_ctlr`` link under ``pci/1``
     - not reported
     - Running IRIX 6.5.30m, read-only listing

Both are inventory reads (:ref:`sgi-measured-octane2-hinv`,
:ref:`sgi-measured-octane2-graph`). They establish that the reference
Octane2 carries two QL1040B controllers at PCI devices 0 and 1 and that the
disk is target 1 on controller 0. They do not establish a PCI revision byte
or any register value. They do not say which drive bay holds the disk. The
``rev. 2`` is IRIX's sub-version label, not the PCI revision byte.

.. _sgi-qlogic-isp-limits:

Model limits
============

- No microcode runs. The RAM holds what was downloaded and nothing executes
  it. Execute firmware answers ``0x4000`` and changes nothing. SCSI, Fibre Channel and
  tape timing, bus phases and negotiation are not modelled. Every setter
  that nothing reads back is accepted and ignored.
- Target mode, flash BIOS, GPIO, the ISP1040 DMA block registers beyond
  burst enable and control, the SXP and RISC register contents (the banks
  are storage) and parity checking are not modelled. A read of an undecoded
  register logs ``LOG_UNIMP`` and a bank access is plain storage.
- A mailbox command outside the set answers ``0x4001`` and logs
  ``LOG_UNIMP``. A host command the model leaves unmodelled logs
  ``LOG_UNIMP``. An unknown host command or a write to a read-only register
  logs ``LOG_GUEST_ERROR``.
- Reset values other than the signature, the RAM size, the 512-request
  budget, the firmware versions, the recovery result codes, the status-flag
  selections and the held-completion rules are *approximation*, listed where
  they appear above.
- No ISP1040 vendor programming manual exists, so its register contract is
  weaker than that of a part with a vendor register table. Its PCI
  revision of 5 is *observed* on Origin 200 captures. The value 6 in bus
  configuration 0 is *inferred* from the family numbering and has not been
  read from hardware.
- The tape models no real DDS mechanics (helical scan, compression
  negotiation beyond a mode page, tension, repositioning time), no medium
  changer, no partitions, setmarks or audio mode, no end-of-medium
  condition and no boot from tape.
- Migration works only between identical builds.

.. _sgi-qlogic-isp-tests:

Tests
=====

- ``tests/qtest/qlogic-isp-test.c`` is the ISP1040 register contract on a
  stand-alone PCI bus. It covers identity and the PCI revision, both BARs,
  bus configuration 0 and 1, soft reset and the signature (including the
  exact Linux product-ID comparison), the DMA block configuration and
  control registers, bus control and status, the semaphore, the mailbox
  register test, the queue pointers, the host commands, the bank select (bit
  3 switches and bit 0 does not), no-op and invalid commands, the firmware
  download, checksum failure, the RAM word round trip, block RAM DMA and its
  byte order, About firmware and Firmware status, every setter, queue
  initialisation with stale high mailboxes, a command round trip, stale
  status bytes, the sense phase flag, the missing-target flags, unknown
  entry types and continuations, the bounded ring walk, SCSI bus resets,
  data transfers through type 0 and 64-bit entries with continuations, NVRAM
  reads, a full response ring, a stale acknowledgement and a held
  completion.
- ``tests/qtest/sn0-qlogic-isp-test.c`` repeats the access contract through
  the XIO to PCI path on the Origin topology: the PCI Status reset value,
  the swap bit, the signature through the fabric, RAM DMA through the
  Bridge, ring byte placement with the swap bit clear and set, missing
  targets, checksum mailboxes, target parameters, reset delay, refused CDB
  reads and refused status writes.
- ``tests/qtest/sn0-isp12160-test.c`` is the ISP12160 contract through the
  same path. It covers identity and 16-bit-only access, both BARs, the
  three banks, bus configuration 1, the flat DMA registers, bus
  configuration 0, the reset signature, mailbox test patterns, RAM word, flat
  and 64-bit load and dump (range, failed DMA, byte order, translation),
  checksum, execute, firmware status, reset delay for both channels, a
  submission while a result is held, queue initialisation in either order
  and with the high words, two LUNs behind one target, writes on channel 1,
  cursor roles, check condition with real sense, missing targets on each
  channel, abort device, abort command, abort target and bus reset scoped to
  a channel, a completion racing an abort, handle reuse and migration of
  distinct state.
- ``tests/qtest/scsi-tape-test.c`` drives the tape through an ISP1040. It
  covers inquiry and its identity, unit ready and load, block limits, mode
  sense with write protect and the partition page, write, rewind and read,
  length mismatches in variable and fixed mode, filemark and end-of-data
  sense, fixed mode select, erase, locate, an illegal opcode, a zero
  allocation length and request sense.
- ``tests/functional/mips64/test_prom_fx.py`` runs the stock disk formatter's
  write-compare and butterfly exercises over the Origin 200's ISP1040.
  ``tests/functional/mips64/test_irix_diag.py`` and its per-machine files
  boot IRIX 6.5.30 and run the online diagnostics, including the online tape
  test against ``scsi-tape`` at ID 5 behind the second adapter. The Origin
  2000 PCI carrier row also checks the ISP12160 in the hardware inventory.
