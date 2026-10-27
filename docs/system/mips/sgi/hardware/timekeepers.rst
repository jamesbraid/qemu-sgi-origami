.. SPDX-License-Identifier: BSD-3-Clause

.. _sgi-timekeepers:

===============================================================
Battery-backed timekeepers (M48T35, DS1742 and DS1744)
===============================================================

The SGI Origin-family boards keep their wall clock and a block of
configuration RAM in a byte-wide, battery-backed SRAM with a clock in its
top eight bytes. The Origin 200 IP29 board carries an M48T35 behind the IOC3
(:doc:`ioc3`), *observed* on a published photograph (see
:ref:`sgi-timekeepers-parts`). The model fits an M48T35 to the Origin 2000
and Onyx2 BaseIO boards, and an M48T35-compatible part at the same place on
the Origin 300 IO8 and Fuel system boards. The physical parts on those
boards are *unknown*. For the IP53 products (Origin 350, Onyx 350, Tezro and
the Onyx4 host) the model fits a DS1744 behind the IOC4; the physical part
is *unknown* too. The Octane and Octane2 use a different clock, the DS1685
(:doc:`ds1685`).

QEMU models the M48T35 family as ``hw/rtc/m48t35.c`` (QOM type
``m48t35``, 32 KiB, with the IOC3 mapping it) and the DS1742 and DS1744 as
``hw/rtc/ds1742.c`` (types ``ds1742``, 2 KiB, and ``ds1744``, 32 KiB, one
shared implementation). The models are close but not the same: they differ
in calendar rule, time source, reset and the file that backs them.

Facts are rated as described in :ref:`sgi-ratings`.

.. _sgi-timekeepers-sources:

Sources
=======

- *M48T35, M48T35Y 5 V, 256 Kbit (32 Kb x 8) TIMEKEEPER SRAM*,
  STMicroelectronics document 2611 revision 10, :src:`st-m48t35-rev-10`.
  Page numbers below are its printed numbers. The SNAPHAT M48T35Y differs
  from the M48T35 in supply and write-protect voltage, not in clock or
  storage behaviour :src:`st-m48t35-rev-10 p. 25`.
- *DS1742 Y2KC
  Nonvolatile Timekeeping RAM*, Maxim/Analog Devices document 19-6715
  revision 6/13, :src:`analog-ds1742-rev-6-13`, and *DS1744/DS1744P
  Y2K-Compliant, Nonvolatile Timekeeping RAMs*, document 19-5502 revision
  6/13, :src:`analog-ds1744-rev-6-13`.
- OpenBSD, tree ``openbsd-sgi`` at
  ``2df91d80c104636deeef8124252ddd5951b55fbf``:
  ``sys/arch/sgi/dev/dsrtc.c``, ``sys/dev/ic/mk48txxreg.h``,
  ``sys/arch/sgi/pci/ioc.c`` and ``sys/arch/sgi/pci/iof.c``.
- Linux at
  ``db2ddb871435`` (v7.2-rc7): ``drivers/rtc/rtc-m48t35.c``,
  ``drivers/mfd/ioc3.c``, ``arch/mips/include/asm/mach-ip27/mangle-port.h``
  and ``arch/mips/include/asm/io.h``.
- A published photograph of the Origin
  200 IP29 board, :src:`theretroweb-origin200-image-1`.

The drivers are software expectations, not hardware evidence.

.. _sgi-timekeepers-parts:

Parts and placement
===================

.. list-table::
   :header-rows: 1

   * - Machines
     - Behind
     - QEMU type
     - Window
     - Year base
   * - Origin 200, Origin 2000, Onyx2
     - IOC3 byte bus, device select 0
     - ``m48t35``
     - 32 KiB at IOC3 BAR offset ``0x80000``
     - 1970
   * - Origin 300, Fuel
     - IOC3 byte bus, device select 0
     - ``m48t35``
     - 32 KiB at IOC3 BAR offset ``0x80000``
     - 1968
   * - Origin 350, Onyx 350, Tezro, Onyx4 host
     - IOC4 byte bus
     - ``ds1744``
     - 32 KiB at IOC4 BAR0 offset ``0x80000``
     - 1968 (fresh seed)
   * - IO9 models that select the legacy DS1742
     - IOC4 byte bus
     - ``ds1742``
     - 8 clock bytes at IOC4 BAR0 offset ``0x87ff8``
     - 1968 or none

The IOC3 BAR offset is the byte bus's first device select
(``SGI_IOC3_BYTEBUS_DEV0``). OpenBSD attaches a clock at that select on
IOC3 and on IOC4 and reads the clock at window offsets ``0x7ff8`` to
``0x7fff`` (OpenBSD ``sys/arch/sgi/dev/dsrtc.c``),
*software expectation*. Linux's ``rtc-m48t35`` driver does the same on IOC3.
Both guests issue plain byte offsets. Linux declares its register structure
in a transposed order on IP27, but its byte accessor XORs the address with 3
(``__swizzle_addr_b``), which restores the offsets above
(Linux ``arch/mips/include/asm/mach-ip27/mangle-port.h``),
*software expectation*. A model presenting the declared order would hand
Linux a clock with transposed fields.

The photograph of the IP29 board shows a part marked ``M48T35-70PCI``
(:src:`theretroweb-origin200-image-1`), *observed* on that board. The
marking is most likely ``M48T35-70PC1``, the 70 ns PCDIP28 part of the ST
ordering table :src:`st-m48t35-rev-10 p. 25`, *inferred*. No cited source
shows the part on the BaseIO boards. The part on the other boards, the
Origin 300 IO8 and Fuel boards included, their capacity, and their
chip-select and address wiring are *unknown*: OpenBSD prints ``DS1742W`` for
the IP35 clock, but its driver does not probe the part and calls its own
IOC3 mapping a guess. A schematic or an address capture would settle it. The
model therefore fits the same M48T35-compatible 32 KiB window to those
boards, an *approximation* with no aliasing at multiples of ``0x800``. The
same holds for the IO9 board: the DS1744 is a compatibility choice, and the
fitted package and the address wiring behind IOC4 are *unknown*.

.. _sgi-timekeepers-m48t35:

M48T35 and compatible parts
===========================

The part is 32,768 byte cells. The clock occupies the top eight,
``0x7ff8`` to ``0x7fff``, and the cells below it are battery-backed storage
:src:`st-m48t35-rev-10 p. 13`, *established*. OpenBSD's shared header maps
the last sixteen cells as one block and names the eight at ``0x7ff0`` to
``0x7ff7`` ``X0`` to ``X7``, saying only that they exist on some models
(OpenBSD ``sys/dev/ic/mk48txxreg.h``), *software expectation*. No driver
here touches them. Whether they are more than ordinary storage on this part
is *unknown*; the ST map shows only the clock above them, so ordinary
storage is *inferred*. The model treats them as storage. A part-level
measurement or the full ST register map would settle it.

.. _sgi-timekeepers-m48t35-regs:

Register map
------------

Offsets are within the 32 KiB window. Every access is one byte wide.

.. list-table::
   :header-rows: 1

   * - Offset
     - Name
     - Reset
     - Access
     - Notes
   * - ``0x0000``-``0x7fef``
     - Storage
     - kept
     - RW
     - Ordinary cells. A reset does not change them.
   * - ``0x7ff0``-``0x7ff7``
     - Bank X0-X7
     - kept
     - RW
     - Modelled as storage.
   * - ``0x7ff8``
     - Control
     - bits 7, 6 cleared
     - RW
     - Bit 6 (R) freezes the readable clock. Bit 7 (W) arms a write.
       Bits 5-0 are calibration and are stored as written.
   * - ``0x7ff9``
     - Seconds
     - kept
     - RW
     - BCD 0-59. Bit 7 stops the oscillator.
   * - ``0x7ffa``
     - Minutes
     - kept
     - RW
     - BCD 0-59.
   * - ``0x7ffb``
     - Hours
     - kept
     - RW
     - BCD 0-23.
   * - ``0x7ffc``
     - Day of week
     - kept
     - RW
     - Stored, not counted. Reads the stored byte, zero on a fresh image.
   * - ``0x7ffd``
     - Date
     - kept
     - RW
     - BCD 1-31.
   * - ``0x7ffe``
     - Month
     - kept
     - RW
     - BCD 1-12.
   * - ``0x7fff``
     - Year
     - kept
     - RW
     - BCD 0-99, counted from the year base.

The register order and the use of the control bits come from ST's Table 5
and sections 3.1 and 3.2 :src:`st-m48t35-rev-10 p. 12-13`, *established*,
and agree with OpenBSD's ``dsrtc.c`` and Linux's ``rtc-m48t35.c``, *software
expectation*. A battery-backed part has no reset value for the clock; the
model keeps cells and time across a machine reset.

Reading and writing the clock
-----------------------------

To read, a driver sets R, reads the six fields and clears R. The model
latches the running time when R rises and answers every field read from that
one instant until R falls, so six reads cannot straddle a tick. Whether the
real counters keep running under a freeze is not observable through these
drivers. The model keeps counting and snapshots, the reading that cannot
lose time, an *approximation*. Freezing and releasing never moves the clock.

To write, a driver sets W, writes the fields and clears W. The model starts
a staging area from the current time when W rises, so a field the driver
does not write keeps its value. Writes to the six counters land in the
staging area. A day-of-week write is stored at once. Reads while W is set
still return the running clock, not the staged values. When W falls the
staged fields become the time, the clock restarts from them, and the stores
are saved (see :ref:`sgi-timekeepers-m48t35-stores`). The datasheet says
clearing W loads the counters :src:`st-m48t35-rev-10 p. 12`, *established*.

A clock write while W is clear has nowhere to go on the part. The model drops
it and logs ``LOG_GUEST_ERROR``. This also drops a day-of-week write made
with W clear. What a direct write of the seconds register does with W clear,
stop bit included, is *unknown* in the datasheet.

The model does not validate the staged fields. A date past the end of its
month runs on into the next month, and a month outside 1 to 12 carries into
the year, as ``mktime()`` would. The part's response to such writes is not
specified, an *approximation*.

The stop bit, seconds bit 7, halts the oscillator when set. A stopped
oscillator does not count :src:`st-m48t35-rev-10 p. 12`, *established*.
The bit is taken from a write staged under W and applied when W clears. A
write session that never writes seconds leaves it as it was. While stopped,
seconds reads return bit 7 set and the time is held. That the bit reads back
and survives power loss is *inferred*: it is a seconds-register bit, and a
part shipped stopped to save its cell only works if it holds.

.. _sgi-timekeepers-m48t35-calendar:

Year base and calendar
----------------------

The year cell holds two BCD digits and the part has no century digits
:src:`st-m48t35-rev-10 p. 13`, *established*. The cell counts from the year
base. On the IP27 machines OpenBSD adds 1970 to it, so 2026 reads ``0x56``.
OpenBSD uses 1968 on IP35, so 2026 reads ``0x58`` there (OpenBSD
``sys/arch/sgi/dev/dsrtc.c``), *software expectation*. A model presenting
calendar digits would make OpenBSD report the mid-1990s on an IP27 machine,
with nothing else looking wrong. The base is the ``year-base`` property:
1970 by default, 1968 for Origin 300 and Fuel. Any other value is refused at
realize.

The part corrects for months of 28 to 31 days and states its leap-year
correction as valid until 2100 :src:`st-m48t35-rev-10 p. 8`, *established*.
It does not state the rule. That the part treats a year cell divisible by
four as a leap year, so that 2100 is the first failure of the last-two-digits
rule, is *inferred* from that sentence. ST's application note AN923, which
the datasheet cites for century rollover, would settle it, as would a
measurement: set 28 February 23:59:59 in cell years ``0x56`` and ``0x54``
and read the date a second later. The model follows the inferred rule:
February has 29 days when the year cell is a multiple of four. With base
1970 that puts 29 February in 2022, 2026 and 2030 and none in 2024 or 2028.
With base 1968 it matches the Gregorian calendar from 1968 to 2067. The
model does not use the real calendar for the first case, because the part
has no epoch and cannot know that cell 54 is 2024. The IRIX 6.5.30 kernel
keeps a one-byte record of the gap in storage (see below), *software
expectation*, which a calendar following the real dates would contradict.

The cell wraps from 99 to 00 after 100 cell years, in 2070 for base 1970 and
after 2067 for base 1968. The day-of-week cell also holds a century-enable
bit (bit 5) and a century bit (bit 4) :src:`st-m48t35-rev-10 p. 13-14`,
*established*. The model keeps the
cell as plain storage and does not toggle the century bit. The
frequency-test bit, bit 6 of the same cell :src:`st-m48t35-rev-10 p. 14`,
*established*, is also plain storage. No guest here
sets either.

A fresh clock follows the host. It starts at the date of the clock ``-rtc``
selects, converted to the year base. With base 1968 a host date outside
1968-2067 fails realization with ``initial year is outside 1968-2067``.

Time source
-----------

The model reads time from the clock ``-rtc`` selects (``host``, ``rt`` or
``vm``), including the ``base=`` offset. It keeps one offset between that
clock and the part's own calendar, so a guest-set time differences against
the same clock the read path uses.

.. _sgi-timekeepers-m48t35-stores:

Stores
------

Two optional block devices back the part.

``nvram-drive``
  A raw image of exactly 32,768 bytes. The model loads it at realize and
  writes each changed storage byte back as the guest writes it. An unchanged
  byte is not rewritten unless an earlier write failed. A backing write
  failure is reported on stderr and is not a guest error.

``nvram-clock-drive``
  The clock record: 16 bytes or more, holding the magic ``M48TREF1`` and
  the instant of the last save, as 64-bit little-endian seconds of the
  ``-rtc`` clock. It is a model construct. It lets a restart add the time
  that passed while QEMU was not running. A clock record without
  ``nvram-drive`` is refused at realize. An NVRAM drive without a clock
  record is accepted. A drive the block layer reports as
  shorter than 16 bytes is refused. For a raw file that means an empty one;
  a shorter nonempty file grows on the first save.

The IOC3 exposes these as aliases of the child ``rtc``'s ``drive`` and
``clock-drive`` properties on boards that fit the M48T35, and removes them
from boards that do not.

The machines bind the stores through the machine properties named after the
catalogue's storage items. Each store must exist as a block node whose size
is exact for the NVRAM and at least 16 for the clock record. Wrong sizes,
a clock record without its NVRAM, an unknown name and a node shared between
two stores are refused when the machine is built.

.. list-table::
   :header-rows: 1

   * - Machines
     - NVRAM
     - Clock record
     - New store starts as
   * - ``origin200``, ``origin2000``, ``onyx2``
     - ``nvram<N>``, 32,768 bytes
     - ``nvram<N>-clock``, 16 bytes
     - the fresh IP27 image
   * - ``origin300``, ``fuel``
     - ``nvram<N>``, 32,768 bytes
     - ``nvram<N>-clock``, 16 bytes
     - zeroes
   * - ``origin350``, ``onyx350``, ``tezro``, ``onyx4-host``
     - ``nvram<N>``, 32,768 bytes (DS1744)
     - ``nvram<N>-clock``, 16 bytes
     - zeroes

``N`` counts the boards in the topology that carry one: one per Origin 200
node with onboard I/O, one per BaseIO card, one per Origin 300 chassis. The
Octane stores (512 bytes and 24 bytes) belong to :doc:`ds1685`. The older
forms still work and are deprecated: ``-drive if=mtd,index=I`` or
``-drive if=none,id=sgi-nvram<I>`` with the partner ``sgi-nvram<I>-clock``.
Giving a store both ways is refused. ``qemu-sgi-machine-init`` creates the
files with the starting contents the table names.

Starting state
~~~~~~~~~~~~~~

A supplied image is authoritative. The model loads all 32 KiB exactly as
given, an all-zero image included, and installs nothing over it. A machine
reset, a close and reopen, and an incoming migration never provision a
fresh image over existing bytes.

A machine with no NVRAM store installs the fresh IP27 image unless
``initial-ip27-fixture`` is off (it is on by default): zeroes, a one-byte
leap-cycle record at ``0x3883`` and a checksum byte at ``0x04bc``. The
checksum is an 8-bit value seeded with 165. Each byte from ``0x000e`` up to
but not including ``0x7fd1`` is XORed in, except ``0x04bc`` and ``0x046f``,
and the running value rotates left one bit after every position, skipped
ones included. For the all-zero image that gives ``0x2d``. The record is 2
for cell years that are 2 modulo 4 from March on, for cell years that are 3
modulo 4, and for cell years that are 0 modulo 4 in January and February. It
is 1 otherwise. A record of 1 gives checksum ``0x6d`` and a record of 2
gives ``0xad``. The image is a model fixture, an *approximation*: a real
part has no factory contents. Its checksum follows the algorithm the stock
BASEIO PROM checks, and its record follows what the IRIX 6.5.30 kernel
reads, both *software expectation*, stated below. The model's tests pin the
fixture.

What guest software does with these bytes: the IRIX 6.5.30 kernel reads the
record while reading the clock and warns that the clock was lost when it
holds neither 1 nor 2, *software expectation*. The BASEIO PROM checks an
environment revision and the checksum, and on a mismatch prints ``NVRAM
checksum is incorrect: reinitializing.`` and writes its own defaults,
*software expectation*. A new
image has a valid record and a blank revision, so a new store shows that
PROM message once. The model does not construct PROM defaults and does not
repair either check. Origin 300, Fuel and the DS1744 machines get a zero
image: their guests own every byte.

Persistence
~~~~~~~~~~~

With an NVRAM store the model saves a complete image and the clock record at
three points: when a guest clock write completes, when the virtual machine
stops (a pause or a quit), and at unrealize. It writes the
running calendar into cells ``0x7ff9`` to ``0x7fff`` in the part's own
format, stop bit included, and leaves storage and the day of week as the
guest wrote them. The freeze and the staging area are not saved. An image read
alone therefore shows the time it was saved at, as a part pulled from a
powered-off board would. Saving does nothing for a store that block
migration has made inactive, and it is retried after an earlier backing
failure.

At realize the model restores the clock from the saved cells.

- Cells that are all zero, or not valid BCD in range, count as never saved,
  and the clock follows the host. - A saved seconds cell with bit 7 set
  restores a stopped clock at the saved instant, with no time added. - A
  saved calendar with a clock record advances by the host time since the
  record's instant, counted in the part's calendar so it crosses February as
  the part would. - A saved calendar with no record or a record without the
  magic is restored as saved, with no time added, and logs ``LOG_UNIMP``. It
  does not fall back to the host time. An old record against a later date
  would break the pairing with the leap-cycle record in storage.

Reset and migration
-------------------

A machine reset of the IOC3 reaches the part. Storage and time survive. R
and W are cleared and any frozen read or staged write is dropped, so the next
guest does not inherit a frozen clock. The datasheet does not say whether the
bits survive power loss, so this is a model policy.

Migration state is version 3 of ``m48t35``: the whole 32 KiB image, the
time offset, the freeze, staging and stop state. Migration works only between
identical builds. An incoming migration or snapshot replaces all 32 KiB
before any guest access, and no post-load step installs the fresh image or
touches the bytes. The parent IOC3 rebuilds its own interrupt output
separately. A source whose stores have gone inactive after migration writes
nothing at shutdown.

Origin 300 and Fuel
-------------------

The same model, with ``year-base`` 1968, ``initial-ip27-fixture`` off and a
zero starting image. The Fuel system board's physical part, capacity and
decode are *unknown*, so the 32 KiB window is the compatibility choice made
under :ref:`sgi-timekeepers-parts`. The Fuel L1 controller's own storage is
a separate part and is not this device (:doc:`l1`).

A requested start date outside 1968-2067 fails for these machines. The
fresh-image record and checksum are not installed.

.. _sgi-timekeepers-ds1742:

DS1742 and DS1744
=================

The DS1742 is a 2 KiB part and the DS1744 a 32 KiB one, with the same
clock. The DS1742W is the 3.3 V DS1742. The last eight cells are the clock;
the rest is SRAM :src:`analog-ds1742-rev-6-13 p. 1-5`,
:src:`analog-ds1744-rev-6-13 p. 3-6`, *established*. Neither part has an
interrupt, DMA or reset pin.

``ds1742`` and ``ds1744`` are the same code with two geometries. Both can be
created with ``-device``. They take ``drive``, ``clock-drive`` and
``fresh-year-base``. On the IP53 products the machine creates a ``ds1744``
and links its region to the IOC4 (:ref:`sgi-ip53-ds1744`). The IOC4 maps a
32 KiB region whole at BAR0 offset ``0x80000``, and a 2 KiB region by its
eight clock bytes only, at ``0x87ff8`` (native offsets ``0x7f8`` to
``0x7ff``). A clock region of any other size fails IOC4 realize. The
legacy 2 KiB selection leaves the rest of the SRAM unreachable
(:ref:`sgi-ip53-ds1742`).

Register map
------------

Offsets are native to the part. The 2 KiB part's control byte is at
``0x7f8``; the 32 KiB part's is at ``0x7ff8``. The rest follow in order.

.. list-table::
   :header-rows: 1

   * - Offset from control
     - Name
     - Access
     - Notes
   * - ``+0``
     - Control and century
     - RW
     - Bit 7 (W) arms a write. Bit 6 (R) freezes the readable clock. Bits
       5-0 are BCD century, 00-39.
   * - ``+1``
     - Seconds
     - RW
     - BCD 0-59. Bit 7 (OSC) stops the oscillator.
   * - ``+2``
     - Minutes
     - RW
     - BCD 0-59.
   * - ``+3``
     - Hours
     - RW
     - BCD 0-23.
   * - ``+4``
     - Day
     - RW
     - Bits 2-0 are the day, 1-7. Bit 7 (BF) is the battery flag, read-only.
       Bit 6 (FT) enables the frequency test.
   * - ``+5``
     - Date
     - RW
     - BCD 1-31.
   * - ``+6``
     - Month
     - RW
     - BCD 1-12.
   * - ``+7``
     - Year
     - RW
     - BCD 0-99.

The specification requires unused bits to be written as zero
:src:`analog-ds1744-rev-6-13 p. 6`, *established*.

Clock transactions
------------------

The sequences match the M48T35's. Setting R or W leaves the century bits as
they were. Clearing W may change them. Clearing R alone does not
:src:`analog-ds1744-rev-6-13 p. 4-5`, *established*.

R freezes the readable image while the clock runs on. The model latches the
image when R rises. After R falls the readable image tracks the running
clock again. The datasheet says the image catches up within a second
:src:`analog-ds1744-rev-6-13 p. 4`, *established*. The model catches up at
once, an *approximation*.

W makes the model start from the current image. Reads of the clock while W
is set return the staged values, unlike the M48T35. Clearing W commits them.
Each field is validated. A non-BCD value or one out of range for its field
(century above 39, day outside 1-7) is rejected with ``LOG_GUEST_ERROR``. A
set of valid fields that is not a date (30 February, 29 February in a common
year) is rejected with ``LOG_UNIMP``. The specification gives the field ranges
and leap correction but does not define the response to an impossible date,
so the rejection is an *approximation*, not a measured response. Either
way W clears, the previous time and century stay, and nothing is saved.
Writing a clock register with W clear logs ``LOG_GUEST_ERROR`` and is
dropped.

The model's calendar is Gregorian. The year is century and year digits
combined. A day register carries the day of the week and advances with the
date; the model keeps the written value and counts forward from it. BF reads
as one, so the model reports a healthy battery and never simulates a flat one.
The part toggles seconds bit 0 at 512 Hz with FT set and the oscillator
running :src:`analog-ds1744-rev-6-13 p. 6`, *established*. The model does so
in virtual time. The phase is unmeasured, an *approximation*.

Time source and fresh state
---------------------------

The clock counts virtual time. A fresh instance (no image, or an image whose
seconds, minutes, hours, date, month and year cells are all zero) starts at
the date of the clock ``-rtc`` selects, with the day of week from that date.
Without an image the SRAM starts at zero. With one, the SRAM is the image's.
The specification gives no factory contents :src:`analog-ds1744-rev-6-13 p.
3`, so this is a model policy.

``fresh-year-base`` is ``0`` or ``1968``. With 1968 the fresh seed maps the
host year ``Y`` to ``2000 + (Y - 1968) mod 100``, so 2026 is seeded as 2058
and the part's leap cycle lines up with real leap years from 1968 to 2067.
This matches OpenBSD's IP35 convention, which counts the year from 1968 and
ignores the century (OpenBSD ``sys/arch/sgi/dev/dsrtc.c``), *software
expectation*. A fresh seed outside the range, or any other property value,
fails realize. Only a fresh or blank seed is shifted. A valid saved calendar
is used as saved, and guest writes use the plain Gregorian model.

Stores
------

``drive`` must be exactly 2048 bytes for ``ds1742`` and 32,768 for
``ds1744``. A mismatch is refused, so an old 2 KiB image is never resized
into a ``ds1744``. SRAM writes go through to the drive byte by byte, and an
unchanged byte is not rewritten unless an earlier write failed.

``clock-drive`` is a record the block layer must report as at least 16
bytes (for a raw file, anything but an empty one), the magic ``DS1742R1``
followed by the host's wall-clock seconds in little-endian 64-bit. It
requires ``drive``. It uses the wall clock, not ``-rtc``.

The machines bind the IP53 DS1744 as ``nvram<N>`` and ``nvram<N>-clock``,
both required, 32,768 and 16 bytes (:ref:`sgi-timekeepers-m48t35-stores`).
The legacy IO9 selection uses ``ds1742-<board>-data`` (2048 bytes) and
``ds1742-<board>-clock``, or the deprecated ``-drive`` ids
``sgi-ds1742-data-<board>`` and ``sgi-ds1742-clock-<board>``, both needed.
The machine never builds, repairs or checksums the SRAM contents: firmware
owns them.

The saved image is the SRAM followed by the eight clock bytes in the part's
format. The model saves it when a valid W commit completes, when the virtual
machine stops, at shutdown and at unrealize, and not when the drive is
inactive. If R or W is set at the time of a save, the model logs
``LOG_UNIMP``: the transaction is not restored. At realize a nonblank image
is restored through the same validation as a guest write. An invalid saved
calendar keeps the seed and logs ``LOG_UNIMP``. A valid one adds the wall
time since the record and advances the day of week by the whole days elapsed.
A stopped clock, or one with no record, gets no time added; the second case
logs ``LOG_UNIMP``.

Reset and migration
-------------------

The package has no reset input, so a machine reset leaves SRAM, calendar,
the control byte, a staged write and a frozen image alone
:src:`analog-ds1742-rev-6-13 p. 1-2`, *established*. The model has no reset
handler. Whether
an unfinished transaction survives a supply transition is not specified.

Migration carries SRAM, the control and day registers, the staged and frozen
images, the stop state and the instant of the save. The destination restarts
counting from that instant. ``ds1742`` and ``ds1744`` have separate state
names, and each refuses the other's stream. Migration works only between
identical builds.

.. _sgi-timekeepers-measurements:

Measurements
============

No reads of these parts have been taken on a physical machine. The register
contracts rest on the manufacturer datasheets and the public drivers. The
photograph of the IP29 board is the only observation of a fitted part.

.. _sgi-timekeepers-limits:

Model limits
============

- The M48T35 calendar rule (leap when the year cell is a multiple of four)
  is *inferred*. The DS1742 and DS1744 use the Gregorian rule.
- Battery state, oscillator-fail reporting and write protection on supply
  loss are not modelled. On the M48T35, calibration, the frequency-test bit
  and the century bits are stored as plain bytes.
- The M48T35 day of week is storage, never counted.
- The M48T35's block at ``0x7ff0`` to ``0x7ff7`` is storage. No alarm,
  watchdog or interrupt exists.
- Neither model raises an interrupt or does DMA. A guest cannot arm an
  alarm.
- The clock records are model constructs, not part of any chip. The
  M48T35 and DS1742 records use different magic values and clocks.
- The M48T35 region is declared one byte wide. A wider access reaching it
  through the address space, as the SGI PIO forwarding path does, is split
  into byte accesses and not refused; no test pins that. The DS1742 region
  refuses any other size, and an IOC4 test pins that.
- The M48T35 fresh image is a model fixture; a real part has no factory
  contents. Base 1968 with the fresh image on is refused at
  realize, because the leap-cycle record is defined for base 1970.
- Physical part, capacity and wiring of the Origin 300, Fuel and IP53
  timekeepers are not established. The model maps a flat 32 KiB window.
- The DS1742 rejects an impossible date on a W release; the M48T35 does not
  validate the fields it stages.
- A guest write to a clock register with W clear is dropped with
  ``LOG_GUEST_ERROR`` in both models.

.. _sgi-timekeepers-tests:

Tests
=====

M48T35, qtests:

- ``tests/qtest/sn0-ioc3-test.c`` pins the clock on an Origin 200: the
  time it is given and the 1970 year cell, the date-derived fresh image,
  the freeze, the year wrapping below its base, the control register reading
  back, a guest-set time, storage surviving a system reset and persisting
  with a drive, February following the part's rule, century and month
  carry, an empty or missing clock record, restore after a restart with the
  record and the fallbacks, the stop bit written by a guest and held across a
  save, backing writes that are no-ops or retried, storage that holds a write,
  migration with a backed clock (running and stopped), and the ``year-base``,
  start-range and supplied-storage cases for base 1968.
- ``tests/qtest/sn0-baseio-test.c`` pins storage across a reset on an Origin
  2000 BaseIO, the leap record and checksum bytes written from several
  processors, and reopening a backed image.
- ``tests/qtest/sn0-migration-test.c`` and
  ``tests/qtest/sn0-snapshot-test.c`` carry the full 32 KiB image and the
  clock across migration and ``savevm``.
- ``tests/qtest/sgi-migration-roundtrip-test.c`` round-trips the device's
  state.
- ``tests/qtest/sn1-address-map-test.c`` pins the IO8 byte bus on the
  Origin 300 (year ``0x58``, storage, reset) and a supplied image.
- ``tests/qtest/sn1-fuel-test.c`` pins the Fuel window (no aliasing at
  ``0x800``), the 1968 year cell, 28 February 2024 into 29 February and
  2026 into 1 March, the roll from 2067 to year ``00``, migration of a
  running and a stopped clock, and elapsed-time restore across the roll.
- ``tests/qtest/sgi-machine-storage-test.c`` pins the ``nvram<N>`` and
  ``nvram<N>-clock`` bindings, their legacy forms and the refusals.

DS1742 and DS1744:

- ``tests/qtest/ds1742-test.c`` covers the calendar, invalid dates, reset
  with W and with R set, the stop and test modes, the fresh seed and its
  range, saved-seed fallbacks, migration (including refusing the other
  part's stream), backed migration with source quit, destination
  persistence of running and stopped clocks, restart persistence, backing
  retries, and the 32 KiB geometry.
- ``tests/qtest/sgi-ioc4-test.c`` covers the clock alias, the 32 KiB window
  (``nvram-window``), the refused region size, wide-access rejection, reset
  and migration of the clock behind IOC4.
- ``tests/qtest/sn-builder-test.c`` checks that the IP53 products build a
  ``ds1744`` linked to IOC4 and bind its stores, and does the same for the
  legacy ``ds1742``.
- ``tests/qtest/sgi-machine-storage-test.c`` checks the DS1744 binding.
