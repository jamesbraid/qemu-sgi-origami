.. SPDX-License-Identifier: BSD-3-Clause

.. _sgi-scsi-cdrom:

==================================================
SCSI CD-ROM drive: block length and identity
==================================================

Used in :ref:`sgi-ip27`, :ref:`sgi-ip30`, :ref:`sgi-ip32` and
:ref:`sgi-ip35`: every SGI machine model that can carry a SCSI CD-ROM
gives QEMU's ``scsi-cd`` the behaviour on this page.

.. seealso::

   Platform pages: :ref:`sgi-ip27`, :ref:`sgi-ip30`, :ref:`sgi-ip32`,
   :ref:`sgi-ip35`.
   Host adapters: :doc:`qlogic-isp`, :doc:`qlogic-isp2200`,
   :doc:`aic7880`.

The CD-ROM drives SGI sold were Toshiba drives with SGI firmware. The
model has no drive of its own: it is ``scsi-cd`` from
``hw/scsi/scsi-disk.c`` with properties set by
``hw/mips/sgi/board-cdrom.c``, and a helper the SGI host adapters call,
``hw/scsi/sgi-scsi-cd.c``. This page owns the drive's contract: the
logical block length it powers on at and reports, what MODE SELECT does
to it, what a reset does, and the INQUIRY text. Facts are rated as
described in :ref:`sgi-ratings`.

.. _sgi-scsi-cdrom-sources:

Sources
=======

- Public newsgroup posts about the drives SGI shipped: by an SGI engineer
  (:src:`usenet-3msttn`), by a customer reporting Toshiba support's
  answer (:src:`usenet-3mu7ea`), by an owner of an Indigo2 drive
  (:src:`usenet-4eofac`), by a reseller (:src:`usenet-3a4eb9bb`) and by a
  user who bought drives from SGI (:src:`usenet-g6j8y1`), and the SGI
  hardware FAQ, which quotes the SGI engineer on what the boot
  :term:`PROM` needs (:src:`sgi-hardware-faq-2000`). No Toshiba SCSI
  reference manual for the XM series is held.
- Published boots of real SGI machines with Toshiba drives: an
  Indy's XM-5401TA under Linux (:src:`miod-openbsd-sgi-story`), and an
  Origin 200's XM-6201TA (:src:`dmesgd-6924`) and XM-5701TA
  (:src:`dmesgd-7106`) under OpenBSD.
- The stock PROMs, read from the shipped images as software expectations:
  :src:`ip30-prom-4.17`, :src:`io6-prom-6.156`, :src:`ip35-prom-6.210`
  (its nested base-I/O image) and :src:`ip32-prom-4.18`. The Indy and
  Indigo2 PROM is not examined.
- The IRIX 6.5.30 IP30 kernel, :src:`ip30-kernel-6.5.30`, as a software
  expectation, and IRIX 6.5.30 run on the model with the drive at each
  power-on length.
- Linux ``drivers/scsi/sr.c`` and ``sr_vendor.c`` at ``db2ddb871435``,
  and OpenBSD ``sys/scsi/cd.c`` at ``2df91d80c104`` (the
  ``sgi-is-alive-at-7.9`` branch), as software expectations, and the
  OpenBSD 6.9 ``install69.iso`` for sgi run on the model.

.. _sgi-scsi-cdrom-block-length:

Block length
============

What the drives SGI sold did
----------------------------

An SGI install CD carries an SGI volume header and an EFS filesystem
laid out in 512-byte blocks. The drives SGI sold for the Indy and
Indigo2 powered on at 512-byte logical blocks:

- the SGI engineer answered whether the Toshiba XM-3501B "powers up with
  512 byte blocks" with "the one we sell does", set by its firmware rather
  than a jumper (:src:`usenet-3msttn`);
- Toshiba support told a customer that SGI's XM-3501 firmware differs from
  the PC version in its block size and identifies the drive "like a hard
  drive" (:src:`usenet-3mu7ea`);
- an Indigo2's XM-3501TA reported itself as a disk at 512-byte blocks, and
  returned to that after every SCSI bus reset (:src:`usenet-4eofac`).

So the XM-3501 SGI sold powered on, and came back from a bus reset, at
512 as a type-0 unit: *inferred* from these three reports, one of them
SGI's own. The Toshiba XM-3401 defaults to 2048 unless pads on its board
select 512 or an SGI setting, whose length is not stated
(:src:`sgi-hardware-faq-2000`), *inferred*.

Later Toshiba drives found in SGI machines answered as CD-ROMs: Linux
reports an Indy's XM-5401TA as type CD-ROM
(:src:`miod-openbsd-sgi-story lines 2161-2162`), *observed*, and
OpenBSD's CD-ROM driver attaches an XM-6201TA (:src:`dmesgd-6924 line
197`) and an XM-5701TA (:src:`dmesgd-7106 line 198`) in Origin 200s,
*observed*, which it does only for a CD-ROM or write-once device type,
so type 5 is *inferred* for those two. Whether each came from SGI is not
stated; that the ``TA`` models are SGI's own is *inferred* from the
XM-3501TA that SGI fitted to the Indigo2. A 2001 statement that drives
sold by SGI answer INQUIRY as a disk (:src:`usenet-g6j8y1`) does not hold
for these. What length they power on at is not stated in any source held:
*unknown*. A captured MODE SENSE or READ CAPACITY from one, taken before
any MODE SELECT, would settle it.

The drive reports its current length in an eight-byte block descriptor
in its MODE SENSE reply, and takes a new length from one in a MODE
SELECT parameter list: the SGI engineer gave both as what the later SGI
PROMs need of a third-party drive (:src:`sgi-hardware-faq-2000`), a
*software expectation*. That a length set by MODE SELECT lasts until a
reset, which returns the power-on length, is *inferred* from the Indigo2
report and from the length being a mode parameter. What a drive does with
a length other than 512 or 2048, with a PF=0 or SP=1 MODE SELECT, and with
a medium change is *unknown*; the drive's SCSI reference manual, or an
identified capture from a drive, would settle each.

What the PROMs do
-----------------

Every examined PROM reads a CD in 512-byte blocks and sets that length
itself when the drive reports another (*software expectation*):

.. list-table::
   :header-rows: 1
   :widths: 30 70

   * - PROM
     - What it does with the block length
   * - Octane, Octane2 (:src:`ip30-prom-4.17 0x1fc747e4`,
       :src:`ip30-prom-4.17 0x1fc7499c`)
     - MODE SENSE(6) for all pages. A block descriptor with a non-zero
       length gives the drive's length; failing that, the PROM looks for
       a length in mode page 3, and with neither it assumes 512.
       At open it wants 512 (or the sector size the volume header names)
       and sends MODE SELECT(6) ``15 10 00 00 0c 00`` with that length
       only when the drive reports another, then checks with MODE SENSE.
       After a failing command it does the same again for a type-5 unit.
   * - Origin 200, Origin 2000, Onyx2 (:src:`io6-prom-6.156 0xc000000011c24d58`)
     - The base-I/O PROM has the Octane's logic.
   * - Origin 300, Origin 3000 (:src:`ip35-prom-6.210 0xc000000013051cd4`)
     - The base-I/O PROM nested in the node PROM has the Octane's logic.
       The address is in the unpacked nested image.
   * - O2 (:src:`ip32-prom-4.18 0x8101ea68`)
     - MODE SELECT(6) for 512 to every type-5 unit, with no MODE SENSE
       first, sent again once on CHECK CONDITION.

So every PROM held reads a drive powered on at 512 without a select,
except the O2's, which selects the length the drive already has, and
reads a drive powered on at 2048 after its select. A drive that reports
no block descriptor and stays at 2048 is read in the wrong unit by all
but the O2's. The Indy and Indigo2 PROM is not examined: the SGI engineer
says it also sets 512 when the descriptor reports another length
(:src:`sgi-hardware-faq-2000`), and that older machines need a drive that
is already at 512, a *software expectation*.

What IRIX does
--------------

IRIX's SCSI disk driver reads a removable unit's length from MODE SENSE
when it opens it, takes the sector size from the volume header when that
is set, and sends the length it holds back with MODE SELECT from its
write and command-retry paths (:src:`ip30-kernel-6.5.30 0x200925bc`,
:src:`ip30-kernel-6.5.30 0x2008f4a8`,
:src:`ip30-kernel-6.5.30 0x20092b20`), a *software expectation*. On the
model, IRIX 6.5.30's removable-media daemon sends MODE SELECT for 512 to
each CD-ROM at boot, reads, and sends MODE SELECT back to the length it
found; with the daemon disabled no MODE SELECT reaches the drive.

On the model, with the drive powered on at 2048 or at 512, IRIX reads an
install CD, an SGI-labelled CD and an ISO 9660 disc whole through the raw
volume device, mounts EFS and ISO 9660 discs, runs inst from a mounted CD
and installs from a CD through the Origin 200 and Origin 300 PROMs. The
Octane2 PROM's Install System Software loads sash64 from the CD and then
panics in it with a read TLB miss, at either power-on length and with the
IRIX 6.5.22 and 6.5.30 CDs, on a machine with no graphics head; the
cause is not known and the block length does not change it. One case
differs. A disc with
neither an SGI volume header nor a filesystem IRIX knows, read through
the raw volume device with the drive at 2048 and the daemon running,
returns only its first quarter: IRIX sizes it in 2048-byte sectors at
open and counts that size in 512-byte blocks after the daemon's
switches. With the daemon disabled, or the drive at 512, it reads whole.
These are runs on the model; that IRIX does the same with a real drive
at 2048 is *inferred* from the daemon and the driver above acting the
same way on any drive that takes the select.

What Linux and OpenBSD do
-------------------------

Linux accepts a drive at 512 from READ CAPACITY and switches it to 2048
with MODE SELECT when it checks the disc's sessions (``sr_cd_check`` in
``sr_vendor.c``), a *software expectation*. It works with either
power-on length.

OpenBSD sends no MODE SELECT to a CD-ROM: it takes the sector size from
READ CAPACITY (``cd_get_parms`` in ``sys/scsi/cd.c``), a *software
expectation*. Its sgi install CD carries a disklabel counted in 2048-byte
sectors. On the model, after the Octane2 or Origin 200 PROM has loaded
the installer from the CD and selected 512, the installer's bus reset
returns the drive to its power-on length. At 2048 the installer mounts
the CD, installs from it and the installed system boots; at 512 the
label no longer fits the device and mounting the CD fails with "Invalid
argument". So OpenBSD needs a drive that comes back from a bus reset at
2048, a *software expectation*.

The model's drive
-----------------

- It powers on at 2048-byte logical blocks, and READ CAPACITY counts in
  that length. That is an *approximation*: the later SGI drives' power-on
  length is *unknown*, and 2048 is the length OpenBSD's install media
  need while every PROM and IRIX work with it.
- MODE SENSE returns an eight-byte block descriptor with the current
  length, and none when DBD is set.
- MODE SELECT(6) or MODE SELECT(10) with a block descriptor sets the
  length, with PF set or clear, and MODE SENSE, READ CAPACITY and READ(10)
  follow it at once.
- A bus, target, adapter or system reset returns the drive to its
  power-on length and raises the reset unit attention.

A ``scsi-cd`` attached with ``-device`` gets the same properties as one
from ``-cdrom``. Given its own ``physical_block_size`` it keeps that
power-on length: ``physical_block_size=512`` gives the length the drive
SGI sold for the Indy and Indigo2 powered on at (*inferred*, above), with
which IRIX reads every disc whole and the
OpenBSD installer cannot mount its CD. Where the drive goes matters to the
PROM: the Octane PROM probes only targets 1 to 3 on its first bus,
``scsi.0``, so a CD there at target 4 is not found
(:ref:`sgi-ip30-launch`).

.. _sgi-cdrom-identity:

CD-ROM drive identity
=====================

This section covers every SGI machine model that boots from a SCSI
CD-ROM. A CD-ROM the model attaches reports the vendor and product text
its ``scsi-cd`` device carries; what the guests do with that text
decides what to choose.

The drive must report peripheral type 5. The stock PROMs classify a unit
by type first, and a type-0 unit is a disk to them, apart from the three
names in the table below. The IRIX kernel presents a type-0 unit as a
CD-ROM only if its text names one (*software expectation*).

No stock PROM examined reads the vendor of a type-5 unit (*software
expectation*, read from the shipped firmware):

.. list-table::
   :header-rows: 1
   :widths: 24 76

   * - PROM
     - What it does with the INQUIRY text
   * - Octane, Octane2 (:src:`ip30-prom-4.17 0x1fc439e4`)
     - Only for a type-0 unit: if its text starts ``TOSHIBA CD``,
       ``SONY    CD`` or ``LMS     CD`` the PROM sends REQUEST SENSE, a
       vendor command ``0xc9`` and INQUIRY again. Any other type-0 text
       is a disk. A type-5 unit takes neither path.
   * - Origin 200, Origin 2000, Onyx2 (:src:`io6-prom-6.156 0xc000000011c3cd38`)
     - The base-I/O PROM has the Octane's logic and the same three names;
       the node-board PROM probes no SCSI targets.
   * - Origin 300, Origin 3000 (:src:`ip35-prom-6.210 0xc000000013098f18`)
     - The base-I/O PROM nested in the node PROM (the address is in the
       unpacked nested image) has the same logic. The
       base-I/O PROMs of the Origin 350, Tezro and Fuel are not examined:
       *unknown*.
   * - O2 (:src:`ip32-prom-4.18 0x8101ea68`)
     - None. It holds no vendor string and tests only the type.

The Indy and Indigo2 PROM is not examined: *unknown*.

IRIX 6.5.30 and 6.5.22 do not need a vendor either, with one cost
(*software expectation*, read from the installed kernels and the media
daemon of 6.5.30; no installed 6.5.22 media daemon was examined):

- The kernel treats a text as a CD-ROM if it contains ``CDROM``,
  ``CD-ROM``, ``CD ROM`` or ``DVD-ROM``. The test matters only for a
  unit that reports type 0. It matches no CD-ROM brand, but a separate
  lookup compares the vendor against ``TEAC``, ``INSITE``,
  ``IOMEGA  Io20S`` and ``SyQuest``; what that changes is *unknown*.
- The removable-media daemon's CD-ROM module matches the text against
  eight brand prefixes, among them ``TOSHIBA CD``, ``SONY    CD`` and
  ``MATSHITACD``. Toshiba texts select vendor opcodes ``0xc4`` and
  ``0xc9``; an unlisted text gets the standard commands. QEMU's
  ``scsi-cd`` implements neither vendor opcode and answers them with
  ILLEGAL REQUEST, so a Toshiba text sends the daemon down paths the
  model cannot serve, while an unlisted text containing ``CD-ROM`` does
  not. That the daemon's software eject then fails was not run:
  *inferred*.

So a ``scsi-cd`` drive on these machines may keep QEMU's default text or
carry the real inventory string ``TOSHIBA CD-ROM XM-5701TA``; the
PROMs examined do not tell them apart. Prefer a text that is not a
CD-ROM brand the daemon knows or a vendor the kernel looks up, unless
the model answers that brand's vendor opcodes. What ``0xc4`` and ``0xc9``
do to a drive is *unknown*. :ref:`sgi-ip32-storage` and
:ref:`sgi-sn-launch-disk-bus` add the machine-specific limits.

Model limits
============

- The XM-3501 SGI sold reported peripheral type 0 until a command from
  SGI's ``scsicontrol`` put it in CD-ROM mode (:src:`usenet-4eofac`),
  *inferred*, and the Octane and Origin PROMs and IRIX handle that case.
  The model reports type 5, as the later drives did, and does not
  implement the vendor command.
- A MODE SELECT length other than 512 or 2048 is taken or ignored by
  ``scsi-cd`` depending on the value, rather than refused, an
  *approximation*; what a drive does is *unknown*.
- ``scsi-disk.c`` applies the block descriptor before it checks the mode
  pages after it, so a MODE SELECT it refuses can still change the
  length, an *approximation*: a drive refusing a MODE SELECT is expected
  to leave its parameters unchanged, *inferred* from the SCSI rules for
  mode parameters.
- PF=0 and SP=1 MODE SELECT headers are accepted, with nothing saved for
  SP=1, through ``scsi-cd``'s ``quirk_mode_page_vendor_specific_apple``, an
  *approximation*. On the model an IRIX 6.5.30 guest on the Octane2 sent
  the CD's descriptor back with PF clear; what sent it is not identified.
- A length set by MODE SELECT is not migrated, as in upstream ``scsi-cd``,
  so a restored guest finds the power-on length until it selects again.
- ``scsi-cd`` keeps the last LBA in the old length after a MODE SELECT
  and keeps the selected length across resets. The SGI adapters correct
  both after the command or reset, using only ``SCSIDevice`` fields
  (``hw/scsi/sgi-scsi-cd.c``). A passthrough drive is not touched.

Tests
=====

``tests/qtest/sgi-scsi-cd-test.h`` holds the register-level contract:
power-on at 2048 in MODE SENSE (with and without DBD) and READ CAPACITY,
the last 2048-byte block read and the one past it refused; MODE SELECT to
512, to 512 again and back to 2048, each followed, before any READ
CAPACITY, by a read of the last block and a refused read past it in the
new length, then MODE SENSE and READ CAPACITY; and each reset from 512
returning the drive to 2048 with a unit attention. It runs through the
QLogic ISP1040 (``tests/qtest/sn0-qlogic-isp-test.c``, which adds a
system reset and a drive with its own 512-byte power-on length), the
ISP12160 (``tests/qtest/sn1-io8-scsi-test.c``), the ISP2200 loop
(``tests/qtest/qlogic-isp2200-test.c``) and the O2's AIC-7880
(``tests/qtest/sgi-o2-test.c``). ``tests/qtest/sgi-cdrom-test.c`` checks
that every SGI machine class lists the properties and that a CD made with
``-cdrom`` or ``-device scsi-cd`` has them.

``tests/functional/mips64/test_sgi_cdrom.py`` checks the software: the
Origin 200, Octane2 and Origin 300 PROMs each boot sash from an IRIX
install CD and list a directory deep in its EFS partition, and the
Octane2 PROM loads fx from the IRIX 6.5.30 overlay CD; IRIX 6.5.30 on the
Origin 200 reads an SGI-labelled CD and the install CD whole and matches
their checksums, finds the install CD mounted and reads a distribution
from it with inst; the Origin 200 PROM loads the miniroot from the CD and
inst in it reads a distribution; and the OpenBSD 6.9 installer, booted
from its CD on the Octane2, mounts the CD and checks its base set. The
O2's PROM is not run there: a fresh O2 PROM uses its graphics console
until the serial console is chosen on it.
