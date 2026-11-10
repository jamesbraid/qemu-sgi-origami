.. SPDX-License-Identifier: BSD-3-Clause

.. _sgi-scsi-hd-inquiry:

==============================================================
scsi-hd-inquiry: SCSI disk with configurable INQUIRY data
==============================================================

Used in :ref:`sgi-ip27`, :ref:`sgi-ip30`, :ref:`sgi-ip32`, :ref:`sgi-ip35` and
:ref:`sgi-ip53`, and in the Origin 3000 I-brick, wherever a SCSI bus takes a
disk: it is a generic SCSI target, not part of any machine, and every
SGI host adapter model takes it as it takes ``scsi-hd``.

.. seealso::

   Platform pages: :ref:`sgi-ip27`, :ref:`sgi-ip30`, :ref:`sgi-ip32`,
   :ref:`sgi-ip35`, :ref:`sgi-ip53`.
   Host adapters: :doc:`qlogic-isp`, :doc:`qlogic-isp2200`,
   :doc:`aic7880`.

``scsi-hd-inquiry`` is ``scsi-hd`` with one difference: the standard
INQUIRY data it returns is configurable. It is a configuration feature of
the model. It does not model a particular drive, and it carries no
vendor's identity of its own: every value comes from the command line.
The type is in ``hw/mips/sgi/scsi-hd-inquiry.c``. Facts are rated as
described in :ref:`sgi-ratings`.

.. _sgi-scsi-hd-inquiry-sources:

Sources
=======

- *SCSI Primary Commands - 3* (SPC-3), section 6.4.2 "Standard INQUIRY
  data" and its standard INQUIRY data format table:
  :src:`t10-spc3-08-309r1 p. 158`. The page number is that of the T10
  ISO-edited final draft and has not been checked against the published
  standard.
- QEMU ``hw/scsi/scsi-disk.c`` at the revision this branch is based on,
  for what ``scsi-hd`` returns today.

Why it exists
=============

Some software reads a disk's identity and serial number from the
vendor-specific part of the standard INQUIRY data, bytes 36 to 55, and not
from the unit serial number VPD page. ``scsi-hd`` returns zeros there and
has no property to change them. The standard INQUIRY data layout is
*established* by SPC-3: byte 0 holds the peripheral qualifier (bits 7 to 5)
and the peripheral device type (bits 4 to 0); bytes 8 to 15 the T10 vendor
identification; bytes 16 to 31 the product identification; bytes 32 to 35
the product revision level; and bytes 36 to 55 are vendor specific
(:src:`t10-spc3-08-309r1 p. 158`).

Properties
==========

The type has every ``scsi-hd`` property. Four of them already set parts of
the INQUIRY data, and the type adds three.

.. list-table::
   :header-rows: 1
   :widths: 28 14 58

   * - Property
     - Bytes
     - Meaning
   * - ``vendor``
     - 8 to 15
     - Vendor identification, at most 8 bytes, left aligned, padded with
       spaces. Default ``QEMU``.
   * - ``product``
     - 16 to 31
     - Product identification, at most 16 bytes, padded with spaces.
       Default ``QEMU HARDDISK``.
   * - ``ver``
     - 32 to 35
     - Product revision level, at most 4 bytes, padded with spaces.
   * - ``serial``
     - none
     - Unit serial number VPD page (``0x80``) and the device identification
       page. Not written to the standard INQUIRY data.
   * - ``inquiry-vendor-specific``
     - 36 to 55
     - A string. Up to 20 printable ASCII characters (0x20 to 0x7e), left aligned and padded with
       spaces; or ``hex:`` followed by up to 20 bytes as hex digit pairs,
       padded with ``0x00``. Empty or unset leaves the bytes at zero.
   * - ``inquiry-peripheral-qualifier``
     - 0, bits 7:5
     - 0 to 7. Default 0.
   * - ``inquiry-peripheral-type``
     - 0, bits 4:0
     - 0 to 0x1f. Default 0, direct access block device.

A ``vendor``, ``product`` or ``ver`` longer than its field is refused when
the device is realized. ``scsi-hd`` cuts it short without a message. A
value outside the range of the qualifier or the type, a ``hex:`` string of
odd length or with a character that is not a hex digit, and a string
longer than 20 bytes, or with a byte outside printable ASCII in text mode,
are refused when set, as is any attempt to set the
properties of a realized device.

SPC pads an ASCII data field with spaces, so the revision level is padded
with spaces here, where ``scsi-hd`` pads it with NUL bytes. The
vendor-specific area has no SPC padding rule; the type pads text with
spaces and an explicit byte string with zeros, so a caller wanting
zero-padded text writes it as ``hex:``.

What stays as in scsi-hd
========================

Everything else, including which commands are answered. A standard
INQUIRY is one with EVPD clear and page code 0, and the type changes the
data only of those that succeed. The CMDDT bit is not looked at, so
INQUIRY with CMDDT set and page code 0 returns the standard data, as
``scsi-hd`` does, and is changed like any other; an INQUIRY with EVPD
clear and a nonzero page code is refused with CHECK CONDITION, as in
``scsi-hd``. SPC-3 obsoletes CMDDT. The VPD pages, including the serial number in page
``0x80``, the block sizes, READ CAPACITY, mode pages and the
``wwn`` and ``port_wwn`` identity are the ones ``scsi-hd`` produces. The
peripheral type changes only byte 0 of the standard INQUIRY data; the
device still behaves as a direct access disk and the VPD pages still
report type 0. Bytes 1 to 7 (removable bit, version, response data format,
additional length and the capability bits) are ``scsi-hd``'s, including
its additional length rule: the field follows the allocation length of the
command (the allocation length minus 5 when it exceeds 36 bytes, and 31
otherwise) and does not count the vendor-specific bytes when fewer than 56
are requested. SPC-3 gives the field as the data's length minus 4, with
the allocation length not changing it, so that is a ``scsi-hd`` departure
the type keeps.

Attaching to host adapters
==========================

The type is an ordinary SCSI device, so it plugs into the SCSI bus of any
host adapter that takes ``scsi-hd``:

- on the QLogic ISP1040 and ISP12160, and the AIC-7880, with ``bus`` and
  ``scsi-id``, as for ``scsi-hd``;
- on an ISP2200 loop, with ``scsi-id`` the loop ID and both ``port_wwn``
  and ``wwn`` set, as the adapter's plug rules require
  (:doc:`qlogic-isp2200`). The names appear as for ``scsi-hd`` in the
  device identification page. The D-brick (:doc:`dbrick`) accepts it in a
  bay, because it is a ``scsi-hd`` by type.

Example::

    -drive if=none,id=d0,file=disk.img,format=raw
    -device scsi-hd-inquiry,drive=d0,bus=scsi.0,scsi-id=1,
        vendor=ACME,product=GENERIC DISK,ver=1.2,serial=SN0123456789,
        inquiry-vendor-specific=SN0123456789

How it works
============

``hw/scsi/scsi-disk.c`` is not changed. The type is a QOM subclass of
``scsi-hd``. Its ``alloc_req`` hook asks ``scsi-hd`` for the request and,
for INQUIRY, swaps in a copy of ``scsi-hd``'s request operations whose
``send_command`` runs the original and then overwrites the configured
bytes in the finished data buffer, before the host adapter is told data
is ready. Only a successful standard INQUIRY (EVPD clear, page code 0)
is touched, and only the bytes inside the transfer length. The settings
live in a table keyed by the device, since ``scsi-hd``'s state structure
is private to its source file.

Model limits
============

- Commands run on the main loop thread. The type is not for a host
  adapter that runs requests in an iothread, which no SGI adapter does.
- The settings are configuration. They are not migrated, so source and
  destination must be started with the same properties, as for other
  command-line identity.
- An INQUIRY to a logical unit that does not exist, answered by the SCSI
  bus, does not come from the disk and is not changed.
- No real drive's identity is claimed: whether the bytes 36 to 55 of any
  particular drive hold a serial number is a property of that drive and
  is *unknown* here.

Tests
=====

``tests/qtest/sgi-scsi-inquiry-test.h`` has cases for ASCII, hex with
a qualifier and type, and defaults. It reads the standard INQUIRY data
with allocation lengths 12, 36, 40, 56 and 96, with CMDDT set, and with a
nonzero page code and EVPD clear, and checks bytes 0, 8 to 15, 16 to
31, 32 to 35 and 36 to 55, the padding, the zeros past byte 55 and the
serial in VPD page ``0x80``. The cases run through the QLogic ISP1040
(``tests/qtest/sn0-qlogic-isp-test.c``), the ISP12160
(``tests/qtest/sn0-isp12160-test.c``) and the ISP2200 loop
(``tests/qtest/qlogic-isp2200-test.c``). No test attaches it to the AIC-7880: it
is the same SCSI device on that adapter's bus, but that is *inferred*
from the bus interface, not run.
