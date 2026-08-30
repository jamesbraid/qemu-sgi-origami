================
SGI SN0 Hub ASIC
================

The Hub is the node controller of the SN0 family: a processor
interface, a memory and directory controller, an I/O interface and a
network interface behind one 8 MB register window per node. The
window is reached two ways through the crossbar's widget 1 space --
one half resolves to whichever node the reading processor sits on,
the other names a node explicitly. QEMU models it in
``hw/misc/sgi-hub.c``, implementing the registers guests touch on the
way to a console and reporting anything else rather than answering
it. The Origami project tree's Hub datasheets carry the field-level
sources.

Two registers decide things a guest cannot recover from: the
processor-number register (which of a node's two processors it is,
hence which copy of every per-processor register) and the network
interface status register (whether nodes are numbered in M or N
mode, on which every node-relative address depends). Both read zero
on an unprogrammed Origin 200.

Register access width
=====================

Every register is a 64-bit doubleword on a big-endian bus, and a
32-bit access names half of the aligned doubleword: offset 0 carries
bits 63:32, offset 4 bits 31:0. The model composes halves itself
rather than letting the memory core do it, because the core's
composition of a 32-bit write is a read-modify-write cycle whose
read half would fire read-to-clear side effects on a store that
reads nothing on real hardware. A 32-bit write replaces its half
against stored state and then takes the ordinary full-width write
path; for the interrupt-modify register -- an instruction, not a
value -- and every register that stores nothing, the unwritten half
contributes zero.

What a 32-bit read of a read-to-clear register does on real hardware
is unobserved -- no guest we hold issues one -- so the three such
registers take a *conservative choice*, not a transcribed fact: only
a full-width read clears, and a half read returns its half with the
latch intact. The same choice governs a 32-bit store to a clear
alias: only a full-width access operates the latch.

Widget identity and control
===========================

The identification register uses the common crosstalk layout --
revision, part, manufacturer, and a lowest bit that always reads
one. That bit is declared by no Hub source; it is inherited from the
crosstalk contract the Bridge and crossbar both state. The control
register resets with this Hub's own widget number at 1 (the
register-level counterpart of the address map) and the crossbar
credit at 2; the tag-mode bit reports hardware and is not writable.
The two crosstalk timeouts are the first registers Linux writes and
accept a write at reset with no unlock step.

The widget status register always reads zero: this model retries
nothing, completes every request before it returns, and never fails
its link. Bit 4 reads zero because SGI's own table records it as
undefined, preserved rather than resolved.

Processor interface
===================

The real-time counter is a free-running 52-bit up-counter at
1.25 MHz -- a rate corroborated three ways (the shipped firmware's
configuration block, OpenBSD's registered frequency, Linux's coded
tick). OpenBSD needs only that it advance monotonically; Linux needs
it to tick *between* two reads a few instructions apart, because it
reads the counter, adds a delta, arms the comparator and re-reads to
check the deadline has not already passed. The model derives it from
the virtual clock, so it cannot drift from the timer a comparator is
armed against; a write moves the origin, not the count, so it never
runs backwards on its own.

Each processor has a comparator. A match sets the pending bit
whether or not the enable bit is set -- the enable only gates pending
onto the processor's line, which lets a guest poll with the
interrupt off. The counter's clock-control register resets with bit
16 (internal clock) set; the network-clock observation bits are
read-only and read zero on a machine with nothing driving a
network-wide clock.

Whether a comparator the counter has already passed fires at once or
after a full wrap is unobserved and the public sources cut both
ways; the model matches at once, chosen on failure mode: a strand
would be silent for a 114-year revolution, while an immediate match
costs a guarded writer one early tick and shows a livelock loudly.
Only a guest-programmed deadline counts as passed -- moving the
counter under a reset-zero comparator is a change of origin, not two
unrequested interrupts. A matched comparator is not re-armed; guests
rewrite it from every handler.

Processor enable is a behavior, not stored state: clearing a
processor's bit removes it, which is how the firmware's halt routine
never returns on real hardware. The error-spool address registers
survive reset deliberately -- the firmware parks a marker in
processor B's across its own reset to learn which pass it is on.
Timed-out-request-buffer registers read zero: nothing times out in a
model that completes every request instantly.

The port reset register carries three resets and only bit 0, the
local Hub reset, is this machine's; it is tested alone because the
three consumers write different companion bits (Linux 0x81, OpenBSD
0x03, the PROM's own self-reset 0x81). The protection register is
deliberately not consulted: OpenBSD arms it before resetting and
Linux does not.

Interrupts
==========

Two pending banks of 64 interrupt numbers each are node-wide state;
each processor has a mask per bank, and a processor's line for a
bank is raised when any bit is set in both the bank and its mask.
The lines are level-triggered and recomputed on every change to a
bit or a mask: raising on set and never recomputing would leave a
line asserted after software cleared the last bit it could see -- an
interrupt storm, not a lost interrupt. The interrupt-modify register
is the only way to change a bit -- seven bits of number, the top bit
selecting the bank, one bit for set or clear -- and it is where a
device interrupt arrives, as a crosstalk write from the Bridge.

Scheduling assist
=================

The stock PROM rendezvouses its processors through Hub registers --
pending-bank bits as barrier and lock mailboxes, the peer's scratch
and comparator registers as progress mailboxes -- polling with a
timeout it measures in its own retired instructions. Under TCG a
poller can spend that whole budget inside one scheduled slice before
the processor it waits for runs at all, and report a live peer dead.

The model's assist has one minimal contract, ``sgi_hub_yield_to_peer``:
wake the named peer and end the current processor's slice. It selects
no successor, and it changes no Hub or interrupt state; it only makes
an already-atomic write visible on the scheduling boundary where real
hardware exposes it. Three register paths invoke it: an
interrupt-modify write releases every other local processor; a write
into a peer node's scratch registers releases that node; and a
re-read of pending bank 1 by the secondary processor, when the
primary's write is already credited, hands the slice back to the
primary. Only an unchanged re-read hands off -- a read whose value
moved is an answered poll, and the reader keeps its slice to act on
it.

Memory and directory
====================

The memory configuration register is masked storage whose reset
value is the fact that matters: eight bank descriptors, a
reply-guarantee count at full, the directory-format bit set, the
error-correction-ignore bit set -- a model zeroing it misreports all
of them at once. The four error registers and their clear aliases
always read zero; the firmware's memory test reads every alias and
aborts on any valid bit. The per-DIMM mode-initialisation registers
are write-only in both public sources and the firmware writes each
sixteen times, one per slot. Performance counter 0 is storage that
never advances: nothing here tracks bus activity for it to count,
and inventing an increment would fabricate a measurement.

The node's one-wire bus master register is 34 bits wide, and that
width is a correctness requirement: the upper timing phase occupies
bits 33:27, so truncating to 32 bits rewrites that field from 44 to
12 with every field below it intact. Starting a slot clears the
completion bit, so the disputed reset value of that bit does not
matter; the slot must not complete inside the write that starts it.
Nothing is attached to this bus, deliberately: an open-drain line
nothing pulls down samples one every slot, and modelling an invented
identity part would turn a visible absence into a plausible identity
nothing downstream could tell was invented. The firmware manual says
every board carries a part; which part is not established.

I/O interface
=============

The seven big windows are 512 MB each, aimed by a thirteen-bit
translation entry naming a widget, one of its two address spaces and
a 512 MB slice. There is no valid bit: a window is disabled by naming
a widget that is not there, which is what both guests do. Every entry
resets to zero, and that zero is load-bearing twice over -- a guest
looking for a spare entry calls zero free, and zero spells widget 0
in memory space, which is OpenBSD's only route to widget 0 (it
redirects every widget-0 access onto the last big window and never
programs the entry). A disabled window must not fault: guests disable
windows deliberately, so the model floors the windows with a region
that answers zero and logs -- a choice, not the hardware's behavior,
which no source gives. A window asking for a widget's I/O space is
reported and left reaching nothing rather than aimed at memory space;
the redirection's space bit is settled at memory by IRIX's own
programming.

The block transfer engines (BTE0, BTE1) copy or zero-fill ranges of
128-byte cache lines through the node-local memory view the machine
hands the Hub -- an address carrying no node number names this node,
exactly as it does for the node's processors. Three facts are
load-bearing: the engines work from reset gated on nothing (the
firmware's diagnostic launches without writing any enable); stored
addresses keep bits 39:7 and nothing else (the firmware stores
cached-window virtual addresses and depends on the high bits being
dropped); and a transfer completes inside the control store that
launches it (the diagnostic observes both completion bits together
and never loops back). Completion is either a pending interrupt on a
named processor interface or an eight-byte status doubleword written
to the notification address. A failed transfer latches error beside
busy and wedges the engine; a length/status store clears busy and
never error; only the I/O error-clear register's engine bit absolves
it. The model moves whole chunks through one bounce buffer rather
than issuing a call per 128-byte line, with the line-at-a-time
engine's register-visible contract unchanged: the transfer completes
inside the launching store, a failed chunk is retried line by line so
the count of lines that never moved is exact, and overlapping source
and destination keep the line-at-a-time propagation order.

The deallocation, queue-depth and CRB registers read zero because
nothing is ever in flight; for the two deallocation registers the
clear read is required, since a guest polls a written pending bit to
clear. The link-status register's state field has a counter-intuitive
encoding -- 0 failed, 1 never came up, 2 and 3 working -- so testing
the field's high bit answers "is the link up", which is the test
Linux performs. The link comes up reporting itself up, a knowing
departure from the documented reset value: a link that reads down
makes Linux return before it identifies anything, and this machine's
link is permanently connected to something always present. That
departure is a ratified machine-contract decision.

Network interface
=================

The status register stays in M mode; the node field resets to zero
and is programmed by firmware (synthesized firmware may supply it
through the nasid property); the region-size bit resets set. The
vector PIO reaches only the directly connected peer chassis: a null
route names that peer, and non-null routes fail with the hardware's
address-error response, so the direct topology never pretends it can
route an Origin 2000 network. With no peer fitted the model latches
nothing and lets the requester's poll time out -- the datasheet's
no-response proposal for its open no-peer question, not an observed
silicon behavior.

Diagnostic indicators and the boot-status word
==============================================

The indicator register is write-only; every write is traced, decoded
against the firmware's own progress and failure enumerations when it
matches one and flagged undecoded otherwise. The boot-status word the
firmware keeps is a *software convention held in FPR0*, not a
hardware register of this chip.

Migration
=========

The big-window aim is re-derived from the restored table entries;
the BTE registers are the whole of an engine's state, so nothing is
in flight to carry. The interrupt outputs are re-driven on load,
which is sound here because each drives an idempotent level input on
the CPU that migrates its own cause bits -- the same re-drive was
rejected for the IOC3's line into the Bridge, whose consumer
edge-detects.
