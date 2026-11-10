.. SPDX-License-Identifier: BSD-3-Clause

.. _sgi-adaptec-aic1160-page:

=============================================
Adaptec AIC-1160: Fibre Channel controller
=============================================

Used in no machine. The controller is the one on the Fibre Channel :term:`XIO` boards,
whose placement is a separate part of the model.

.. seealso::

   Related parts: :doc:`qlogic-isp2200` is the other Fibre Channel
   controller the models carry, behind a PCI slot; :doc:`bridge` is the chip
   a PCI card sits behind.

The AIC-1160 is a Fibre Channel controller built around a microsequencer.
The host downloads microcode into an on-chip sequencer RAM through a data
port, starts the sequencer, tells it where its rings are by writing the
sequencer's scratch RAM, and then hands it command blocks held in host
memory with a producer index. ``hw/scsi/adaptec-aic1160.c`` models the
controller as the PCI device ``aic1160``: its identity and two register
windows, the banked register file, the sequencer download, the serial
SEEPROM, the interrupt status and status block, and the command path from a
posted command block to a completion in the done queue.

The model does not execute microcode. It accepts the download, keeps it,
and reads it back, which is all the download handshake asks of the chip. A
guest that completes the handshake has shown that the model took the
words, not that the words would run on the real part. The work the
microcode does is done in the model: it fetches command blocks, runs SCSI
commands against the devices on its private loop, moves their data, and
posts the completions (see `Commands`_).

Sources
=======

No programming manual for the AIC-1160 is public. The register and command
facts come from the shipped IRIX 6.5.30 Fibre Channel driver for the Octane,
Origin 2000 and Origin 300 platforms, read as a binary, and each is a
*software expectation*: it shows what that driver does, not what the chip
is. The offsets, access widths, bank selects, ring setup and layouts are
identical in the driver builds for all three platforms.

Owners' reports establish what IRIX shows of the board and what it carries:

- A two-port :term:`XIO` card with an AIC-1160 and its ``hinv`` line and loop
  initialisation log, :src:`usenet-4197ca17`.
- The hardware-graph paths of the board's two controllers,
  :src:`usenet-3be73104`.
- An Octane ``hinv`` listing two AIC-1160 controllers, :src:`usenet-3a0602c2`.
- The board's markings, :src:`usenet-3adb3ac0`.
- The marketing name, XT-FC-2P, :src:`usenet-3fafc818`.

Identity
========

The board is a two-port Fibre Channel XIO controller, sold as the XT-FC-2P
(*observed*, :src:`usenet-3fafc818`). Its chip is marked AIC-1160
(*observed*, :src:`usenet-3adb3ac0`). IRIX reports each port as an
``Integral SCSI controller`` with ``Version Fibre Channel AIC-1160,
revision 2``, and owners also report revision 1 (*observed*,
:src:`usenet-4197ca17`, :src:`usenet-3a0602c2`). The hardware graph names
the board's two functions ``pci/0`` and ``pci/1`` under a ``fibre_channel``
node, each holding a ``scsi_ctlr`` (*observed*, :src:`usenet-3be73104`). The
driver treats the two as independent controllers (*software expectation*).
Whether the two are separate dies or one dual-function part is *unknown*.

The driver claims a function by its PCI vendor and device pair,
``0x9004`` and ``0x1160`` (*software expectation*). It reads the revision
from configuration space and prints it (*software expectation*). It maps
three 256-byte windows for each function (*software expectation*): the
chip's own PCI configuration space, which is not a base address register;
BAR 0, the chip registers; and BAR 3, the sequencer memory
(*software expectation*). Both BARs are I/O space (*inferred*): IRIX enables
the function with command ``0x55``, I/O space, bus master,
memory-write-and-invalidate and parity response, and never sets memory space
(*software expectation*), but no one has read the BAR type bit from the chip.
The chip has BARs 0 to 3; the driver writes 0 to BARs 1 and 2 as a workaround for
a chip bug and never uses them, and IRIX sizes all six (*software
expectation*). It connects one interrupt per function
(*software expectation*).

The model's choices for what no source gives, each an *approximation*:

- The class code is ``0x0c04``, Fibre Channel. Attach does not check it
  (*inferred*) and the real value is *unknown*.
- The revision byte defaults to 2 and is set with the ``revision``
  property.
- The interrupt pin is INTA.
- The subsystem pair is left at 0.
- BAR 0 and BAR 3 are 256-byte I/O windows. BARs 1 and 2 are not modelled
  and read 0; the driver's zero writes to them change nothing.
- Config register ``0x40`` is writable. The driver sets bit 2, the
  dual-address-cycle enable, at attach (*software expectation*), and the
  model stores it without gating DMA on it. The cache line size and latency
  timer are writable (the driver sets 32 and ``0xf0``, *software
  expectation*).
- DMA addresses are 64 bits, carried as eight little-endian bytes
  (*software expectation*).

The device is little-endian on the bus. The driver uses byte and 16-bit
accesses and byte-swaps the 16-bit quantities that cross between host and
chip (*software expectation*). A 32-bit access to either window is refused
by the memory core. Whether the chip takes one is *unknown*.

Register map
============

BAR 0, an I/O window. The offsets and widths are what the driver uses
(*software expectation*); the register names are chosen from what the
driver does with each. The register file is banked: register ``0x2c``
selects which of eight overlapping register sets the cells from ``0x28`` up
belong to, so one offset can mean different things in different banks.

.. list-table::
   :header-rows: 1

   * - Offset
     - Width
     - Register
     - Model
   * - ``0x00``
     - 8
     - Sequencer control
     - Stores the byte; bit 0 enters load mode, bit 6 runs
   * - ``0x02``
     - 16
     - Sequencer RAM address
     - Stores; advances on each data port access
   * - ``0x14``
     - 16
     - Sequencer RAM data
     - 2048 words; reads back
   * - ``0x22``
     - 8
     - Host control
     - Bit 2 master interrupt enable, bit 6 pause, bit 7 pause at top,
       bit 0 chip reset
   * - ``0x23``
     - 8
     - Error status
     - Reads 0
   * - ``0x24``, ``0x25``
     - 8 or 16
     - Interrupt status, bytes 0 and 1
     - Set by events; a byte write of 1s clears
   * - ``0x2a``
     - 16
     - New-TCB producer index and doorbell
     - A 16-bit write starts command fetch
   * - ``0x2c``
     - 8
     - Register bank select
     - ``0x00``, ``0x11`` to ``0x77`` select banks 0 to 7
   * - ``0x34``
     - 8
     - Scratch page select
     - Chooses the 128-byte page BAR 3 shows
   * - ``0x68`` to ``0x6b``
     - 8
     - Bank 0: serial SEEPROM controller
     - Data low and high, address, opcode and busy
   * - ``0x6a``, ``0x6b``
     - 16
     - Bank 4: TCB consumer index
     - Stores; advances as the chip fetches commands
   * - ``0x28``, ``0x29``
     - 8
     - Bank 4: completion reporting control
     - Bit 5 of ``0x29`` arms status-block and completion posting
   * - ``0x70`` to ``0x77``
     - 8
     - Bank 4: status block address
     - Eight bytes, low byte first
   * - ``0xa1``
     - 8
     - Bank 1: loop state
     - Stores; bit 7 LIP, bit 6 bypass, bit 5 enable received
   * - ``0x63``
     - 8
     - Bank 4: clock and link speed
     - Reads ``0xbe`` after a reset, full speed; stores
   * - ``0x40`` to ``0x5f``, ``0x62``, ``0x64``, ``0x65``, ``0x7c``, ``0x98``,
       ``0x99``, ``0xa2``, ``0xa3``, ``0xd0``, ``0xe1``, ``0xeb``
     - 8
     - Bank-local control cells
     - Storage only, one cell per bank

Every other offset logs the access, reads 0 and ignores writes. Byte
accesses to the data port are refused with a log entry, since the driver
uses 16-bit accesses there.

Config collection
-----------------

Before any reset, SEEPROM read or microcode load, the driver collects the
chip's configuration once per function (*software expectation*): it pauses
the sequencer and waits for the pause acknowledge, clears the interrupt
status with ``0xff`` to ``0x24`` and ``0x25``, selects bank 4 and writes
``0x81`` to ``0x62`` (eight-byte DMA addressing, four scratch pages), then
reads the clock register ``0x63``. It takes the value ANDed with ``0xfc`` and
ORed with ``0x02``, and accepts only ``0xbe``, ``0x2e``, ``0xfe`` and ``0x6e``;
any other fails the function with "cannot find link speed", which the kernel
reports as "error -2 in getting config" and the port as an adapter fatal
error. The model reads ``0xbe`` after a reset, the full-speed value, an
*approximation*: the real value follows the negotiated speed.

The driver then probes the TCB SRAM: it writes an index to ``0x64`` and
``0x65``, writes the bytes ``0x66``, ``0xdd``, ``0x55``, ``0xaa`` through the
sequencer memory window, reads them back, and steps through smaller SRAM sizes
until one verifies (*software expectation*). The model's window stores and
echoes every offset, so the first size verifies; the index only stores.

Bank select and reset
---------------------

Register ``0x2c`` takes ``0x00``, ``0x11``, ``0x22``, ``0x33``, ``0x44``,
``0x55``, ``0x66`` and ``0x77`` to select banks 0 to 7; ``0x44`` is bank 4, ``0x11`` bank 1 and
``0x22`` bank 2 (*software expectation*). It is not a reset. The model
refuses any other value with a log entry and keeps the bank.

The driver resets the chip through host control when it recovers from an
error, not at first attach, which writes only ``0x40``: it sets the pause
bit, waits for the acknowledge, then writes ``0x43``, the pause, FC datapath
reset and chip reset bits (*software expectation*). The model resets on any
write with bit 0 set. The sequencer stops, the bank select returns to
bank 0, the banked cells, the interrupt status, the doorbell, the consumer
index and the done queue position clear, and requests in flight are
cancelled. The reset also clears the microcode, the scratch variables and
the interrupt enables, so the driver reloads the microcode and reprograms the
rings, the status block and the masks afterwards (*software expectation*).

The pause and pause-at-top bits read back as written, which is the
acknowledge the driver polls for (*software expectation*); the model
acknowledges at once, an *approximation*.

Power-on and bus reset set every register to 0 and clear both RAMs. A read
taken from a running system is not a reset value, and none of these zeros
has a source, so each is an *approximation*.

Sequencer download
==================

The driver loads a microcode image in this order (*software expectation*):

1. Disable interrupts.
2. Write ``0x01`` to the sequencer control register and 0 to the
   sequencer RAM address.
3. Write the image to the data port as 16-bit words, each byte-swapped.
4. Set the address back to 0 and read the words back from the data port to
   verify them.
5. Write ``0x40`` to the sequencer control register to run it.
6. Restore interrupts.

Two images are shipped inside the driver: a mainline image of 3912 bytes
and a loop-initialisation image of 1644 bytes, which it swaps in for a loop
initialisation and then replaces with the mainline image again
(*software expectation*). The sequencer RAM is 4096 bytes and the address
advances on both reads and writes of the data port (*software expectation*).

The model stores each word the data port takes at the address pointer and
advances the pointer, and returns the stored word and advances the pointer
on a read. A word at or past the end of the RAM is logged as a guest error,
the write is dropped and a read returns 0. Writing the control register with
bit 6 set marks the sequencer running and clearing it marks it stopped. Bit 1
restarts the program counter at the entry point, so the driver's ``0x42`` is
run and restart (*software expectation*); the model has no program counter
and ignores it.
Writing bit 0 starts a new download. The other bits have no described
meaning and are logged. The model keeps the image and the running state but
does nothing further with the image, an *approximation*.

Sequencer memory
================

BAR 3 shows the sequencer's memory: 128-byte scratch RAM pages chosen by
register ``0x34``, then further RAM (*software expectation*). The driver
tests it by sweeping the ``0x55aa`` and ``0xa5`` patterns across each
page in 16-bit steps from offset 0 to ``0x7e`` (*software expectation*). The
driver selects a layout of four pages through the size-select register at
bank 4 ``0x62``; it tests pages 0 to 2 and leaves page 3 reserved for
firmware (*software expectation*). The model has four pages and 128 further
bytes, all plain storage that reads back, and the size select only stores, an
*approximation*: what the further RAM holds (the driver's description has the
TCB SRAM and the scatter/gather RAM there) is *unknown*. Page 1 is where the
loop-initialisation microcode puts the loop-position table (*software
expectation*); the model does not fill it. The window takes byte and 16-bit
accesses.

The driver does not give the chip its ring addresses in registers. It writes
them into page 0, each address as eight little-endian bytes
(*software expectation*):

.. list-table::
   :header-rows: 1

   * - Page 0 offset
     - Contents
   * - ``0x08``
     - PCI address of the done queue
   * - ``0x1c``
     - PCI address of the TCB ring
   * - ``0x45``
     - Done-queue pass byte, written ``0xff`` and read back
   * - ``0x48``
     - Done-queue size, the negated entry count

The model reads these when it fetches a command and when it posts a
completion. It takes the size as a 16-bit two's-complement value, an
*approximation*: its width is *unknown*. The TCB ring has as many entries as
the done queue, at most 512 (*software expectation*). A write to the size
restarts the done queue at slot 0 and lap 0, an *approximation*: when the
chip does so is *unknown*. The model does not use scratch offset ``0x45``;
the lap is the model's own counter.

Self-test RAMs
==============

The driver's start-up RAM test goes beyond the sequencer memory window
(*software expectation*). Each region is written with a pattern and read back,
and the chip fails to initialise ("initialization failure -3") if any does not
echo. The model backs each as plain storage:

.. list-table::
   :header-rows: 1

   * - Region
     - Reached through
     - Model
   * - Per-bank cells
     - ``0x40`` to ``0x5f`` under all eight bank selects
     - 32 bytes in each bank
   * - Global cells
     - ``0x38`` to ``0x3f``
     - 8 bytes, the same in every bank
   * - Scatter/gather RAM
     - Bank 2 data port ``0x60``, TCB index ``0x64``, rewind ``0x8b``
     - 512 areas of 128 bytes; the port is byte-wide and advances on every
       access, and a write of 0 to ``0x8b`` rewinds it
   * - Command buffer
     - Bank 4 data port ``0x20``, address ``0x9e``
     - 128 bytes; the port advances on every access, and a write to ``0x9e``
       sets the address

The test writes the values 0 to ``0x7f`` and then ``0xa5`` to each
scatter/gather area (*software expectation*). The sizes, and the way the data
port wraps, are each an *approximation*. The separate TCB-count sizing that writes
``0x66 0xdd 0x55 0xaa`` through BAR 3 passes on the sequencer memory alone.

Commands
========

The driver builds a 128-byte transfer control block (TCB) in the ring and
writes the new producer index to the doorbell at ``0x2a`` as a 16-bit
store (*software expectation*). Both the producer and the chip's consumer
(bank 4, ``0x6a``) wrap at the ring size (*software expectation*). On the
doorbell write the model fetches each TCB from the consumer up to the
producer, with the sequencer running and the rings set up; without them it
logs a guest error and fetches nothing.

A TCB starts with a 32-byte Fibre Channel frame header. Its control fields
are little-endian and so are the descriptors and the residual block: the
driver's byte-swap routines are empty (*software expectation*). Only the
FCP data length is big-endian. The R_CTL byte at ``0x07`` is ``0x01`` for an
FCP command in the driver, which is the chip's encoding, not the wire value
(*inferred*):

.. list-table::
   :header-rows: 1

   * - Offset
     - Contents
   * - ``0x00``
     - Originator exchange ID (2 bytes); zero in a receive buffer
   * - ``0x04``
     - Destination ID; its low byte is the target AL_PA. The chip supplies
       the source ID
   * - ``0x07``
     - R_CTL: ``0x01`` an FCP command, ``0x22`` a link service request
   * - ``0x0f``
     - FC-4 TYPE: ``0x08`` FCP, ``0x01`` extended link service
   * - ``0x21``
     - Control: ``0x00`` SCSI read or no data, ``0x01`` receive buffer,
       ``0x02`` SCSI write, ``0x0a`` send a link command and receive its
       reply
   * - ``0x23``
     - Descriptor mode: ``0x08`` one buffer, ``0x0b`` a list, ``0x0c`` no data
   * - ``0x24``
     - FCP LUN
   * - ``0x30``
     - SCSI CDB, 16 bytes
   * - ``0x40``
     - FCP data length (4 bytes, big-endian)
   * - ``0x4c``
     - Data address, or the address of the descriptor list (8 bytes)
   * - ``0x54``
     - Data length (4 bytes); 0 for a list
   * - ``0x44``
     - The next-TCB link (8 bytes), not a response buffer
   * - ``0x58``
     - Address of the 256-byte response buffer (8 bytes)
   * - ``0x68``
     - The handle (2 bytes), echoed in the completion

The model sorts a TCB by those bytes. A TCB with control ``0x01`` is a
receive buffer, which the driver leaves with the chip until a frame arrives:
the model keeps no state for it, completes nothing and raises no interrupt,
since it has no frame to give. Its three addresses (``0x24``, ``0x44`` and
``0x58``) go unread. Handle 0 is the driver's reserved primitive slot, and a
completion that carried it would draw "primitive completed, but not active"
(*software expectation*). One with control ``0x0a``, TYPE ``0x01`` and R_CTL ``0x22`` is a
login. One with a descriptor mode of ``0x08``, ``0x0b`` or ``0x0c`` is a SCSI
command. Any other completes with the frame-format error. A loop initialisation is
not a ring TCB (see `Logins and loop initialisation`_).

A list is 16-byte entries: an 8-byte address, a 4-byte length and four
reserved bytes, with the last entry's length marked by a top byte of 1
(*software expectation*). The driver leaves the TCB's count at 0, so the
model walks the list until the entry whose length has that top byte, with
the length masked to 24 bits. A list that never ends is refused with the
frame-format error.

SCSI commands
-------------

The model maps the AL_PA to a loop ID, the LUN from the first two bytes of the
FCP LUN, finds the device and runs the CDB against it. The data moves by
DMA through the descriptors, in the direction the command has, and no more
than the host's buffers hold. An AL_PA with no device completes at once as
undelivered. When the command ends the model posts the completion and raises
the interrupt.

The driver parses the FCP response only when the completion status is
``0x04``, error response (*software expectation*). So the model writes it
only then: the flags at ``0x0a`` (``0x02`` sense length valid, ``0x04``
residual over, ``0x08`` residual under), the SCSI status at ``0x0b``, the
residual at ``0x0c`` as four big-endian bytes, the sense length at ``0x10``
and the sense bytes from ``0x18`` (*software expectation*). It writes the
residual as the shortfall or excess against the FCP data length.

Whenever the transfer is not exactly the FCP data length the model also
writes the adapter residual-info block at response offset ``0x80``. The driver
compares a figure it builds from the block with the FCP residual and fails the
command, with "residual mismatch", if they differ (*software expectation*). The
fields are little-endian: an 8-byte pointer at +0, the 3-byte residual at +8
and an index in the high nibble of +12; byte +11 is not read. For a single
buffer the pointer is zero and the driver takes the residual as it stands. For
a list the pointer is a position in the command's own list: the list's bus
address plus 0x80 for each page of eight descriptors passed plus 0x80, and the
index is the next descriptor within its page. The driver steps back one
descriptor to the one in flight and adds the lengths of all that follow, up to
the one marked last. The residual is what the descriptor in flight has left.
The pointer arithmetic is the driver's (*software expectation*); that the chip
returns the address of the page after the one it works in is *inferred*.

Done queue
----------

The done queue is entries of four bytes: the handle (2 bytes, little-endian),
a status byte and a pass byte (*software expectation*). The driver sets
every pass byte to ``0xff`` and expects pass 0 on the first lap; the chip
writes the number of laps it has made around the ring, modulo 256, and the
driver consumes entries while the pass byte matches (*software
expectation*). The model keeps a slot and a lap and does the same. A
completion is not posted if the done queue is not set up.

The status codes (*software expectation*), and what produces each in the
model:

.. list-table::
   :header-rows: 1

   * - Code
     - Meaning
     - Model
   * - ``0x00``
     - Success
     - A command or login that ran in full
   * - ``0x01``
     - Data overrun
     - Nothing: see below
   * - ``0x02``
     - Data underrun
     - Nothing: see below
   * - ``0x04``
     - Error response
     - A SCSI status other than GOOD, or a transfer that is not the FCP data
       length, with the response buffer written
   * - ``0x06``
     - Frame format error
     - A login with no payload or an unreadable descriptor list
   * - ``0x08``
     - Undelivered, no device
     - An AL_PA with no device on the loop

A command that moves a different number of bytes than the FCP data length,
such as an INQUIRY with room for more than it returns, completes with
``0x04`` and an FCP response carrying GOOD status and the residual, with the
under or over flag set. The bare underrun and overrun codes are not used,
because the driver logs them as errors ("Error code 2 (data underrun)") for
every short INQUIRY, which a working board does not draw; so which code the
chip uses is an *approximation*. The codes ``0x03``, ``0x05``, ``0x07``, ``0x09``, ``0x0a``, ``0x0b`` and
``0x0f`` (parity, frame order, count overflow, bad delimiters, aborted,
primitive timeout) have nothing in the model to produce them. The driver
never issues an abort (*software expectation*).

Each completion sets bit 4 of ``0x25``, the sequencer and completion
interrupt, and raises the line (*software expectation*). The driver does not
gate its queue drain on the bit: it scans the pass bytes on every interrupt
(*software expectation*).

Logins and loop initialisation
------------------------------

A login TCB carries the ELS command code in the first payload byte, which the
data pointer and length at ``0x4c`` and ``0x54`` give: ``0x03`` PLOGI, ``0x20``
PRLI, ``0x52`` ADISC (*software expectation*). The model reads the code and
DMAs an accept into the response buffer, then completes with status
``0x00``:

.. list-table::
   :header-rows: 1

   * - Login
     - Accept the model writes
   * - PLOGI
     - LS_ACC (``0x02`` in byte 0), service parameters, the port's port and
       node names at offsets 20 and 28, a class 3 parameter block
   * - PRLI
     - LS_ACC, a 20-byte FCP page: type ``0x08``, image pair established,
       and ``0x12`` at offset ``0x13`` for target function with read
       XFER_RDY disabled
   * - ADISC
     - LS_ACC, the hard address, the port and node names, the port ID

The driver needs for PRLI a good status, LS_ACC, type ``0x08`` and the
service byte at offset ``0x13`` equal to ``0x12``; anything else fails the
port. It stores the names and parameters from PLOGI without checking them,
and does not parse the ADISC reply (*software expectation*). The names are the
device's ``port_wwn`` and ``wwn`` properties; zero when unset. The remaining
service parameter bytes are plausible values and not the target's, an
*approximation*. An unknown command code gets LS_RJT. A login to an AL_PA with
no device completes ``0x08``. No login state is kept: a SCSI command to a port
that never logged in still runs, an *approximation*.

The driver finds the disks by a loop initialisation, and it does not use the
ring for it. The driver requests the LIP primitive by writing PCI config byte
``0x4b``, alternating a request bit and a close bit, and waits for LIP ready,
status byte 1 bit 0 (*software expectation*). The model raises that bit and the
interrupt line on a write of a nonzero value, the request. It does nothing for
the close, a write of 0: the driver writes one when a loop initialisation is
over, and a LIP ready then sends it round the whole sequence again, an
*approximation*. What the chip does with the primitive is *unknown*.

On the LIP ready the driver (*software expectation*):

1. Pauses the sequencer, and aims the TCB pointer at ``0x64``/``0x65`` at the
   reserved LIP TCB number N in each of banks 0, 2, 1, 5, 4 and 3. N is 4 in
   the observed boot, after the three receive buffers.
2. Copies the 128-byte LIP TCB to TCB RAM slot N, BAR 3 offsets ``0x80`` to
   ``0xff``, selects scratch page 1 and writes the 128 valid AL_PAs to BAR 3
   ``0x00`` to ``0x7f``, then selects page 0.
3. Streams in the loop image, which is 822 words and four of padding, against
   1956 for the mainline image, and verifies it.
4. Writes ``0xff`` to scratch ``0x45``, restarting the done queue, and clears
   the host's done entries.
5. Starts the sequencer with ``0x40`` and then ``0x42``, and clears the pause
   bits in ``0x22``. The unpause is the cue to run the loop image.

The LIP TCB has its own layout: the map size at ``0x00``, a frame header from
``0x08`` (destination ``0xef``, R_CTL ``0x22``, TYPE ``0x01``), the address
preferences at ``0x2c`` and ``0x2e``, the port name at ``0x30``, the loop-map
address at ``0x38`` and the next-TCB link at ``0x44``; ``0x58`` is unused
(*software expectation*). The model reads only the map address.

The model runs loop initialisation when the sequencer is unpaused with it
running and 800 to 900 words in the sequencer RAM, an *approximation*: the
real chip tells the images apart by what it executes. It DMAs a
count-list map of the other ports to the map address, 132 bytes, in ascending
loop ID order and without the host, with the bytes in memory order. It then
posts one done entry for TCB N, whose status is the host AL_PA, that of loop
ID 125, the driver's default (``0x01``), raises the completion interrupt, and
posts nothing for TCB 0. The interrupt is mandatory, since the driver has
nothing in flight to poll for. The model sets no loop-state register and
raises no loop attention. The AL_PA preferences in the TCB are not read, and
the bitmap form of the map is not produced.

When the completion arrives the driver reloads the mainline image: it writes
the TCB's next-TCB link back to scratch ``0x1c``, restarts the done queue
again, starts the image with ``0x42`` and, for a valid AL_PA, writes bank 1
``0xa0`` and ``0x6b``. The model follows both. Scratch ``0x1c`` is the pointer
to the next TCB: the driver writes the ring's start there, and after a loop
initialisation the next free entry, zeroing its consumer and producer indices
there. The model fetches from the pointer and advances it round the ring. A new
value inside the ring is a position in it, with the indices counted from there,
and any other is a new ring, an *approximation*. A completion that follows a
restart goes to slot 0 with pass 0.

A TCB that is none of a SCSI command, a login or a receive buffer completes
with status ``0x06`` and a log entry.

Interrupts
==========

The interrupt status register is 16 bits: byte 0 at ``0x24`` and byte 1 at
``0x25``. The driver decides that an interrupt is pending by masking the
16-bit value with ``0xff08``: the whole of byte 1 and bit 3 of byte 0
(*software expectation*). Bit 3 of byte 0 is the loop attention interrupt.
Byte 1 carries PCI error ``0x80``, hardware error ``0x40``, software error
``0x20``, sequencer completion ``0x10``, completion-count ``0x04``,
breakpoint ``0x02`` and sequencer-ready-for-LIP ``0x01``
(*software expectation*). The driver clears the bits it has handled by
writing them back as byte writes (*software expectation*), and the model
clears the bits written as 1, an *approximation* inferred from the
write-back.

Host control bit 2 is the master interrupt enable (*software expectation*).
The model drives one INTA line to the :term:`Bridge` while the status holds
a bit of ``0xff08`` and the enable is set.

When a status bit arises and bank 4 register ``0x29`` has bit 5 set, the
chip writes status bytes ``0x24`` and ``0x25`` to the eight-byte status
block at the address in bank 4 ``0x70`` to ``0x77``, and the driver clears
its copy (*software expectation*). The model writes the two bytes, the
current value of the whole register, each time a bit is set.

After a loop attention interrupt the driver selects bank 1 and reads
the loop-state register ``0xa1``: bit 7 is a LIP received, bit 6 a bypass,
bit 5 an enable, meaning the loop is down or up (*software expectation*).
The model has no loop event of its own, since it keeps a private loop that
is always up, and the per-source posting masks are not applied. The
``x-loop-event`` property sets register bits and raises the attention
interrupt, for tests. The properties ``x-words-loaded`` and
``x-sequencer-running`` read the download state, and ``x-raise-status`` sets
status bits. All are test hooks, not part of the guest interface and not
migrated.

Serial SEEPROM
==============

The driver reads the board's SEEPROM through a command controller in bank 0
(*software expectation*): it pauses the sequencer, writes the word address
to ``0x6a``, writes the read opcode ``0x61`` to ``0x6b``, polls bit 0 of
``0x6b`` until it clears, then reads the word's low and high bytes at
``0x68`` and ``0x69``. Word 0 is a control word, word 2 the media interface
sense and words 3 to 6 the world-wide name, assembled most significant word
first (*software expectation*). The driver checks the low nibble of word 0
against ``0xf``, the multi-host speed value, and otherwise tries to reprogram
the SEEPROM, which fails with a warning. It accepts a name that is non-zero,
not all ones and not the unprogrammed signature, and a port with an invalid
name is not brought up (*software expectation*). An invalid name only warns and skips the media-type read. The driver
does not store its loop host ID in the SEEPROM; the preferred address is a
driver tunable.

The model completes a read at once and returns the word from a 256-word
image, an *approximation*. The image reads ``0xffff`` except word 0, which
reads ``0x000f``, word 2, which reads 0, and words 3 to 6, which hold the
``wwn`` property. The property defaults to ``0x2100000000000001``, a
placeholder: no source gives a board's names, and the board gives its two
ports consecutive names. Any other opcode is logged, clears the busy bit and
leaves the data bytes alone. Whether the sequencer must be paused is not
enforced.

The private loop
================

The controller keeps a private arbitrated loop inside the device, as the
ISP2200 does (:doc:`qlogic-isp2200`). Each SCSI device on the controller's
bus is a LUN of the loop port whose loop ID is its ``scsi-id``, 0 to 125. The
loop ID maps to an AL_PA through the standard table, loop ID 0 being
``0xef``, an *approximation*: the driver keeps its own table between AL_PAs
and target IDs (*software expectation*), and the table the real loop gave is
*unknown*. A device needs its ``port_wwn`` and ``wwn`` only for the accepts a
login gets. The controller's bus is named with the ``bus-name`` property.

Nothing of the Fibre Channel wire is modelled: no frames, arbitration or
login state. Devices do not migrate with commands in flight, an
*approximation*.

Model limits
============

- The LIP TCB's path, its page 1 selector and the position table it fills, the
  LIP-received and bypass loop events, and the driver's preferred address.
- Login service parameters other than the names, and the PRLI service byte.
- Which field the driver reads for the sequencer completion count at
  ``0x25`` bit 2, and the per-source posting masks.
- The width of the size at scratch offset ``0x48``, and when the done queue
  restarts.
- Hardware error status at ``0x23``.
- The PCI class code, the XIO widget identity and the Bridge revision.
- Commands in flight do not migrate.

Tests
=====

``tests/qtest/adaptec-aic1160-test.c`` runs the controller behind the
Bridge on the libqos head. It covers the identity, class, config space and revision
property, the two windows and the absence of the others, the reset values,
the register banks, the chip reset, host control, the download and its
read-back, the RAM capacity, the sequencer memory pages and RAM sweep, the self-test RAMs, the config collection, the
interrupt line with the master enable, the status mask, the loop event hook,
the status block, the SEEPROM name and other opcodes, and a doorbell with
the sequencer stopped. With a disk on the loop it runs INQUIRY, READ into
one buffer and into a descriptor list, WRITE with a read back, a short
transfer with its residual, a failing command with sense data and every
status code the model produces, an absent port, a missing LUN, PLOGI, PRLI
and ADISC accepts, a malformed login, held receive buffers, the LIP
request, the loop-position map, the whole loop bring-up, a done queue lapped
past its end with the consumer index, and a batch of commands posted
with one doorbell.
