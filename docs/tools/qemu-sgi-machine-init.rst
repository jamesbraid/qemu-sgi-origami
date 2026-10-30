.. SPDX-License-Identifier: GPL-2.0-or-later

=====================================
QEMU SGI machine state creation tool
=====================================

Synopsis
--------

**qemu-sgi-machine-init** **--machine** *OPTIONS* [**--boot-prom** *FILE*] [**--io-prom** *FILE*] *DIR*

**qemu-sgi-machine-init** **--machine help**

Description
-----------

:program:`qemu-sgi-machine-init` creates the writable state of a new SGI
machine: its node boot flash, its BASEIO or GIGAchannel I/O PROM flash,
its Timekeeper NVRAM with the clock record beside it, and each module
system controller's configuration NVRAM. It writes one raw
file per store into the new directory *DIR*, each named for the store and
exactly the size the machine requires.

The stores, their sizes and what each starts from come from the machine
catalogue compiled into QEMU, the same document ``query-sgi-machines``
returns. The tool resolves *OPTIONS* with the code the machine uses for
``-M`` and builds each store's contents with the code the machine uses when
it is started with no files. A machine started on the result therefore
reads the same flash as one started unbacked on the same images.

Every node of a machine gets the same boot PROM.

Options
-------

.. program:: qemu-sgi-machine-init

.. option:: -M OPTIONS, --machine OPTIONS

  The machine selection, exactly as ``--machine help`` lists it, for
  example ``origin2000,topology=origin2000-deskside,nodes=2``. A selection
  the machine would accept in another spelling is refused with the
  catalogue's own spelling, so that one machine has one name. ``help``
  lists every selection.

.. option:: -b FILE, --boot-prom FILE

  The node boot PROM image. IP27, IP30 and IP35 images may be raw or SGI's
  version-6 single-segment container, uncompressed or gzip. An IP35 image
  must decode to exactly the PROM size the machine's ``-bios`` requires.

.. option:: -i FILE, --io-prom FILE

  The BASEIO or GIGAchannel I/O PROM image. It is stored as supplied,
  container included, because that is the form the IP27 PROM reads from
  the part.

.. option:: -h, --help

  Print a summary of the options.

.. option:: -V, --version

  Print the version.

A selection requires each image its catalogue entry marks required, and
refuses one it does not read.

Initial contents
----------------

Each store starts from one of these:

``boot-prom``
  IP27 node flash holds the PROM image over a default template, with the
  product's machine type and flash read and write cycle counts written into
  the image's configuration block. IP35 node flash holds the image in its
  first PROM position and the blank-media label in its second. Octane boot
  flash holds the image followed by erased bytes.

``io-prom``
  The I/O PROM file as supplied, followed by erased bytes.

``ip27-nvram``
  The image an IOC3 Timekeeper on an IP27-family machine installs when it
  has no backing: zeroes, with IRIX's leap-cycle record for the current
  month and the PROM environment checksum over the result. The month is
  taken from the host clock in UTC, as the Timekeeper takes it under the
  default ``-rtc``.

``controller-nvram``
  A module system controller's 2048-byte configuration NVRAM as the
  controller leaves it after its first power-up: zeroes, then at 0x700
  the validity marker ``0x37``, the password ``none``, the configuration
  byte, the module number and the clock-select byte at 0x70b. The Origin
  200 build writes configuration ``0x02``, module 0 and clock ``0x78``;
  the Origin 2000 build writes configuration 0 and the module number the
  topology selects for that module, and the store keeps 0 at 0x70b.

``kona-flash``
  An InfiniteReality pipe's 524288-byte flash EEPROM as a new pipe's
  model starts it: zeroes, apart from the big-endian header state word 2
  at 0x7ff1c, which the PROM requires before it accepts the pipe. A
  combination ircombine stores later goes at 0x20000.

``zero``
  Zero-filled. A selected system Timekeeper, as Origin 300 and Fuel fit,
  and Octane's NVRAM and every clock record start this way.

Creating the directory
----------------------

*DIR* must not exist; an existing directory, file or symbolic link,
dangling or not, is refused and left untouched. The stores are written and
flushed in a private directory created beside *DIR*, which is then renamed
to *DIR* without replacing anything. If any step fails, the private
directory and every file in it are removed, so a failure leaves nothing at
*DIR* or beside it. Every image is read and checked before the first file
is written.

The new directory keeps the owner-only mode of the private directory it
was created as. A launcher that runs QEMU as a different user must change
it.

On hosts without ``renameat2()`` and ``RENAME_NOREPLACE``, the final rename
checks that *DIR* is absent and then renames, which another process
creating *DIR* at the same moment can race.

Once *DIR* is in place, a failure to flush its parent directory is reported
as a warning, and the tool still exits 0, because *DIR* is complete.

Using the result
----------------

Attach each file as a block node named for its store and bind it through
the machine property of the same name::

  qemu-sgi-machine-init --machine origin2000,topology=origin2000-deskside,nodes=2 \
      --boot-prom ip27prom.img --io-prom io6prom.img vm0

  qemu-system-mips64 \
      -M origin2000,topology=origin2000-deskside,nodes=2,node0-flash=node0-flash,node1-flash=node1-flash,io0-flash=io0-flash,nvram0=nvram0,nvram0-clock=nvram0-clock,module0-nvram=module0-nvram \
      -blockdev driver=raw,node-name=node0-flash,file.driver=file,file.filename=vm0/node0-flash.raw \
      -blockdev driver=raw,node-name=node1-flash,file.driver=file,file.filename=vm0/node1-flash.raw \
      -blockdev driver=raw,node-name=io0-flash,file.driver=file,file.filename=vm0/io0-flash.raw \
      -blockdev driver=raw,node-name=nvram0,file.driver=file,file.filename=vm0/nvram0.raw \
      -blockdev driver=raw,node-name=nvram0-clock,file.driver=file,file.filename=vm0/nvram0-clock.raw \
      -blockdev driver=raw,node-name=module0-nvram,file.driver=file,file.filename=vm0/module0-nvram.raw \
      -smp 4 -m 512M

A launcher can build this command line from the ``storage`` list of the
machine's ``query-sgi-machines`` entry. No ``-bios`` is needed: every
machine boots from its bound node flash.

Exit status
-----------

0 when *DIR* was created or the selections were listed, 1 on any error.
