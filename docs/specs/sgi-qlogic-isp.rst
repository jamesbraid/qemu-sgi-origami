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

One PCI identity answers for the whole family; the configuration
revision byte selects the part -- 5, the ISP1040C, matching a
captured Origin 200 boot. A guest reads the revision from two places
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
