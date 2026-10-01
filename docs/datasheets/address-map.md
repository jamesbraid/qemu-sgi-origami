# Datasheet: SN0 physical address map

References rendered as citation identities name material outside this document
set. Those targets are not included or cleared for redistribution. Review and
runtime statements retain their recorded dates and scope.

**Status:** reviewed (coordinator with codex, 2026-08-09) — **ready**; the
round-one findings are all closed.
**Fork file(s):** `hw/mips/sn0.c` (memory regions and aliases),
`hw/mips/sn0.c` (the PROM-shaped region and its configuration block)

**Sources used:** Linux `arch/mips/include/asm/sn/addrs.h`,
`asm/sn/sn0/addrs.h`, `asm/sn/sn0/hubio.h`, `asm/sn/sn0/hubmd.h`, `asm/sn/io.h`,
`arch/mips/include/asm/mach-ip27/spaces.h` and `arch/mips/sgi-ip27/ip27-init.c`
(public, GPL, and themselves derived from SGI headers); OpenBSD
`sys/arch/sgi/include/mnode.h`, `sys/arch/sgi/xbow/hub.h`, `sys/arch/sgi/sgi/ip27.h`
and `sys/arch/sgi/sgi/ip27_machdep.c` from the `the-machine-hall/openbsd-sgi`
tree (public, ISC). OpenBSD retired its SGI port, so those files exist on no
checked-out branch: the reference clone's working tree holds only a README, and
the mainline OpenBSD clone beside it has no SGI architecture directory at all.
Read them without checking anything out, and **cite the commit rather than a branch**:
`git -C refs/openbsd-sgi show 2df91d80c1:sys/arch/sgi/xbow/hub.h`. That commit is the
head of the branch matching OpenBSD 7.9 GENERIC-IP27, the kernel this project boots,
and everything here was read from it.

**A branch name is not a citation, and this page carried one until 2026-08-10.** It said
the facts were byte-identical on the tree's other branch — true when written, and
unfalsifiable within a week, because that branch advances and the sentence named no commit.
**A claim pinned to a moving reference does not become wrong, it becomes uncheckable**, and
a reader cannot tell the difference. The working tree has also moved during this project,
so *look at the directory* is not reproducible either; a commit is. Also used: SGI's *Origin
and Onyx2 Programmer's Reference Manual*, document 007-3410-001, which specifies this
M-mode address model directly, and SGI's *Origin200/2000 IP27 PROM
Technical Reference Manual*, document 108-0170-001, both in the project document
library; the IRIX 6.5.7 machine-readable Hub register table
`stand/arcs/lib/libkl/hwreg/hwreg_hub.hwreg` and the headers
`irix/kern/sys/SN/SN0/addrs.h`, `hubmd.h`, `hubcore.h`, `ip27config.h` and
`irix/kern/sys/mips_addrspace.h`; the shipped-PROM measurement recorded in
the firmware boot analysis (citation: `docs/research/11-firmware-and-boot-entry.md`). We only
read reference sources and never copy their code or file structure
(source-use policy, decision D-0002).

**Facts that are IRIX-derived.** The exact field order and byte offsets of the
PROM configuration block, the constant values SGI's own build uses for an
Origin 200 with a 180 MHz processor, the four-way numbering of the Hub's uncached
attributes, the Hub's internal sub-block split of the memory-directory register
region, and the placement of the Hub's core arbitration registers inside that
region all come from the IRIX 6.5.7 sources. Every one of them except the
attribute numbering, the sub-block split and the arbitration-register
placement is confirmed against a public header: OpenBSD's `struct ip27_config`
is field-for-field the same structure at the same offset with the same magic
number. Everything else in this document is dual-sourced between Linux and
OpenBSD, and most of it is confirmed a third time by SGI's PROM manual.

## Role

The Origin 200 presents one flat 40-bit physical address space that every node
shares. The node number lives in the top eight of those forty bits, so each
node's memory, Hub registers, XIO windows and boot flash appear at a fixed
stride in one global space, and a program can reach any node's resources by
changing those eight bits alone. Alongside that global view, each processor also
sees a set of position-independent aliases that always resolve to its *own*
node, so firmware and per-node kernel code can be written once and run anywhere
without knowing which node it is on. Above the physical address space sits the
processor's own addressing: a 64-bit window that carries the cache attribute in
the high bits, and the two 512 MB thirty-two-bit compatibility segments that the
reset vector and the firmware parameter block are reached through.

**Our model carries a 42-bit machine bus address, and the two counts measure
different things rather than disagreeing.** Forty of those bits are the machine's
own address space described above. The other two are the processor's uncached
attribute — the code that distinguishes an uncached reference to node memory from
one to a Hub register, a crossbar widget or a special memory operation. On the real
machine that attribute travels to the Hub as a separate signal beside the address,
so it has nowhere to live inside forty bits. Our model gives it somewhere: it rides
as bits 41:40, above the forty, so a single address value says both *where* and
*which space*, and the memory system can route on it without a second channel.
James ratified this representation as a machine contract, and changing it is a
contract change of the largest kind available here rather than an owner's
judgement — the reasoning and the three rejected alternatives are in
the address-space model review (citation: `docs/reviews/address-space-model-review.md`), and the
ratification in
the decision that the machine speaks a 42-bit machine bus address (citation: `docs/decisions/D-0014-42-bit-machine-bus-address.md`).

**The attribute-to-window mapping is rotated by one, and that rotation is what
catches a hand-written constant.** Attribute 3, plain uncached memory, is placed at
window **zero**, so an ordinary physical address still means memory and nothing that
ignores the attribute has to know any of this exists. The other three shift up by
one: Hub special, attribute 0, becomes window 1; I/O space, attribute 1, becomes
window 2; and memory special, attribute 2, becomes window 3. Reading the four bases
in order and assuming the window matches the attribute number gives the wrong space
for every one of them. QEMU routes compatibility-segment references through
window zero, which makes node-local physical `0x1000` reachable at
`0xffff_ffff_8000_1000`. That model choice does not settle what attribute or
route real hardware uses for every compatibility-segment reference.

Everything on our side of the guest speaks this form — every qtest constant, the
monitor, and whatever the debugger prints. The processor address this page gives for
the base I/O controller's console, `0x9200_0000_0862_0178`, is machine bus address
`0x0000_0200_0862_0178`: the attribute bits are removed from bits 58:57 and reappear
as window 2 in bits 41:40, and the low forty bits are untouched. **Anyone comparing a
constant on this page against one in a test is comparing two different notations for
the same address**, and the translation between them is that one substitution.

Getting this document right is what makes the rest of the machine possible. The
Hub registers, the crossbar, the I/O bridge and the boot firmware are all
addressed through the regions defined here, and every one of them is a
cross-cutting contract: a change to a constant on this page changes the machine
for every device.

The base Origin 200 projection has one node, so its selected node number is zero
and its node-strided bases collapse onto the same window. The separate dual-module
target uses the same node-relative arithmetic with two node bases; node count is
therefore part of the existing machine contract rather than only a future option.

## Register map

### Processor-side address spaces

The processor issues 64-bit addresses. The high bits pick a cache attribute and,
for uncached references, which of the Hub's four uncached spaces the reference
belongs to. These are the bases with the node number zero; a node's window
starts at the base plus the node number shifted left by 32.

| Base address | Width | Name | Meaning |
|---|---|---|---|
| `0xa800_0000_0000_0000` | 4 GB per node | cached memory | Coherent cached access to node memory. |
| `0x9600_0000_0000_0000` | 4 GB per node | uncached memory | Uncached access to the same memory. |
| `0x9000_0000_0000_0000` | 4 GB per node | Hub special | Boot flash windows, the node's own uncached memory alias, and the directory/ECC back door. |
| `0x9200_0000_0000_0000` | 4 GB per node | I/O | Small and big windows onto crossbar widgets, including the Hub's own registers. |
| `0x9400_0000_0000_0000` | 4 GB per node | memory special | Fetch-and-op and other special memory operations. Not on the first-boot path. |
| `0xffff_ffff_8000_0000` | 512 MB | compatibility cached | Node-local physical `0x0000_0000`–`0x1fff_ffff`, cached. |
| `0xffff_ffff_a000_0000` | 512 MB | compatibility uncached | The same node-local physical range, uncached. This is where the reset vector lands. |

The four uncached bases differ only in two address bits, which the processor
passes to the Hub as an uncached-attribute code: zero selects Hub special, one
selects I/O space, two selects memory special and three selects plain
uncached memory. That four-way numbering is IRIX-derived, from the SN0 Hub
header's list of uncached attributes. Linux confirms the mechanism and one of the
four values, saying in its Origin address-space header that the machine uses the
processor's uncached-attribute feature and that attribute three selects uncached
memory. The PROM manual's address-entry shortcuts for the power-on diagnostic
monitor independently spell out the same five top bytes.

Both operating systems reach the firmware parameter block through the
compatibility cached segment rather than through the 64-bit window, because a
compatibility address needs no attribute chosen for it: the processor supplies one,
and it is always attribute 3, uncached memory. Our model must therefore make
node-local physical `0x1000` readable at `0xffff_ffff_8000_1000`.

### The node number in the physical address

| Mode | Node-number bits | Node window size | Node count |
|---|---|---|---|
| M mode (what we implement) | 39:32, eight bits | 4 GB | up to 256 |
| N mode (not implemented) | 39:31, nine bits | 2 GB | up to 512 |

**M mode is what real hardware reports, and the guest reads it rather than assuming it** —
an earlier version of this page called it a default, which understated the evidence and
misdescribed the mechanism. OpenBSD derives the mode from a bit in the Hub's
network-interface status register, computes its node shift from that bit, and only then
prints the line the first-banner boot is aiming at. So the printed mode is **an echo of a
hardware bit**, not a compile-time assumption, and two real Origin 200 captures eighteen
years apart both report M mode. Everything below is M-mode geometry.

**That makes the bit's reset value load-bearing in a way a default never would be**, and it
is the exact failure this project's working rules keep as a worked example: a
network-interface register that decodes and answers but was never reset reads its
zero-initialised value, reports the wrong mode, and sends the guest off computing every
node-relative address with the wrong shift — silently, because the machine looks like it
works. **The two captures are what turn "we chose M mode" into "the machine says M mode",
and only the second constrains our model.**

### Node memory

Node memory occupies the bottom of each node's window, in up to eight banks of
512 MB, bank *b* starting at node offset *b* × 512 MB. Eight full banks exactly
fill the 4 GB node window.

**Eight is the Hub's capacity; this machine wires four.** SGI's PROM manual states it for the
Origin 200 directly — banks are populated from the two outermost memory slots, making bank 0,
to the two innermost, making **bank 3** — and a real machine's firmware reports exactly four.
The geometry above is the chip's and stays as written, because the stride and the addressing
do not change; **what changes is that banks four through seven are empty on this board, and a
model populating them describes a machine that was never built.**

The size of each bank is a three-bit code in the
memory-directory configuration register, where zero means the bank is empty and
a code *n* means 4 MB shifted left by *n*. The IRIX register table gives every bank's
code a reset value of seven, so an unprogrammed Hub claims eight full 512 MB banks. The
PROM manual's bank shortcut for its diagnostic monitor adds 512 MB per bank, which
confirms the stride from a second source.

**A bank's size is written two different ways in two different places, and confusing them is
the trap here.** The **register** holds a code: eleven are defined, `4 << code` megabytes
each, and SGI annotates exactly four as valid in the memory configuration register — empty,
64 MB, 128 MB and 512 MB — with a fifth marked broken on the first Hub revision. That
annotation is **SGI's own**, in its own headers, and is carried into Linux verbatim. The
**configuration graph** holds something else entirely: a per-bank array of **plain megabyte
counts**, which a guest prints and sums directly without ever consulting a code table.

**So our synthesized firmware must write megabytes into the graph and codes into the
register, and crossing them produces a machine that works on one path and not the other.**
A guest reading the graph would see a bank of size 5 megabytes where 128 was meant; a guest
reading the register would decode 128 as 512.

**An earlier version of this page got that backwards, and the correction is worth keeping
because the error is the day's own lesson.** A real Origin 200's firmware reports a **256 MB
bank**, which is the code between 128 and 512 and carries no valid annotation — so this page
concluded the annotation was one author's partial view rather than a constraint, and that a
model rejecting an unannotated code would reject a real layout. **The 256 MB never came from
that register.** It is the graph's plain megabyte field, printed by a guest walking the
configuration graph, and it says nothing whatever about which codes the register accepts.
**Right number, wrong instrument** — applied to a table it was never measured against.

**What is established, and what is not.** Established: the graph really does describe a
256 MB bank on a real machine, and SGI really does annotate only four codes as valid in the
register. Both are firm and they do not conflict, because they are about different fields.
**Not established: what that machine's memory configuration register actually held** — no
capture we hold reads it. Whether a 256 MB bank is expressed there as an unannotated code,
as two 128 MB banks, or some other way is open, and it wants a capture of the register
rather than an argument.

**The same capture shows the banks are independently sized, which the uniform description
here could hide.** A real 1 GB machine reports four banks of 256, 512, 128 and 128 MB — not
four equal ones. That is consistent with the fixed 512 MB stride above, because a bank is a
slot at a fixed offset whose contents may be smaller than the slot; but it means **a single
bank size is a property of that machine's memory configuration and never a constant of the
architecture.** It is also the layout to prefer for a test, since equal banks cannot
distinguish a model that reads each bank's own code from one that applies the first bank's
size to all of them.

### I/O space windows

Each node's 4 GB I/O space window carves up unevenly, and the shape is worth
stating exactly because it decides which memory regions the model creates.
Sixteen small windows of 16 MB each fill node offsets `0x0000_0000` through
`0x0fff_ffff`, one sixteenth of the window. The next sixteenth, `0x1000_0000`
through `0x1fff_ffff`, decodes to nothing. Seven big windows of 512 MB each fill
`0x2000_0000` through `0xffff_ffff`, the remaining seven eighths. Every constant
that forces this shape is in Linux's Origin address headers: the small window
size is 16 MB, the first big window sits one big-window size above the XIO
I/O base, the big window size is 512 MB in M mode, and there are seven entries.

| Node offset | Size | Contents |
|---|---|---|
| widget number × 16 MB, for widgets 0 through 15 | 16 MB each | Small window: the first 16 MB of that widget's XIO address space. |
| `0x0100_0000` | 8 MB | The Hub registers of whichever node the reading processor is attached to. The address carries node number **zero**, not the reader's own. |
| `0x0180_0000` | 8 MB | The Hub registers of the node named in the address. |
| `0x2000_0000` + big window number × 512 MB, for windows 0 through 6 | 512 MB each | Big window: a programmable 512 MB slice of some widget's XIO space. |

**A small-window number is an address field, not an inventory slot on an
Origin 200.** The plain machine has no Crossbow between the Hub and its base
Bridge. The Hub's direct link therefore carries every outbound small-window
address to that one Bridge, regardless of the widget-number field. Widget 1 is
the exception because the Hub decodes its own registers locally. The Bridge's
complete 16 MB face, including its registers, translation entries, PCI
configuration apertures and programmed-I/O device windows, consequently answers
through widgets 0 and 2 through 15. Widget 8 remains the number firmware assigns
to and inventories for the Bridge. Widget 0 remains the discovery address. A
reference through widget 2 does not establish a second physical device at
widget 2.

This is the settled Origin 200 no-crossbar contract recorded by D-0009, not a
new address-map choice. It follows the physical topology in SGI's
*Origin200 and Origin200 GIGAchannel Owner's Guide*, document 007-3708-002,
Figures 1-4 and 1-5, printed pages 6-7, PDF pages 32-33, original SHA-256
`ae6857707a113bd63b77a3a8187c81beed76cea1406592a38a735c4aa2c462dc`,
retained at
`refs/techpubs/techpubs.sgi.com/library/manuals/3000/007-3708-002/pdf/007-3708-002.pdf`.
Both figures were checked in the original page images. Figure 1-4 draws the Hub
to the base Bridge directly. Figure 1-5 leaves that base path intact while a
separate link reaches the GIGAchannel cabinet's Crossbow and second Bridge.
The figures establish physical connectivity, not the firmware's chosen widget
number. The Bridge's 16 MB face comes from *Bridge ASIC Specification*, revision
4.0, Tables 43-44, printed pages 55-56, PDF pages 69-70, original SHA-256
`8b30514b718f9259aa28dc95552fcbc7114cceeeb54fcb8330fd69aff377e01a`.
For that scan, PDF page equals printed page plus 14. The mapping is trusted for
these tables because their original page images were reopened and matched by
printed page number, table title and surrounding register names.

Widget 1 is the Hub itself, which is why its small window is split into a
position-independent half and a node-explicit half. The position-independent
half is how every guest reaches its local Hub without first learning its own
node number, and how it does that is worth stating precisely. The address is
formed with the node-number field *zero*, not with the reader's own node number,
and the Hub the reading processor is attached to answers it. Linux builds its
local-Hub address as widget 1's small window on node zero, and its remote-Hub
address as the named node's small window plus `0x0080_0000`; OpenBSD's Hub header
does the same. On an Origin 200, where the only node is node zero, the two
readings coincide and nothing breaks. They diverge the moment a second node
exists, which is the case this document models the striding for from the start.

#### The node number a Hub answers to is programmed, not fixed

**Settled, 2026-09-01 — decision route recorded here.** This is a reference-determinable
hardware fact (what the real Hub does), not a design choice. James delegated the
four-CPU/two-node fix to that stream and declined to personally ratify facts of this kind — his
words, relayed: "you're responsible for getting it fixed; I'm not ratifying anything." So the
route for this rule is the source verdict in this datasheet, the implementer review on the pull
request, then the normal review-then-land flow — not a ratification hold. The paragraph above
says a node-qualified address with the node field zero
is answered by the reading processor's own Hub. The precise rule, from the IRIX PROM and the
public Hub headers, is that **a Hub treats a node-qualified access as local when the address's
node field equals its own *programmed* node number** — the writable node field of the
network-interface status register (`NSRI_NODEID` in `NI_STATUS_REV_ID` at NI offset `0x600000`
— the same register and field names Linux's SN0 hub headers use; OpenBSD's sys/arch/sgi carries
neither name). That field is **zero at reset** and stays zero until the global
master assigns real node numbers, which happens only after topology discovery and arbitration.
So at reset every Hub answers node-field-zero locally — `REMOTE_HUB(own_number)` is always the
local Hub — and the decode must be a **comparison against the programmed field, not a fixed
board position**.

**This bites on every multi-node boot, not as an orphan corner case.** Every node runs its
early memory- and directory-initialisation through `REMOTE_HUB(get_nasid(), …)` at programmed
number zero, because assignment has not happened yet. On real hardware each node's
node-field-zero access lands on its own Hub, so each initialises its own memory and directory.
A model that instead places node-qualified windows by a fixed board number sends every node's
node-field-zero access to the physical node-zero Hub: node zero is initialised twice, and the
second node's own memory and directory are never touched — its later directory test
miscompares. The PROM corroborates the intent in its own comment: right after a node programs
its real number, it **re-initialises its directory** because "the directory entries are all
marked owned by node ID 0" — the early zero-numbered init is by design and must have landed on
that node's own directory.

Edge semantics, for the model and its test:

- **Routing of a non-local access is table-driven and two-stage — that is the binding rule.**
  When the address's node field does not equal the issuing Hub's programmed number, the Hub
  routes the access by looking the target up in its per-node routing table (the PROM programs
  `NI_LOCAL_TABLE(target)` with the route), out over the CrayLink; the receiving Hub then claims
  it against its own programmed number. The issuing Hub does not forward unconditionally — an
  unrouted target has nowhere to go — and the global master programs these tables before any
  node switches its number. **A model of a fixed small topology may reduce this** without
  consulting the table on the access path: for the two-node pair, decode local if the address
  number matches the issuing Hub's own programmed number, else route to the single peer if it
  matches the peer's, else take the unclaimed-number error shape below. That reduction is valid
  only because there is exactly one possible peer; a machine with more nodes or a crossbar must
  model the per-node routing table as the register state it is. The reduction is an explicitly
  permitted implementation of the hardware rule, **not** a restatement of the contract.
- **Assignment is distributed and barrier-synchronised.** Each node's local master programs its
  own number (a write to its local Hub's status register), with the routing tables already in
  place and the whole phase bracketed by inter-node barriers, so all nodes switch together. The
  model may treat each Hub's programmed-number write as the atomic switch point; on the normal
  path there is no long span where two Hubs answer the same number. The directory re-init above
  is the post-switch fixup.
- **Two Hubs holding one number are resolved by the routes, not refused.** The decode above is a
  comparison at each Hub against its own programmed number, and nothing in the sources read gives
  a Hub any view of what number another Hub holds, so nothing in the machine can notice that two
  hold the same one. An access whose field matches two Hubs reaches whichever one the programmed
  turns lead to, and that Hub claims it. On the normal path this does not arise, because
  assignment is barrier-synchronised; it arises in a model where the nodes renumber far apart. A
  model that refuses a duplicated number instead leaves a renumbered node with no route to the
  reset number zero while its neighbours still carry it, which takes away that node's own
  firmware text and exception vectors: ours did, and a node whose renumbering ran long fetched
  its exception vector from the claim error floor 826 times before it recovered.
- **(Open) The fault shape of an access to an unclaimed node number is not established here.**
  The PROM relies on such an access failing — discovery treats a non-completing probe as "node
  not present" — but the sources read do not pin whether the hardware signals a timeout, a bus
  error, or a silent discard. Conservative model, pending observation: an access whose node
  field matches no programmed Hub returns an error / does not complete, and is **never** silently
  served by the wrong Hub, which preserves the probing semantics without asserting the fault
  type. The qualified-window error path is in any case lightly exercised: the PROM's remote
  probing during discovery rides vector-routed PIO, which carries its own status and timeout
  semantics, so pinning only the never-wrong-Hub property here is sufficient.

Provenance: read from the **IRIX 6.5.7** IP27 PROM — the network-interface node field and its
programming, the memory/directory init and its `REMOTE_HUB(get_nasid())` addressing, the
Change-Node-ID sequence and its directory re-init, and the routing-table programming. The
running machine boots **IRIX 6.5.30**, and releases can disagree; the nasid-0-local decode is
**confirmed on 6.5.30** by the instrumented two-node runs, and where a release difference
appears the 6.5.30 observation governs and this file records the delta. The register and field
names are public — Linux's SN0 hub headers carry `NI_STATUS_REV_ID` (`0x600000`), the
`NSRI_NODEID` node field, and `NI_LOCAL_TABLE`; the pinned OpenBSD SGI branch does not carry
these particular names. **This changes the address-map contract; it was flagged to James, who
delegated it as a reference-determinable hardware fact — see the decision route at the top of
this subsection.**

Small window 0 has a defect recorded against it, and a workaround built around
that: a processor-interface fault in the Hub is said to make accesses to it
unreliable, and firmware programs the last big window, window 6, to cover widget
0's first 512 MB so that software can reach widget 0 another way. Big window 6
is consumed this way and that much is settled.

**Two different accounts of why widget 0 is avoided are now in play, and this
document does not choose between them.** The claim above — a Hub
processor-interface fault making the small window unreliable — is the one this
tree has carried, and it is already flagged here as unestablished. **SGI's own
firmware gives a different reason in its own comment: that it avoids widget 0
because of a problem with error handling there**, and it does not say what the
problem is.

**Nobody has established that these are the same thing.** They may be one defect
seen from two sides, or two unrelated ones, and the firmware's own wording is too
thin to tell. **Both are recorded here rather than reconciled**, because choosing
between two accounts is deciding rather than documenting — and the practical
consequence is identical either way: firmware programs big window 6, and small
window 0 must still decode because one guest ignores the workaround.

**What does not follow, and was asserted here until Linux disproved it, is that
software therefore uses the big window.** Mainline reads widget 0 through small
window 0 directly, by name, using the raw address macro rather than the
substituting helper that exists for exactly this. So the workaround exists, the
window it targets is right, and one guest ignores it. Small window 0 must
decode.

**The other guest does not ignore it, so this machine owes both paths.** OpenBSD
has now been checked against the pinned release branch and it redirects: the one
function both its identification paths call returns the big-window address when
the widget number is zero. **So each guest reaches widget 0 by the path the other
does not use**, and satisfying either alone leaves the machine broken for the
other.

**The second path is a firmware obligation rather than a decoding one, which is
what makes it easy to miss.** OpenBSD needs translation entry 6 to already point
at widget 0 when it starts, and it never programs that entry itself — its window
allocator skips entry 6 while scanning for a free one, so it both depends on the
entry and refuses to claim it. On real hardware the firmware sets it. Here that
is ours to set, and **no Linux boot can ever reveal that it has not been done**,
because Linux never reads through that window. Both determinations, with file and
macro citations, are in
how each guest forms the widget-0 address (citation: `docs/library/linux-widget0-address-formation.md`).

Each big window is programmed by one translation-table entry in the Hub's I/O
interface, at Hub register offset `0x40_0160` plus eight times the window
number. The entry carries the target widget number in bits 11:8, a space select
in bit 12 that selects the widget's memory space when clear and its other space
when set, which Linux and IRIX name I/O space and OpenBSD names device space, and five
offset bits in bits 4:0 that supply XIO address bits 33:29 — that is, they
choose which 512 MB slice of the widget's space the window lands on. Linux
numbers these entries 0 through 6 and notes that SGI's hardware manuals number
them 1 through 7; the IRIX register table indeed names them starting at one.
The offsets and the field positions agree exactly between the two, so only the
names differ.

Widgets 8 through 15 are the crossbar's device ports **on a machine that has a
crossbar. An Origin 200 does not** — its Hub's XIO link goes straight to the
I/O bridge, so nothing routes by widget number and every widget address reaches
the one chip.

**That is why widget 0 reports itself as a bridge**: it is the same bridge, not a
reduced crossbar answering on its behalf. SGI's own firmware says a directly
connected bridge can be given any widget number the firmware wants, so **widget 8
is a convention two firmware routines agree on rather than a place the bridge
sits.** OpenBSD's Origin 200 path — assuming widget `0xa` is the interrupt-carrying
face of that same bridge — is the ordinary case on such a machine rather than a
quirk. The determination and its five sources are in
what sits at widget 0 on the Origin 200 (citation: `docs/library/widget0-on-the-origin-200.md`).

### The reader-local aliases, and which forms carry reader-local expectation

The node-field decode — a node-qualified address is answered locally only when its field equals the
reader's own programmed node number — governs an address that *names* a node in its own bits. SN0
also has a second, orthogonal way for a low address to mean "the reading node's own memory":
**position-independent aliases** that route to the reader regardless of any node field. There are two
of them, one cached and one uncached, plus a mapped variant — and a model that folds them together,
or extends either past its bound, sends a node's own accesses to node zero. Both the running kernel
and the PROM depend on the split.

- **CALIAS — cached, and small.** A cached reference to low physical through the plain cached segment
  (`CAC_BASE`, `TO_CAC(x)`) is reader-local **only for the first N bytes**, where N is the Hub's
  programmed cached-alias size (`PI_CALIAS_SIZE`). The running kernel programs that size to **8 KB**
  on every Hub during node initialisation and warns if the PROM left it larger, so in the OS era a
  cached low reference is reader-local for `[0, 8 KB)` and **node-zero-absolute beyond it**. One
  hardware quirk the model must carry: a CALIAS access is validated as a node-zero address, so **node
  zero must be present in the Hub's region-present register (`PI_REGION_PRESENT`) or CALIAS accesses
  fault** — the kernel sets that bit on every node for exactly this reason.
- **UALIAS — uncached, and wide.** An uncached reference through the Hub-special window (`HSPEC_BASE`,
  `UALIAS_BASE`) is reader-local **unconditionally, for the full 256 MB** of the window,
  register-independent. Its bottom 128 KB is the `CPU_UALIAS`, whose low half is flipped per
  processor slice (a flip bit XORed for slice 1) so the two processors of a node get distinct
  scratch. This is the reader-local *uncached* face.
- **Plain uncached low is NOT an alias.** An uncached reference through the ordinary uncached segment
  (`UNCAC_BASE`, `TO_UNCAC(x)`) is **node-zero-absolute** — it is `TO_NODE_UNCAC(0, x)` by identity,
  a node-qualified address with the field zero, not a reader-local alias. Only the Hub-special UALIAS
  window aliases uncached low to the reader.
- **Node-qualified is how the OS era usually reaches per-node memory.** For everything else per-node
  — the config header, the KLCONFIG and KLDIR blocks, the per-node data area, the ARCS vectors — the
  kernel composes an explicit `TO_NODE_*(get_nasid(), …)` address, reader-local *by computation* (the
  running node id in the field) rather than by a hardware alias. These carry the node field, so the
  programmed-number decode routes them; they are not aliases and depend on neither bound.

**The mapped variant, and the one open line.** A reference through a TLB entry carries an uncached
cache-attribute in its page-table entry, and this model routes a mapped uncached reference by that
attribute — one value to the reader-local (Hub-special) carrier, another to node zero — independent
of the bare page frame the entry names. So a mapped uncached access is reader-local or node-zero per
the attribute the mapping sets, with no separate bound. What is **not established** is whether real
SN0 silicon routes a mapped uncached reference by that same attribute: the public sources do not pin
it (it lives in a non-public chapter), and a healthy boot exercises the form without stressing the
distinction, so it stays an open hardware-fidelity question. It gates nothing — the model serves
both routings consistently — and the [firmware re-base documentation](firmware-structures.md)
carries the same open line for the PROM-era transition.

**Provenance.** The alias and node-qualified form names are the public SN0/Linux names (`CAC_BASE`,
`UNCAC_BASE`, `HSPEC_BASE`/`UALIAS`, `CPU_UALIAS`, `TO_NODE_*`). The 8 KB cached-alias size and the
region-present requirement are read from the IRIX 6.5.7 node-initialisation path, stated
behaviourally. The mapped-attribute routing is this model's, verified structurally against the
xkphys attribute table. Released against IRIX 6.5.7, with the running 6.5.30 boot as a
non-discriminating cross-check.

### Composing a device address

**Compose these addresses from named terms; do not write the total.** Every error this
project has made on a device address was a dropped term in a hand-written sum, twice on
the same field, by three people who each had every constant correct. The terms are all
documented; what fails is the addition.

A register on a device behind an I/O bridge is the bitwise union of five terms:

| Term | Rule | Documented in |
|---|---|---|
| Space base | One of four, selecting the uncached attribute in bits 58:57. Device access uses I/O space, `0x9200_0000_0000_0000`, attribute 1. | the processor-side address spaces |
| Node offset | The node number shifted left 32 in M mode, 31 in N mode. | the node-number rule |
| Widget window | The widget number times 16 MB, for widgets 0 through 15. | the I/O space windows |
| Bridge device window | Slot 0 at `0x20_0000` and slot 1 at `0x40_0000`, both 2 MB; slots 2 through 7 at `0x60_0000 + (slot − 2) × 0x10_0000`, 1 MB each, ending at `0xbf_ffff`. | [the Bridge datasheet](bridge.md) |
| Register offset | Whatever that chip's own map says, relative to the chip. | the device's own datasheet |

The bridge's *own* registers take the first three terms only — its local register block
sits at the base of its widget window, with no device-window term.

**One collision makes this genuinely treacherous, and it is why the console field went
wrong twice.** Three unrelated regions of a single widget sit within a factor of sixteen
of one another:

| Address | What it is |
|---|---|
| `0x02_0000` | The bridge's PCI configuration space |
| `0x20_0000` | Slot 0's device window |
| `0x02_0178` | Serial port A's offset *inside* the I/O controller |

A dropped or added hex digit moves between them, **and all three decode and answer**.
The console base was once written as the widget plus `0x2_0178` with no device-window
term; that lands in configuration space at the first device's function 0. It is a real
region, it responds, and it is not a serial port — which is why the failure surfaced as a
bus error on a byte store rather than as an obviously dead address.

**A composed address can be checked term by term against a captured one. A hand-written
total cannot**, which is the whole argument for generating these rather than writing
them: give the generator a slot number and a chip-relative offset, and the terms it
cannot drop are the ones nobody has to remember.

### A worked check: four independent constants against one captured address

This is the strongest single confirmation of the addressing geometry we have, and it is
worth keeping as a worked example rather than as a claim, because whoever builds the
Bridge model will want an end-to-end address to reproduce.

**The observation.** An instrumented kernel on a real Origin 200 dumped the firmware's
configuration graph as it walked it, and the console descriptor's serial-port base read
`0x9200_0000_0862_0178`. That is a value a real machine's firmware wrote, catalogued with
the real-machine boot logs (citation: `docs/library/boot-logs.md`).

**The prediction.** Four constants, each documented from a different source before anyone
tried this sum, and none of them fitted to it:

| Term | Value | Where it comes from |
|---|---|---|
| I/O space, uncached attribute 1 | `0x9200_0000_0000_0000` | the processor-side address spaces |
| Widget 8's small window, eight times 16 MB | `0x0000_0000_0800_0000` | the small-window rule, and the I/O bridge answering as widget 8 |
| The Bridge's window for its third device | `0x0000_0000_0060_0000` | [the Bridge datasheet](bridge.md): devices 0 and 1 in 2 MB windows from `0x20_0000`, then devices 2 to 7 in 1 MB windows from `0x60_0000` |
| Serial port A inside the I/O controller | `0x0000_0000_0002_0178` | [the I/O controller datasheet](ioc3.md): the serial block's window at `0x2_0000`, port A at `0x178` inside it |

They sum to `0x9200_0000_0862_0178` exactly, with nothing left over.

**Why that is worth more than four separate confirmations.** Each constant was derived
from its own source — an address-space header, a bridge specification, a controller
register map — and none was chosen to make this address come out. The sum reproduces a
captured value at the first attempt, which tests **four layers at once**: that XIO
space is entered with attribute 1, that a widget's window is its number times 16 MB, that
the Bridge lays its device windows out as documented, and that the controller places its
serial block where the controller datasheet says. A single wrong constant anywhere in
that chain would leave a remainder.

Even the device-window term is not assumed: the same capture lists the machine's bus as
two disk controllers followed by the I/O controller, so the controller is the third
device, which is what selects the `0x60_0000` window.

**The negative it settles.** A console base of `0x178` — the value this project briefly
specified — is `0x2_0000` short of the chip's serial block and misses the device window
entirely. The composition above is what makes that visible; the guest's own generation
check does not, because it reads only the low twelve bits and `0x178` satisfies them.

### Hub register space

The Hub's 8 MB register window, reached through either half of widget 1's small
window, divides by function.

| Offset within the window | Unit |
|---|---|
| `0x00_0000` | Processor interface: real-time counter, interrupt pending and mask, error and status registers. |
| `0x20_0000` | Memory and directory controller. |
| `0x40_0000` | I/O interface: widget identity, translation-table entries, interrupt destination. |
| `0x60_0000` | Network interface: CrayLink status, reset and routing. |

Within a unit's 2 MB region the Hub decodes further 64 KB sub-blocks. The
memory-directory unit puts its main registers at `0x20_0000`, its performance
counters at `0x21_0000`, and the node's serial-bus microcontroller, light-emitting
diode registers and slot identity at `0x22_0000`. The processor interface uses
the same trick for two registers that sit outside its main block: a soft-reset
flag at `0x02_0068` and — the one that matters for us — the real-time counter at
`0x03_0100`. All three of Linux, OpenBSD and the IRIX headers give the real-time
counter that same offset, so it is a genuine decode and not an annotation. The
reply level is not a third: the IRIX processor-interface header and Linux both
place it at `0x00_0060`, inside the main block, and only the IRIX
machine-readable register table gives it `0x01_0060`.

The Hub's queue-arbitration registers
sit at `0x20_00b0` through `0x20_00e0`, inside the memory-directory region rather
than in a region of their own.

### Hub special space

The Hub special window per node, all offsets from that node's Hub special base.

| Node offset | Size | Contents |
|---|---|---|
| `0x0000_0000` | 256 MB | Uncached alias of the reading processor's own node memory. The bottom 128 KB flips on address bit `0x10000` according to which processor of the pair is reading, so each processor gets its own private low uncached page. |
| `0x1000_0000` | 256 MB | The reading node's own boot flash and other devices on the Hub's directory bus. This is the window OpenBSD reads the machine-type field through. |
| `0x2000_0000` | 256 MB | Uncached memory of the node named in the address. OpenBSD's remote Hub-special accessor biases by exactly this amount. |
| `0x3000_0000` | 256 MB | The boot flash of the node named in the address. |
| `0x8000_0000` | 1 GB | Back door onto the memory error-correction bytes. |
| `0xc000_0000` | 1 GB | Back door onto the directory entries and the page protection entries. |

**Both back doors are backed by width-limited storage for a populated bank,
and by a fixed non-storage answer everywhere else — not a uniform catch-all.**
We model no coherence directory and no error correction, so nothing about
either range carries meaning beyond what real firmware's own classification
needs. That firmware uses these windows for two things at once, off one
read-back compared against two masks of different width: testing a location
it already trusts is populated for stuck bits, and *discovering* whether a
bank exists at all, and which of two directory formats it carries, before it
trusts anything else about it. A window that echoes everything written to it
satisfies the first and defeats the second twice over: every bank reads back
present, and reads back carrying the richer of the two formats a real Hub can
report — format this machine's own hardware never has, per SGI's manual —
so firmware's sizing walk both mis-trusts an absent bank's ordinary memory,
taking a bus error real hardware would never have given it, and mis-reports a
populated bank's directory format even where the fault never fires
(behavioural contract in
[the Hub memory and directory datasheet](hub-memory-directory.md)). Each
window is implemented in `hw/mips/sn0.c` as storage that keeps only the low
sixteen bits of whatever is written to each eight-byte-aligned entry — this
machine's own, plainer directory format — for however many of the eight
banks the running configuration actually populates, 128 MB per bank (the
window is the node's memory address space compressed four to one, confirmed
from the public Linux headers for this platform rather than chosen, so a
bank's own stride there is a quarter of its 512 MB stride in ordinary
memory), layered over a fixed floor covering the rest of the 1 GB. Coverage
in `tests/qtest/sn0-address-map-test.c` covers a populated bank's low
sixteen bits round-tripping while the rest do not, the classification that
produces on both a populated and an absent bank, and that one bank's entries
survive a neighbouring bank being written.

**Cost, measured rather than assumed, and one gap that follows from the
storage no longer being plain RAM.** A running machine shows the two windows
adding to virtual address space, not resident memory — the populated share's
backing buffer is allocated zeroed and its pages are resident only once
touched, so an untouched back door costs nothing real and parallel instances
do not pay for it, the same property plain RAM had. What changed is
migration: a `memory_region_init_ram` region migrates automatically, and this
buffer does not, because nothing here gives it a `VMStateDescription`. The
back door's content is diagnostic scratch no guest relies on surviving a
snapshot — this page already states that nothing behind either window
carries meaning beyond a plain round trip — so the gap is recorded here
rather than closed. Before this storage change, the one place the size showed
up was a live-migration snapshot, which walked the full 2 GB of RAM-backed
back door even though the untouched, zero pages were cheap to walk; now that
none of it migrates, a snapshot does not touch it at all.

There is a matching cached alias at the base of cached space: the first *n*
bytes of cached space resolve to the reading processor's own node memory, where
*n* comes from a four-bit size code in the processor interface. The code resets
to fifteen, meaning 64 MB. Linux sets the code to zero on the boot node and then
to 8 KB on each additional node so that node's processors get their own
exception vectors; OpenBSD sets it to zero and leaves it there. A single-node
machine therefore never needs this alias to work, but the model must accept
writes to the size register without complaint because both guests write it
early.

### The boot flash region

The flash is a 1 MB device, sixteen 64 KB sectors, and the guest sees it two
ways. The processor's reset vector `0xffff_ffff_bfc0_0000` reaches it as
node-local physical `0x1fc0_0000` through the uncached compatibility segment, and
the Hub special boot window reaches the same bytes at the node's Hub special base
plus `0x1000_0000`. The IRIX firmware's non-maskable-interrupt handler tests for
both views by name, and the IRIX flash utility addresses the device through the
Hub special window, so both are real rather than one being a software
convention.

**Which bus the flash hangs off is settled: the Hub's memory and directory unit owns
it.** That unit's configuration register carries the flash's **cycle timing** in a
five-bit field at bit 49 and its **write timing** in a five-bit field at bit 44, both
with programmed reset defaults, and the same unit's status register carries the flash's
**ready** bit. A unit that owns a device's bus timing and watches its ready signal is
the unit that device is attached to.

That is also why the reset vector reaches the flash in **uncached memory space** rather
than through an I/O space: memory space is what the memory and directory unit decodes,
so the flash appears inside it rather than beside it. SGI's own address header agrees
independently, defining the mapped boot PROM as the uncached-memory base combined with
node offset `0x1fc0_0000` — the same place the reset vector lands.

**Read this from the IRIX register definitions, not from Linux's.** Mainline carries
only the ready bit and none of the timing fields, so a modeller working from Linux alone
sees a flash-related status bit in the memory-directory unit and nothing explaining why
it is there. This is one of the places where the two source families are not
interchangeable.

**A narrower question remains open and is not the same one.** Knowing the flash hangs
off that unit does not explain *how an access at node offset `0x1fc0_0000` resolves to
flash rather than to memory*, since that offset is also inside the range the memory banks
cover. **The decode is not software-programmed, which is the part now established:** the
memory-directory unit's registers carry the flash's cycle timing, its write timing and
its ready bit, and per-bank memory sizing, and **no base, limit or enable register
naming where the flash decodes** — searched across that unit's full register definitions
in the IRIX headers. So the remaining candidates are a fixed hardware decode or an
interaction with how the banks are sized, and a model must choose one deliberately rather
than inherit it. Nobody needs to search the memory-directory registers again.

**That negative now has a second instrument, and it holds.** SGI's machine-readable register
table enumerates thirty-nine registers for this unit, and the list contains nothing that
could place the flash: protection, memory configuration, refresh, a fetch-and-op status, the
page-migration group, four error registers with their clear aliases, two initialisation
registers, a queue size, a serial-bus control, six performance counters, the general-purpose
register file, a slot-identity and status register, and two indicator registers. **No base,
no limit, no enable.**

The two instruments are worth naming separately because they fail differently: a header can
omit a register nobody wrote a definition for, while **the table is what SGI's own
diagnostics enumerate from**, so a register absent there is absent from the vendor's own idea
of the chip's register set. **A negative confirmed by two sources that could each miss it for
different reasons is a materially stronger negative** than the same negative twice.

**And the positive half of that sentence is confirmed exactly rather than merely
corroborated.** The flash's cycle timing and write timing are two adjacent fields of the
memory configuration register, and the ready bit is in this unit's slot-identity and status
register — so *the flash's timing and status live here and its address decode does not* is
now a statement about specific named fields rather than about a search.

Ten entry points sit at the very start of the flash, eight bytes apart. The
shipped IP27 firmware image begins with eight jump-and-delay-slot pairs
covering the first sixty-four bytes, which is direct byte-level confirmation
that these are branch stubs and not a data table.

| Address | Purpose |
|---|---|
| `0x1fc0_0000` | Reset entry. |
| `0x1fc0_0008` | Restart. |
| `0x1fc0_0010` | Slave processor idle loop. |
| `0x1fc0_0018` | Enter the power-on diagnostic monitor. |
| `0x1fc0_0020` | Enter that monitor on the serial chip's port. |
| `0x1fc0_0028` | Flash the front-panel lights. |
| `0x1fc0_0030` | Re-enter the diagnostic monitor. |
| `0x1fc0_0038` | Launch a secondary processor. OpenBSD calls this one by address to start its second processor. |
| `0x1fc0_0040` | Wait for a secondary processor. |
| `0x1fc0_0048` | Poll a secondary processor. |

### The firmware configuration block

The block starts at flash offset `0x60`, so at `0x1fc0_0060` through the
compatibility segment and at the Hub special boot base plus `0x1000_0060`. Its
declared fields run to seventy-six bytes: the last one is four bytes wide at
offset `0x48`. The structure that declares them contains 64-bit members, so it is
eight-byte aligned and eighty bytes long once padded, and the four bytes past
`0x4c` hold nothing a guest reads. Both numbers describe the same block —
seventy-six bytes of content, an eighty-byte structure — and this document
counts the content, and
[the synthesized firmware structures datasheet](firmware-structures.md) counts the padded
eighty and now says so, giving both numbers with the same field layout. **The two pages
never disagreed** — they were counting different things and only one of them said which.
OpenBSD reads the block to decide whether to introduce itself as an
Origin 200 or an Origin 2000, which makes it the mechanism by which our
synthesised firmware tells the guest which machine it is.

| Offset from `0x1fc0_0060` | Width | Field | Value we emit | Reset behaviour |
|---|---|---|---|---|
| `0x00` | 32 | Timing constant | `0xb` | Read-only flash content. |
| `0x04` | 32 | Processor reset mode word | `0x1211_a785` | Read-only flash content. |
| `0x08` | 64 | Magic | `0x6970_3237_636f_6e66`, the eight characters `ip27conf` | Read-only flash content. |
| `0x10` | 64 | Processor frequency in hertz | 180,000,000 | Read-only flash content. |
| `0x18` | 64 | Hub frequency in hertz | 90,000,000 | Read-only flash content. |
| `0x20` | 64 | Real-time counter frequency in hertz | 1,250,000, an 800 ns tick | Read-only flash content. |
| `0x28` | 32 | Error correction enabled | 1 | Read-only flash content. |
| `0x2c` | 32 | Flash read-cycle timing | 15 | Read-only flash content. |
| `0x30` | 32 | Machine type | 1, meaning Origin 200 | Read-only flash content. |
| `0x34` | 32 | Checksum adjustment | 0 | Read-only flash content. |
| `0x38` | 32 | Flash program count | 0 | Read-only flash content. |
| `0x3c` | 32 | Flash write timing | 4 | Read-only flash content. |
| `0x40` | 32 | Firmware version | our choice; the shipped image says 6 | Read-only flash content. |
| `0x44` | 32 | Firmware revision | our choice; the shipped image says 11 | Read-only flash content. |
| `0x48` | 32 | Configuration flags | 0 | Read-only flash content. |

Machine type zero means Origin 2000 and one means Origin 200.

The values in the column above are not invented. SGI's firmware source carries a
table of build configurations, and one entry pairs a 180 MHz processor with a
90 MHz Hub, a 1 MB secondary cache, machine type Origin 200, flash read timing
fifteen and flash write timing four. Its processor mode word works out to
`0x1211_a785`, which is bit-for-bit the word measured in the shipped firmware
image. So the block we emit describes a real Origin 200 that SGI shipped
firmware for, and the mode word is a genuine sample of what the firmware feeds
the processor at reset rather than a guess.

## Behavioral contract

**The reset fetch must work before anything else does.** The processor starts
executing at `0xffff_ffff_bfc0_0000`. Our machine model must have the boot region
present and readable there from reset, with a valid instruction at the first
entry point, or the machine dies before it can report anything.

**One region, two views.** The flash contents must appear identically at
node-local physical `0x1fc0_0000` through the compatibility segments and at the
Hub special base plus `0x1000_0000`. Modelling this as a single memory region
with a second alias is both simpler and impossible to get out of step. The
Hub-special view is the one OpenBSD uses for its machine-type probe, and the
compatibility view is the one the reset vector and the secondary-processor launch
call use.

**Low memory must be reachable through the compatibility segment.** OpenBSD
reads the firmware parameter block at node-local physical `0x1000` as
`0xffff_ffff_8000_1000`, and Linux copies its exception vectors out of the same
segment. Both are on the first-boot path.

**The Hub register window must answer at both widget-1 halves.** Guest code
reaches its own Hub through the node-offset `0x0100_0000` half without ever
learning its node number, and reaches a named node's Hub through the
`0x0180_0000` half. On a single-node machine both resolve to the same device, but
both must decode, because OpenBSD reads its processor slice number through the
first and clears interrupt state through the second in the same boot.

**Small window 0 must decode, and this paragraph previously said the opposite.**
It said that *both guests* expect firmware to have redirected widget 0 onto big
window 6, and that a raw access to small window 0 should therefore be reported
rather than answered. **Linux does not.** The model was built to that
instruction, and mainline's `xtalk_init` walked straight into the resulting hole
and took a data bus error at widget 0, offset 4.

**What is true, and what was over-generalised.** The redirection mechanism is
real and the big window is the right one — `SWIN0_BIGWIN` resolves to the last
of the seven translation entries, window 6, exactly as described. A substituting
helper exists in Linux that performs it. **Linux does not call that helper for
this read.** `xtalk_probe_node` forms the address with the deliberately-named raw
macro, `RAW_NODE_SWIN_BASE(nasid, 0) + WIDGET_ID`, where `WIDGET_ID` is `0x04`;
all three places in `ip27-xtalk.c` that reach for a widget address use the raw
form, and the substituting helper has no live caller in any probe path. The
determination, with the file and macro citations, is in
how Linux forms the widget-0 address (citation: `docs/library/linux-widget0-address-formation.md`).

**So the model maps small window 0 and lets widget 0 answer there.** The
identification register a guest reads at offset 4 is the same register reached
through big window 6; the two are one device seen through two windows, and the
machine must not present them as different.

**And OpenBSD takes the other path, so the machine owes both.** It redirects, in
the one function both its identification paths call, which returns the big-window
address when the widget number is zero — checked against the pinned release
branch and derived twice independently. So Linux reaches widget 0 through small
window 0 and OpenBSD reaches it through big window 6, and **a machine that
satisfies either alone is broken for the other guest.** Both windows must reach
the same device, and the identification register read through one must equal the
register read through the other.

This paragraph said the OpenBSD half was unverified until it was verified
elsewhere in this document and not here — a correction landing in one place while
the same claim stood as fact in another, which is the failure this file has now
produced twice about one register. It is recorded rather than quietly fixed
because the second instance was found by a reader rather than by the author of
the first.

**The hardware defect this rested on is also unestablished.** The claim that
small window 0 is unreliable through a processor-interface defect is what
justified holing it. Linux reads it on real hardware and boots, so either the
defect is narrower than "unreliable", or it does not affect this access. Nothing
here now depends on the answer, but the claim should not be repeated as settled.

**A big window is disabled by naming a widget that is not there, and nothing about a
translation entry marks it invalid on its own.** The entry has no valid bit, so the guests
disable a window by writing an absent widget number — three, by SGI's convention — and the
model must accept any widget number without faulting and simply fail to decode for a widget
that is not present.

**This page previously said the entries start disabled, and that does not follow.** The
entries reset to zero, and a zero entry is not a null entry: it names widget zero, offset
zero, and memory space, because the fields are a widget number, a slice number and a space
select with no spare encoding among them. On a full crossbar widget zero is the crossbar's
own register block; on an Origin 200 it answers with the I/O bridge's identity. **Either
way it is present**, so at reset all seven big windows name the first 512 MB of widget
zero's memory space rather than nothing at all. The two sentences that used to sit under
that heading were about a different thing — that an entry naming an *invalid* widget must
not fault — and the heading generalised them into a claim about reset that no source
supports.

The practical consequence is small and worth stating so nobody re-derives it: a guest that
reads an unprogrammed big window gets widget zero, and our synthesized firmware programs
the widget-zero redirect into the last entry regardless, so nothing on the first-boot path
depends on which of the two readings is right. **What depends on it is any model that
treats "unprogrammed" as "reaches nothing"** — that is a choice about our machine, not a
property of the hardware, and it needs saying out loud wherever it is made.

## Interrupts

The address map itself raises no interrupts. Two addresses in it are load-bearing
for the interrupt path and are specified here so that the Hub and bridge
documents can refer to a settled constant: the Hub's interrupt-modify register is
at Hub register offset `0x90`, so a device on the crossbar reaches it at
XIO address bit 47 set, the node number at bit 38, and the offset
`0x0180_0090`. Both Linux and OpenBSD compute exactly that address to hand to the
I/O bridge, from opposite directions and with different expressions, and arrive
at the same value.

## DMA

The address map defines the target side of every direct memory transfer, but the
translation from a device's bus address to one of these physical addresses
belongs to the I/O bridge and its address-translation entries, and is specified
in the bridge's own datasheet. Two constraints from this document constrain that
one. First, a device's translated address carries the node number in bits 39:32
exactly as a processor address does, so the bridge's translation output is a
plain physical address in the space described here. Second, OpenBSD deliberately
restricts transfers to the low 2 GB on this machine because it declines to use
the bridge's translation table, so a bridge model that only implements the
direct-mapped window is enough for an OpenBSD boot.

## Out of scope

- **Memory special space.** Nothing on the path to a first operating-system
  banner uses fetch-and-operate memory operations. The region gets a catch-all
  that logs unimplemented accesses.
- **~~The directory and error-correction back doors~~ — no longer out of scope.**
  Both windows are backed by real storage; the Hub special space section of
  this datasheet describes the implementation. Left as a struck-through entry
  rather than deleted, because for part of 2026-08-11 this page said the
  range was mapped with a logging catch-all when the tree had no catch-all
  there at all, and a silent deletion would erase that this page was wrong
  twice about the same range before it was right.
- **N mode.** Nine-bit node numbers with 2 GB windows are a configuration
  neither guest we care about selects.
- **~~Remote node windows~~ — the second node is now instantiated.** All the
  node-strided arithmetic is modelled, and the model now brings up a second node at
  a non-zero node number, its memory and Hub registers answering at the strided
  window, exactly as this striping intended — the configuration change it was built
  to allow, not a redesign. What a two-node machine still lacks is a *guest* proven
  to boot across both nodes and the fabric routing of a device DMA to a remote node;
  those live in the multi-node addressing brief (citation: `docs/plans/archive/multi-node-addressing.md`),
  not here. Left with the old title struck through rather than deleted, because this
  page asserted for a time that non-zero node numbers held nothing, and the model has
  since moved past that.
- **The uncached-memory alias of the reader's own node and the split of its
  bottom 128 KB by processor.** Needed when a second processor exists, not
  before.

## Behavioral notes

**The node number is placed, not routed.** SGI's own firmware manual describes
addressing a remote node as nothing more than writing that node's number into
the high address bits, for memory, registers, devices and flash alike. That is
why one aliasing scheme covers the whole machine and why adding a node later is
a matter of instantiating more devices rather than of changing how anything is
addressed.

**The position-independent aliases are the reason firmware can be node-blind.**
Every one of them — the cached alias at the base of cached space, the uncached
alias at the base of Hub special space, the boot window, and the lower half of
widget 1's small window — resolves to the reader's own node. A single firmware
image runs on every node without relocation because of them, and a kernel that
has not yet worked out its own node number can still reach its own Hub.

**Three sources agree on the real-time counter's odd offset.** The counter is
not at the natural `0x100` slot in the processor interface's register block but
at `0x03_0100`. Linux, OpenBSD and the IRIX register table all say so
independently, so the sub-block decode is real hardware behaviour. What the
other sub-block values mean is not settled by anything we have read, and a
reviewer should treat the sub-block bits as opaque: decode the three offsets that
guests actually use, and log anything else.

**Two conflicts and one gap found while writing this**, the gap since closed. They
are listed here rather than buried, because they are the parts a reviewer should look
at hardest, and because a closed question is worth keeping beside the reasoning that
closed it.

- The IRIX machine-readable register table places the processor interface's
  reply-level register at `0x01_0060`, while both the IRIX header it was
  generated alongside and Linux place it at `0x00_0060`. We take `0x00_0060`:
  the designer's own header plus running code outranks a derived table, and that
  table is already known to carry transcription slips (source-authority
  decision). No guest on our path touches this register, so the conflict does not
  block anything, but it means the register table is not infallible and its
  unusual offsets deserve a second source each.
- **Closed 2026-08-09: why the compatibility reset address can name the boot flash.**
  This was listed as an open question, on the premise that the compatibility
  segments carry no uncached-attribute bits of their own. That premise was wrong and
  the question has an answer. The processor sources the uncached attribute for every
  *unmapped* reference from virtual-address bits 58:57, and the manual defines a
  compatibility-segment address as one whose bits 61:31 are all ones — so bits 58:57
  are necessarily `11`, which is attribute 3, uncached memory. The rule reproduces
  the whole scheme rather than just this case: the four uncached bases yield
  attributes 0, 1, 2 and 3, each landing on the space its own name gives it.

  QEMU routes the reset vector as an access to **uncached memory** at node offset
  `0x1fc0_0000`. The flash is attached to the memory/directory side of Hub: that
  unit supplies its cycle timing, write timing, and ready signal. The Hub-special
  boot window is a second view of the same device for reaching a named node's
  flash. The sources do not establish how hardware chooses flash instead of RAM
  at the overlapping node offset, so the model's overlay is a compatibility
  choice, not a recovered decode rule.

  One consequence worth carrying. Nothing else we have read is overlaid into memory
  space — the serial-bus microcontroller, the light-emitting diode registers and the
  slot identity all decode in the Hub register window instead — but that is "nothing
  else turned up", not a table of overlays with one entry, and the memory-directory
  unit's own datasheet is where it should be settled properly.

  **What is closed here is the flash's MD attachment and the model's reset route,
  not real compatibility routing.** An earlier version of this entry went further and
  asserted that a compatibility-segment access therefore cannot reach a widget at all,
  "a fact about the hardware rather than about our model". **That claim is contested and
  should never have been written here as settled.** Real-hardware evidence points the
  other way, the question is open and ranked for observation under real firmware, and
  the compatibility-segment routing note (citation: `docs/library/compatibility-segment-routing.md`)
  carries the current state with the evidence on both sides.

  Whether a compatibility-segment address carries an uncached attribute, and
  whether such an address can reach a widget, remains a separate question. This
  document does not rule on it.

  Also traced in the Linux device-mapping trace (citation: `docs/library/linux-ip27-device-mapping.md`).
- The crossbar and widget facts on this page rest on Linux, OpenBSD and SGI's
  firmware manual. SGI's Crossbow crossbar specification, which is a scan with no
  text layer, was not readable when this document was written; it has since been
  read, both by character recognition and by a page-image transcription, and
  [the Crossbow datasheet](xbow.md) was written from it. Nothing that
  specification carries contradicts the widget geometry here.

## Open questions

- **What flash is populated on the IP29 System Board, how its selection signals are wired,
  and how overlapping memory and flash accesses are resolved on that board.** The boot-flash
  contract defers this exact question here. Two candidates are both unsettled: that the board
  uses the Bridge external-flash interface exactly as the public Bridge specification
  describes, which establishes the address translation but not the IP29 population or
  selection wiring; or that additional board logic or memory-bank state participates in
  selecting the responder, for which no reviewed source establishes such IP29-specific logic.
  A hardware model must choose its decode deliberately even though the available evidence does
  not establish how IP29 resolves a memory and flash collision. An IP29 schematic, board trace,
  or controlled real-machine access would settle the populated device, selection wiring, and
  collision behavior.

## qtest checklist

- Reading the eight bytes at `0x1fc0_0068` through the compatibility uncached
  segment returns the characters `ip27conf`.
- Reading the same eight bytes through the Hub special boot window, at the node's
  Hub special base plus `0x1000_0068`, returns the identical value.
- The machine-type field at `0x1fc0_0090` reads as 1, and the three frequency
  fields read as 180,000,000, 90,000,000 and 1,250,000.
- The first word at each of the ten entry-point addresses from `0x1fc0_0000` to
  `0x1fc0_0048` decodes as a branch, so a guest that calls one returns rather
  than executing whatever follows.
- A write to the flash region is rejected without faulting the machine and is
  logged as an unimplemented access.
- Node-local physical `0x1000` written through the 64-bit uncached memory base
  reads back through the compatibility cached segment at
  `0xffff_ffff_8000_1000`, proving the two views of low memory are the same
  storage.
- The Hub's widget identity register reads the same value through the
  node-offset `0x0100_0000` window and the `0x0180_0000` window.
- Writing a big-window translation entry at Hub register offset `0x40_0160` plus
  eight times the window number, then reading it back, returns the widget number,
  space bit and offset bits unchanged.
- **Small window 0 answers**, and the identification register a guest reads at
  offset 4 through it returns the same value that register returns through big
  window 6. Two windows onto one device must not disagree, and this is the
  assertion that catches a machine which maps one of them and forgets the other.
  This item previously required the opposite — that the access be logged as
  unimplemented — and a test written to it would have encoded the withdrawn
  answer and failed the day Linux was booted.
- An access anywhere in memory special space is logged as unimplemented.
