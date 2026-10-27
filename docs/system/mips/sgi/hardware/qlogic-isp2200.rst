===========================================
QLogic ISP2200 Fibre Channel host interface
===========================================

The ISP2200 is a single-chip Fibre Channel host adapter: an on-chip
RISC processor running downloaded firmware, a Fibre Channel protocol
manager and a PCI bus-master DMA engine. The host talks to the firmware
through 24 16-bit mailboxes and through two rings of 64-byte entries in
host memory. ``hw/scsi/qlogic-isp2200.c`` models that host interface
as the PCI device ``qlogic-isp2200``.

As with the SCSI parts (see :doc:`/system/mips/sgi/hardware/qlogic-isp`), the model does not
execute vendor firmware. It implements the mailbox commands and the
rings that firmware provides, accepts and keeps the guest's download,
and answers the checksum question over the words actually sent. No
vendor firmware is present or required, and a successful download does
not show that a guest's firmware would work on the real part.

No chip programming manual for the ISP2200 was found. Every register,
mailbox and ring fact below comes from the public drivers that run
against real adapters: Linux ``drivers/scsi/qla2xxx``, OpenBSD
``sys/dev/ic/qla.c`` with ``sys/dev/pci/qla_pci.c``, and NetBSD
``sys/dev/ic/isp.c`` with ``sys/dev/pci/isp_pci.c``. Where at least two
of them agree the fact is treated as settled. Where they are silent or
disagree the model makes a choice, and this page calls that choice an
approximation. The QLogic *Hardware Installation Guide for the QLA2200
Board Family* (FC0151103-00 D) gives the board's 128 KiB of SRAM, its
256-byte NVRAM and its use of interrupt line A; it has no register
content.

Service boundary
================

The adapter has two halves. The host interface is PCI, registers,
mailboxes, semaphore, interrupt, controller RAM, the initialisation
control block, the rings and their entry formats, and the delivery of
asynchronous events. The Fibre Channel side -- firmware state, loop,
port database and whatever answers behind a loop ID -- is a loop
service that the host interface reaches in four places: the Get
Firmware State answer, the submission of one SCSI command, the loop
mailbox commands, and the events the service posts. The service is a
private loop kept inside the device; see `Private loop`_.

For qtests, the device records what reached the loop service. It
exposes two read-only QOM properties that are not part of the guest
interface and are not migrated: ``x-fc-submissions`` counts submitted
commands, and ``x-fc-last-submission`` describes the last one (handle,
loop ID, LUN, control flags, CDB, byte count and segments). Both reset
with the device.

PCI identity
============

Vendor ``0x1077``, device ``0x2200``. The drivers match on those two
alone. Class code ``0x0c04`` (Fibre Channel) and the subsystem pair
``1077:0002`` (the retail QLA2200 entry in the PCI ID list) are model
choices; no driver reads them. The revision byte is 0. The adapter has
one INTx line on pin A and no power-management capability. BAR 0 is a
0x100-byte I/O window and BAR 1 a 0x100-byte 32-bit memory window onto
the same registers. The board's flash BIOS ROM is not modelled, so there
is no expansion ROM BAR. Every adapter DMA goes through the function's
PCI address space and depends on the bus-master enable: with it clear a
transfer that would start is refused and logged.

Registers
=========

All drivers use 16-bit accesses, little-endian on the bus. A 32-bit
access is logged and answered as a 16-bit access at its address; byte
accesses are refused by the memory core. Whether the chip takes either
is unknown.

========  ========================  ==================================
Offset    Register                  Model
========  ========================  ==================================
``0x00``  Flash BIOS address        Reads 0; writes logged
``0x02``  Flash BIOS data           Reads 0; writes logged
``0x06``  Control/status            Bit 0 soft reset; bits 5:4 bank
``0x08``  Interrupt control         Bits 15 and 3, the two enables
``0x0a``  Interrupt status          Bit 3 RISC latch; bit 15 the line
``0x0c``  Semaphore                 Bit 0 lock
``0x0e``  NVRAM                     Serial EEPROM port
``0x10``  Mailboxes 0-7             2 bytes apart
``0x20``  Undescribed               Reads 0; writes logged
``0x80``  Banked window             Reads 0; writes logged
``0xc0``  Host command/control      Command in, RISC state out
``0xcc``  GPIO data, enable         Storage only
``0xe0``  Mailboxes 8-23            2 bytes apart
========  ========================  ==================================

Only Linux places mailboxes 8 to 23; the count of 24 comes from both
Linux and NetBSD. Linux writes all 24 on every command, so the model
takes writes to every one. The registers no consumer gives a meaning --
the range Linux leaves undescribed, the window behind the bank select,
flash -- log the access, read 0 and ignore writes rather than bus-error,
because NetBSD writes its DMA-engine controls at ``0x20``, ``0x40`` and
``0x60`` after a soft reset and OpenBSD writes the banked window during
its own reset. The CSR's flash bits are not modelled: Linux and NetBSD
disagree on which bit selects the flash bank.

The NVRAM data-in line never carries data, so a bit-banged read
returns zeros and fails the drivers' ``ISP`` signature check. That is
what an unprogrammed part looks like, and both drivers fall back to
built-in defaults. The board's real NVRAM contents are unknown.

Reset and RISC control
======================

No source states the RISC or mailbox state after any reset; every row
of this table is a model choice consistent with the three drivers, each
of which follows a soft reset with an explicit RISC reset and release.

================================  ===========  ======================
Event                             RISC         Mailboxes
================================  ===========  ======================
Device reset (power-on, PCI)      Running ROM  Product-ID image
Soft reset (CSR bit 0)            Held         m0 ``0x0004``, rest 0
HCCR reset (``0x1000``)           Held         m0 ``0x0004``, rest 0
HCCR release from reset           Running ROM  Product-ID image
HCCR pause (``0x2000``)           Paused       m0 ``0x0004``, rest kept
HCCR release from pause           Running      m0 0, or the result of
                                               a command that waited
================================  ===========  ======================

The product-ID image is m1..m3 = ``0x4953 0x5020 0x2020`` ("ISP  ")
and m7 = ``0x0004``, the ROM revision Linux treats as the ISP2200A;
every other mailbox reads 0. OpenBSD reads the image at attach before
it resets anything, so a device reset leaves it showing.

Both resets that hold the RISC also discard the downloaded firmware,
the stored firmware options, both rings and everything waiting to be
posted. Every supported driver downloads again after a reset; a guest
that expects firmware to survive one is not supported. A soft reset
also clears interrupt control, the semaphore, the NVRAM port and both
interrupt latches. The CSR reset bit reads set on the next register
read and clear after it.

The host command register takes a command in bits 15:12. ``0x5000``
rings the doorbell and sets the host-interrupt bit (bit 7) until
``0x6000`` clears it; ``0x7000`` clears the RISC interrupt latch.
``0x4000`` and ``0x4001``, which OpenBSD, NetBSD and Linux write between
reset and release, are logged and change nothing, nor do the parity,
breakpoint, BIOS and test commands. A read returns bit 7, bit 6 (held
in reset) and bit 5 (paused). The model never pauses on its own.

Mailboxes and the command window
================================

The host writes the inputs, mailbox 0 first, and rings the doorbell.
The command runs then, never on a mailbox write. The model keeps an
input bank, which commands read, and an output bank, which the host
reads; a host write lands in both, so a mailbox reads back until a
result replaces it, and an input the host does not rewrite keeps its
last value. A result rewrites the whole output bank: mailboxes the
command does not name read 0.

Mailboxes 4 and 5 are also the request and response queue pointers once
INIT_FIRMWARE has built the rings, and the drivers write them for three
reasons: as command inputs (Linux writes 0 there with every command),
as ring pointers with no doorbell, and as resets of the pointers. The
model tells them apart by a *command window*: a write to mailbox 0
opens it, and the doorbell or any reset closes it. With the rings built
and the window closed, a write to mailbox 4 is the request producer and
a write to mailbox 5 the response consumer; otherwise both are plain
mailbox storage, which is what Linux's register test needs before the
rings exist. A pointer value at or past the ring length is logged and
ignored. A read of mailbox 4 or 5 returns request-out and response-in
while the semaphore is free, and the result's data while it is held.

This rule is an approximation that needs only the order of register
accesses. It holds for Linux, which writes the mailboxes and rings the
doorbell under the lock its ring paths also take. OpenBSD writes ring
pointers without that lock, so a pointer write can land between a
command's mailbox 0 write and its doorbell; the model takes it as
storage. A lost producer write publishes nothing until the next one; a
lost response-out write leaves the response ring looking fuller than it
is, and a completion held behind it waits. What the real part does is
unknown.

The mailbox command set
=======================

Before Execute Firmware the ROM answers No-op (``0x00``), Execute
(``0x02``), Write and Read RAM word (``0x04``, ``0x05``), the mailbox
register test (``0x06``), Verify Checksum (``0x07``), About Firmware
(``0x08``) and the two RAM loads (``0x09``, ``0x0b``). Every other
command answers ``0x4001`` until Execute; which others a real ROM
takes is unknown.

=========  =====================  ======================================
Command    Name                   Model
=========  =====================  ======================================
``0x00``   No-op                  ``0x4000``
``0x02``   Execute firmware       Marks the firmware running; runs nothing
``0x04``   Write RAM word         m1 word address, m2 data
``0x05``   Read RAM word          m1 word address; data in m2
``0x06``   Register test          Echoes m1..m7
``0x07``   Verify checksum        See below
``0x08``   About firmware         m1..m3 version 1.0.0, m6 ``0x0002``
``0x09``   Load RAM               m1 word address, m4 count, DMA
``0x0b``   Load RAM extended      As ``0x09``; nonzero m8 is ``0x4006``
``0x1f``   Firmware status        m2 = 512 commands
``0x28``   Get firmware options   The stored m1..m3, zero after reset
``0x38``   Set firmware options   Stores m1..m3; changes nothing else
``0x60``   Initialise firmware    See below
``0x69``   Get firmware state     m1 the loop state, see `Private loop`_
=========  =====================  ======================================

The DMA address of the RAM load and of INIT_FIRMWARE is split m2 =
bits 31:16, m3 = bits 15:0, m6 = bits 63:48, m7 = bits 47:32. The load
reads the whole transfer as little-endian 16-bit words before it keeps
any of them, so a refused DMA (``0x4002``) leaves RAM unchanged; a range
past the 64K-word RAM is ``0x4006``. Verify Checksum takes word 3 of the
image at m1 as its length, as all three loaders do, and sums that many
words: zero is ``0x4000`` with the sum in m1 and m2, anything else
``0x4003``, and a header or length past RAM ``0x4006``. The real
algorithm is unknown; this follows the SCSI parts. The firmware version
is a model constant, not a vendor release, and the attributes report
expanded-LUN addressing only.

Abort device (``0x16``), bus reset (``0x18``), target reset
(``0x66``), get retry count (``0x22``), resource counts (``0x42``),
stop firmware (``0x14``) and the SCSI parts' queue setup (``0x10``,
``0x11``) answer ``0x4001`` and are logged as unimplemented; neither
driver issues the first three. The loop and recovery commands are under
`Private loop`_.
Unknown commands answer ``0x4001`` and are logged as guest errors.

Initialisation control block
============================

INIT_FIRMWARE reads the 96-byte, version-1 block by DMA. The model
takes the request and response ring lengths at offsets 36 and 38 and
their 64-bit bases at 40 and 48, keeps the rest for the Fibre Channel
service, sets all four ring pointers to 0 and marks the rings built.
It refuses with ``0x4002`` a block it cannot read, and with ``0x4006``
a version other than 1, a zero length, a zero base or the fast-post
option, which would need completion events this model does not post.
A reduced-interrupt timer mode -- Linux asks for mode 4 on this part --
is accepted, logged as unimplemented and ignored: every completion is an
ordinary status entry. Re-initialising drops completions still held for
the previous rings and cancels, without status, every command still in
flight, whose completion would otherwise land in the new rings under a
stale handle. It also restarts the loop.

Rings and entries
=================

Entries are 64 bytes with little-endian fields. The adapter consumes
the request ring as fast as the host publishes, so the guest never sees
it full. The request types modelled are the 32-bit command (``0x11``,
three 8-byte segments), the 64-bit command (``0x19``, two packed 12-byte
segments), their continuations (``0x02``, seven segments from byte 8;
``0x0a``, five from byte 4), OpenBSD's segment-list command (``0x15``,
a pointer at byte 46 to a list of 12-byte segments) and the marker
(``0x04``), which is consumed and answers nothing. A command's entry
count says how many slots it takes: a 64-bit continuation has no
handle, so continuations are found by count, never dispatched alone. A
command whose continuations are not all published yet waits for them.

The command's target is the high byte of the field at offset 8 and its
LUN the 16-bit field at offset 10, the expanded-LUN layout both Linux
and OpenBSD build once About Firmware reports it.

A malformed request comes back on the response ring as itself with an
entry-status flag set: bit 2 for an unknown type, bit 3 for more
segments than the entries hold, bit 4 for an impossible entry count,
bit 5 for a continuation where a command belongs or the wrong kind of
continuation, bit 6 for a refused read of a continuation or of a
``0x15`` segment list. A rejected command takes the slots its entry
count claims, or one slot when that count is itself invalid, and never
reaches the Fibre Channel service. Which flag a real firmware sets for
each is unknown.

A status entry (``0x03``) carries the handle at offset 4, SCSI status
at 8, completion status at 10, sense length at 18, residual at 20 and up
to 32 sense bytes from 32. Every one of its 64 bytes is written, because
Linux finds new entries by a signature it writes over the last four
bytes of each one it has processed. The state and status flag words are
0; no status continuation is ever posted.

Posting and interrupts
======================

A mailbox result or event writes the output bank, takes the semaphore
lock and sets the RISC interrupt latch. The interrupt line is asserted
while the latch is set and both interrupt-control enables are set. A
write of 0 to the semaphore releases it; a write of anything else is
logged and changes nothing, and bit 1 reads 0.

Nothing is posted while the latch is set, the semaphore is held or the
RISC is paused or in reset. Linux walks the response ring, writes the
consumer, then clears the latch, so an entry written in that window
would lose its interrupt; holding it until the clear is the model's
answer. Completions wait in arrival order and post as one batch, as many
as the ring has room for, with response-in advanced once and the latch
set once. A waiting mailbox result posts before any response entry, and
alone. The model re-checks on a completion, on ``0x7000``, on a
semaphore release, on a response-out write and on release from pause.
A real part may write entries under a set latch; this costs at most one
interrupt round trip.

No mailbox result or event is dropped or overwritten. One command
completion waits behind a posted result, ahead of any queued event. A
doorbell that finds that completion still waiting is not run yet: it
stays latched, HCCR bit 7 still reads set, and the command runs once the
waiting completion has posted, on the mailbox inputs as they are then.
The latch is a single bit, so ringing again before the command runs
runs one command, not two; the second ring is logged. Response entries
wait up to the 512-command budget Firmware Status announces, at which
point the adapter stops consuming requests until the host makes room.

The host interface raises two events of its own, with m1..m3 zero:
``0x8003`` when a request-ring read is refused (the entry stays
unconsumed until the next producer write) and ``0x8004`` when a
response-entry write is refused (the entry is dropped). Such an event
adds nothing while an identical one is still queued: the queued one
already reports the failure. A guest that keeps retrying a refused ring
therefore sees one event per release, and the queue cannot overflow.

Private loop
============

Firmware hides Fibre Channel from the host, which sees mailboxes and
rings and never a frame. The model therefore keeps a private
arbitrated loop inside the device rather than a Fibre Channel
subsystem. Frames, CRC, exchanges, ordered sets, the loop
initialisation phases, port and process login, fabric services,
point-to-point, link speed and link timing are not modelled. Everything
below is a reading of what the public drivers expect -- mainly OpenBSD
``qla``, with Linux ``qla2xxx`` where it agrees -- and not firmware
evidence.

Ports
-----

The adapter has one SCSI bus, ``<id>.0``, taking SCSI targets 0 to
0x7d and LUNs 0 to 255; both drivers treat loop IDs 0 to 0x7d as loop
ports and 0x7e up as fabric and special ports. A device at target *n*
is a LUN of the loop port at loop ID *n*. The port's names are the
device's ``port_wwn`` and ``wwn`` properties, which ``scsi-hd`` also
reports in its device identification page, so one identity serves the
loop and the disk. A plug is refused unless the device gives an
explicit ``scsi-id``, both names are nonzero, every LUN at that loop ID
carries the same two names, no other loop ID has the same port name and
the loop ID is not the adapter's own. Every port is logged in and has
the FCP target role; the loop holds no initiators and no ports that
refuse login.

A port's AL_PA follows from its loop ID by the standard table, ``0xef``
at loop ID 0 down to ``0x01`` at 0x7d (NetBSD ``sys/dev/ic/isp.c``
gives the same table; the value set matches the AL_PA list in Emulex
publication 860-0227-001, which does not give the order). A private
loop has domain and area 0, so a port ID is its AL_PA alone.

The adapter's own loop ID is chosen when initialise-firmware runs. It
takes the control block's hard address (offset 20) when option bit 0
asks for it, the value is at most 0x7d and no port holds it; otherwise
the lowest loop ID no port holds. Real loops settle this during loop
initialisation; which address a port wins there is unknown. The choice
holds until the next initialise-firmware or reset. With every loop ID
taken the adapter has none, and the loop never comes up.

Loop state
----------

Get Firmware State reports 0 (configuration wait) from reset until
initialise-firmware, 1 (waiting for AL_PA) while the loop comes up, and
3 (ready) after. The other states are never reported. The loop comes up
a fixed 100 ms of virtual time after initialise-firmware, a host LIP
(``0x62`` or ``0x72``), a plug that makes a new port or the unplug of a
port's last LUN. The time is a model constant chosen so a test can see
state 1; real loop initialisation time is unknown. A new trigger
restarts the wait.

Extended options bits 6:4 (control block offset 64) choose the
connection. Loop only (0), loop preferred (2) and point-to-point
preferred (3) bring up the loop; with no point-to-point partner mode 3
falls back to loop. Point-to-point only (1) and the undefined values
leave state 0, post nothing and log a guest error.

When the loop comes up the model posts, in order: LIP occurred
``0x8010``, loop up ``0x8011``, and, when control block option
``0x0100`` is set, port database changed ``0x8014`` with m1 ``0xffff``,
which Linux reads as a change to every port. Every other payload word
is zero; nothing public gives them a meaning on this part. The real
sequence and any coalescing are unknown. Loop down ``0x8012`` is never
posted: the model has no link to lose. A trigger takes back every event
an earlier bring-up queued and has not yet posted, so the queue holds at
most one bring-up's three; an event already posted stays until the host
releases it. With the two events the host interface raises itself, at
most five events ever wait, and the eight-entry queue cannot overflow.

Loop commands
-------------

=========  =====================  ======================================
Command    Name                   Model
=========  =====================  ======================================
``0x20``   Get adapter ID         Ready: m1 loop ID, m2 AL_PA (area 0
                                  above it), m3 domain 0, m6 topology 0
                                  (private loop), m7 0. Not ready:
                                  ``0x4005`` with m1 7, the value Linux
                                  reads as a loop in transition
``0x62``   Initiate LIP           ``0x4000`` and the loop comes up again;
``0x72``   Full login LIP         ``0x4005`` before a loop exists
``0x63``   Position map           Mapping D, 128 bytes: the port count
                                  including the adapter, the adapter's
                                  AL_PA, then the ports' in loop ID
                                  order, then zeros
``0x64``   Get port database      m1 loop ID in its high byte, mapping
                                  D, 128 bytes (below)
``0x75``   Port name list         m1 options, mapping D: 10 bytes per
                                  port, m1 the byte count
``0x7c``   Get ID list            Address m1 31:16, m2 15:0, m3 63:48,
                                  m6 47:32: 4 bytes per port, m1 the
                                  count
=========  =====================  ======================================

The discovery commands answer ``0x4005`` until the loop is ready, and
Get Port Database also for a loop ID with no port, the adapter's own or
one past 0x7d. Port name list options other than bits 1 and 3, the two
OpenBSD sets, are ``0x4006``. A refused DMA is ``0x4002``. The public
definitions name these codes but not which refusal returns which, so
each is a model choice. The lists leave the adapter out, run in
ascending loop ID and write nothing past their last entry; whether real
firmware lists itself, and in which order, is unknown. An ID list entry
is the AL_PA, area 0, domain 0 and loop ID. A port name list entry is
the port name with its wire-order bytes reversed, then the loop ID as a
little-endian word: OpenBSD decodes that name as a little-endian integer
but the port database's as big-endian, and compares the two. The real
byte order is unknown.

The port database is 128 bytes, zero but for: bytes 2 and 3, master
and slave state 6 (logged in); byte 6, the hard address, the port's
AL_PA; bytes 8-11, the port ID as domain, a zero byte, AL_PA and area;
bytes 12-19 the node name and 20-27 the port name, in wire order; byte
78, ``0x10``, the target role both drivers find in the low byte of
service parameter word 3; and bytes 80-81 the loop ID, little-endian.
Byte 0 (options) is 0, class 3 only.

The enhanced port database (``0x47``), the login, logout, name-server,
link-status and loop-port commands (``0x40``, ``0x41``, ``0x6a``,
``0x6b``, ``0x6e``, ``0x6f``, ``0x71``, ``0x74``) answer ``0x4001`` and
are logged as unimplemented. Linux's request-ring mailbox entry
(``0x39``) is an unknown type and returns with entry status bit 2.

Commands on the loop
--------------------

A command entry's loop ID and LUN select a device on the bus. A loop ID
with no port, the adapter's own or one past 0x7d completes with
``0x0028`` (port unavailable) and no SCSI status. A port without that
LUN answers through QEMU's target-level handling, as an FCP target
with a missing LUN would: REPORT LUNS lists the port's LUNs, INQUIRY
reports peripheral qualifier 3 and anything else is CHECK CONDITION,
ILLEGAL REQUEST. A LUN past 255 gets that CHECK CONDITION without
reaching a device. Data moves between the segments and the device
through the function's PCI address space.

Completion precedence follows what the two drivers decode, not firmware
evidence. A SCSI status other than GOOD completes ``0x0000`` with the
status in the low byte, bit 9 and the sense bytes when the device gave
sense, and bit 11 with the residual when the transfer was short. GOOD
and short is underrun ``0x0015`` with bit 11 and the residual. GOOD and
whole is ``0x0000`` with no flags. A transfer is short when fewer bytes
moved than the larger of the segments' total and the command's own
length. At most 32 sense bytes are returned; if they would end in
``0xdeaddead``, the mark Linux writes over entries it has processed,
the last four are dropped and the cut is logged.

Entry timeouts are ignored: a command completes when its device does.
Markers are consumed without a response, and the model does not refuse
commands to a target until a marker arrives. Commands to present ports
run while the loop is coming up. With no loop -- the connection mode
left it down, or every loop ID was taken -- every command completes
``0x0028`` with no SCSI status and reaches no device.

A data transfer the host bridge refuses, for read data or for write
data, is logged and completes the command ``0x0002`` (DMA error) with
no SCSI status. A write shorter than its command, which otherwise goes
to the device through QEMU's DMA helpers, takes the buffered path when
a segment would be refused, so its refusal is reported the same way.

Recovery and disconnect
-----------------------

The recovery commands mark every command they select with the completion
it owes, cancel each, and post that completion with the command's own
handle, whether the cancellation or the device finishes it. The
completions are the public definitions and the reset semantics follow
the SCSI parts' (see :doc:`/system/mips/sgi/hardware/qlogic-isp`); what
a real ISP2200 does, including any frames it sends the target, is
unknown. Each result posts before the entries its command retired.

=========  =====================  ======================================
Command    Name                   Model
=========  =====================  ======================================
``0x15``   Abort command          m1 loop ID in its high byte, m3:m2 the
                                  handle, m6 the LUN: that one command
                                  completes ``0x0005``. ``0x4005`` when
                                  none matches
``0x17``   Abort target           m1 loop ID in its high byte: every
                                  command there completes ``0x0004`` and
                                  every LUN there is reset
``0x7e``   LUN reset              m1 loop ID in its high byte, m2 the
                                  LUN: as abort target, for that LUN
``0x6c``   LIP reset              m1 loop ID in its high byte, ``0xff``
                                  for all: as abort target, then the
                                  loop comes up again; ``0x4005`` before
                                  a loop exists
=========  =====================  ======================================

Abort reads the handle bytes of each command entry as a little-endian
word, like the entry's other fields, and compares it with m3:m2, the
halves Linux passes. A big-endian Linux guest stores its handle most
significant byte first, so its aborts match nothing and fail; how real
firmware compares the two is unknown. A reset device reports the reset
as a unit attention on its next command.

A LIP reset's bring-up posts no LIP occurred event. With bit 1 of the
set-firmware-options m1 set it posts LIP reset ``0x8013`` in its
place; without it, only loop up and, with option ``0x0100``, port
database changed. That bit's meaning comes from the OpenBSD macro name
alone, and what real firmware reports here is unknown.

A port leaving the loop is not a device leaving a SCSI bus: both
drivers expect its commands to complete. Unplugging a device first
completes every command on it with ``0x0029`` (port logged out). If
that was the port's last LUN the port leaves the loop, which comes up
again without it, and later commands to the loop ID are port
unavailable; another LUN's unplug runs no bring-up.

A soft reset, PCI reset or system reset cancels every command without
status and resets the loop's devices; an HCCR RISC reset cancels them
without resetting the devices. Each also drops the loop: state 0, no
loop ID, the bring-up stopped and its events gone. A cancelled command
moves no more data, including one whose device finishes while the
reset waits on another command's cancellation; so does one cancelled
by a second initialise-firmware.

Migration
---------

The stream carries the loop state, the adapter's loop ID, the running
bring-up timer and, through the SCSI layer's request hooks, every
command in flight with its loop ID, LUN, segments and progress. The
loop itself is not in the stream: it is the devices on the bus, which a
matching configuration makes identical, and a load whose adapter loop
ID one of them holds fails. So does one whose queued events this build
could not have queued -- more than five, a repeated code, or more than
one bring-up's LIP event -- or whose commands in flight and held
entries together pass the 512-command budget.

Limitations
===========

- Abort matching assumes a little-endian handle, so a big-endian
  mainline Linux abort never matches.
- The loop is an approximation shaped by two public drivers, not a
  model of ISP2200 firmware. No frames, logins, fabric, point-to-point,
  link speed or link timing; no initiators or failing ports; no loop
  down or cable removal.
- Linux's request-ring mailbox entries are not served, so mainline
  Linux, which logs in through them, cannot bring a port online.
- ``scsi-hd`` tags the port name in its device identification page
  with the SAS protocol identifier; the value matches the loop's, the
  protocol does not.
- No reduced-interrupt or fast-post completions, target mode, IP,
  2300-class layouts, flash BIOS, frame-buffer or FPM banks, parity
  diagnostics or single step.
- The reset states, the command window, the posting window, the
  firmware identity and the per-command error codes are model choices
  the drivers leave open, as stated above.
- The ISP2200 and ISP2200A differ only in mailbox 7 here; the model
  reports the 2200A value.
- Migration is between identical builds only.
- A write shorter than its command is checked against the host bridge
  once, when the command is queued to its device. QEMU's DMA helpers do
  not report a refusal: a segment they cannot map directly goes through
  a bounce buffer whose failed read is ignored. A refusal that begins
  after the check would therefore not be seen, and the write would
  reach the disk with whatever that read left and complete normally.
