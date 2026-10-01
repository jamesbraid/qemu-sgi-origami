# Datasheet: the Hub's one-wire bus master

References rendered as citation identities name material outside this document
set. Those targets are not included or cleared for redistribution. Review and
runtime statements retain their recorded dates and scope.

**Status: reviewed, gated and cleared. Implementation is unblocked.** An independent
second-opinion pass reviewed this document and returned four blocking findings; all four were
fixed, the reviewer re-gated against the current text rather than accepting the fixing commit's
subject, found one paragraph still unreconciled, and cleared it once that landed. **The gate
record is the one-wire gate review (citation: `docs/reviews/hub-onewire-gate.md`).**

**This banner is the mechanism, not decoration** — datasheet-before-device is enforced by this
line and nothing else. **It stood at "blocked" after the gate cleared**, because four commits
corrected the document's contents and none of them swept the sentence describing the document's
own state. An implementer held rather than proceed on two people's assurances that the gate was
open, **which is the correct reading of what this line is for**, and it is why the line is now
kept accurate as a first-order obligation rather than a formality.

**Fork file(s) (proposed, not yet created):** the register lives in the Hub's memory/directory
block and its host is `hw/misc/sgi-hub.c`; whether the bus engine belongs there or in a
separate file is an implementation question this document does not settle. Whatever answers on
the wire needs its own model and its own name.

**Why this exists.** A firmware stall was traced to a single Hub register: written once, then
polled hundreds of thousands of times while our model answers zero. It is this device's control
register, and **the firmware cannot proceed past memory and directory configuration until it
answers.** Everything downstream of that point — including booting an operating system — is
behind it.

## Provenance, and it is unusually good for this project

**Every register fact in this document comes from the public Linux headers for this platform**,
which name the register and give its complete field layout. **An implementer can verify this
specification against a public source rather than taking it on trust**, which is a stronger
starting position than most datasheets here begin from, and it means nothing in the register
map depends on anyone's reading of a vendor tree.

**What is *not* from those headers is marked as such below** — chiefly what physical part
answers on this bus, and what the timing values mean in real time rather than as encodings.

## Role, and the thing it is not

**This is a bus master: a register that drives one bit at a time onto a single-wire serial bus
and samples what comes back.** It is not storage, not a status word, and not a device
identifier. Its purpose is to let firmware read a small NIC hanging off the wire.

**Three different things share the vendor's name for the part, and this document distinguishes
them because they are modelled differently.** The firmware prints all three and calls each of
them the identity chip:

- **The bus identifier** — the sixty-four-bit value a bus search returns, family code plus a
  laser-burned serial plus a check byte. **This is what enumeration reads, and it is not stored
  in pages.**
- **The board serial** — a barcode serial number held *in the part's pages*, behind that
  identifier, reached by a read rather than a search.
- **The board part number** — also page content, also behind the identifier.

**One is what the bus returns; the other two are content behind it.** A sentence that does not
say which it means is not checkable, so every claim below names one. Hazard identified by
ds-firmware from the firmware's own output.

**This is also the third distinct one-wire bus in this machine and they must not be conflated** — a
mistake this project's own component taxonomy already warns about, because two of them were
being described as "the one-wire bus" until somebody separated them:

- The **I/O controller's** MicroLAN, carrying one Ethernet-address NIC.
- The **Bridge's** own one-wire line, carrying a board-identity NIC.
- **This one, on the Hub**, carrying a DS2505-compatible record for the installed PIMM.
  Its physical package and route remain open.

## Register map

**One register, at Hub-local offset `0x20_00a8`, named in the public Linux headers for this
platform as that platform's MicroLAN control register.** Both the name and the offset are given
here deliberately: this document's stated strength is that an implementer can verify it against
a public source, **and a specification that identifies its register by neither name nor offset
cannot be verified against anything.** An earlier revision omitted both out of caution about
naming, which disabled the promise the document is built on; the sibling one-wire datasheet
identifies its equivalent by offset and this one now does the same.

**It is at least 34 bits wide** — see the access-width constraint below, which is a build
requirement rather than a note.

| Bits | Field | Purpose |
|---|---|---|
| 33:27 | Upper timing phase | Seven bits. A timing parameter; **what it shapes is not established** — see the open items. |
| 26:20 | Lower timing phase | Seven bits. The other, same caveat. |
| 19:10 | Pulse width | Ten bits. How long the master drives the wire low to open a bit slot. |
| 9:2 | Sample point | Eight bits. How long after the pulse the master samples the wire. |
| 1 | Completion | Set by the device when a bit slot has finished. **Firmware polls this.** |
| 0 | Received data | The level sampled from the wire during the slot just completed. |

**Bit order is settled and is not a judgement call.** The public header names the two low bits
with separate constants at separate positions: **completion is bit 1, received data is bit 0.**
Anything that states them as "two single bits" without fixing the order is under-specified, and
this table is the authority.

**Reset values are NOT settled, and an earlier version of this document over-read them.** The
public header carries a constant *named* for this register's reset defaults, holding **both
timing-phase fields at a non-zero value and nothing else**. It is tempting to read that as the
hardware's reset state. **It has no consumer anywhere in mainline** — the constant, the
completion bit and the received-data bit are all defined and none is used — **so what it
establishes is that somebody named a constant that way, not what the register holds out of
reset.** The earlier wording here said the header "defines this register's reset defaults", which
claimed more than a name with no reader can carry.

**There are three positions in this tree, not two, and an earlier revision of this paragraph
knew about two.**

- **Resets *set*** — the position the Hub memory-and-directory datasheet used to assert, on the
  argument that idle means nothing in flight. Sound in its own terms and **unsourced**. **That
  document no longer asserts it**: its row now says the value is not settled, says not to
  implement from that cell, and points here. **So this is a position that was held, not one still
  held**, and an earlier revision of this list described it in the present tense after the other
  document had already changed.
- **Resets *clear***, read off the public header's reset-defaults constant. **Also unsourced**,
  for the reason above: the constant has no consumer anywhere.
- **Resets *clear*, from a specification** — the Bridge datasheet gives its equivalent bit that
  way, from that chip's own document. **This is the only one of the three with a source behind
  it.**

**Handle the third carefully, because it is about the sibling and not this register.** It is
evidence that the *Bridge's* bit resets clear. The two parts are the same design — which the
vendor's own library states rather than this document inferring — **but "the same design" is
still a claim standing between that specification and this register**, and reading a sourced
fact about an adjacent part as a sourced fact about this one is the error this document warns
about twice elsewhere.

**So: two unsourced readings about this register, and one sourced reading about its sibling that
agrees with the weaker of them. That strengthens the case for *clear* and settles nothing.**

**Which makes the resolution below stronger than a hedge.** With three positions in play and the
best-sourced one belonging to a different chip, **declining to pick is not caution — it is the
only defensible position**, and the model is correct under all three.

**Found by arch-eng, who counted a position neither this document nor the reviewer had.**

**What resolves it without needing to pick — and this is the load-bearing paragraph of the whole
document.** On real hardware, **starting a transaction must clear the completion flag**, or a
requester could never distinguish this operation's completion from the previous one's. **Model
that, and the reset value stops mattering for correctness:**

- if completion resets **clear**, the first poll spins;
- if completion resets **set**, the firmware's own request clears it and the poll spins.

**Either way the stall stays loud and the fabrication is impossible**, which is exactly the
property to preserve while the bus behind it does not exist. **The "read-only" description means
the guest cannot set it, not that the device cannot.** Contributed by ioc-dev, who found the
contradiction while implementing and stopped rather than resolving it by guess; recorded as a
design argument rather than a source reading, because that is what it is.

### The standing rule for this device: where the specification is uncertain, prefer the loud failure

**This is a design principle for this register rather than a note about one bug, and it should
govern every future decision here.** Its two failure modes are not symmetric:

- **Getting it wrong one way leaves a stall that announces itself.** Somebody notices in
  minutes, and no wrong data enters the machine.
- **Getting it wrong the other way hands the firmware a serial number that was never on a wire,
  and reports nothing.** It looks like progress, it boots further, and nothing downstream can
  tell that the identity it carries was invented by the register meant to read it.

**So where this datasheet is uncertain — and it is uncertain in several named places — implement
the variant that stalls.** Not the one that proceeds. **A model that proceeds on a guess is not a
partial implementation of this device; it is a different device that answers plausibly**, and
this project has already recorded what that costs on a bus one level down.

### How two correct sections produced a wrong instruction, which is worth its own note

**The contradiction ioc-dev found was not a wrong sentence. It was two right ones interacting.**
The register table's reset value is defensible on its own terms; the trap section is right on
its own terms; **and a faithful implementation of the first reaches the outcome the second
forbids, on the first poll, silently.**

**Neither section is wrong alone, which is why no review of either would have caught it.** The
defect lived in the join, and the only reason it surfaced is that somebody implementing from
both at once had to satisfy them simultaneously. **That is an argument for an implementer's
questions being treated as findings rather than interruptions**, and it is the third time in
this document's short life that one has improved it.

## The access-width constraint, which is a build requirement

**The upper timing-phase field occupies bits 33 down to 27, so the register does not fit in a
32-bit word.** Firmware accesses it eight bytes at a time and the observed write genuinely sets
bit 32.

**Truncating to 32 bits does not lose a spare bit — it rewrites the upper phase field.** In the
one value observed, from 44 to 12: a timing parameter wrong by nearly a factor of four, **with
every field below it intact and looking entirely reasonable.** There is no fault, no warning,
and nothing in the low bits to suggest anything happened.

**And the public header has a defect at exactly this field, which an implementer following this
document's central promise will hit.** Its isolation mask for the *lower* timing-phase field is
written with the *upper* field's shift, so the two masks are identical and the lower one selects
the wrong bits. **The header states its own convention of isolating fields with those masks, so
it instructs a consumer to use the artifact that is wrong.**

**Use the shift, not the mask, for that field.** The shifts are consistent and the field is seven
bits; the value the firmware writes decodes correctly under that reading and does not under the
mask's. **This is stated rather than quietly worked around because the document's promise is that
the public source can be checked** — and a reader who checks will find the contradiction and needs
to know which side of it to trust. Found by arch-eng; what makes it blocking rather than cosmetic
is the header's instruction to use the masks, found in review.

**So the model must declare the widths its handler is written for, explicitly, up to eight
bytes.** This is the same access-width hazard this project has already been bitten by on a
firmware-call region, arriving on a register where the wrong answer is a plausible number
rather than a crash.

## Behavioural contract

**One write starts one bit slot.** Firmware programs the timing fields and writes; the device
drives the wire for the programmed pulse, samples it at the programmed offset, and then reports.

**One poll ends it.** Firmware reads the register repeatedly until the completion bit is set,
and takes the received-data bit beside it as the bit read from the wire. **The observed write
has both low bits clear** — a request being issued, not a result being reported — which is what
confirms the request-then-poll reading rather than merely being consistent with it.

**A byte is eight of these, and a part's contents are many bytes.** Nothing in this register is
byte-oriented; the firmware assembles bytes itself, one slot at a time.

### When completion arrives, which is a separate question from when it must not

**The contract above fixes when completion may not be reported. It does not fix how a model
delivers it, and the two available answers cost very different amounts.** Ours derives completion
from the slot's own deadline on the virtual clock, evaluated when the register is read. The
alternative, a timer callback that writes the flag into the register, is what we had.

**A timer callback needs the big lock, and firmware bit-banging a one-wire part holds it.**
Two four-node, eight-processor Origin 2000 firmware boots on one revision, differing only by
which way completion is delivered, ran **the same 17,720 slots**. Delivered by the callback they
cost 7,161,412 accesses to this register, 404 per slot, and a slot armed for one microsecond of
virtual time was not seen finished for 8.6 to 15.3 milliseconds. Derived from the deadline they
cost 51,269 accesses, 2.9 per slot, and the wait falls to between 17 and 663 microseconds.
**Every access in both boots fell before completion was first seen and none after it**, so the
count is the guest waiting on a starved callback rather than surplus polling. The cost lands
during interconnect bring-up, because the firmware reads the node identities in the same phase
in which its processors synchronise with each other.

**Neither answer changes what a guest may observe.** Starting a slot clears completion, and no
read before the deadline reports the slot finished, under both.

**The slot length itself is a modelling choice.** No source in this tree gives a real pulse width
in register units, so a model may pick one. What it may not do is report completion before it.

## The trap, and it is the first thing an implementer will reach for

**Making the read return "completion set" empties the poll immediately and the firmware
proceeds. Do not do it.**

What it proceeds *into* is reading a part's contents **one bit at a time off the data bit beside
the flag.** A model that reports completion without a bus attached answers every one of those
bits with whatever constant it returns — and the firmware assembles them into **a part number, a
serial number and a checksum that were never on any wire.**

**The stall is loud. A fabricated NIC is silent.** It looks like progress, it produces
a machine that boots further, and nothing downstream can tell that the identity it is carrying
was invented by the register that was supposed to read it. **A model that makes this mistake is
strictly worse than the stall it fixes**, because the stall is a defect anyone can see.

**So the completion bit is not a field to be answered. It is the output of a bus that has to
exist.**

## What we have already built, and it is most of this

**We already model this exact design one level down.** The Bridge carries a one-wire master of
its own at a widget offset, and **its low twenty bits are the same layout as this one's, field
for field**: received level at bit 0, completion at bit 1, sample offset at bits 9:2, pulse
width at bits 19:10, and a write starting one bit slot. **The Hub's register is that design plus
the two timing-phase fields above it.**

**And we already model a responder for it** — the Bridge's board-identity NIC, which answers a
match, a read-status and a plain read, and carries **two separate checksums**: one framing the
read protocol and one embedded in the page's own last bytes. That part took four rounds to get
right, and the reason is worth carrying rather than re-earning: **a guard that asserted only the
framing checksum passed a page whose embedded checksum was never computed**, and the guest's
first check on the part failed.

**So an implementer should read the Bridge's one-wire sections before writing a line of this.**
The master is a near-copy; the responder is a worked example including its own failure history;
and **the failure modes rhyme.**

## Settled responder contract and open physical placement

For Origin 2000, retained IRIX 6.5.7 PROM source shows that each node-local Hub bus may
enumerate both the family-`0x0b` node/PIMM manufacturing part and a family-`0x09`
module-identity part used during initial KLCONFIG construction. The Hub module-serial
proposal (citation: `docs/designs/origin2000-hub-module-serial-route.md`) records the public transport,
release qualification, exact clean two-page payload schema and unresolved physical replication.

**The node-local Hub reads a DS2505-compatible manufacturing record for the installed
PIMM.** The reviewed clean-side contract settles the logical role and record semantics. It does
not locate the package on the PIMM or IP29 System Board, identify a reference designator, or map
the connector contact and copper route. The Origin-family identity and topology evidence
ledger (citation: `docs/library/origin-family-identity-and-topology-evidence.md`) records that boundary.

The consumer-side evidence establishes these protocol constraints:

- **It is a Dallas one-wire NIC** — the family the industry calls 1-Wire and SGI's
  manuals call a *number in a can*. The same family as the parts on the other two buses.
- **The identity read is sixty-four bits**, the size of that family's ROM identifier: a family
  code, a serial number and a check byte. **Not sixty-four bytes** — the Bridge's *contents*
  read is a different and longer transaction, so the prior worth testing against does not
  transfer directly.
- **The firmware runs the family's search algorithm**, carrying discovery state across bits.
  **So the bus is enumerable and may carry more than one part**, which a model of it has to
  allow for rather than assume a single responder.
- **It is checked**, with a distinct failure for a bad check value, one for nothing present, and
  one for a redirection that loops — so the contents can point elsewhere.
- **What comes out is rendered as a text string** and stored as the firmware inventory's
  **processor-node board identity**. That names the record receiving the value; it does not
  establish which physical board carries the part. The system controller uses a different
  bus.

**And the vendor's own library states the relationship this datasheet derived from register
layouts.** Its access routine is deliberately abstracted so that one identity implementation can
drive **the Hub's registers and the Bridge's registers** among other media — named as such. **So
"the Hub's master is the Bridge's design one level up" is not merely an inference from matching
bit positions; the firmware is written to treat them as the same thing.**

**The compatible class and semantic contents are established.** The pages describe the installed
PIMM population, SGI part number, installed revision and barcode serial. The responder also has a
separate factory ROM identity. A model must use synthetic per-unit values; it must not copy a real
machine's ROM serial, barcode, manufacturing revision or page bytes.

**A vendor manual does describe such a part on a node card, and it is a strong inference rather
than a reading for our machine.** SGI's own PROM technical reference states that there is an
identity chip on each type of board in the system **including the node card**, that it holds a
48-bit laser-burned identifier, and that its pages carry board type, revision, barcode serial and
board-specific data. **That is the structure derived above from the firmware's own use, stated by
the vendor** — the 48-bit identifier being the sixty-four-bit read less its family and check
bytes, and the pages being what the operating system walks.

**Two limits explain why the manual did not settle the Origin 200 placement by itself.** It covers
two machines and switches between them mid-section with nothing marking the change; that has
produced retracted findings before. **The passage in question names the larger machine and lists
boards ours does not have**, so it establishes only that machine's node-card placement. The
reviewed clean-side contract now settles the Origin 200's logical PIMM-record role without
transferring that placement.

**Second, it does not name a catalogue part number**, only the manufacturer. The public DS2505
datasheet and OpenBSD 7.9 family table corroborate the compatible class. The exact fitted package
and route remain open.

**The board-document route has been searched and we do not hold one.** The roots are named so
the negative is checkable rather than a claim about a vocabulary: the library catalogue at 983
rows, the whole of `docs/`, 32 files across six cache directories, the three ASIC specifications
we hold, OpenBSD's pinned SGI branch, and the preservation-wiki export. **One fact explains the
gap better than any search technique: we hold ASIC specifications for the Bridge, the Crossbow
and the XBridge, and none for the Hub.** Searched and reported by ds-firmware.

**A second negative is stronger because it carries its own control, and it bears on how much of
this bus matters.** OpenBSD's kernel configuration for this platform declares one-wire on the
**I/O controller and nothing else**. The control: the same project's configuration for the
Octane declares it on **both** that machine's system ASIC and its I/O controller. **So the
configuration format can express a one-wire bus on a system ASIC, another SGI platform does
exactly that, and this one does not** — a negative bounded by the source rather than by anyone's
search terms.

**What that did not license when it was written, and the answer has since arrived.** The
tempting inference was that the firmware is this line's only consumer, and therefore that its
transaction shape is very nearly the whole specification. **That check covered one guest**, and
the two that decided it were unchecked.

**Both have since been checked, and the inference is refused rather than deferred.** IRIX drives
this register from its kernel and **needs strictly more than the firmware does**; Linux defines
the register in both trees and uses it in neither. **So there is no scope reduction to write** —
see the section on IRIX as a second consumer, which supersedes this paragraph's caution rather
than sitting beside it.

**This paragraph stood unreconciled after its own question was answered, and that is worth
leaving visible.** It is the same failure the review found in the open-items tail — correct new
material landing beside older text nobody swept — **recurring inside the fix for it, because I
swept the tail and did not sweep the body.**

### Is the identity ever compared, or only stored? Compared — and the answer has a twist

**Asked because if the firmware only validated the protocol and then stored and rendered what
it got, a structurally-valid identity with an obviously-synthetic serial would fabricate nothing
it semantically checks, and the stall could be lifted without inventing a fact about the
hardware. That reasoning is sound and the premise is false.**

**The string contains delimited manufacturing fields and the firmware searches it.** It finds
the first module-name substring, searches forward for the first serial-field label, then copies
bytes up to the next delimiter and encodes them into a serial-number structure. The search is
order-sensitive substring logic, not a formal keyed-record parser. A synthetic identity therefore
becomes this machine's serial number rather than merely passing through.

**So the standing rule holds for this device and the relaxation is refused.**

**The twist, and it points at a defect we already have.** That whole parse sits behind a
**machine-variant test, and is skipped on the variant this machine is supposed to be.** On a
real Origin 200 the firmware would not run it.

**But our machine does not currently read as that variant** — the same configuration flag is
already implicated in the Hub's slot-identity decode, which is this project's open co-requirement
for the console. **So on our machine as it stands, the parse runs when a real one's would not.**

**Two consequences worth stating separately.** The rule holds **more tightly on our machine than
on the hardware**, which is an unusual direction and worth knowing before anyone reasons from
"a real Origin 200 would not care." And **the variant flag now has a second consumer**: it
decides the arbitration signal the system controller must broadcast, and it decides whether the
board identity is semantically parsed. **A fix there moves both**, which raises its value and
means whoever lands it should expect this document to need re-reading.

### IRIX is a second consumer, and it needs strictly more than the firmware does

**The firmware is not this line's only consumer, and the tempting scope reduction this document
declined to write is now positively refuted rather than merely unsupported.** IRIX's own kernel
carries a one-wire driver whose Hub accessor **writes phase and timing into this same control
register and polls it until the completion bit sets** — the register the firmware stalls on,
followed through the call chain rather than inferred, in IRIX 6.5.7.

**And it asks for more than the firmware does. It loops reading manufacturing data until the
part stops returning it.** So IRIX expects **an add-only memory part with walkable pages**, not
an identity-only part. **A model sized to the firmware's identity read would satisfy the
firmware and stall IRIX**, which is the guest this whole effort is aimed at.

**The staging that follows, and it is the useful planning consequence:** the **identity**
transaction is what stands between us and the vendor's standalone shell; **the page walk is what
stands between us and booting IRIX.** Those are different amounts of work behind one register,
and knowing which is which decides what a first implementation should aim at.

**Linux is not a third consumer.** The register is **defined by name in both the current mainline
tree and the 2018 snapshot, with no C user in either** — so the name is public in both and can be
cited here, while the behaviour has no Linux-side requirement. **That negative carries its own
control history worth repeating**: ds-firmware's first control for it returned zero as well, so it
discriminated nothing and was replaced by one that demonstrably finds a C user where one exists.

### Two findings that converge, from different sources answering different questions

**This document already recorded that the firmware's failure paths include a redirection that
loops — contents that can point elsewhere. IRIX's driver walks pages until they run out.** Both
describe **add-only memory with redirection** rather than a bare NIC.

**Those were read off different sources, by different people, answering different questions**,
which makes it corroboration rather than replication — the distinction this project keeps having
to draw. Neither of us set out to confirm the other.

**The compatible part class is now multiply sourced.** The public manual pages name this class on
SGI's other one-wire lines, while the DS2505 datasheet and OpenBSD 7.9 family table corroborate the
add-only-memory family. The reviewed clean-side contract settles which logical record this bus
serves; none of those sources settles the Origin 200 package placement or copper route.

### What this does to the stall rule

**The rule was written for one transaction and there are two.** A protocol-valid synthetic
identity fabricates comparatively little for the firmware's read. **Fabricating manufacturing
page contents for IRIX is a materially worse proposition** — more content, more consumers, and a
guest that acts on it.

**The contract now settles both records.** The bus must expose a DS2505-compatible PIMM responder
and a separate family-`0x09` module-identity responder. Each has its own synthetic ROM identity.
Neither may borrow another board's identity or copy a real unit's manufacturing values. Until
those responders exist, an empty bus describes only the model's incomplete state, not the Origin
200 hardware contract.

### A similar record format does not make this the Bridge responder

**The manufacturing fields rendered from both Hub responders have the same text shape as the
Bridge's board identity, which [the Bridge datasheet](bridge.md) documents in full.** Format
resemblance does not make the concatenated Hub string a formally parsed keyed record.

**Matching field descriptions are an observation, not a shared mechanism.** The family-`0x0b`
Hub responder carries the PIMM record rather than the Bridge's IP29 board identity. The separate
family-`0x09` responder carries module identity. Format resemblance does not establish shared
contents, parser code or physical routing.

**The constraint list this section carried has been withdrawn.** It was copied from the Bridge
datasheet, which on 2026-09-24 corrected its reading of the stored page: the part holds a binary
record with fixed field offsets, and the `Field:value;` text is what SGI's formatter produces
from it. The rules for a text parse (a delimiter at a fixed byte, check bytes inside the text,
a terminator after every value, iterative construction) do not apply to the stored bytes.
[The Bridge record layout](bridge.md#the-record-layout-and-every-constraint-on-building-it)
gives the offsets, and the
Hub module-serial design (citation: `docs/designs/origin2000-hub-module-serial-route.md`) already builds
its family-`0x09` pages at those offsets, padding unused bytes with `0xff`. Whether the family-`0x0b` PIMM record uses the same
offsets is not established here.

### An adjacent-thing hazard on this exact question, which is tighter than usual

**A published specification names a Dallas part, by manufacturer part number, on this same
protocol, for the Crossbow.** Every element of that citation is correct — the protocol matches,
the part number is real, the document is authoritative.

**It is not this part.** It is a different ASIC, and **our machine has no crossbar at all.**
Anyone reaching for it because it is the only concrete part number in reach would be citing a
true fact about the adjacent thing, with everything around the citation checking out. **Recorded
here so that the next person to find it meets this paragraph first.**

**What a paragraph like this actually buys, corrected by the person it was about.** It was
tempting to record that this one *prevented* a wrong citation. **It did not** — the suggestion
had already gone out before the note was consulted. **What it did was make the withdrawal fast
and unambiguous** once a contradicting source turned up, because the reason the citation was
untrustworthy was already written down and did not have to be reconstructed under pressure.
**That is a lower bar than prevention and still worth the paragraph**, and stating it at the
lower bar is the honest case for writing more of them.

**A prior about the part family exists and is deliberately not stated as a finding.** Every SGI
one-wire line anyone has named carries an add-only memory part of the same family. That is
consistent enough to be worth testing a trace against and nowhere near enough to assert, so it
is not in this datasheet's claims.

### One discrepancy, not to be resolved from memory

**A real-machine boot log in our cache reports a one-wire family code that our pinned OpenBSD's
driver does not match, and that the same project's family table does not list.** Both cannot
describe one driver version.

**Nobody should reconcile that from recollection.** If it becomes load-bearing, **the
manufacturer publishes family codes per part directly** — third-party published documentation,
open to anyone including a clean-side implementer, which makes it the cheapest instrument here
and the one with no provenance question attached.

**What the timing values mean physically.** The header gives encodings and a reset default. It
does not say what pulse width or sample offset a real part requires, or what the two phase
fields shape. **A model that ignores the timing entirely and always completes will satisfy this
firmware** — and would not satisfy a guest that programs different timing and expects different
behaviour. Recorded as a known simplification rather than a fact, so nobody later reads silence
as licence.

**Whether anything but this firmware drives it — CLOSED, and this entry is retained only to
mark that it was.** It is answered above: IRIX's kernel drives this register and needs strictly
more than the firmware does. **The entry stood here as open after the answer landed**, telling a
reader the opposite of what the body says, which is the reopening sweep running backwards — a
question was *closed* and the places owning it as open were never updated. The stale text
follows, struck: ~~a register only one guest touches can be modelled more loosely than one two
guests disagree about.~~

## What an empty bus reads as, and why that is the honest thing to build first

**This document previously left this open. It is answerable from the bus's own electrical
convention, which is public and not specific to this machine.**

**The line is open-drain with a pull-up: idle is high, and a device signals by pulling it low.**
**So with nothing attached, every sampled bit reads one** — the master drives its pulse, releases,
samples a line nothing is holding down, and gets a one. A part announces itself by pulling low;
silence on the wire is electrically indistinguishable from an empty bus, because that is what an
empty bus *is*.

**So an empty bus is a defined state with a defined signature, not an absence of behaviour.**
Completion signals normally — the master's own timing ran — and the data bit is one every time.

**That matters because it makes a genuinely empty bus buildable without inventing anything.** A
master with correct slot mechanics and no part attached moves the machine from *"the register
never answers"* to *"the bus answered and nothing is out there"* — **a different and more
accurate failure**, and one the firmware already has a path for, since its identity library
distinguishes *nothing present* from *bad check value*.

**An empty bus remains useful as a model-development state, but it is not the hardware
contract.** The finished model needs the settled DS2505-compatible PIMM responder. It must use
synthetic per-unit values and must not borrow another board's identity or copy a real machine's
manufacturing record.

## Open questions

- **Which package and reference designator identify the DS2505-compatible responder for the
  installed PIMM's manufacturing record on the Origin 200 (IP29), and which connector contact
  and copper route connect it to the Hub.** Public photographs do not show a legible package
  marking or trace continuity, so neither of the two candidates below is established over the
  other: the responder package mounted on the PIMM itself, or mounted on the IP29 System Board
  and programmed for the fitted PIMM. The hardware contract fixes the logical Hub-to-PIMM
  manufacturing identity and its contents; the available evidence does not assign a package,
  reference designator, connector contact, or copper route. An IP29 or PIMM schematic, assembly
  drawing, bill of materials, package closeups that identify the responder and its connector
  trace, or a continuity measurement would settle it.

## qtest checklist

- **The four guest-writable fields round-trip** — the two timing phases, the pulse width and
  the sample point — **and specifically a value with bit 32 set survives**, which is the
  truncation this document warns about and which a 32-bit implementation passes every other test
  while failing. **"The full field layout" was the earlier wording and it is wrong: the low two
  bits are driven by the device, not the guest**, and a test asserting they round-trip would
  demand exactly the behaviour that makes the completion flag guest-settable — the thing the
  contract forbids.
- **Reset values: assert the two timing-phase fields and *nothing about the completion bit*.**
  **Both phase fields carry `0x31`** — stated here because an earlier revision instructed
  asserting the actual constants without carrying them, so an implementer had to take them from
  another document. **The same caveat as everywhere else applies: that value comes from a header
  constant named for reset defaults which has no consumer anywhere**, so assert it as the value
  this model chooses rather than as a hardware fact. **The
  completion bit's reset value is unsettled — three positions exist and this document refuses to
  pick — so a test asserting either would certify a guess while looking like a check.** That is
  this document's own stated failure mode, and it reached the checklist; found by both reviewers
  independently.
- **Assert instead the property that holds under every candidate reset value**: that after a
  transaction is started, the completion bit reads clear until the transaction finishes. **That
  is the behaviour the resolution rests on, it is testable, and it does not depend on which
  reset reading is correct.**
- **A write starts a slot and the completion bit is not set before the slot completes.** Assert
  the *not-yet* case explicitly: a model that reports completion immediately passes any test
  that only checks completion eventually.
- **A slot finishes at its deadline on the virtual clock and not before.** Step the clock to one
  nanosecond short of the slot length, assert completion still reads clear, step the last
  nanosecond, assert it reads set with the sampled level beside it. **An eventually-completes
  check passes both of the ways this goes wrong** — completion reported inside the write, and
  completion delivered by a host timer callback that arrives whenever the big lock next frees
  up. Pinning the boundary separates them.
- **The received-data bit reflects what an attached responder drove**, and differs between a
  responder driving one and driving zero. **A test that only checks completion would pass a
  model with no bus at all**, which is the trap this document exists to prevent.
- **With no part attached, the sampled bit reads one, every slot, and completion signals
  normally.** See the empty-bus section: an empty bus is a *defined* state, not an absence of
  behaviour, and a model must produce it rather than fall silent.
