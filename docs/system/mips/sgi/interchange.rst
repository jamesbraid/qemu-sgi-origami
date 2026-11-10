.. SPDX-License-Identifier: BSD-3-Clause

.. _sgi-interchange:

=====================================================
Formats shared by the QEMU and iris emulators
=====================================================

A machine's files can be opened by more than one emulator: the NVRAM and
flash images and the clock records beside them, the disks, the firmware
images and the network wire. This page states the formats the QEMU machines
and the iris emulator both read and write, so that either can open what the
other left. Each machine page owns the hardware behind its stores; this page
owns the bytes in the files.

It covers store images and their clock records, machine identity, how
firmware images are taken from the published catalogue, CHD hard disks and
the stream network framing. It leaves out the control protocols (QMP on one
side, iris's command socket on the other), snapshots and saved machine state,
the input paths and the consoles' byte rules. Those differ by
design and no file is exchanged through them. A snapshot taken by one
emulator is never opened by the other. The shared-memory display that QEMU
offers external viewers has its own page, :doc:`display-shm`.

How facts are rated
===================

A statement about hardware or firmware carries one rating from
:ref:`sgi-ratings`, as on every page. Most of this page describes files the
emulators define, not hardware. A file layout that stands in for something
a real part does, such as a clock that keeps counting while the machine is
off, is an *approximation* and says what it leaves out. A statement of a
layout or a name that the code and a test fix, and that stands in for no
hardware, is not rated; it names the test that holds it in :ref:`Tests
<sgi-interchange-tests>`.

QEMU is the reference wherever it models the hardware. A format the iris
emulator alone defines is listed under :ref:`sgi-interchange-limits` and is
not interchange.

.. _sgi-interchange-stores:

Store images and clock records
==============================

A store is one raw file whose size is exact. The catalogue
(``hw/mips/sgi/sgi-machines.json``) lists each machine's stores by name,
size and initial content; :doc:`/tools/qemu-sgi-machine-init` creates them.
The bytes in a file do not depend on the emulator that wrote it. The file's
name is the launcher's choice: QEMU binds a store to the machine property of
the same name as the store, and iris takes one path per family in its
configuration.

.. list-table::
   :header-rows: 1

   * - Part
     - Machines
     - Image
     - Clock record
   * - M48T35 Timekeeper
     - Origin 200, Origin 2000, Onyx2, Origin 300, Origin 3000 family, Onyx 3000, Fuel
     - ``nvram<N>``, 32,768 bytes
     - ``nvram<N>-clock``, 16 bytes, magic ``M48TREF1``
   * - DS1744 Timekeeper
     - Origin 350, Onyx 350, Tezro, Onyx 4
     - ``nvram<N>``, 32,768 bytes
     - ``nvram<N>-clock``, 16 bytes, magic ``DS1742R1``
   * - DS1685 real-time clock
     - Octane, Octane2, O2
     - ``nvram<N>``, 512 bytes
     - ``nvram<N>-clock``, 24 bytes, magic ``DS168RF1``

The M48T35 is 32,768 byte cells, the clock in the top eight and storage
below it (*established*, :src:`st-m48t35-rev-10 p. 13`), which is the image's
size. The DS1685 has 242 bytes of user RAM, 50 common, 64 in bank 0 and 128
extended (*established*, :src:`maxim-ds1685-ds1687-rev-10-12 p. 1, 14`); the
model keeps its registers and RAM in the first 0x140 bytes of a 512-byte
image and the rest zero (*approximation*, since the part has no such image).
The physical Timekeeper of the Origin 300, Fuel and IP53 products is
*unknown*, so the model maps a flat 32,768-byte window, an M48T35 on the
first two and a DS1744 on the IP53 products (*approximation*,
:ref:`sgi-timekeepers-limits`). The Timekeeper pages own the register layouts
and the part of every machine (:ref:`sgi-timekeepers-parts`,
:ref:`sgi-timekeepers-m48t35-stores`, :ref:`sgi-timekeepers-ds1742`,
:doc:`hardware/ds1685`).

The M48T35 year cell is two BCD digits counted from the machine's year base
(*software expectation*, :ref:`sgi-timekeepers-m48t35-calendar`). The base is
1970 on the IP27 machines, which set none, and 1968 on every M48T35 topology
in ``hw/mips/sgi/sgi-machines.json`` that sets one: Fuel, Origin 300, Onyx
3000, Origin 3200, Origin 3400, Origin 3800, Onyx 3400 with InfiniteReality3,
and the Origin 3900 in its IP53, IP59 and memory-node profiles. The same
calendar reads ``0x56`` for 2026 on one and ``0x58`` on the other, so a store
moved between machines of different bases changes its date. The vectors below
are the base-1970 ones.

The clock record
----------------

A real part keeps counting from its battery while the machine is off, and a
file cannot. So each store has a second small file that remembers when the
image was last saved, and the next open adds the elapsed time to the saved
calendar. The model does this in place of the battery (*approximation*): QEMU
counts the clock ``-rtc`` selects, so a machine started with a fixed ``-rtc``
base or ``clock=vm`` need not advance across downtime, and changing ``-rtc``
between runs changes the time added.

All integers are 64-bit signed, little-endian, in seconds. The record is a
model construct, not a part's register (*approximation*).

.. list-table::
   :header-rows: 1

   * - Offset
     - Length
     - Contents
   * - ``0``
     - 8
     - The ASCII magic: ``M48TREF1``, ``DS1742R1`` or ``DS168RF1``.
   * - ``8``
     - 8
     - The instant of the save, in seconds since 1970-01-01T00:00:00Z. QEMU
       counts the clock ``-rtc`` selects for all three parts. The iris
       emulator counts the host's wall clock, so the emulators differ when
       ``-rtc`` is not the host clock.
   * - ``16``
     - 8
     - The DS1685 only: the running counter at the save, the guest calendar
       read as UTC, in seconds since 1970-01-01T00:00:00Z.

A file is read in its first 16 or 24 bytes; any further bytes are ignored.
QEMU refuses at start a record file the block layer reports as shorter than
the record, which for a raw file means an empty one. A shorter nonempty file
is read as zeros past its end and grows on the first save.

On open, the image is restored first and then the record applied:

- The model restores the calendar the cells hold. A store whose clock cells
  are all zero or not a valid BCD date was never saved: the clock follows the
  host or the seed, and the record is not used.
- A usable record adds the time since its instant. The M48T35 base is the
  image's calendar, counted in the part's own calendar, which has two digit
  years and a leap day whenever the year cell is a multiple of four
  (*inferred*, :ref:`sgi-timekeepers-m48t35-calendar`). The DS1744 base is the
  image's calendar, counted in the Gregorian calendar. The DS1685 base is the
  record's counter, not the image's calendar, and the counter must be a date
  the model can represent.
- The model adds no time to a stopped oscillator.
- A missing record, one with the wrong magic, or an unreadable one gives the
  saved calendar with no time added, and the emulator logs it. The DS1685
  additionally treats an image whose first 320 bytes are all zero as new and
  takes its calendar from the selected clock, ignoring even a usable record.
  That fallback is a model choice, an *approximation*.
- A saved instant later than now adds nothing. On the DS1685 a sum that
  leaves the representable range adds nothing either.
- The M48T35 and DS1744 of iris open a pair with no record at the saved
  calendar, as QEMU does. Only iris's DS1685 differs: it opens such a pair at
  its ``rtc_offset`` setting, and it writes a record only once the guest has
  set the clock.

On save the emulator writes the image, the clock cells holding the running
calendar and the stop bit, and then the record, so a record always follows its
image. QEMU saves when the machine stops and at shutdown. The M48T35 and the
DS1744 also save when a guest write of the clock completes, and the DS1685
when a guest write changes a persistent byte, at reset and after a snapshot
loads. The record's instant is refreshed only at those points. The M48T35's
ordinary storage bytes reach the image as the guest writes them but do not
rewrite the record.

Vectors
-------

The vectors are built when the test runs, not stored. Each starts from an
image whose calendar reads 2025-12-31 23:00:00 (the DS1685 image reads
2024-06-15 10:00:00, and its record's counter reads 2025-12-31 23:00:00) and a
record saved two hours before the emulator's clock, and must read 2026-01-01
01:00:00 after opening. The year boundary is on purpose: it is where a cell
year and a Gregorian year differ. A second case per part gives the record a
wrong magic and must leave the saved calendar as it was. A byte of storage at
a marker offset is checked unchanged.

.. list-table::
   :header-rows: 1

   * - Part
     - Calendar cells before the open
     - Record before the open
     - Cells and record after the save
   * - M48T35
     - ``0x7ff9``: seconds ``00``, minutes ``00``, hours ``23``, weekday
       ``00``, date ``31``, month ``12``, year ``55``, in BCD
     - ``M48TREF1``, then ``20 58 1d 6a 00 00 00 00`` (1,780,308,000)
     - Hours ``01``, date ``01``, month ``01``, year ``56``; record instant
       ``40 74 1d 6a 00 00 00 00`` (1,780,315,200)
   * - DS1685
     - Registers 4, 6, 7, 8 and 9: hours ``10``, weekday ``7``, date ``15``,
       month ``6``, year ``24``, in binary with the 24-hour bit; century
       ``20`` at ``0x88``
     - ``DS168RF1``, then ``20 58 1d 6a 00 00 00 00`` and
       ``f0 aa 55 69 00 00 00 00`` (1,767,222,000)
     - Hours ``1``, weekday ``5``, date ``1``, month ``1``, year ``26``;
       instant 1,780,315,200 and counter ``10 c7 55 69 00 00 00 00``
       (1,767,229,200)
   * - DS1742 geometry (bare ``ds1742`` device, 2 KiB)
     - Last eight bytes of the 2 KiB window: century ``20``, seconds ``00``,
       minutes ``00``, hours ``23``, weekday ``04``, date ``31``, month
       ``12``, year ``25``, in BCD
     - ``DS1742R1``, then ``20 58 1d 6a 00 00 00 00`` (1,780,308,000)
     - Hours ``01``, weekday ``05``, date ``01``, month ``01``, year ``26``;
       record instant ``40 74 1d 6a 00 00 00 00`` (1,780,315,200)

The DS1742 geometry vector is in a 2 KiB window because the test builds the
bare ``ds1742`` device. The 32 KiB DS1744 offset ``0x7ff8``, where the IP53
products' clock sits, is not exercised by this vector. The weekday of the
DS1744 model carries flag bits in its top two
(:ref:`sgi-timekeepers-ds1742`); a reader masks it with ``0x07``.

.. _sgi-interchange-identity:

Machine identity
================

:doc:`identity` owns the rules: the inputs, their precedence, what each
family accepts and how the guest reads the result. This section restates only
the layout both emulators must produce, since a machine moved between them
keeps its serial and station address only if both compute the same text.

The system id is a 32-bit number. The emulator layout is a pure function of
the kind, the emu-id (0 to 4095) and a sub-index, with no hashing. The kind
is 1 for a machine QEMU built and 2 for one iris built; it says who minted a
value, so the same emu-id gives different values in each. The layout itself is
a model choice that makes a value plainly not a real unit's (*approximation*).
On the address families, whose id comes from the station address, the
firmware fixes the first bytes ``08:00:69`` (*software expectation*,
:ref:`sgi-identity`); on the O2 that prefix is this project's policy, not the
firmware's.

.. list-table::
   :header-rows: 1

   * - Value
     - Layout
     - emu-id 42, kind 1
     - emu-id 42, kind 2
   * - System serial (IP35 family)
     - ``LEMU``, kind digit, emu-id as four digits, ``0``
     - ``LEMU100420``
     - ``LEMU200420``
   * - Module serial (Origin 2000, Onyx2)
     - ``KEMU``, kind digit, emu-id as four digits, module 0 to 9
     - ``KEMU100420``, module 1 ``KEMU100421``
     - ``KEMU200420``
   * - Station address
     - ``08:00:69:E<kind>``, emu-id >> 4, then emu-id & 15 over the sub-index
       (a node or port, 0 to 15)
     - ``08:00:69:e1:02:a0``
     - ``08:00:69:e2:02:a0``
   * - Board serial
     - ``EMUN`` and the node as two hex digits; independent of the emu-id
     - ``EMUN03`` for node 3
     - the same
   * - O2 clock serial
     - bytes ``45 4d 55``, the kind, the emu-id as two bytes
     - ``45 4d 55 01 00 2a``
     - ``45 4d 55 02 00 2a``

The id IRIX reports is the fold of the first ten characters of a serial text
(low nibble of each, low 32 bits kept) or the last four bytes of the station
address (*software expectation*, :ref:`sgi-identity`). ``LEMU100420`` folds to
``d5100420`` and ``08:00:69:e1:02:a0`` to ``69e102a0``. The extremes are
emu-id 0, ``LEMU100000`` and ``08:00:69:e1:00:00``, and emu-id 4095,
``LEMU140950`` and ``08:00:69:e1:ff:f0``.

Vectors for these values are in ``tests/unit/test-sgi-board-identity.c``,
built from the layout by hand. The iris emulator carries the system serial and the
station address, with kind 2 and the same vectors. It has no module serial,
board serial or O2 clock serial, and takes an emu-id on the Fuel and Tezro
only, so every other iris machine has the fixed address
``08:00:69:12:34:56``.

.. _sgi-interchange-prom:

Firmware images from the catalogue
==================================

Firmware is not in any archive. A machine's :term:`PROM` image comes from the
published catalogue, ``prom/index.json`` on ``origami-dist.irix.fans``, which
every consumer treats as the only manifest. It is a JSON object with
``base_url``, ``format`` (1) and ``proms``, a list of rows. A row has
``board``, ``version``, ``rev``, ``part``, ``banner``, ``kind`` (``shipped``
or ``dumped``), ``from``, ``name``, ``url``, ``size`` and ``sha256``; a held
row, described below, has no ``url``. A row is identified by ``name`` or
``sha256``; ``board`` and ``version`` do not, since some pairs have two
images. A consumer picks a row, downloads ``url``
and checks ``size`` and ``sha256`` before use. A row with ``"publish": false``
is held: its image is not uploaded yet, so it has no ``url``, and a consumer
skips it. Published rows carry no ``publish`` field. The catalogue describes
published files, not hardware, so these rules carry no rating. A consumer
that pins one image by its hash is conforming. The catalogue does not say which version a machine
offers, so each consumer names one.

The image a row names is the published object, in the form shipped. Each
board needs a different form of it for the machine:

- IP27, IP30 and IP35: a raw image or the version-6 container, plain or
  gzip. :doc:`/tools/qemu-sgi-machine-init` accepts either and
  :doc:`prom-image` states the container (*established* for the images it
  hashes).
- The I/O PROM of the Origin 2000 and Onyx2 baseio is stored as supplied,
  container included, because the IP27 PROM unpacks it itself
  (*software expectation*, :doc:`prom-image`). An emulator that stores the
  unpacked payload of the node PROM must still store the I/O PROM wrapped.
- The O2's published object is an installer image, not the flash contents.
  It starts with the ASCII ``PROM`` at offset 0 and, at offset 12, a
  big-endian 32-bit length. In the 4.18 image (431,872 bytes) that number is
  431,616, the file size less 256, so the payload starts after 256 bytes
  (*inferred* from the length of the published images 4.8, 4.17 and 4.18; no
  other version is checked). The model's flash array is the payload followed
  by erased bytes to 524,288 bytes; that those bytes are ``0xff``, as the
  ``rom-pad-byte=255`` of :doc:`ip32` has it, is an *approximation*, since the
  erased value of the part is not established. The QEMU tool and the
  ``rom-image`` machine option take only the finished array, never the
  installer form, so the consumer strips the header and pads. QEMU's tool
  refuses any other size.

.. _sgi-interchange-chd:

CHD hard disks
==============

A disk image is a CHD hard disk with 512-byte sectors, as MAME's ``chdman``
writes it (:doc:`/system/qemu-block-drivers`). Raw files are an exchange
format too; the qcow2 format is QEMU's alone and a CHD of a CD-ROM is not
supported here (see :ref:`sgi-interchange-limits`).

The two emulators agree on these names and behaviours. They are conventions of
the software, not hardware, and carry no rating. QEMU's driver refuses a CHD
that is not a hard disk with 512-byte sectors, which is how a CD-ROM image is
refused.

- An uncompressed image is written in place.
- A compressed image is never written. Guest writes go to an uncompressed
  ``<image>.diff.chd`` beside the image, which holds only changed hunks and
  names the image by its SHA-1. An existing diff is attached again on the next
  open. An image with no SHA-1 cannot take a diff.
- On a clean exit the diff is folded back: the image is rebuilt with its own
  codecs and hunk size in ``<image>.synctmp.chd`` and renamed over the image,
  and the diff is removed.
- A fold writes the SHA-1 of the rebuilt image into ``<diff>.folding`` before
  the rename and removes the marker last, so a crash is finished by the next
  read-write open from the marker and an image that already holds the data is
  not folded twice. Any other stop leaves the diff for the next open.
- ``overlay`` keeps the diff and never folds it, also for an uncompressed
  image.
- One image must not be open in both emulators at once, and running one
  against the other's diff is untested in either direction.

CHD is a software format, so the sidecar names have no hardware source. The
block driver's tests (``tests/qemu-iotests/tests/chd``) fix the QEMU side.

.. _sgi-interchange-stream:

Stream network framing
======================

Both emulators can join an Ethernet frame stream over a TCP or Unix socket.
In QEMU this is ``-netdev stream``. The framing is one 32-bit big-endian
length followed by the frame, with no frame check sequence. Either side may
be the client or the server. In a pairing with iris, iris is the client: it
retries the connection once a second until QEMU, started with ``server=on``,
is listening, and it connects again if the peer goes away, so a side that
starts first waits. QEMU can be the retrying client too, with ``server=off``
and ``reconnect-ms=1000``; QEMU refuses ``reconnect-ms`` in server mode.

QEMU's receive buffer holds one frame of up to 69,632 bytes
(``NET_BUFSIZE``). A sender should keep frames within 1,518 bytes, the
largest an iris receiver takes; iris drops a longer one and a frame shorter
than an Ethernet header. These limits are software's and carry no rating.

.. _sgi-interchange-limits:

Not interchange
===============

- Raw ``.overlay`` files, the DS1386 and 93CS56 stores of the Indy and
  Indigo2 and the ``IRIS_CHD_DIFF_DIR`` hashed diff naming are iris's
  alone. The QEMU machines have no counterpart and do not read them.
- A CHD of a CD-ROM is read by iris and refused by QEMU's driver
  (``chd`` takes hard disks only).
- qcow2 is QEMU's alone.
- The user-mode networks differ: QEMU's built-in one and iris's NAT have
  different subnets. IRIX caches the address of its DHCP server, so a machine
  moved between the emulators keeps its network only on the stream mode.
- Whether iris's first-run IP27 NVRAM and controller NVRAM equal what
  :doc:`/tools/qemu-sgi-machine-init` writes is *unknown*. A byte comparison
  of the two images after a first boot of each would settle it.

.. _sgi-interchange-tests:

Tests
=====

- ``tests/qtest/sgi-store-format-test.c`` builds the vectors above at test
  time, with no committed fixture. The M48T35 runs on the Origin 200's own
  :term:`IOC3`. The DS1685 runs on an Origin 200 with a second IOC3, added
  with ``ds1685-drive`` naming the image drive; the clock record is the drive
  named ``<image drive>-clock``. The DS1742 geometry case is a bare
  ``ds1742`` device with a 2 KiB image. Each case opens the pair, reads the
  calendar a guest would, exits the emulator and checks the saved image and
  record byte for byte: magic, length, instant, counter and an untouched
  marker byte. Each part also has a case with a record of the wrong magic.
- ``tests/qtest/sn0-ds1685-test.c`` covers the DS1685 restore rules beyond
  those vectors: no reference drive, a wrong magic, a negative elapsed time (a
  record instant 20 seconds after the ``-rtc`` base, which adds nothing), a
  stopped clock, a new image whose first 320 bytes are zero, and an invalid
  calendar. No test reaches the overflow rule, where the sum of the record
  and the elapsed time leaves the representable range.
- ``tests/qtest/sn0-ioc3-test.c`` covers the M48T35 restore rules: no
  reference drive, a reference drive never written, the stop bit, a never
  saved image and an impossible month. An empty reference drive is refused
  at realize; the test tries no other size. No test gives the M48T35 a
  negative or out-of-range elapsed time.
- ``tests/qtest/ds1742-test.c`` covers the DS1742 calendar, invalid dates, the
  DS1744 geometry and backed reopen, the fallback from an invalid saved
  calendar to the RTC seed, and the fresh-seed year range. It has no test of
  a negative or out-of-range elapsed time.
- ``tests/unit/test-sgi-board-identity.c`` pins the identity vectors.
- ``tests/qemu-iotests/tests/chd`` pins the diff, the fold and the markers. No
  test opens a CD-ROM CHD to see it refused.
- ``tests/unit/test-sgi-prom-image.c`` and
  ``tests/qtest/sgi-machine-storage-test.c`` pin the PROM container rules and
  the store sizes and names.
- The stream netdev framing is upstream QEMU behaviour with its own tests
  (``tests/qtest/netdev-socket.c``).

The vectors are meant to be written into iris's own tests; until they are
there, iris tests only its own restore rules. Neither repository shares code
or data with the other.
