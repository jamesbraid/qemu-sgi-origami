=========================================
QLogic ISP1040 and ISP12160 SCSI adapters
=========================================

The ISP1040 and the ISP12160 are two members of one family of
intelligent SCSI controllers: vendor microcode on an internal RISC
processor performs selection, message and data phases itself, and the
host hands the adapter whole commands as descriptors in a memory ring
and collects completions from a second ring. One QEMU model
(``hw/scsi/qlogic-isp.c``) covers both parts and this page does the
same, naming the part wherever the two differ. The model implements
what that microcode does -- the mailbox command set and the two rings
-- and never executes vendor code: guests download microcode at every
attach, and the model accepts the download, retains it, and answers
checksum verification honestly over what was actually sent. No vendor
firmware is present or required. On the ISP1040, load-RAM and
dump-RAM take the word offset within the adapter in mailbox 1 and
the word count in mailbox 4, with the host address in the usual
four-mailbox split, and that offset advances as a 16-bit quantity and
wraps within storage sized to that range. The ISP12160 carries the
same load under a distinct A64 opcode, and the model carries no dump
for it; a range that would wrap is refused instead.

The register contract is driver-sourced for both parts -- no vendor
programming manual for either exists. The Origami project tree carries
a datasheet per part recording the cross-check: Linux qla1280 against
NetBSD isp for the map, with OpenBSD 7.9 qlw for ISP1040 behavior and
a clean-room review of the selected IRIX guest for the ISP12160. No
vendor or driver code is reproduced.

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
is in the part's mailbox set, whether the three abort mailboxes
``0x0015``-``0x0017`` are, and the three about-firmware words
``0x0008`` returns. The ISP1040 answers PCI identity ``0x1020``,
revision ``0x05``, one channel, a revision through bus configuration 0,
the two-way bank, wrapping RAM, an overwritten mailbox result, no A64
RAM load, no abort mailboxes, and firmware version 1.0.0. The ISP12160
answers PCI identity ``0x1216``, revision ``0x06``, two channels, no
revision through bus configuration 0, the four-way bank, non-wrapping
RAM, a refused mailbox submission, the A64 RAM load command, the abort
mailboxes, and firmware version 10.4.0, matching the selected IRIX
ISP12160 program. Vendor identity ``0x1077`` and the SCSI storage class
are common to both.

Bus configuration 1 bits 9 and 8 are the ISP1080 family's bank
selector: value 0 picks the RISC bank, matching the ISP1040's own
bank 0, and values 1, 2 and 3 pick SXP0, SXP1 and DMA. Each bank is
separate 32-word storage behind the same ``0x80``-``0xc0`` window
the ISP1040 presents its two banks through, so the observable of the
select working is that offset holding four different values instead
of two. The ISP12160 retains only bits 9 and 8 of bus configuration
1 -- the bank selector -- reset to zero. Other written bits are
logged as unimplemented and read back as zero, because the datasheet
establishes the bank selector and leaves the rest of the writable mask
open. The flat DMA configuration and control registers at ``0x20``,
``0x22``, ``0x40`` and ``0x42``, which the ISP1040 decodes directly,
are unimplemented on the ISP12160: reads answer zero and log, writes
are ignored and logged, because the banked layout replaces those
registers rather than adding to them.

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

The checksum-verification command reads that same array without a DMA
transfer, so the same range rule applies directly: on the ISP12160 it
refuses, with parameter error ``0x4006`` and the sum mailboxes left
untouched, a header word or a summed span that would pass the end of
controller RAM, instead of wrapping as the ISP1040 does.

The two parts move those words in different cycles. The ISP1040 has a
16-bit data path and runs one cycle per word in both directions; the
ISP12160 moves the whole block in a single transfer. The distinction is
kept rather than collapsed onto one width because the width a part
transfers in belongs to the part. The fabric between the adapter and
memory does state its byte-lane correction per access size, but at the
alignments a RAM transfer uses, the two decompositions were measured to
deliver the same bytes; nothing observed so far tells them apart from
the guest's side. On the ISP1040, a dump the fabric refuses answers the
same host interface error ``0x4002`` a refused load answers: told the
dump completed, a guest would read its image back out of memory the
adapter never wrote and take whatever was already there. Load RAM
``0x0001`` and dump RAM ``0x0003`` are outside the ISP12160's cleared
command set and answer invalid command ``0x4001`` there; only write
word ``0x0004``, read word ``0x0005`` and the A64 load ``0x0009`` are
modelled on that part.

A mailbox result and its pending interrupt persist until the guest
acknowledges, in either order, by clearing the semaphore and issuing
the RISC-interrupt-clear host command. On the ISP1040 a second
submission arriving while that result is still held simply overwrites
it. On the ISP12160 it does not: the submission is refused, logged as a
guest error, and the held result is left in place, so a command's
answer cannot be lost under a second command the guest had no business
sending before acknowledging the first.

Four recovery mailbox commands are implemented, three of them on the
ISP12160 alone. Command abort ``0x0015``, abort device ``0x0016`` and
target reset ``0x0017`` are not in the ISP1040's mailbox set and answer
invalid command ``0x4001`` there; the ISP1040's one recovery command is
bus reset ``0x0018``, which both parts carry. Command abort takes the
channel in mailbox 1 bit 15, the target and LUN in the rest of mailbox
1, and the handle split across mailboxes 2 and 3, and cancels the one
outstanding command identified by that handle on that channel, target
and LUN. Abort
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
further phase was reached after the cancellation. Bus reset does this
on the ISP1040 too, where it is the only recovery command there is: a
request outstanding when the bus goes down is retired ``0x0004``
rather than vanishing. Command abort answers a non-complete mailbox
result for a handle that is unknown or does not match the request
found under that handle's channel, target and LUN, and touches
nothing. The model returns command error ``0x4005`` there, but the
datasheet leaves the exact code open, so the qtests pin only that the
answer is not completion and that nothing is retired. Abort device
completes ``0x4000`` when nothing is outstanding for the selected LUN.

All four commands answer parameter error ``0x4006`` for a channel
selector at or beyond the part's channel count, and only bus reset can
reach that answer today: its channel number is a whole mailbox, so a
guest can name a bus that does not exist. The three abort commands take
their channel from one selector bit, which on the only part that
carries them names one of two channels and can never name a third, and
the part with a single channel does not carry the commands at all. The
check is there for a part with more channels than a selector bit can
name, not because either of these two reaches it.

The branch carries the ISP12160's additional state in a vmstate
subsection, ``qlogic-isp/isp12160``, needed only when the part is
banked or carries two channels, so it never appears in an ISP1040
migration stream. The subsection carries the SXP1 and DMA banks the
ISP1040 does not have, and the per-channel reset-delay values that
mailbox command ``0x003a`` sets. The held completions are not in it:
they travel as top-level fields both parts carry, a count of zero for
an adapter whose response ring never filled. The top-level
``qlogic-isp`` stream is version 10 and its minimum version is still
6, so a stream written before either addition loads as it did.

Several of the choices above are compatibility selections where the
driver-sourced evidence does not establish the hardware's own answer.
They are not claims about unobserved silicon:

- Bus configuration 0 reads 0 on the ISP12160, because no consumer
  evidence reads a revision from that offset on this part and the
  datasheet assigns it no other meaning.
- Bus configuration 1 retains only the bank-selector bits on the
  ISP12160 and logs every other written bit as unimplemented, because
  the datasheet establishes the selector and leaves the rest of the
  writable mask open.
- Bank 0 of the four-way select is the RISC bank, matching the
  ISP1040's own bank 0; getting this wrong would cost a guest register
  contents it never reads back, since every bank is undecoded storage.
- The reset-delay setter places channel 0's value in mailbox 1 and
  channel 1's value in mailbox 2, following the family's pattern for
  its other dual-channel setters; nothing reads either value back, so
  only the vmstate fields and code review check that the two channels
  stay independent.
- The recovery retirement codes -- completion ``0x0005`` for an abort,
  ``0x0004`` for a reset, parameter error ``0x4006`` for an
  out-of-range channel, and ``0x4000`` for an abort device with
  nothing outstanding -- are the selected scope for these commands,
  not values read back from a captured device. Command error
  ``0x4005`` for an unknown or mismatched handle is the same kind of
  selection, but unreviewed even as a choice; the qtests pin only that
  the answer does not complete and retires nothing, not the code
  itself.
- The ``0x4006`` answer for a checksum header or span past the end of
  ISP12160 controller RAM is a selection following the RAM-command
  precedent above; the datasheet leaves the failure code open, and the
  test pins only that the command does not complete or report a
  checksum result.
- Invalid command ``0x4001`` answers the flat load RAM ``0x0001`` and
  dump RAM ``0x0003`` opcodes on the ISP12160, because the datasheet
  places neither in that part's command set and records no answer of
  its own for an opcode a part does not carry; both take what every
  other unmodelled opcode gets. The test pins that neither completes
  and that controller RAM is left as it was.

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
Sense-data validity rides in the SCSI status word's own bit 9. On the
ISP1040, a missing target is answered incomplete with bus-reached plus
selection-timeout -- the one answer all four guests read as an empty
slot rather than a fault. The ISP12160's datasheet leaves this
status-flags word unresolved for a missing target; the model answers
incomplete there with bus-reached alone and a zero status-flags word.

The model applies each part's own rule for what a completed command's
phase bits mean, per its reviewed datasheet. The ISP12160's datasheet
forbids a phase flag for work the command skipped: its status entry
always reports bus, target, CDB and status, adds the data-transferred
bit only once a byte has moved, adds transfer-complete only once every
byte the request described has moved, and adds the sense bit only
once sense data was collected. A zero-data command such as TEST UNIT
READY therefore reports neither data bit even on success.

The ISP1040's datasheet instead records the all-phases mask a
completed command traverses: every phase bit except sense is set, and
sense alone is gated on the completion actually carrying sense.
Applying that mask to a zero-data command -- raising bits for a data
phase that never ran -- is an acknowledged approximation of that part
rather than a case its datasheet excerpt tests directly, and stands
until revisited against the ISP1040's own datasheet.

A completion that finds the response ring full is held in the order
it finished and posted once the guest's acknowledgement frees a
slot. The number that can pile up is bounded by the budget the
adapter announces in its firmware status: the model consumes no
further request entry while any completion is held, or while that
many commands are outstanding, and resumes when a held completion is
posted or a request retires. Held completions therefore never exceed
the commands that were in flight when the ring filled by more than
one -- an entry naming no target is answered without becoming a
command, and only while fewer than 512 are outstanding -- so at
most the announced 512. A stream offering
more than that on load is refused. Real hardware never reaches the
full-ring case -- it completes one command at a time with real bus
latency -- but a model with none can. Re-initialising the response
queue while the rings are live is model behaviour, not a documented
hardware answer: it discards every held completion without posting it,
and a request ring stalled behind that backlog stays stalled until the
guest writes the request-in pointer or acknowledges a response.

Transfers the fabric refuses are dropped, never guessed at: a
refused CDB read drops the request with the consume index left on
the entry, a refused status store neither advances the produce index
nor raises the interrupt, and each drop is logged with its entry
address.

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

Power-on and the guest's soft reset -- a trigger bit the guest polls
to completion -- land on one reset state, and both cancel in-flight
commands silently through the SCSI core, since the same reset clears
the rings a completion would have been posted to. The bus-reset
mailbox command is not one of them: it resets the named channel's bus
and leaves chip state standing, and every request outstanding on that
bus is retired through the response ring with completion ``0x0004``
before the bus goes down. Reset values other than the signature are
zero under the standard for an unconfirmed value: no driver source
contradicts them.

In-flight requests migrate with the SCSI core's request stream; the
segment count read back on load is bounded by the entry format's
ceiling and a stream claiming more fails the migration. Held
completions migrate too, in the top-level ``qlogic-isp`` stream rather
than the ISP12160 subsection, and a stream written before they existed
loads with none held. The backlog travels as a counted array bounded to
512 entries, the most the ring rule above allows; a stream claiming
more is refused before a single entry is read, so a stream can never
make the load allocate more than the model itself could hold.
