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
undefined, preserved rather than resolved; the firmware's one
power-on write of zero to it is logged as unimplemented, not as a
guest error. The I/O interface's two scratch registers are 36-bit
words of four packed sub-fields, everything above bit 35 reading
zero -- the layout SGI's own table and Linux agree on -- and the
firmware uses register 0 as private boot state (a measured 234 reads
against 6 writes per boot). Of the incoming-crosstalk request buffer
array (fifteen slots of four sub-registers), only slots 7-14's C
sub-register is decoded, matching the firmware's measured diagnostic
sweep; every field a real slot would hold is zero in a model that
has never tracked an incoming transaction, and the firmware's early
zero write to all eight is a generic range clear, logged
unimplemented.

Writes to the processor-number register are a documented vendor
convention aimed at a logic analyzer or RTL simulation -- the
standalone firmware's Hub header and IRIX's own drivers store marker
values there, nothing ever read back -- and log as unimplemented, not
as guest misbehavior.

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

A comparator the counter has already passed does not fire at once:
the model arms it for the full forward distance, so a comparator
written behind the count matches only when the counter genuinely
reaches it, up to a near-full revolution later -- 114 years at
1.25 MHz. This was an open failure-mode choice until the two-node
IRIX miniroot settled it: a secondary processor parks its tick by
writing zero to its comparator with the counter high and no
read-back guard, and depends on silence -- a model that matched a
behind comparator at once turned that write into a permanent
interrupt storm, and real Origin 200s boot IRIX, so the silicon
wraps. Moving the counter under a reset-zero comparator remains a
change of origin, not two unrequested interrupts. A matched
comparator is not re-armed: the arm that reaches its deadline fires
once, its timer is deleted so the deadline cannot pend a second
time, and guests rewrite it from every handler. An unpopulated
slice never arms a timer. Of the clock-control register, bits 7:1
(maximum count) and bit 0 (network-clock enable) are writable; bit
15 and bits 14:8 observe the network-wide clock and read zero.

Each processor's error-status record is modelled to its first
doubleword; the second pair reads zero. The plain registers refuse
writes; the clear alias accepts only a full-width store of zero as a
clear, so a nonzero store cannot clear what its own bits assert. A
guest clears a timed-out request buffer by writing it, which is
accepted with nothing to clear.

Processor enable is a behavior, not stored state: clearing a
processor's bit removes it, which is how the firmware's halt routine
never returns on real hardware; the halt is requested rather than
forced because it runs inside a memory write, the idiom MIPS's own
power controller uses. The error-spool address registers
survive reset deliberately -- the firmware parks a marker in
processor B's across its own reset to learn which pass it is on.
Timed-out-request-buffer registers read zero: nothing times out in a
model that completes every request instantly.

The port reset register carries three resets and only bit 0, the
local Hub reset, is this machine's; it is tested alone because the
three consumers write different companion bits (Linux 0x81, OpenBSD
0x03, the PROM's own self-reset 0x81, measured on both PROM images).
The link-reset and warm-reset-out bits are accepted and dropped. The
protection register is deliberately not consulted: OpenBSD arms it
before resetting and Linux does not. The error-spool addresses keep
bits 31:3, start at zero from construction (the datasheet's power-on
value), and survive reset for the marker the firmware parks in
processor B's.

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

The model's assist has one minimal contract, ``sgi_hub_yield_to_processor``:
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

The memory configuration register is masked storage -- eight bank
descriptors, a reply-guarantee count, the directory-format and
error-correction-ignore bits, an arbitration bit, an I/O-protection
enable, a module select and two timing-field pairs, dual-sourced
against SGI's composite reset constant -- whose reset value is the
fact that matters: the count at full, directory-format set,
error-correction-ignore set, and a model zeroing it misreports all of
them at once. The four error registers and their clear aliases,
evenly strided eight bytes apart from 0x200050, always read zero; the
firmware's memory test reads every alias and aborts on any valid
bit. The aliases are inferred, not proven, to clear on read; writes
to any of the eight are accepted and logged unimplemented. The
per-DIMM mode-initialisation registers (DIMM_SELECT at bits 35:32,
DIMM_MODE at 11:0) are write-only in both public sources and the
firmware writes each sixteen times, stepping the select through
every slot. Performance counter 0 is a 20-bit event counter kept as
storage that never advances: nothing here tracks bus activity for it
to count, inventing an increment would fabricate a measurement, and
its selecting sibling register is not implemented.

The slot-identity and microcontroller status register samples board
signals: the machine supplies the master/slave sense and the XIO
jumper; the bus-fault line and processor-speed strap read clear. The
table declares no reset value for any of its fields, so what the
absent signals read is this machine's decision.

The node's one-wire bus master register is 34 bits wide, and that
width is a correctness requirement: the upper timing phase occupies
bits 33:27, so truncating to 32 bits rewrites that field from 44 to
12 with every field below it intact. Starting a slot clears the
completion bit, so the disputed reset value of that bit does not
matter; the slot must not complete inside the write that starts it.
A write cannot set the register's two status bits. The two documents
describing the register disagree, unsourced both ways, on whether
completion resets set or clear; the reset phase constant is taken
from the memory/directory page because the one-wire page -- the
authority on the fields -- does not carry it while asking for an
exact assertion, a discrepancy reported rather than papered over. A
slot's duration is nominal and chosen: what the timing encodings mean
in real time is not established, and the contract needs only that a
slot not complete inside the write. A slot armed before a reset must
not complete after it.

The bus carries one responder: a DS2505-compatible part holding the
installed PIMM's manufacturing record, which is the settled contract
of the Origami project tree's hub one-wire datasheet -- the firmware
reads its ROM identity as a remote node's NIC -- its name for this
Number-In-a-Can identity chip, not a network device -- during
headless bring-up, and without it that read reported nothing present
and the peer node's identity never resolved. The part answers a bus
reset with a presence pulse and speaks the family's ROM protocol:
search, match and skip, least-significant bit first, with the public
DS2505 datasheet's family code and its CRC-8 check byte over family
and serial. The 48-bit serial is a synthetic per-unit value the
machine wires per node; a real machine's serial must never be
copied. The pulse-width thresholds sorting a slot into reset, zero
or one are the same model choices the Bridge's one-wire master uses.
The memory reads -- plain, status and extended, with the public part
specification's geometry and CRC16 framing, 2 KB in sixty-four
32-byte data pages, and a status field addressed at that same width
whose implemented regions are scattered below 0x13f with gaps
between them, everything above it to 0x7ff unimplemented -- serve an
unprogrammed part: every byte reads 0xff behind a correct check
pair, implemented or not, and
a redirection byte of 0xff means no redirection by the part's own
definition, so a blank part invents nothing. What the
manufacturing-record bytes should say is still an open question --
the record's key names are not established -- so the part stays
blank rather than carrying an invented record the guest would parse
into this machine's serial number; the programming commands are not
answered.

The PCF8584 is mapped as a subregion of the Hub's own register region
at higher priority, so both of the machine's mappings of the window
reach it; it has no interrupt line, since the port is polled. The
optional bring-up UART takes its 16550 clock argument as the
oscillator divided by sixteen. The character-device assignment is
fixed in one place: IOC3 ports A and B at 0 and 1, the system
controller's AUX port at 2, the bring-up UART at 3 -- the IRIX
console (``console=d``) is IOC3 serial A, not the AUX port. The
system controller's 2 KB NVRAM answers eight consecutive I2C
addresses of one 256-byte page each (one part or several is not
established and invisible on the wire); page 7 is the
controller-reserved page the PROM reads before probing a console,
and only its validity marker has a required value.

I/O interface
=============

The seven big windows are 512 MB each, aimed by a thirteen-bit
translation entry naming a widget, one of its two address spaces and
a 512 MB slice. There is no valid bit: a window is disabled by naming
a widget that is not there, which is what both guests do. Bits 7:5
of an entry are inside its width and named by no source, so they are
stored rather than dropped. Every entry
resets to zero, and that zero is load-bearing twice over -- a guest
looking for a spare entry calls zero free, and zero spells widget 0
in memory space, which is OpenBSD's only route to widget 0 (it
redirects every widget-0 access onto the last big window and never
programs the entry). A disabled window must not fault: guests disable
windows deliberately, so the model floors the windows with a region
that answers zero and logs -- a choice, not the hardware's behavior,
which no source gives; the floor logs the window number, since an
offset alone does not say which of the seven was disabled, and it
covers the big windows only -- a crossbar port with no widget is an
unmodelled device, not a disabled window, and keeps faulting. A
window aimed at a widget's I/O space forwards byte writes to widgets
8-15. Wider writes and general reads remain unsupported; the outgoing
path otherwise admits only an eight-byte widget-identity read.

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
and destination keep the line-at-a-time propagation order. A chunk
is 4096 lines (512 KB), so the diagnostic's transfer is one call
each way and the 8 MB maximum takes sixteen. Several launch shapes
are stated-open choices: a zero-length armed launch (never
programmed by any observed driver) completes as zero lines; whether
zero-fill reads its source is open, and the model does not; a
control store with busy clear does nothing, the datasheet's own
proposal; what a control/terminate load returns is unobserved and
answered with the written bits; poison mode launches, but the
directory state it would poison belongs to the unmodelled directory.
An engine's source, destination and notification addresses resolve
by programmed node number through the Hub's own engine view of the
claim layer, exactly as a processor's addresses do, so a
node-field-zero address names whichever Hub holds number zero and
never the engine's own board by position. The notification is exactly
eight bytes in guest byte order -- whether hardware writes more of
the line is unobserved -- and in interrupt mode the node field names
a processor interface on whichever Hub the fabric's map holds for
that number, this Hub included; the completion is a direct call into
that Hub, so the launching store never re-enters this Hub's own
register region, and a number no reachable Hub holds is logged and
dropped. On hardware a failed transfer
surfaces through the Hub's error machinery, not a completion signal;
a notification write on error is not established, so neither
completion path runs, and the Hub error interrupt is not modelled.
The engine enable mask stays unimplemented: the sense of its bits is
open and nothing writes them before first use.

The deallocation, queue-depth and CRB registers read zero because
nothing is ever in flight; for the two deallocation registers the
clear read is required, since a guest polls a written pending bit to
clear. The queue-depth register has no write case -- the datasheet's
one conditional rewrite never fires because the register always
reads below its threshold; the arbitration register is not decoded
(no guest path reads it); the per-widget response buffers are
excluded because their credit field resets to one. Which visible bits
the I/O error-clear register touches is open in the datasheet; the
engines' error bits are the only latches this model has, and the
firmware writes it three times per boot on its normal error path. The link-status register's state field has a counter-intuitive
encoding -- 0 failed, 1 never came up, 2 and 3 working -- so testing
the field's high bit answers "is the link up", which is the test
Linux performs; reporting 2 rather than 3 is a choice, nothing
distinguishing them. The link-enable bit is left clear at reset:
firmware operates it itself with a load-modify-store once it has
identified a part, and presetting it would model that write. Burst,
retry count and null-packet timeout are writable configuration, the
warm-reset trigger a one-shot the model accepts and drops; the state
field and eight-bit mode are read-only, since a guest that could
write the state could tell the machine its own link was down. The link comes up reporting itself up, a knowing
departure from the documented reset value: a link that reads down
makes Linux return before it identifies anything, and this machine's
link is permanently connected to something always present. That
departure is a ratified machine-contract decision.

Network interface
=================

The status register stays in M mode; the node field and region size
are the writable fields, programmed by firmware (synthesized firmware
may supply the node through the nasid property), while the link state
and chip identity are read-only; the region-size bit resets set and
link-up reports whether the product wires a link. Vector PIO goes
through the machine's fabric, on which this Hub's network interface
is one endpoint: the fabric walks the route from that endpoint's port
and performs the access on whatever the route ends at. On the direct
adapter, the fabric of two Hubs joined by one link, a null route
names the other Hub and any other route, which would need a Router,
fails with the hardware's address-error response, so the direct
topology never pretends it can route an Origin 2000 network. A route
nothing answers -- no link, or the link down -- latches nothing and
lets the requester's poll time out, the datasheet's no-response
proposal for its open no-peer question, not an observed silicon
behavior. A remote vector access decodes through the target Hub's
pure store or peek, never its write hook, so it has no local side
effects; the fabric is serialised only by BQL MMIO dispatch, which
the launch path asserts before it calls in.

The node field is what the Hub decodes node-qualified addresses
against, not the board position. The fabric gives each Hub a
reachability map -- for every node number, the endpoint programmed
with that number that the fabric connects, the Hub itself first --
and in each attached processor's view, and in its own engines' view,
the Hub places one alias per reachable endpoint at that endpoint's
number stride, aimed at the endpoint's board stride in the canonical
map, over an error floor across every stride; a number no reachable
Hub holds answers with a decode error, never by board position and
never from a Hub the number does not name. The Hub knows no peer,
Router, module or cable: it holds its endpoint and its map. The
machine wires the per-view overlay containers, the canonical map, the
address geometry and the endpoint at build time; the Hub owns the
overlay aliases and re-aims them inside one memory transaction
whenever its map changes, which the fabric recomputes when any Hub's
field is stored, at reset exit, and after a migration load. At reset
every Hub answers node zero, so each node's
early self-addressed initialisation stays on its own Hub until the
global master assigns real numbers. The contract is the "node number
a Hub answers to is programmed, not fixed" section of
docs/datasheets/address-map.md in the origami project tree.

The cached-alias size register bounds each processor view's nameless
low window in the memory space, and only there: the register's
domain is the cached space alone -- Linux's sn0 addrs.h defines
CALIAS as "the first n bytes of memory on the reader's node" based
at the cached base. Zero disables the window, one is 4 KiB, and each
further code doubles it, up to 64 MiB at fifteen (sn0 hubpi.h).
Within the window a node-field-zero memory access names the reader's
own node; beyond it the access falls to the claim overlays, which
route stride zero by programmed number -- the node's own Hub before
assignment, the global master's after. The machine pre-creates one
window per view at the largest encodable size and hands it over at
build time; the Hub resizes and gates it inside one memory
transaction whenever the register is stored, at reset exit, and
after a migration load. The other spaces' nameless windows are the
position-independent aliases of asm/sn/addrs.h and stay wide and
reader-local regardless of the register: the Hub-special space is
the UALIAS (uncached references to the memory of the reader's node,
its bottom 128 KiB flipped per processor), the crosstalk space's
window is the IALIAS (the local Hub's registers), and the
memory-special and LBOOT paths are likewise local. Every guest we
hold drives the register: the PROM stores the 4 KiB code once in its
earliest per-processor bring-up and never reads it back, OpenBSD
disables it at kernel initialisation, and Linux zeroes every node's
copy before bringing processors up, then sets 8 KiB per node to
switch to node-local exception handlers.

Diagnostic indicators and the boot-status word
==============================================

The indicator register is write-only; every write is traced, decoded
against the firmware's own progress and failure enumerations when it
matches one and flagged undecoded otherwise -- a run with the event
armed and no output means no write reached the register. Values
outside 0x00-0x60 and 0x81-0xb5 are not in the enumeration, and the
pattern the firmware flashes while waiting for a character is 0x80
XORed with a per-device bit pattern, so an undecoded value is not a
fault. The boot-status word the
firmware keeps is a *software convention held in FPR0*, not a
hardware register of this chip.

Migration
=========

The big-window table field is version-gated, so a stream from before
the table existed loads with the windows at their reset aim. The
big-window aim is re-derived from the restored table entries, and
the cached-alias windows from the restored size code;
the BTE registers are the whole of an engine's state, so nothing is
in flight to carry. The interrupt outputs are re-driven on load,
which is sound here because each drives an idempotent level input on
the CPU that migrates its own cause bits -- the same re-drive was
rejected for the IOC3's line into the Bridge, whose consumer
edge-detects.

The one-wire responder's protocol state used to travel in this
stream and now belongs to the part on the bus, which migrates its
own. Unused spans of the original size hold its place so an older
stream still loads, and a conversation half-finished when such a
stream was written is discarded rather than translated. Discarding
is not what a bus reset does -- a reset resets the master too, and a
load does not -- so a guest resumed from one of those older streams
keeps talking to a part that is now idle and has its next read slots
sampled as a command byte of all ones. That produces one "unknown
ROM command 0xff" on resume. It is benign and self-clearing at the
guest's next reset pulse, and it is absent from streams written
since the part became a device, which carry the part's own section
and preserve the conversation intact.
