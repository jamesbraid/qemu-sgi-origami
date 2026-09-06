==========================
QLogic ISP1040 SCSI adapter
==========================

The ISP1040 is an intelligent SCSI controller: vendor microcode on
an internal RISC processor performs selection, message and data
phases itself, and the host hands it whole commands as descriptors
in a memory ring and collects completions from a second ring. QEMU
(``hw/scsi/qlogic-isp.c``) implements what that microcode does --
the mailbox command set and the two rings -- and never executes
vendor code: guests download microcode at every attach, and the
model accepts the download, retains it, and answers checksum
verification honestly over what was actually sent. No vendor
firmware is present or required. Load-RAM and dump-RAM take the word
offset within the adapter in mailbox 1 and the word count in mailbox
4, with the host address in the usual four-mailbox split; the offset
advances as a 16-bit quantity and wraps within storage sized to that
range.

The register contract is driver-sourced -- no vendor programming
manual for this part exists. The Origami project tree's ISP1040
datasheet records the cross-check (Linux qla1280 against NetBSD isp
for the map, OpenBSD 7.9 qlw for behavior); no vendor or driver code
is reproduced.

Identity and revisions
======================

One PCI identity, ``0x1020``, answers for the ISP1020 and ISP1040
members of the family; the configuration revision byte selects the
part -- 5, the ISP1040C, matching a captured Origin 200 boot. The
ISP12160 answers a distinct identity, ``0x1216``, rather than a
revision of this one. A guest reads the revision from two places
numbered from different origins: OpenBSD and IRIX read the PCI byte
(and OpenBSD derives the transfer clock from it), Linux reads bus
configuration 0's low four bits, which run one ahead across the
family -- so revision 5 in configuration space is field value 6
there. The model derives one from the other rather than stating two
constants. Linux reads the bus-configuration-0 field for exactly one
decision -- detecting the ISP1040A's broken FIFO -- and stores nothing.

The 0x100-byte window of 16-bit registers decodes behind both BARs
-- OpenBSD tries the memory window first and falls back to I/O.
Unimplemented registers log and answer rather than bus-error: some
are later work, others (the GPIO/termination pair) a guest touches
without meaning anything by it. The window from 0x80 to 0xc0
presents either the SXP or the RISC bank, selected by bus
configuration 1 *bit 3* (a driver header defines a bit-0 constant it
never exercises); the banks are separate storage, so the observable
of the select working is the same offset holding two values.

The NVRAM interface bit-bangs a serial EEPROM whose data-in line the
model never asserts: every bit-banged read returns the same fixed
pattern, indistinguishable from an EEPROM whose contents fail
validation, and OpenBSD's fallback path prints that the NVRAM is
corrupt and proceeds on built-in defaults. A real Origin 200's EEPROM
contents were never sourced, so failing the way real silence would
is the only honest answer.

The ISP12160 and the family layout
==================================

The ISP12160 is a second member of this device family: two independent
SCSI channels behind the same mailbox and ring protocol, sharing the
ISP1040's firmware abstraction but differing in PCI identity, register
bank layout, RAM policy and channel count. QEMU keeps one abstract base
type for the family. Each part supplies its own record of values,
applied by that part's own class-init function; a part differs from
its sibling only where that record says so.

The record fixes the PCI device identifier and revision, the channel
count, whether bus configuration 0 reports a revision at all, whether
the bank window at ``0x80``-``0xc0`` is the ISP1040's two-way select or
the ISP1080 family's four-way select, whether a RAM command wraps at
the end of controller RAM or is rejected instead, whether a mailbox
submission arriving while the previous result is still held overwrites
that result or is refused, whether the A64 RAM load command ``0x0009``
is in the part's mailbox set, and the three about-firmware words
``0x0008`` returns. The ISP1040 answers PCI identity ``0x1020``,
revision ``0x05``, one channel, a revision through bus configuration 0,
the two-way bank, wrapping RAM, an overwritten mailbox result, no A64
RAM load, and firmware version 1.0.0. The ISP12160 answers PCI identity
``0x1216``, revision ``0x06``, two channels, no revision through bus
configuration 0, the four-way bank, non-wrapping RAM, a refused mailbox
submission, the A64 RAM load command, and firmware version 10.4.0,
matching the selected IRIX ISP12160 program. Vendor identity ``0x1077``
and the SCSI storage class are common to both.

Bus configuration 1 bits 9 and 8 are the ISP1080 family's bank
selector: value 0 picks the RISC bank, matching the ISP1040's own bank
0, and values 1, 2 and 3 pick SXP0, SXP1 and DMA. Each bank is separate
32-word storage behind the same ``0x80``-``0xc0`` window the ISP1040
presents its two banks through, so the observable of the select working
is that offset holding four different values instead of two. The
ISP12160 stores bus configuration 1 whole -- all sixteen written
bits, reset to zero -- rather than the ISP1040's FIFO threshold and
burst enable bits, because the ISP1040's field meanings belong to a
register the ISP1080 family repurposes for bank selection. The flat DMA
configuration and control registers at ``0x20``, ``0x22``, ``0x40`` and
``0x42``, which the ISP1040 decodes directly, are unimplemented on the
ISP12160: reads answer zero and log, writes are ignored and logged,
because the banked layout replaces those registers rather than adding
to them.

The ISP12160 owns two SCSI buses where the ISP1040 owns one. Every
request's device word and every mailbox channel selector carries a
channel bit -- bit 15 of the word, equivalently bit 7 of the target
byte at request offset ``0x09`` -- that picks between them; target and
LUN numbers are local to the channel they arrive on. Each bus carries
its own channel number stamped into it, and again into every SCSI
device plugged onto it, so that QEMU's own address bookkeeping -- which
otherwise assumes channel zero -- sees the same target number on two
different addresses, one per channel, instead of a collision; the same
stamp keeps the two channels' devices on distinct migration paths.

The request ring and the response ring belong to the adapter, not to a
channel: one pair of ring bases and one pair of ring cursors serves
both buses, and the channel bit inside each entry routes that entry to
the right one. The mailbox commands that initialise a queue carry no
channel selector, so a queue pair per channel is not something the
protocol can express; a guest that initialised two ring pairs expecting
both to remain live would find the second initialisation replacing the
first.

Controller RAM is 65,536 16-bit words on both parts, cleared on device
reset and on soft reset. The ISP1040 folds a RAM command whose range
runs past the end of that array back to the start; the ISP12160 does
not. A range whose widened start plus count exceeds 65,536 is rejected
with parameter error ``0x4006`` and nothing is written. The whole
transfer is read from host memory before any word of it is kept, so a
DMA read the fabric refuses is rejected with host interface error
``0x4002`` without committing a prefix into controller RAM -- a
committed prefix would make the guest's own checksum blame the image
for bytes the model invented rather than bytes the guest sent.

A mailbox result and its pending interrupt persist until the guest
acknowledges, in either order, by clearing the semaphore and issuing
the RISC-interrupt-clear host command. On the ISP1040 a second
submission arriving while that result is still held simply overwrites
it. On the ISP12160 it does not: the submission is refused, logged as a
guest error, and the held result is left in place, so a command's
answer cannot be lost under a second command the guest had no business
sending before acknowledging the first.

The branch implements four recovery mailbox commands: command abort
``0x0015``, abort device ``0x0016``, target reset ``0x0017``, and bus
reset ``0x0018``. Command abort takes the channel in mailbox 1 bit 15,
the target and LUN in the rest of mailbox 1, and the handle split
across mailboxes 2 and 3, and cancels the one outstanding command
identified by that handle on that channel, target and LUN. Abort
device takes the same channel, target and LUN selector with mailboxes
2 and 3 zero, and cancels every outstanding request for that LUN.
Target reset takes the channel and target packed into mailbox 1's
upper byte and cancels every outstanding request to that target on
that channel, across LUNs. Bus reset takes the configured reset-delay
value in mailbox 1 and the channel directly in mailbox 2, and cancels
every outstanding request on that channel. A cancelled request retires
through the response ring like any other completion, carrying a
completion value distinct from the mailbox result the recovery command
itself returns: command abort and abort device retire their victims
with completion ``0x0005``; target reset and bus reset retire theirs
with completion ``0x0004``; state flags are zero either way, since no
further phase was reached after the cancellation. Command abort answers
command error ``0x4005`` for a handle that is unknown or does not match
the request found under that handle's channel, target and LUN, and
touches nothing. All four commands answer parameter error ``0x4006``
for a channel selector at or beyond the part's channel count. Abort
device completes ``0x4000`` when nothing is outstanding for the
selected LUN.

The branch carries the ISP12160's additional state in a vmstate
subsection, ``qlogic-isp/isp12160``, needed only when the part is
banked or carries two channels, so it never appears in an ISP1040
migration stream. The subsection carries the SXP1 and DMA banks the
ISP1040 does not have, and the per-channel reset-delay values that
mailbox command ``0x003a`` sets. The top-level ``qlogic-isp`` stream's
version becomes 9 with its minimum version still 6; the ISP1040's own
field list is unchanged, so an ISP1040 stream at any supported version
loads exactly as it did before the ISP12160 existed.

Several of the choices above are compatibility selections where the
driver-sourced evidence does not establish the hardware's own answer.
They are not claims about unobserved silicon:

- Bus configuration 0 reads 0 on the ISP12160, because no consumer
  evidence reads a revision from that offset on this part and the
  datasheet assigns it no other meaning.
- Bus configuration 1 stores all sixteen written bits on the ISP12160,
  because the datasheet leaves the writable mask open.
- Bank 0 of the four-way select is the RISC bank, matching the
  ISP1040's own bank 0; getting this wrong would cost a guest register
  contents it never reads back, since every bank is undecoded storage.
- The reset-delay setter places channel 0's value in mailbox 1 and
  channel 1's value in mailbox 2, following the family's pattern for
  its other dual-channel setters; nothing reads either value back, so
  only the vmstate fields and code review check that the two channels
  stay independent.
- The recovery retirement codes -- completion ``0x0005`` for an abort,
  ``0x0004`` for a reset, command error ``0x4005`` for an unknown or
  mismatched handle, parameter error ``0x4006`` for an out-of-range
  channel, and ``0x4000`` for an abort device with nothing outstanding
  -- are the selected scope for these commands, not values read back
  from a captured device.

Mailboxes and queue pointers
============================

Eight 16-bit mailboxes; mailbox 0 carries a command in and the
result out, the rest are per-command arguments. Mailboxes 4 and 5
double as the queue pointers once queues exist -- each address means
the host's pointer when written and the adapter's when read. The
request side is plain storage (the guest is the only writer). The
response side is not: a read answers the adapter's own producer
count directly, and the guest's acknowledgement write records the
consumer position without ever touching that count -- the aliasing
is exactly the reserve-one-slot hazard the datasheet warns about.
Before a queue is initialised both addresses are ordinary mailboxes,
which the firmware's walking-pattern register test requires. Queue
initialisation takes the entry count in mailbox 1 and a 64-bit base
split across mailboxes 2, 3, 6 and 7 -- OpenBSD's opcode 0x10 and
Linux's A64 opcode 0x52 are one operation -- and rejects a zero base
or count. The Linux-only parameter setters issued from
``qla1280_nvram_config()`` are accepted; their caller discards the
result.

After reset, mailboxes 1-4 present the product signature ("IS",
"P ", two spaces, and 0x0001). Linux's ``qla1280_chip_diag()`` reads
all four immediately after reset and aborts the attach on any
mismatch; a second independent implementation reproduces the same
values, which is what makes this a hardware fact rather than one
driver's habit. The signature is also the observable that proves
reset ran at all: most registers here reset to zero and say nothing.

Rings
=====

Queue entries are 64 bytes, byte offsets confirmed against packed
driver structures with no compiler padding. The type-0 request entry
and Linux's A64 entry (the only format Linux issues) share their
first 32 bytes field for field; only the segment area differs. A64
segments are two 32-bit words, low half first, then a length. An A64
continuation entry has no handle -- offset 4 is the low half of a
DMA address -- so continuations are never dispatched alone: the
owning command's entry count steps the ring walker past them.

The walk is driven by the count of published entries, decremented
per entry with each entry's claimed slot count clamped to it. This
terminates for every guest-supplied combination; an exact-equality
exit does not, because a stride sharing a factor with the ring
length steps over the producer index forever.

A status entry's state-flags word records which transaction phases
were reached; drivers read it only for incomplete completions, and
Linux's ladder turns it into which failure to report. The
status-flags word is distinct: IRIX accepts an incomplete completion
as a benign empty slot only when its selection-timeout bit is set,
a stricter check than any of the public parallel-SCSI drivers.
Sense-data validity rides in the SCSI status word's own bit 9. A
missing target is answered incomplete with bus-reached plus
selection-timeout -- the one answer all four guests read as an empty
slot rather than a fault.

Completions that find the response ring full are queued and posted
as the guest's acknowledgements free slots; real hardware never
reaches that case (it completes one command at a time with real bus
latency), but a model with none can. Transfers the fabric refuses
are dropped, never guessed at: a refused CDB read drops the request
with the consume index left on the entry, a refused status store
neither advances the produce index nor raises the interrupt, and
each drop is logged with its entry address.

The reported request-queue capacity is 512: no source states a real
adapter's number, and OpenBSD clamps whatever is reported to 512, so
this exercises the ring arithmetic at its actual ceiling.

Interrupts
==========

One level interrupt line, recomputed from scratch on every change.
Only the RISC interrupt request bit and its own enable drive the
physical line; the plain request bit and the global enable beside it
are documented without observable effect, and a second independent
implementation states outright that they do nothing on real
hardware.

Reset and migration
===================

Power-on, guest soft reset (a trigger bit the guest polls to
completion) and the bus-reset mailbox command all land on one reset
state, and both guest-visible resets cancel in-flight commands
through the SCSI core. Reset values other than the signature are
zero under the standard for an unconfirmed value: no driver source
contradicts them.

In-flight requests migrate with the SCSI core's request stream; the
segment count read back on load is bounded by the entry format's
ceiling and a stream claiming more fails the migration. Held
completions do not migrate -- the condition that creates them is one
real hardware never reaches.
