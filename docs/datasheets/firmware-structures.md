# Datasheet: synthesized firmware structures

References rendered as citation identities name material outside this document
set. Those targets are not included or cleared for redistribution. Review and
runtime statements retain their recorded dates and scope.

**Status:** reviewed (coordinator with codex, 2026-08-09) — **ready**, all five findings
from the datasheet review closed as of 2026-08-11. The three minor findings — the
reset-vector list's single-sourcing, the parameter block's advertised length, and the
firmware-operation field's real channel — are corrected in place in their own sections
rather than tracked here. Both should-fix findings are closed: the length disagreement
with the address map — both documents now state seventy-six declared bytes padded to
eighty — and the global data area's debugger-pointer
row, corrected from sixteen bytes to the twenty-four three 64-bit pointers actually take.
**Fork file(s):** `hw/mips/sn0.c` (structure builders, the memory region that
stands in for the boot flash, and the call dispatcher — this document named
`hw/mips/sn0_prom.c` until 2026-08-11; no such file exists or ever has, and the
builders have always lived in the machine description file itself).
**Sources used:** the firmware boot analysis (citation: `docs/research/11-firmware-and-boot-entry.md`)
for the shape of the problem; OpenBSD's SGI port at commit `2df91d80c1` of
`refs/openbsd-sgi`, the head of the branch matching the OpenBSD 7.9 `GENERIC-IP27` kernel
this project boots — mainline OpenBSD retired the port, so `refs/openbsd-src` has no
such tree, and nothing here is on the checked-out branch, so read it without checking it
out, as `git -C refs/openbsd-sgi show 2df91d80c1:<path>`
(`sys/arch/mips64/include/arcbios.h`,
`sys/arch/mips64/mips64/arcbios.c`, `sys/arch/sgi/include/mnode.h`,
`sys/arch/sgi/sgi/ip27.h`, `sys/arch/sgi/sgi/sginode.c`, `sys/arch/sgi/sgi/machdep.c`,
`sys/arch/sgi/sgi/ip27_machdep.c`, `sys/arch/sgi/pci/ioc.c`) for everything a guest
actually reads; Linux mainline in `refs/linux` (`arch/mips/include/asm/sn/kldir.h`,
`sn/sn0/kldir.h`, `sn/klconfig.h`, `sn/gda.h`, `sn/addrs.h`, `sn/launch.h`,
`arch/mips/sgi-ip27/`) as the second independent consumer. **IRIX-derived** (read to
learn, written here in our own words — the source-use rule is that we read reference
sources and never copy their code): the firmware call-vector and private-vector entry
order and the firmware-capability ladder, the parameter-block field order and its
version and revision values, the memory-type enumeration, the boot-flash configuration
block's field list, the fixed reset-vector entry addresses, and which stage of the real
firmware writes which structure — read from the IRIX 6.5.7 tree (the SN0 kernel structure
and address-map headers, the ARCS firmware-block headers, and the IP27/IO6 PROM boot and
directory sources), in our own words, with no vendor file structure transcribed here.
Every structural
fact that could be checked against a public header was checked, and the two agree; the
places where they do not are called out in the notes at the end. Structure sizes and
field offsets quoted here were computed from the field lists with natural alignment for
a 64-bit big-endian target and cross-checked against the symbolic offsets the consumers
use. **IRIX's own consumer behaviour** — which fields it dereferences unconditionally,
what it panics on, and what it never checks on the boot path — comes from
the KLCONFIG guest-consumer survey (citation: `docs/library/klconfig-guest-consumers.md`), commit
`f57f7b9`, which reads the IRIX kernel directly and independently confirms every claim
cited from it here; the bank-sizing and directory-initialiser facts come from
the critical-path register-facts survey (citation: `docs/library/critical-path-register-facts.md`).

## Role

On a real Origin 200 the firmware runs in two stages — a processor-board flash that
sizes memory and lays down the machine description, then an I/O-board flash that
installs the callable firmware interface — and only two things it does cross the
boundary into the operating system: **the bytes it leaves at fixed addresses in the
node's memory**, and **the routines the operating system calls afterwards**. Everything
else the real firmware does is internal to itself. That is why the emulator synthesizes
the outputs instead of executing the process.

This datasheet is the contract for those outputs. It covers the low-memory structures,
the two call vectors, the small readable region at the boot-flash address that the
machine-identification path needs, and the traps that make a guest fail silently rather
than loudly. It is the implementation contract for
the plan that gets an operating system to its console switch with no device models (citation: `docs/plans/archive/b2-device-free-boot.md`).

Everything is per node and every offset is measured from the start of that node's
memory. A single-node Origin 200 has one node, node identifier 0, whose memory starts at
physical address 0. Two access paths must both work for the same bytes, because guests
use both: the compatibility-space window (the guest reads the firmware parameter block
that way, because the flat 64-bit physical window is carved into sub-spaces on this
machine and the compatibility window is not), and the per-node uncached window (the
guest reads the hardware directory, the configuration graph and the global data area
that way).

## Low-memory map

Offsets from the start of the node's memory. "Processor-board firmware" and
"I/O-board firmware" name the two real firmware stages; in the emulator one builder
writes all of it, but the split is worth keeping because it explains why the global data
area is the only structure that exists once per machine rather than once per node.

| Offset | Size | Contents | Written by | Read by |
|---|---|---|---|---|
| `0x0000` | `0x400` | exception handlers | processor-board firmware | the guest, until it installs its own |
| `0x0400` | `0x400` | cache-error exception frame | processor-board firmware | cache-error path only |
| `0x0800` | `0x800` | early cache-error stack, also the firmware's own register save area | processor-board firmware | cache-error and non-maskable-interrupt paths |
| `0x1000` | `0x400` | **firmware parameter block** | I/O-board firmware | every guest that uses the firmware call interface; this is where it starts |
| `0x2000` | `0x400` | **hardware-directory block** (sixteen 64-byte entries) | processor-board firmware, except the global-data-area entry | every guest; also the machine-identification fallback |
| `0x2400` | `0x400` | **launch blocks**, two of them at a `0x200` stride | processor-board firmware | the guest's second-processor startup path |
| `0x2800` | `0x400` | **firmware call vector** (master node only) | I/O-board firmware | every guest that calls firmware routines |
| `0x2c00` | `0x400` | **SGI-private call vector** (master node only) | I/O-board firmware | IRIX and the SGI standalone shell; not the open-source guests |
| `0x3000` | `0x1000` | non-maskable-interrupt handler and its two per-processor blocks (`0x40` each at a `0x40` stride) | processor-board firmware | the non-maskable-interrupt path |
| `0x4000` | `0xc000` | **configuration graph** (48 KB) | processor-board firmware | every guest, for processors, memory and the console |
| `0x10000` | `0x400` | exception handlers, uncached-alias copy | processor-board firmware | the guest |
| `0x10400` | `0xc00` | cache-error frame and early stack, uncached-alias copies | processor-board firmware | cache-error path |
| `0x11000` | `0x400` | **global data area** | I/O-board firmware | every guest, for the master node and the node table |
| `0x11400` | `0x400` | kernel non-maskable-interrupt register save area, `0x200` per processor | the guest | the firmware, on a non-maskable interrupt |
| `0x11800` | `0x400` | kernel non-maskable-interrupt exception-frame save area, `0x200` per processor | the guest | the firmware |
| `0x12000` | `0x4000` | processor-interface error spools | processor-board firmware | the guest's error handling |
| `0x16000` | `0x2000` | per-node global data page | the guest | the guest |
| `0x19000` | — | **first free memory** | — | the guest allocates from here up |
| `0x25000` | `0xe000` | debugger stacks, two at a `0x7000` stride | processor-board firmware | the kernel debugger only |

The emulator must write the parameter block, the directory, the launch blocks, both call
vectors, the configuration graph and the global data area. The exception-handler,
cache-error and non-maskable-interrupt areas can be left zeroed for the banner: a guest
that takes an exception before installing its own handlers has already failed, and
zeroing is a more legible failure than executing whatever was there. They must still be
ordinary writable memory, because the guest installs its own non-maskable-interrupt
handler into that block during setup. The error-spool and debugger-stack areas need no
content at all; they exist so the directory can point at them and so the memory
descriptors can declare them reserved.

## The boot-flash region and its reset-vector entries

Even the fully synthesized path needs a readable, executable region standing in for the
boot flash, because the guest reaches into it directly in two ways. **It must appear in
two windows**, and guests use one each: compatibility space at `0x1fc00000`, which is how
the entry stubs are called, and the hub's special-address space at node-local
`0x10000000`, which is how the configuration block is read. Same bytes, two addresses.

**Callable entry stubs.** The guest calls fixed addresses in that region as function
pointers. **Real firmware has twelve**, spaced eight bytes apart, which is exactly one
jump-and-delay-slot pair each — counted in both shipped revisions of the real image. Ten
of the twelve are named by the IRIX headers. **This list is single-sourced from IRIX, not
dual-sourced**: none of the Linux public headers this datasheet cites carries all ten
names, so what corroborates the count of twelve is the independent measurement just
stated — both shipped firmware images decoding to the same twelve slots — rather than a
second header agreeing on the names.

| Address | Entry |
|---|---|
| `0x1fc00000` | firmware entry (the reset vector proper) |
| `0x1fc00008` | restart |
| `0x1fc00010` | slave-processor idle loop |
| `0x1fc00018` | enter the diagnostic monitor |
| `0x1fc00020` | enter the diagnostic monitor over the I/O-board serial port |
| `0x1fc00028` | flash the front-panel indicators |
| `0x1fc00030` | re-enter the diagnostic monitor |
| `0x1fc00038` | **launch a slave processor** |
| `0x1fc00040` | wait for a slave processor |
| `0x1fc00048` | poll a slave processor |
| `0x1fc00050` | unnamed in any header, present in both real revisions |
| `0x1fc00058` | unnamed, and **a call rather than a jump** — it returns to its caller |

The table cannot be longer than twelve: the configuration block starts at `0x60`, and in
the real image the word after the last slot is that block's first field, with nothing
between them. Emit twelve. The last one is different in kind from the rest — the other
eleven are jumps that never come back, and this one is a subroutine call — so a stub that
never returns is wrong there specifically. What it does is not known; that it returns is.

The launch entry at `0x1fc00038` is the one that matters. OpenBSD calls it through the
uncached compatibility window, with six arguments — node, processor, function, parameter,
stack pointer, global pointer — to start a secondary processor, and does so before it
tears down the firmware's page mappings. It is the *only* fixed address OpenBSD calls,
and only in its multiprocessor kernel: the uniprocessor kernel we are aiming the banner
at never touches any of them. Linux calls five — launch, wait, poll, the idle loop and
the indicator flash. Emit each stub as a couple of generated instructions that jump into
the trap page, using QEMU's existing bootloader helpers, and give the ones we do not
implement a handler that logs an unimplemented access rather than executing garbage.

**The configuration block at offset `0x60` in the flash region.** This is the mechanism by
which the firmware tells the guest which machine it is on, and it is read out of the flash
region, not out of memory — and read through the *hub special-address* view of it, not the
compatibility view, so the block sits at node-local `0x10000060` for the reader even
though the same bytes are at `0x1fc00060` in compatibility space. **Eighty bytes on flash —
seventy-six bytes of declared fields, padded to an eight-byte boundary** — big-endian, with
a signature eight bytes in. The last declared field is four bytes wide at offset `0x48`, so
the four bytes past `0x4c` hold nothing a guest reads; the structure declaring these fields
has 64-bit members and is therefore eight-byte aligned. **Emit eighty and expect a guest to
read seventy-six.**

| Offset from the block | Width | Field | Value for our machine |
|---|---|---|---|
| `0x00` | 4 | processor timing constant | `0xb` |
| `0x04` | 4 | processor reset mode word | `0x1211a785` (a 180 MHz processor with a 1 MB secondary cache) |
| `0x08` | 8 | **signature** | `0x69703237636f6e66`, the eight characters `ip27conf` |
| `0x10` | 8 | processor clock, in hertz | `180000000` |
| `0x18` | 8 | hub clock, in hertz | `90000000` |
| `0x20` | 8 | real-time counter clock, in hertz | `1250000` (an 800 ns tick) |
| `0x28` | 4 | error-correction enable | `1` |
| `0x2c` | 4 | flash read timing — the cycle-speed control for the memory unit's flash field | `15` for Origin 200 |
| `0x30` | 4 | **machine type** | `1` = Origin 200. `0` = Origin 2000 |
| `0x34` | 4 | checksum adjustment | `0` |
| `0x38` | 4 | flash-programming count | `0` |
| `0x3c` | 4 | flash write timing — the write-speed control for the memory unit's flash field | `4` for Origin 200 |
| `0x40` | 4 | firmware version | our choice; the shipped Origin firmware says `6` |
| `0x44` | 4 | firmware revision | our choice; the shipped Origin firmware says `11` |
| `0x48` | 4 | configuration-specific flags | `0` |

**The two flash timing entries are not free-standing numbers: they are the values for two
named fields of the memory unit's configuration register, and SGI's own structure names them
after those fields.** So this block is where firmware learns what to program, and the
register is where it programs it — which makes the pair checkable against each other rather
than two unrelated constants.

**Checked, and they tell us something about what firmware actually changes.** That register's
hardware defaults are **15** for the cycle field and **7** for the write field. This block's
values for the Origin 200 are **15** and **4**. So the cycle setting a real firmware writes is
**already the power-on default and its write changes nothing**, while the write-timing setting
is a genuine reprogramming from 7 down to 4.

**For our model that means the register's pre-firmware state is not zero**, and a test that
reads it before firmware runs should expect the hardware defaults rather than a cleared
register. The register's own specification belongs in the memory-and-directory datasheet;
what belongs here is that **these two numbers have a destination**, and that one of them is a
no-op on this machine — the kind of coincidence that makes a missing write invisible, exactly
like the widget-zero redirect.

| Offset from the block | Width | Field | Value for our machine |
|---|---|---|---|
| `0x4c` | 4 | tail padding to the eight-byte boundary | `0` |

The machine-type field is the whole point. OpenBSD checks the signature and prints
"Origin 200" for `1`, "Origin 2000" for `0`, and "Unknown IP27 type" with the raw value
for anything else — including when the signature does not match, in which case it
substitutes the value `2`. The signature and the machine type are the only two fields it
reads; the rest are declared and unused, so they cost nothing to get right and we should
get them right anyway. Linux reads none of the block and does not even define its address.
The clock values are measured from a shipped Origin firmware image and the real-time
counter rate is confirmed independently by OpenBSD's own timer code declaring 1,250,000
hertz, so they are not guesses.

The machine type has consequences well past the banner line. It is one of the three
conditions — with the I/O bridge being at widget 8 and the I/O controller being the third
device on that bus — that decide which functions of the on-board I/O controller OpenBSD
claims. Get it wrong and the machine prints a plausible banner and then comes up with no
serial port, no clock and no ethernet.

## The firmware parameter block

**Ninety-six bytes at offset `0x1000` is what we emit and advertise, not the size of
IRIX's own structure** — its declaration continues past the adapter count with
adapter-vector fields this datasheet does not cover, because nothing on our path reads
them. `0x60` is the right length to advertise: no guest we boot reads past it. The guest
finds everything else it needs from here.

| Offset | Width | Field | Value |
|---|---|---|---|
| `0x00` | 8 | **signature** | `0x53435241` |
| `0x08` | 8 | length of this block | `0x60` |
| `0x10` | 2 | version | `64` |
| `0x12` | 2 | revision | `0` |
| `0x14` | 4 | padding | `0` |
| `0x18` | 8 | restart block | `0` |
| `0x20` | 8 | debug block | `0` |
| `0x28` | 8 | general exception vector | address of our handler, or `0` |
| `0x30` | 8 | user-address-translation-miss vector | address of our handler, or `0` |
| `0x38` | 8 | length of the firmware call vector | `0x118` |
| `0x40` | 8 | **address of the firmware call vector** | node-local `0x2800`, as an address the guest can dereference |
| `0x48` | 8 | length of the private call vector | `0x88` |
| `0x50` | 8 | **address of the private call vector** | node-local `0x2c00`, as a dereferenceable address |
| `0x58` | 8 | adapter count | `0` |

Two things about the signature are easy to get wrong and both are fatal.

First, the value is the numeric constant `0x53435241`, not the four characters `ARCS`
laid out in memory order. Read as big-endian text those bytes spell `SCRA`. OpenBSD
accepts either that constant or its byte-reversed twin, and calls the reversed one
"wrong, but required", so writing the constant is what matches real SGI firmware.

Second, it must be written as a full 64-bit field, so the first four bytes of the block
are zero and the constant sits in the second four. A guest checks the 32-bit form of the
block *first* — it reads the first four bytes and compares them against the same
constant — and only if that fails does it try the 64-bit form. Write the constant into
the first four bytes and OpenBSD will conclude the firmware is 32-bit, then read every
pointer at half its real offset and take garbage for the call vector.

Of everything in this block, only the signature and the call-vector address are actually
load-bearing: OpenBSD reads no other field on the 64-bit path — not the length, not the
version or revision, not the vector lengths, not the adapter count. Write them anyway.
They cost nothing, the SGI standalone shell and IRIX are a different matter, and a block
that is right only where somebody currently looks is a trap for whoever comes next.

## The firmware call vector

Thirty-five entries of eight bytes each at offset `0x2800`, so entry *n* counting from
zero sits at byte offset `8n`. Every entry is a pointer into the trap page whose accesses
land in our dispatcher. The full list, in order, with its offset:

| Offset | Entry | Offset | Entry |
|---|---|---|---|
| `0x00` | load an image | `0x98` | reserved |
| `0x08` | invoke a loaded image | `0xa0` | get the time of day |
| `0x10` | load and invoke an image | `0xa8` | get the relative time |
| `0x18` | **halt** | `0xb0` | read a directory entry |
| `0x20` | **power down** | `0xb8` | open |
| `0x28` | **restart** | `0xc0` | close |
| `0x30` | **reboot** | `0xc8` | read |
| `0x38` | enter interactive mode | `0xd0` | get read status |
| `0x40` | reserved | `0xd8` | **write** |
| `0x48` | get the peer component | `0xe0` | seek |
| `0x50` | **get the child component** | `0xe8` | mount |
| `0x58` | get the parent component | `0xf0` | **get an environment variable** |
| `0x60` | get configuration data | `0xf8` | set an environment variable |
| `0x68` | add a component | `0x100` | get file information |
| `0x70` | delete a component | `0x108` | set file information |
| `0x78` | get a component by path | `0x110` | flush all caches |
| `0x80` | save the configuration | | |
| `0x88` | **get the system identifier** | | |
| `0x90` | **get a memory descriptor** | | |

The banner subset is in bold. Counted exactly it is **five routines the guest genuinely
calls and five pointers it merely reads out and keeps** — the boot
plan (citation: `docs/plans/archive/mvp-boot.md`) and the firmware boot
analysis (citation: `docs/research/11-firmware-and-boot-entry.md`) both say "about seven", which is
this same set counted loosely. The five that are read rather than called still have to be
real addresses, because the guest will jump to one of them the moment anything goes
wrong.

- **Write**, on handle 1, routed to the emulator's character device. This carries *all*
  early output: the firmware-identification line, the machine-identification line, the
  "Machine is in M mode" line and the memory summary all go through it before the guest
  looks at a serial port.
- **Get an environment variable**, for the console-output, graphics, keyboard,
  ethernet-address, console-baud-rate, load-options and load-partition variables.
- **Get a memory descriptor**, walking a list; the ordering rules are below and they are
  the single most dangerous thing in this document.
- **Get the child component**, which must return null. See the note on machine
  identification.
- **Get the system identifier**, which must return a pointer to sixteen bytes: eight
  characters of vendor followed by eight of product. It is called unconditionally and
  dereferenced on the failure path, so it may not return null even though nothing on the
  success path reads it.
- **Halt**, **power down**, **restart** and **reboot**, which the guest does not call
  through the vector — it reads them out as raw pointers from offsets `0x18`, `0x20`,
  `0x28` and `0x30` and keeps them for later. They must be real, callable addresses. The
  entry at `0x38` is read the same way, so make it callable too.

One mechanical detail the dispatcher must respect: the guest enters a firmware routine
with a **jump, not a call**. Its trampoline loads the entry out of the table and jumps to
it, so the routine runs on the guest's own stack, with the guest's own return address
still in place, and returns by jumping to that register. Arguments arrive in the usual
argument registers under the 64-bit calling convention. A dispatcher that assumes it owns
the return address, or that it can use a stack of its own, will corrupt the caller.

Everything else may point at a stub that logs an unimplemented call and returns an error
code. The file and device entries become real work only when we get to loading the SGI
standalone shell from a disk.

## The SGI-private call vector

Seventeen entries of eight bytes each at offset `0x2c00`, total length `0x88`. IRIX and
the SGI standalone shell need it; the open-source guests never look at it. It is listed
here because the parameter block points at it from day one and because one entry is a
trap that fails invisibly.

| Offset | Entry |
|---|---|
| `0x00` | device control |
| `0x08` | read the non-volatile-memory variable table |
| `0x10` | load at an absolute address |
| `0x18` | invoke at an absolute address |
| `0x20` | load and invoke at an absolute address |
| `0x28` | register a filesystem driver |
| `0x30` | unregister a filesystem driver |
| `0x38` | install a signal handler |
| `0x40` | get the hardware-test state |
| `0x48` | **firmware capability level** |
| `0x50` | processor identifier |
| `0x58` | bus information |
| `0x60` | processor frequency |
| `0x68` | find the inventory |
| `0x70` | produce the hardware inventory listing |
| `0x78` | number of processors |
| `0x80` | node processor identifier |

The last four exist only on this machine family; the first thirteen are common to all
SGI ARC-style firmware. The hardware-inventory entry is how the standalone shell's
inventory command works — it reads our configuration graph and prints it, which makes it
a direct, human-readable assertion that the graph is coherent.

The capability entry is a version ladder, not a decoration: level 1 means the firmware
can run graphical standalone programs, level 2 means it can load a plain executable, and
level 3 means it can load a **relocatable** object. Standalone programs linked against
SGI's loader library only hand loading to the firmware when this query returns 3 or more,
and otherwise silently take their own, different path. The SGI standalone shell shipped
on the install media is a relocatable object with no program headers and no entry point,
so a firmware that answers less than 3, or that answers 3 without implementing a
relocating loader, cannot load it — and the failure looks like a loader wandering off,
not like a missing call. We must answer 3 and mean it. None of this is on the banner
path; it is here so the number is written down before someone picks a smaller one.

## The hardware-directory block

Sixteen entries of 64 bytes each at offset `0x2000`. It exists so that the other
structures can move without breaking firmware and kernels that predate the move, and its
index assignments are permanent.

Each entry:

| Offset | Width | Field |
|---|---|---|
| `0x00` | 8 | signature |
| `0x08` | 8 | offset from the start of node memory |
| `0x10` | 8 | absolute pointer, used instead of the offset by some entries |
| `0x18` | 8 | size in bytes |
| `0x20` | 8 | repeat count, 1 if not an array |
| `0x28` | 8 | stride, 0 if not an array |
| `0x30` | 16 | reserved; the partition entry stores partition data here |

The signature is `0x434d5f53505f5357` in every valid entry.

| Index | Entry | Offset | Size | Count | Stride |
|---|---|---|---|---|---|
| 0 | launch blocks | `0x2400` | `0x400` | 2 | `0x200` |
| 1 | **configuration graph** | `0x4000` | `0xc000` | 1 | 0 |
| 2 | non-maskable-interrupt blocks | `0x3000` | `0x40` | 2 | `0x40` |
| 3 | **global data area** | uses the pointer field, not the offset | `0x400` | 1 | 0 |
| 4 | first free memory | `0x19000` | `-1` | 1 | 0 |
| 5 | debugger stacks | `0x25000` | `0xe000` | 2 | `0x7000` |
| 6 | processor-interface error spools | `0x12000` | `0x4000` | 1 | 0 |
| 7 | kernel variables | written by the guest | | | |
| 8 | kernel cross-partition area | written by the guest | | | |
| 9 | partition identifier | written by the guest | | | |

The global-data-area entry is the odd one: because there is only one such area per
machine, its directory entry carries an **absolute address the guest can dereference**
in the pointer field rather than a node offset. Every other entry the firmware fills uses
the offset field and leaves the pointer at zero. Getting that asymmetry backwards is a
silent fault. Entries seven through nine belong to the guest; leave them zeroed, signature
included, so nothing mistakes them for valid.

Only three of these entries are read at all by the guests we care about, and only one is
checked. OpenBSD reads the signature and offset of the configuration-graph entry, the
offset of the non-maskable-interrupt entry, and the pointer and size of the
global-data-area entry — nothing else, and it verifies only the configuration-graph
entry's signature. Linux reads the configuration-graph offset and the global-data-area
pointer and checks nothing. The non-maskable-interrupt entry deserves particular care:
OpenBSD **writes** through it early in setup, installing its own handler address and a
complement, so a wrong offset there is a four-word store to an arbitrary address before
the console exists.

**Why the signature matters.** It is the machine-identification fallback. OpenBSD's
machine-identification routine asks the firmware for the root component's child; if that
returns null it checks the signature of the *configuration-graph entry* — index one, on
node zero, through the node's uncached window — and if it finds the signature it declares
the machine to be this family and carries on. That is the only path by which OpenBSD ever
identifies an Origin, so the directory's configuration-graph entry has to be right even
though a guest that never falls back would not have read it yet.

## The configuration graph

A 48 KB region at offset `0x4000`: a header, then a singly-linked list of board records,
each pointing at component records. All links inside it are 32-bit offsets from the start
of the node's memory, never pointers, so the whole graph is position-independent within
the node — with the one exception noted below.

**The offset, the signature and the version are observed on a real Origin 200, not read out
of a header.** Its firmware announces the graph's address and its own signature and version
on the console before any guest runs, and the address it prints is node-local `0x4000`
reached through **uncached memory space** — attribute 3 — rather than through a
compatibility segment or any other attribute. So the offset, the magic number, the version
being zero, and the space the firmware uses to reach its own structures are all
machine-reported here rather than derived, and the board and component type numbers below
are corroborated by the same continuous console session.

**That capture sat in a block trimmed out of our own boot-log catalogue** and was recovered
by a checker built to compare quoted blocks against the retained pages they came from. The
facts on this page did not change when it was restored — they were already right — but their
standing did, from *read from a guest's header* to *stated by the machine*. **Those support
different conclusions**, and the second is what a contract wants.

**Header**, 112 bytes at `0x4000`:

| Offset | Width | Field |
|---|---|---|
| `0x00` | 8 | signature, `0xbeedbabe` |
| `0x08` | 4 | structure version — real firmware writes **0**, and no consumer checks it |
| `0x0c` | 4 | offset of the allocation-arena headers |
| `0x10` | 4 | **offset of the console descriptor**, which must be `0x18` |
| `0x14` | 4 | **offset of the first board record** |
| `0x18` | 48 | console descriptor, inline |
| `0x48` | 36 | three allocation-arena headers, twelve bytes each |
| `0x6c` | 2 | two confidence bytes |
| `0x6e` | 2 | tail padding to the eight-byte boundary |

The console offset is redundant with the inline descriptor and must agree with it: a
guest reaches the descriptor by adding that offset to the header's own address, so it has
to be `0x18`, the offset at which the descriptor is embedded. Getting the two out of step
points the console at the middle of the header.

**Console descriptor**, 48 bytes, inline in the header at `0x18` and also reachable
through the header's console offset:

| Offset | Width | Field |
|---|---|---|
| `0x00` | 8 | serial-port register base address |
| `0x08` | 8 | serial-port configuration base address |
| `0x10` | 8 | memory base address |

**All three derive from one IOC3 placement, and getting any one wrong is invisible
from the others.** Each starts with the crosstalk-I/O base and widget 8. The UART
and memory pointers then add PCI device 2's memory window at `0x60_0000`. The
configuration pointer instead adds device 2's Type 0 configuration aperture at
`0x02_2000`.

| Field | Chip-relative part | Who reads it, and for what |
|---|---|---|
| Serial-port register base | `0x60_0000 + 0x20178`, producing `0x9200_0000_0862_0178` | OpenBSD, which splits it into a bus and an offset and drives the port through them |
| Serial-port configuration base | Type 0 configuration base `0x02_0000` plus device 2's `0x2000` stride, producing `0x9200_0000_0802_2000` | IRIX's machine-level reset. OpenBSD and Linux do not read it. |
| Memory base | PCI device 2's memory window, producing `0x9200_0000_0860_0000` | Linux's early console, which adds the chip's serial-register offset itself |

### Which guest reads which field, counted rather than argued

**This was a general caution until irix-structs counted the references per field across the
whole IRIX kernel source. It is now a map, and a map says which field to be careful about
rather than telling a builder to be careful.**

| Field | Offset | IRIX references | Who else reads it |
|---|---|---|---|
| Serial-port register base | `0x00` | **zero** | **OpenBSD**, which splits it into a bus and an offset |
| Serial-port configuration base | `0x08` | **one** — the I/O controller's reset | nothing else known |
| Memory base | `0x10` | **five**, across four subsystems | **Linux**, which dereferences it directly |
| Base-I/O board identity number | `0x28` | **zero** | nothing else known |

**The field OpenBSD depends on entirely is one IRIX never touches, and the reverse.** That is
the literal situation with a third guest placed on it, and it is why a passing boot proves so
little here: no guest's path touches more than one of these, so a test following OpenBSD to a
working console proves nothing about the field Linux reads, and neither says anything about the
field IRIX resets the controller with. **A stale field sits undetected until a different guest
runs, which may be a milestone away.**

**This is a measurement of IRIX's source, not of the hardware.** It says what that kernel's
authors wrote, which is strong evidence about what the vendor OS requires and no evidence at
all about what these fields mean on real silicon.

**Do not read the zero-reference rows as fields that do not matter.** They are zero-reference
**for IRIX**. The serial-port register base is the field OpenBSD's console depends on
completely, and a builder who takes IRIX's indifference as permission to leave it wrong breaks
the guest we already boot. **One guest's indifference is not a property of the field** — that
substitution is the same shape as any other adjacent-thing error, with one guest standing in
for another.

**The memory base earns separate treatment, because the concentration is the opposite of
reassuring.** Its five references span four subsystems: the machine-level reset passes it to
the controller reset, the non-volatile-memory base is computed as an offset from it, the clock
driver takes its controller base from it, and two separate sites extract the widget number out
of its high bits. **One wrong value there reaches the controller, the clock and the widget
routing simultaneously, so the failure appears in four subsystems at once and no single symptom
points back at the field.** Given that this descriptor has already carried three successive
wrong values here, each surviving because only one consumer ever looked, this is the field most
worth getting right before the vendor-OS phase.

**What follows for a builder, unchanged in substance and now aimed:** derive all of these from
one chip address and assert the *relationships* between them rather than each value alone. That
is the only form in which one guest's boot can protect another's field.

This has already happened once: the device-window term was added to the serial-port
register base and not to the other two, leaving the field Linux depends on pointing into
the Bridge's configuration space.

**The configuration base at `0x08` gained a consumer on 2026-08-12, and this row said it had
none.** IRIX's machine-level reset loads that field and passes it as the **first** argument to
the routine that resets the I/O controller, with the memory base at `0x10` as the second — the
two loads four instructions apart in the same function, the second sitting in the call's delay
slot, which is also why it must come last: it overwrites the register it indexes. Measured by
irix-structs from `assets/boot/6.5.30/unix.IP27` with the instructions recorded, and the
argument order cross-read against the vendor's own source for that call, which takes the two
in the same order. The instruction-level record is in the structure layout
catalogue (citation: `docs/library/irix-struct-layout-catalogue.md`).

**Say what kind of correction this is, because it is a scope change and not a flat error, and
the next reader of this paragraph will want to know whether its author was careless.** They
were not. The claim was true of the two guests in scope when it was written — neither OpenBSD
nor Linux reads this field, and that remains true. IRIX is a third consumer, and it matters now
because the vendor-OS phase is the direction of travel. **The field moves from *unconstrained*
to *constrained by a guest we intend to boot*, so what was a builder's free choice is now a
value that has to be right**, and "write the least surprising thing" is no longer advice this
document can give about it.

**The configuration pointer is now derived from public Bridge geometry.** The
Bridge Type 0 configuration aperture begins at widget offset `0x02_0000` and
allocates `0x1000` bytes per PCI device. IOC3 is device 2, so its configuration
pointer is widget 8 plus `0x02_2000`, or `0x9200_0000_0802_2000`. This value has
not been captured from real firmware. Its standing is a binding derived from
the public Bridge geometry, distinct from the observed UART pointer.

**That is the second instance of one shape in a day: a fact checked correctly in one scope,
with the scope moving underneath it.** The other is the widget byte immediately below. Neither
was caught by re-checking the claim — both were caught by a new consumer arriving, which is why
a claim of this kind should carry the scope it was checked in rather than reading as
unconditional. This row now names the guests it was checked against.

| Offset | Width | Field |
|---|---|---|
| `0x18` | 2 | baud rate — **the one field here a booting guest has read and used** |
| `0x1a` | 2 | flags — **no consumer in any guest tree; our zero is the allocator's, not a decision** |
| `0x1c` | 4 | console type — **we write zero, which no guest's comparison matches** |
| `0x20` | 2 | node identifier |
| `0x22` | 1 | crossbar widget number — **IRIX does not read this field; OpenBSD does** |
| `0x23` | 1 | **PCI device number of the console** — not a count; see the correction below |
| `0x28` | 8 | base-I/O board identity number — **no consumer in any guest tree; unwritten by us** |

**The byte at `0x23` was named "count of devices behind the I/O bridge" in this datasheet and
that name is wrong. It is a device number.** Both guests treat it as one: IRIX assigns it to a
variable named for the console's PCI slot, and OpenBSD assigns it to a local called `device`,
uses it as the device field of a device location, and says in its own comment that the PCI
location is computed from that field. **The model now writes 2, matching the controller's
observed PCI placement.**

**The value remains unexercised today.** IRIX stashes it in a static whose
accessor has zero callers, and OpenBSD reads it only on a path this machine never
reaches. The placement and both consumers agree on device 2, but no current boot
proves this descriptor byte.

**What this rests on, and the point is that it answers one question strongly and a different
one not at all — rather than answering a single question weakly.**

**On what the field *means*, the evidence is strong.** Two guests treat it as a device number,
and they are **independent implementations, not two readings of one source** — which is exactly
the case this project's rules separate from same-source agreement, and it is the strong side of
that line. Two teams working from different documentation do not misread the same byte the same
way by chance. **The name correction rests on this, and it is well supported.**

**On what firmware actually *writes* there, the same evidence is silent.** Consumers tell you
what their authors believed; they are not a record of what the descriptor arrives holding. **We
hold no capture of this field from a real Origin 200**, and a captured descriptor is what would
settle the value outright — the one field in this structure we do have a real capture for is
the serial-port register base, which matches ours whole.

**So: the name and model value now agree.** A captured descriptor remains useful
as an independent check of what real firmware writes.

**The shape is worth more than the fix, because a wrong name does not behave like a wrong
value.** A wrong value eventually produces a symptom. **A wrong name is read as documentation:
it shapes what the next person writes, and everyone who came after read it as settled.** This
one was inferred once and inherited from there. When a field's name and its consumers'
treatment of it disagree, the consumers are the evidence.

**IRIX takes the console's widget number from the memory base address, not from the widget
byte — measured, and the negative is the load-bearing half.** The vendor kernel's
master-bridge base setup loads the **memory base** at `0x10` and extracts bits 31 through 24
of it, with a right shift of 24 followed by a mask to a byte, and uses *that* as the widget
number. **No instruction in that routine reads offset `0x22`.** So a memory base whose widget
bits are wrong sends IRIX at the wrong widget **while the widget byte beside it still reads
correctly** — and a reader checking the widget byte would find nothing wrong.

**"IRIX does not read this field" is correctly scoped and easy to read as "nobody does".
OpenBSD does read it**, in its glass-console path. So the widget byte is not dead — it is
unread by one guest and read by another, which is the same split the consumer map above
describes for the base addresses.

Measured from `assets/boot/6.5.30/unix.IP27`, IRIX 6.5.30, in the PROM's master-bridge
base setup; the field naming cross-read against IRIX 6.5.7 source, which
takes the widget identifier out of the memory base through a macro rather than reading a
stored field. Recorded with the rest of the measured layout facts in
the structure layout catalogue (citation: `docs/library/irix-struct-layout-catalogue.md`).

### Four fields are unexercised only because this machine has no graphics console

**Console type, node identifier, widget number and PCI device number are all read by OpenBSD
inside one routine — the one that works out where the console device physically sits — and that
routine's only caller returns early when there is no glass console.** Our machine has none.

**So these four sit in the class this project distrusts on principle: correct or incorrect
without consequence, because the machine is small.** They are not validated by our boots; they
are merely never consulted. **Model a graphics console and all four go live in the same
commit** — not gradually, and not with a warning.

**One of them would bite immediately, and it is worth understanding why it does not now.**
OpenBSD compares the console type against the I/O controller's PCI identifier and **we write
zero, which is not that identifier**, so the comparison fails. What saves us is that the same
test is an *or* whose second arm asks whether the machine belongs to the later family — ours
does not, so the correct branch is taken **regardless of what the type field holds.** The value
is wrong and currently harmless, **and it stops being harmless on the other machine family
rather than on this one.** That is the distrust-what-works-because-the-machine-is-small case
exactly: not a defect to file, and not a value to call fine.

### Two fields nobody has decided, and one nobody needs to

**The flags byte at `0x1a` and the base-I/O board identity at `0x28` have zero consumers across
all three guest trees, and our model never writes either.** They read as zero because the buffer
is allocated zeroed. **Nobody chose that**, and a later reader should not take the zeroes for
deliberate values — if a consumer ever appears, there is no reasoning here to inherit.

**The baud rate at `0x18` is the one field in this structure genuinely confirmed by a booting
guest.** OpenBSD takes its console rate straight from it and reaches a banner on this machine,
so it is a value a guest has read and *used* — not merely plausible, and not merely unexercised.
**Nothing else in the descriptor has that standing**, which is worth stating precisely because
the rest of the table looks equally settled and is not.

**Sourcing for this section**, which is worth copying: OpenBSD read at the pinned release branch
rather than the moving one, Linux at current mainline, IRIX at 6.5.7, with per-field consumer
counts taken over each whole tree rather than sampled. The full check is
the console descriptor value check (citation: `docs/library/console-descriptor-value-check.md`).

**This is the third consumer to read something out of one of these three base addresses, which
is the argument for deriving all three from one chip address rather than writing them
separately.** OpenBSD splits the serial-port base, Linux dereferences the memory base, and
IRIX now takes a widget number out of the memory base's high bits. Each reads a different
field for a different purpose, so no single guest's boot can protect another's — and this one
extracts a *part* of a field the other two use whole.

**Same shape as the crossbar component's hub-link widget number**, where IRIX also derives the
widget from a scan rather than reading the stored scalar the record provides. A widget number
that looks stored and is actually derived has now turned up twice, on two different structures.
Do not assume a guest reads a widget field because the structure has one.

Linux takes its early console straight from this descriptor and drives that serial port
directly, with no firmware calls at all — which is exactly why booting Linux later is the
best independent check that our graph is right. The field it uses is **the memory base,
not the serial-port base**: it treats that address as the base of the I/O controller chip
and reaches the first serial port at the chip's own register offset from there. It
validates nothing — no null check, no signature, no timeout — so a wrong value gives
either a bus error or an eternal spin waiting for a transmitter-empty bit that never
sets, and in both cases the symptom is a completely silent boot with no diagnostic. Of
everything in the graph, this one field is the one most worth asserting in a test.

**Because Linux dereferences that address without adding anything to it, the memory base
must already be an address the processor can issue — not a bare physical one.** On this
machine an unmapped reference takes its uncached attribute from bits 58 and 57 of the
address, and the attribute that routes a reference to a crosstalk widget is 1, which
means the field has to carry the crosstalk space base of `0x9200_0000_0000_0000` over
the node offset and widget shift. A bare physical address carries attribute 0, which is
hub-special space, and reaches the wrong place with no fault.

**This requirement is observed rather than argued, which matters because an earlier
version of it rested on a contested claim.** Real firmware's own console descriptor,
captured from an Origin 200, holds `0x9200_0000_0862_0178` — the crosstalk space base
with attribute 1 over the widget, device window and register offset. So the machine
itself shows what belongs in this field, and nothing here depends on any argument about
what *other* address forms would do. An earlier version justified the requirement by
asserting that a compatibility-segment address reads node memory and can never reach a
widget; that claim is contested and open, and
the compatibility-segment routing note (citation: `docs/library/compatibility-segment-routing.md`)
carries its current state. The requirement stands without it.

The two consumers therefore do not necessarily want identical content in the two base
fields: Linux dereferences the memory base as-is and so needs the attribute in it, while
OpenBSD takes the serial-port base apart and derives a bus base from its high bits, so
what it needs there is whatever makes that split come out right. Emitting one field and
assuming the other follows is the mistake to avoid. This is the second time this
descriptor has turned out to be load-bearing in a way that is silent when wrong — the
first was its low twelve bits selecting which generation of I/O controller OpenBSD
believes it is talking to.

OpenBSD reads the descriptor too, after the banner, and uses the **serial-port base**
rather than the memory base. It splits that address with **two different masks, and
getting them confused writes a value that passes every check and points at nothing**:

- **The register offset is the low twenty bits.** OpenBSD clears the low twenty bits of
  the base to get the bus it will talk to, and keeps those twenty bits as the offset it
  drives on that bus. This is the part that has to be a real register.
- **The controller generation comes from the low twelve bits of that offset**, compared
  against `0x380`: below means the generation this machine has, at or above means the
  later one, in which case it goes off and reads a configuration register at an address
  that does not exist on our machine.

**Where the guest cuts that address is a property of the guest's release, not of the
machine, and a capture from a different year will show a different cut.** The masks above
are OpenBSD 7.9's, the kernel this project boots. A 2008 capture of the same machine shows
an older kernel splitting the identical firmware address **twenty-four** bits along instead
— its bus base coming out as the widget's own base with the device window still in the
offset, where 7.9 puts the device window in the bus base and keeps only the chip-relative
offset.

**Both split the same firmware value, and only the firmware value is a fact about the
machine.** That is the reason the descriptor carries the whole address and the guest does
the cutting: firmware states where the port is, and each release decides for itself which
part it calls a bus. **So a reader reconciling our fields against a capture must check the
capture's year before concluding anything is wrong** — the older split's bus base looks like
a defect against 7.9's and is simply a different guest doing different arithmetic on the
same correct input.

**The descriptor holds one address, not a bus base and an offset, and a capture showing two
named fields is showing derived values rather than stored ones.** This needs saying because
the derived names look structural: an instrumented kernel prints a *bus base* and an *I/O
address* that sum to the console address, and they read exactly like two fields somebody
wrote. They are not. The guest fetches a **single** console address from the descriptor and
applies two masks to it in the same function; the two printed values are what those masks
produced. **A careful reader has already taken them for structure fields**, and the mistake
is a good one, because it would put a boundary into our contract that the machine does not
have — and a boundary in a contract is something implementers then have to preserve.

**What the two-field view gets right, and is worth keeping in the correct form.** A
decomposition that sums to the right total does not establish that each term falls on the
correct side of the cut a guest makes, and this project's console failure was exactly a term
sitting on the wrong side of that cut. But since the cut is the guest's and moves between
releases, **the check cannot be against a dumped pair.** It is: does our single address, put
through *each* guest's own masks, produce a bus the guest can drive and an offset that
selects the right controller generation? Our address satisfies both the twenty-bit and the
twenty-four-bit split — under either, the low twelve bits of the offset come out below the
generation threshold and select the controller this machine has. **That is the whole-value
form of the check, and it is stronger than matching either capture.**

**Running it produces the strongest confirmation this offset has, by two routes that never
meet.** The captured address was dumped by a kernel cutting it twenty-four bits along, which
therefore never computed `0x20178` at all. Put that same address through the twenty-bit cut
our guest makes and the offset comes out **exactly `0x20178`** — the value this datasheet
specifies, derived independently from the controller's register map in two vendors' headers.
A number taken from register maps and a number falling out of real firmware's own console
address agree to the digit, and neither computation is downstream of the other.

| Cut | Bus base | Offset | Generation bits |
|---|---|---|---|
| twenty bits, the kernel we boot | `0x9200_0000_0860_0000` | `0x2_0178` | `0x178`, below the threshold |
| twenty-four bits, the 2008 kernel | `0x9200_0000_0800_0000` | `0x62_0178` | `0x178`, below the threshold |

Both sum back to the captured address and both select this machine's controller generation.
**The generation bits come out identical under either cut, which is worth noticing rather
than passing over:** the twelve-bit test lands inside the part both splits keep, so it cannot
distinguish them. Passing it says nothing about whether the rest of the address is right —
which is exactly the failure this page's console history is built on.

**Write the serial-port base as the widget's base, plus the Bridge device window for the
slot the controller sits in, plus `0x20178`.** Three terms, and an earlier version of
this datasheet had only two.

- **`0x20178` is the chip-relative offset.** The controller puts its serial block in a
  window at `0x20000` and serial port A at `0x178` inside that window.
- **The device window is `0x60_0000` on this machine.** The controller is the third
  device on the Bridge's bus, and [the Bridge datasheet](bridge.md) places device 0 at
  `0x20_0000` and device 1 at `0x40_0000`, both 2 MB, then devices 2 through 7 in 1 MB
  windows from `0x60_0000`. The third device is device 2, so its window is `0x60_0000`.

**Write it as a sum of named parts, never as one number.** This field has been wrong
three times and each wrong value was a plausible-looking total; a composition shows
which part is missing, and a total cannot.

| Part | Value |
|---|---|
| Crosstalk I/O space, uncached attribute 1 | `0x9200_0000_0000_0000` |
| Widget 8's small window | `0x0800_0000` |
| The Bridge's window for its third device | `0x0060_0000` |
| Serial port A inside the controller | `0x0002_0178` |
| **Total for an Origin 200** | **`0x9200_0000_0862_0178`** |

**An unqualified `0x20178` does not merely miss the controller — it lands somewhere
real.** The Bridge places PCI type 0 configuration space at widget offset `0x02_0000`
and its first device window at `0x20_0000`, so a base of widget-plus-`0x20178` points
into **configuration space**, at the first device's function 0. It decodes, it answers,
and it is not a serial port. That is why the failure presented as a bus error on a byte
store rather than as an obvious dead address, and it is the sharpest argument for
composing the address from named parts: the wrong total was inside a region that exists.

**The device window term is invisible until it is not, which is why it went missing.**
While no Bridge is modelled, dropping it costs nothing: the guest takes the register
offset from the low twenty bits, which come out `0x20178` with or without it, so the
console still resolves to the right chip register. The moment the controller sits behind
a modelled Bridge, that term is what decides whether the guest's writes reach the chip at
all — because the bus base it computes is the *other* part of the split, and that part
does differ.

**This one is observed in the strongest available form: a dump of the field itself.** The
instrumented-kernel capture in the real-machine boot logs (citation: `docs/library/boot-logs.md`)
prints the configuration graph as the kernel walks it, and the line reading
`console 0x9200000008620178 baud 9600` **is this field**, straight out of a real Origin
200's firmware. Every term is accounted for:

| Term | Value |
|---|---|
| Crosstalk I/O space, uncached attribute 1 | `0x9200_0000_0000_0000` |
| Widget 8 | `0x0800_0000` |
| The Bridge's window for its third device | `0x0060_0000` |
| The chip-relative serial port A offset | `0x0002_0178` |

**Note what that settles beyond the offset.** The base is a full crosstalk-space address
carrying attribute 1, not a bare physical one — the same requirement the memory base has,
and for the same reason. And the device-window term is present in the *firmware's own*
descriptor, so there is no question of the early console path using a shorter form and
something re-pointing it later: real firmware writes all three terms.

**A weaker piece of evidence was nearly used for this and is worth naming, because it
would have been wrong to lean on.** A **separate** capture — a different machine, three
years earlier, running Linux rather than OpenBSD — carries the line
`ttyS0 at IOC3 0x8620178`, which arrives at the same number. But that is Linux reporting
where it mapped the port after its own bus enumeration, not a record of what firmware
wrote: a different claim that happens to agree. Citing it would have been citing a
coincidence of values as though it were a dump of the field.

**An earlier version of this paragraph called them the same capture. They are not**, and
the mistake is worth leaving on the record because it sat inside the paragraph warning
against exactly this. Two independent machines agreeing on the value is *stronger*
evidence than one capture carrying both lines would have been — so the error understated
the case while being made in the course of arguing for precision about evidence.

**Take the whole captured address, not the part that answers the question you asked.**
This field has now been wrong twice, and both times the error was a term that the check in
hand could not see. First it was written as `0x178`, which passes the twelve-bit
generation check that `0x20178` also passes. Corrected to `0x20178`, it passed the
twenty-bit register-offset check — and still dropped the device window, because that term
lives in the *other* half of the guest's split and the offset check cannot reach it.
Twenty bits was weaker than the whole address in exactly the way twelve had been weaker
than twenty.

**Three independent kinds of evidence now agree, which matters because the wrong value
survived agreement of the weaker kind.** The correction was first derived from OpenBSD's
console setup; a real machine's boot capture then confirmed it; and SGI's own controller
header and mainline Linux's driver both compute the same offset by **structure
arithmetic** — adding the serial block's position to the port's position within it —
rather than by the twelve-bit check that the wrong value also passes. That last point is
the one worth keeping: those two arrive at `0x20178` by a route on which `0x178` is not
expressible, so they could not have agreed with the mistake.

**The trap, which is worth stating because we fell into it.** `0x178` alone also passes
the generation check — both `0x20178` and `0x178` have low twelve bits of `0x178`, so
both select the correct controller generation and the machine looks configured. Only one
of them is a register. Writing the discriminator value where the register offset belongs
gives a guest that identifies the console correctly and then drives an address that
answers nothing, and the first symptom is a bus error on a byte store of `0x03` to
offset `+3` — the line-control register, the guest setting 8N1 — a long way from anything
that mentions the console.

So the low bits of the serial-port base are not cosmetic twice over: the low twelve
select a device model, and the low twenty have to land on a real register. A value that
satisfies the first and fails the second is the failure mode to watch for, because
nothing rejects it.

**Board record**, 192 bytes:

| Offset | Width | Field |
|---|---|---|
| `0x00` | 4 | offset of the next board, or zero to end the list |
| `0x04` | 1 | record kind: 1 = local board, 2 = remote board |
| `0x05` | 1 | **board type**: the high nibble is the class, the low nibble the type within it |
| `0x06` | 1 | record version |
| `0x07` | 1 | board revision |
| `0x08` | 1 | board firmware version |
| `0x09` | 1 | flags |
| `0x0a` | 1 | slot number |
| `0x0c` | 2 | debug switches |
| `0x0e` | 2 | module number |
| `0x10` | 1 | partition number |
| `0x12` | 2 | diagnostic result |
| `0x14` | 2 | diagnostic parameter |
| `0x16` | 1 | inventory history |
| `0x17` | 1 | **number of components** |
| `0x18` | 8 | board identity number |
| `0x20` | 2 | node identifier |
| `0x24` | 96 | **twenty-four component offsets**, 4 bytes each |
| `0x84` | 4 | offset of the board's error record |
| `0x88` | 8 | parent board |
| `0x90` | 4 | graph link |
| `0x94` | 1 | confidence |
| `0x96` | 2 | owning node |
| `0x98` | 1 | extra-identity flags |
| `0x99` | 32 | board name |
| `0xb9` | 7 | tail padding to the eight-byte boundary |

**Open question, not a requirement: what the graph link at `0x90` should arrive as.** Our model
leaves it zero. **IRIX initialises it to all ones — the graph's own "no vertex" marker — as it
walks the board list**, measured by irix-structs from a store instruction in the vendor kernel's
own early graph setup, which fixes the offset and the width exactly as a load would.

**Nobody has established that this matters, and it is recorded here as a question because it is
one.** IRIX is initialising a field for its own later use, so a zero arriving from firmware may
simply be overwritten before anything reads it. **What would settle it is whether anything reads
the field before IRIX overwrites it**, which nobody has checked.

**Why it is worth checking rather than assuming in either direction:** the Hub port descriptor's
node identifier has all-ones-means-absent as a datasheet requirement for exactly this reason, so
the shape is one this machine already uses. That makes "zero is fine" and "all ones is required"
both plausible, and neither observed. **Nothing should change in the model on the strength of
this paragraph.**

Board classes: `0x10` node, `0x20` input/output, `0x30` router, `0x40` midplane,
`0x50` graphics. The types we need are `0x11` for a processor-node board, `0x41` for an
eight-slot midplane carrying the crossbar, and `0x21` for a base-I/O board.

**The high nibble carrying the class separately from the low nibble carrying the type
within it is required by a second consumer, not only convenient.** Mainline Linux's
board-list walker has two matchers over exactly this split: one compares a board's whole
type for an exact match, the other compares only the class nibble. Both walk the board
list's own next-board links from a starting point, so a board list built to satisfy
OpenBSD's walk — which never uses the class matcher — still has to chain correctly past
every board and encode its class correctly for a consumer that does. Our three board
types already carry the class in the high nibble as declared above, so this is
confirmation of an existing choice rather than a change; recorded because it was a
choice made for one guest and is now known to be required by a second one that reads the
graph in a different shape. Source:
what Linux reads out of KLCONFIG, against OpenBSD (citation: `docs/library/linux-ip27-klconfig-consumers.md`),
commit `7b5977b`, for the requirement itself.

**And the board walk this requirement protects has now run, not merely been read.**
Mainline Linux booted on this machine, walked the board and component lists, and printed
"Discovered 1 cpus on 1 nodes", the R10000's topology, "Running on node 0" and "Machine
is in M mode" — all of it correct, from a consumer that reads the graph in the different
shape this section describes. That is an observation of the board-list walk succeeding,
not only a source-read prediction that it should. **What it does not confirm is
anything past that point**: this same boot died in memory initialisation before reaching
the Bridge, on an unrelated defect in the memory map rather than in the graph. Source:
the first Linux boot's findings (citation: `docs/library/linux-first-boot-findings.md`), booted
2026-08-11.

**The record version has a required value, and it is not one.** IRIX's board-naming
routine treats a board whose record version is below two as unnamed, substituting a
generic label for it — the same fallback fires from every one of the three call sites in
that routine, not from a special case. SGI's firmware writes two on an ordinary board and
three on the node board specifically, for a reason stated in its own source comment: a
later register addition to the per-node error structure the node board's record carries.
**Write two on the midplane and the base I/O board, three on the node board.** OpenBSD
never reads this field, which is why the requirement was invisible until IRIX's own
source was read; a board below version two is a machine that finds no name for a board it
otherwise finds correctly, and every device path beneath that board follows the generic
name rather than the real one — including a stored root-device path, which stops
resolving. The same routine finds a board by its module and slot fields in order to name
it, so those two are the mechanism's other half.

Three things about board traversal are easy to get wrong. The list is terminated **only**
by a zero next-board offset — a consumer dereferences the record first and checks
afterwards, so there is no null-pointer escape. A board whose record-kind byte does not
have the local-board bit set is skipped entirely, components and all. And a board's
components are addressed relative to **the board's own node identifier**, not the node
whose list is being walked, so that field has to be right on every board even in a
single-node machine — and it has to be in range, because it also indexes a
hundred-and-twenty-eight-entry table with no bounds check.

**The board's own flags byte at `0x09` has a real consumer, and zero is the
confirmed-correct value for it rather than an unconfirmed placeholder.** Mainline
Linux's router-distance walk skips any board whose flags carry a duplicate-board marker.
That is a meaningfully different claim from "nothing writes it yet": every board we emit
has this byte at zero because nothing writes it, and zero happens to be right for this
consumer, because none of our boards is a duplicate. Source: what Linux reads out of
KLCONFIG (citation: `docs/library/linux-ip27-klconfig-consumers.md`), commit `7b5977b`.

**Component record**, 56 bytes, and the first field of every typed component:

| Offset | Width | Field |
|---|---|---|
| `0x00` | 1 | **component type** |
| `0x01` | 1 | record version |
| `0x02` | 1 | **flags**: bit 0 enabled, bit 1 failed |
| `0x03` | 1 | revision |
| `0x04` | 2 | diagnostic result |
| `0x06` | 2 | diagnostic parameter |
| `0x08` | 1 | inventory history |
| `0x10` | 8 | component identity number |
| `0x18` | 1 | physical identifier |
| `0x1c` | 4 | virtual identifier |
| `0x20` | 1 | crossbar widget number |
| `0x22` | 2 | node identifier |
| `0x28` | 8 | **back-pointer to the matching firmware component** |
| `0x30` | 4 | offset of the component's error record |
| `0x34` | 4 | padding |

A component whose enabled bit is clear, or whose failed bit is set, is skipped by the
consumers. Set the enabled bit on everything we emit; that is not optional.

Component types: 1 processor, 2 hub, 3 memory bank, 4 crossbar, 5 I/O bridge, 6 I/O
controller, 10 graphics, 11 disk controller, 19 controller serial port.

Bytes `0x24` and `0x25` are unused on this machine and must be written as zero. The
reference kernel calls them padding; OpenBSD names the same two bytes as a bitfield
holding an Origin 3000 controller port number, PCI bus number, multifunction flag and
PCI function number. Nothing on an Origin 200 reads them, but a builder that leaves
them uninitialised is writing Origin 3000 PCI routing at random.

### The typed components, with offsets

Every offset below was derived from the reference kernel's own declarations, checked
field for field against the IRIX 6.5.30 and OpenBSD 7.9 copies of the same structures,
and then **measured with a compiler rather than worked out by hand** — the alignment
arithmetic around the eight-byte identity number and back-pointer is exactly where hand
derivation goes wrong. Each total below equals the size this datasheet already stated
before any offset was filled in, which is the check that the layouts are right.

**Processor**, 72 bytes:

| Offset | Width | Field |
|---|---|---|
| `0x00` | 56 | common record |
| `0x38` | 2 | **processor identification**, 16 bits |
| `0x3a` | 2 | floating-point implementation register value |
| `0x3c` | 2 | clock in megahertz |
| `0x3e` | 2 | secondary cache size in megabytes |
| `0x40` | 2 | secondary cache clock in megahertz |
| `0x42` | 6 | tail padding |

**The identification field is sixteen bits, not thirty-two, and this is the single
easiest way to corrupt this structure.** It holds the low half of the processor
identification register: the implementation number in the upper byte and revision in the
lower byte. A builder that writes a 32-bit value here overwrites the floating-point field
next door.

OpenBSD takes the boot processor's clock, cache size and identification straight from
the first processor component it meets, and counts the rest. There is no other source:
with no enabled processor component the kernel's recorded clock stays zero, which breaks
its delay loop, and its translation-buffer size stays zero, which leaves the buffer
unusable — and it dies silently just after the last line it managed to print.

The field at `0x3a` is the floating-point implementation and revision register value.
Later machines reuse the slot: IRIX 6.5.30 renames it to carry mode bits
for `hinv` to report, and OpenBSD marks that reading as Origin 3000 behaviour. Neither
changes the field layout on the earlier machines described here. The processor and
floating-point fields are separate identities and must be populated independently.

**Hub**, 96 bytes:

| Offset | Width | Field |
|---|---|---|
| `0x00` | 56 | common record |
| `0x38` | 4 | flags |
| `0x3c` | 2 | port descriptor: node identifier |
| `0x3e` | 1 | port descriptor: flag byte |
| `0x40` | 4 | port descriptor: offset |
| `0x44` | 4 | padding |
| `0x48` | 8 | containing box's identity number |
| `0x50` | 4 | offset of a manufacturing identity string |
| `0x54` | 4 | padding |
| `0x58` | 8 | hub clock in hertz |

The port descriptor is the same eight-byte shape used throughout the graph: a two-byte
node identifier, a one-byte flag, one byte of padding, and a four-byte offset that
natural alignment places at `0x40`. On Origin 3000 this record grows a trailing vector
of inter-hub distances, so the 96-byte size is specific to the machine we model.

**An unconnected port descriptor's node identifier has a required value, and it is not
zero.** IRIX treats any value other than the invalid marker — all ones — as a real link:
it forms a board address from the node identifier and the offset unconditionally and
dereferences it, and on a mismatch it panics outright rather than warning. Zero is a live
node identifier on this machine, not a safe default, so a zeroed port descriptor is read
as a link to node zero at offset zero — our own hardware directory — and the lookup that
follows fails to recognise it as a board. **This is not only a consumer preference: real
firmware writes the invalid marker into every Hub component's port descriptor
explicitly**, so an unconnected port reading zero is a divergence from what the reference
firmware does, not only a value a stricter guest happens to require. The same convention
applies to a router's port descriptors, which use the identical eight-byte shape. **Write
the port descriptor's node identifier as all ones when the port names no link**, which is
every port on this machine, since the Hub sits at a fixed widget with no crossbar
component modelled.

**Memory bank**, 80 bytes:

| Offset | Width | Field |
|---|---|---|
| `0x00` | 56 | common record |
| `0x38` | 2 | total memory in megabytes, signed |
| `0x3a` | 2 | bank-to-address mapping selector |
| `0x3c` | 16 | **eight bank sizes** in megabytes, two bytes each |
| `0x4c` | 2 | directory-memory attribute |
| `0x4e` | 2 | tail padding |

All five fields are signed 16-bit values. The attribute at `0x4c` is two bytes and not
four: IRIX describes it as the directory flavour and OpenBSD as directory memory per
bank.

**The bank-array length is coupled to the hub's network interface, and the two guests
resolve it differently.** OpenBSD decides at run time — it reads the hub's
network-interface status register, takes the more-nodes bit as the mode, and then parses
every memory-bank component as an eight-entry record in M mode or a four-entry record in
N mode. The N-mode record is 72 bytes, not 80, so the mode does not merely change the
array: it changes where the attribute sits and how long the whole component is. Mainline
Linux does not decide at run time at all; its bank count is fixed when the kernel is
built and defaults to the eight of M mode, so a Linux guest cannot adapt to a machine
that reports otherwise.

So the eight-entry layout here is correct only while the hub reports M mode, and that is
a contract between two datasheets rather than a property of this one. The
[hub network interface datasheet](hub-network-interface.md) specifies the more-nodes bit
as bit 18 of the status register, resetting to zero for M mode. If that bit ever reads
one, OpenBSD reads four bank sizes out of a record that holds eight and picks up the
fifth bank's size as the attribute, while separately computing every node-relative
address with the wrong shift. Both failures are silent, and they come from one bit.

**This is cheap to test, because OpenBSD prints the answer.** It emits a line naming the
mode it decided on before it walks the memory banks, so a boot transcript that says M
mode is direct evidence that the bit and this layout agree.

**Read that check for exactly what it is, and pair it with something that measures
progress.** The line reports the decision the guest made about the mode; it does not
report that the memory-bank records were parsed, and it is printed *before* they are.
More generally, a printed line is evidence for the one decision it names and is not
evidence that the boot got further — a transcript can be **identical on both sides of a
substantial fix**. That is observed here, not hypothetical: correcting the node table's
element width moved the boot from early node enumeration all the way to interrupt setup
and the output stayed at the same three lines throughout. Any assertion in this
datasheet of the form "the guest prints X" is a check on X alone. Where the question is
whether something advanced, assert on a thing that advances — a later line that only
appears once the structure is walked, a count of firmware calls dispatched, or a
register the guest only writes after the step in question.

**Crossbar**, 192 bytes:

| Offset | Width | Field |
|---|---|---|
| `0x00` | 56 | common record |
| `0x38` | 128 | sixteen port descriptors, eight bytes each |
| `0xb8` | 4 | widget number of the link to the hub |
| `0xbc` | 4 | tail padding |

Each port descriptor has the same shape as the hub's. The hub-link widget number is the
field that matters: it is what the guest uses to address the hub from the I/O side once
devices come up. A real Origin 200 emits no such component, so this is documented for
completeness and for larger machines.

**Module serial number**, 72 bytes:

| Offset | Width | Field |
|---|---|---|
| `0x00` | 56 | common record |
| `0x38` | 16 | serial number: ten bytes of text, or a 64-bit integer read over the same bytes |

The two readings overlap at the same offset, and the field is sixteen bytes rather than
ten because the eight-byte integer reading forces the alignment. This is the only
component a real Origin 200's midplane board carries.

On the reviewed Origin 2000 startup path, firmware creates this component before module
inventory and leaves its serial bytes at their zero-initialized value. Firmware may populate
the field by finding the module-identity record after the node-local Hub has concatenated its
family-`0x0b` node/PIMM and family-`0x09` module records, then encoding the first serial field
that follows the module name. The ordering is part of the parser contract. The
Hub module-serial proposal (citation: `docs/designs/origin2000-hub-module-serial-route.md`) defines this
source-backed path and records the independently reviewed clean payload schema. Otherwise firmware
conditionally reads the module-owned one-wire identity through the system controller, decodes
the serial and writes this field. A populated, non-memoryless first node causes an early return
before that controller read. In the retained four-node run, the kernel rejected the empty field
twice and then reported that it found no serial. The first zero byte and two non-duplicate
midplane views are control-flow inferences; that run has no KLCONFIG dump or controller-bus trace.

**The minimum board list** for a single-node Origin 200, in list order:

1. **A processor-node board**, type `0x11`, with one processor component, one hub
   component and one memory-bank component. The firmware's own convention places the
   first processor at component index 0, the second at 1, the hub at 2 and the memory
   bank at 3; consumers scan by component type rather than by index, so a board with one
   processor is well-formed.
2. **A midplane board**, type `0x41`. On a real Origin 200 this board carries **no
   crossbar component at all** — a capture from real firmware shows it holding a single
   module-serial-number component, type 26, and nothing else. That is why the guest's
   fallback of assuming the hub sits at widget `0xa` is the normal path on this machine
   rather than a degraded one. **The machine has no crossbar at all** — not a reduced one —
   so there is nothing for the graph to describe and the guest probing only widget 8
   without consulting it is the ordinary case. Follow the real firmware: emit the board
   with its serial-number component and no crossbar.

   **And real firmware writes a component count of *zero* on this board while populating
   that component, which is not a transcription error in the capture.** The captured dump
   states a count for each board and separately lists what it finds: the node board says
   four and lists four, the base-I/O board says four and lists four, and the midplane says
   **zero and lists one**. Two exact agreements make the third a fact about the firmware
   rather than about the instrumentation.

   The consequence is behavioural, because **OpenBSD walks a board's components by that
   count** — its enumeration loop runs to the count field, not to the end of the array. So
   on real hardware **the midplane's serial-number component is present and never
   visited.**

   **This has a mechanism, not only a capture, and the mechanism is why the count has to
   be zero rather than merely why real firmware happens to write it that way.** SGI's
   firmware writes the serial-number component and then decrements the board's component
   count deliberately, by one, after populating it — and its own consumer for that
   component reads it back at the index **equal to** the (decremented) count, which is
   exactly the slot the decrement excludes from an ordinary walk. So a board whose count
   reads one, rather than zero, puts the serial number one slot away from where that
   consumer looks for it: the component that is supposed to sit just past the declared
   count no longer does. **The capture's internal consistency established this fact
   first; the firmware's own source, read independently, explains it by a completely
   different route and agrees.** A machine that writes a count of one here does not
   merely make OpenBSD visit a component the real machine leaves alone — it also breaks
   the one guest that depends on finding that component exactly one slot past the count.
   Write zero.

   Origin 2000 hardware can carry a crossbar component, and Linux looks for that
   record on those topologies. The Origin 2000 hardware reference (citation: `docs/machines/origin2000.md`)
   records the physical Crossbows without claiming that the current `origin200`
   QEMU target synthesizes their firmware inventory.
3. **A base-I/O board**, type `0x21`, carrying **four components in this order: the I/O
   bridge, two disk controllers, and the I/O controller** — component types 5, 11, 11 and
   6. The count says four and the list holds four, so this board is one of the two whose
   count and contents agree exactly.

   **An earlier version of this entry named the bridge and the controller and then "its
   serial and ethernet sub-components", which is wrong twice.** It omitted the two disk
   controllers entirely, and the sub-components it named appear nowhere in the real board's
   list — the component type numbers for a serial port and an ethernet exist, and this board
   does not carry them. **The list is flat and it is these four.**

   **The two disk-controller components are worth more than the correction.** They are a
   third independent statement that this machine has *two* disk controllers, agreeing with
   what a guest's own PCI probe reports at devices 0 and 1, and with what the interrupt-pin
   rule requires of the bus for the two guests to compute the same pin. **Three routes that
   share nothing: firmware's own description of the board, a guest enumerating the bus, and
   a guest deriving an interrupt from which device numbers are occupied.**

   Nothing on the banner path reads this board. It is in the minimum list because the moment
   the banner works the next thing the guest does is walk it looking for a console, and
   because the configuration graph must describe the machine the emulator actually built.

**Every component's back-pointer to a firmware component must be a valid address or
zero, never garbage.** OpenBSD casts that field for every component it visits and
dereferences it for graphics components to find a graphics console, and prints its
contents for every component in a debug build. We emit no graphics component, so zero is
the right answer everywhere and costs nothing; a builder that leaves the field
uninitialised will work until the day someone builds a guest with debugging on, and then
it will crash somewhere unrelated.

## The global data area

One kilobyte at offset `0x11000`, one per machine rather than one per node, reached
through the absolute pointer in the hardware directory's global-data-area entry.

| Offset | Width | Field | Value |
|---|---|---|---|
| `0x00` | 4 | signature | `0x58464552`, the four characters `XFER` |
| `0x04` | 2 | **version** | `2` or higher |
| `0x06` | 2 | master node and processor number | `0` |
| `0x08` | 4 | firmware-operation request | invalid-request marker |
| `0x0c` | 4 | virtual dip switches | `0` |
| `0x10` | 24 | three exception-handler hook pointers | `0` |
| `0x28` | 4 | partition identifier | `0` |
| `0x2c` | 4 | debugger symbol count | `0` |
| `0x30` | 24 | debugger symbol and name table pointers, kernel replication mask | `0` |
| `0x80` | 128 | **node table**: 64 entries of **2 bytes each**, a signed node identifier per node indexed by compact node number, negative for absent | entry 0 = `0x0000`, the other 63 entries = `0xffff` |

**The firmware-operation request field is not the channel a request actually reaches
firmware through, and this datasheet should not be read as saying it is.** OpenBSD's
reboot and shutdown requests write a word to a Hub processor-interface register instead
— a register belonging to the Hub's own contract, not this structure's — with a comment
in the guest's own source stating plainly that the structure field is not where firmware
looks. This structure's field costs nothing to write correctly and nothing on our path
currently reads it either way, but a synthesized firmware that ever implements reboot
and watches only this field will silently ignore every request that arrives.

**The debugger-pointer row was eight bytes short.** Three 64-bit pointers is twenty-four
bytes, running from `0x30` to `0x47`, not sixteen — Linux's global-data-area header lays
the same three fields out at byte offsets 48, 56 and 64. The node table's offset is fixed
at `0x80` regardless, so nothing downstream shifts; a builder writing from the previous
sixteen-byte figure would leave the last eight bytes of this row undefined instead of
zeroed.

**Each entry is a signed sixteen-bit identifier, and the width is as load-bearing as
the sentinel.** The two facts are one fact and stating either alone is not enough: an
implementer who reads "256 bytes, entry zero is zero, the rest are minus one" can
satisfy every word of it with one byte per entry, because a byte table also reads as a
zero followed by all ones. It is wrong and it looks right. The guest scales the index
by two and loads a signed halfword, so against byte-wide entries its first load pairs
node zero's identifier with the absent marker sitting beside it, reads `0x00ff`, and
tests that as **positive** — so nothing is skipped, it goes scanning a node numbered
255, faults on the first address it forms there, and hangs three lines into the boot
with no diagnostic. Absent is all ones across **both** bytes.

**The count is 64, not 128, and this was unsourced for the datasheet's whole life.** Real
firmware's table-building routine writes exactly the maximum-compact-nodes constant.

**Cite the conditional, not the value, because the header defines that constant three
times.** IRIX 6.5.5f's SN0 architecture header selects between them with build switches:
**64** on the default branch, **128** for the top-of-line 128-processor Origin 2000 — a
larger machine that ships its own firmware image and carries a metarouter, which a
64-processor configuration has no need of — and **4** for the software simulator. A flat citation
saying "the header says 64" would be as wrong as one saying 128; what makes 64 right is
that ours is the default branch. That matters here more than usual, because the
256-byte figure this replaces came out of the same header.

Mainline Linux agrees independently and unconditionally: its node-count shift defaults to
six, giving 64, with no override for this platform. The 256-byte figure entered this datasheet in its first draft with no
citation and every later correction, including the element-width fix, built on top of it
without re-deriving it.

**A guest's reader may declare more than firmware writes, and one does.** OpenBSD's
structure declares 128 entries and clamps its walk first to that capacity and then to 64
in M mode; the second clamp is what binds, so it reads exactly the 64 entries firmware
wrote and stops. That is a coincidence of two numbers agreeing, not a guarantee — **if the
Hub ever reported N mode, only the reader's own 128-entry clamp would apply and it would
read 64 entries past the end of our table.** One more consequence of the more-nodes bit,
on top of resizing the memory-bank record.

**And that over-read fails quietly rather than loudly, which is worse.** The space past
the table is inside the same one-kilobyte region and is zeroed, so the extra entries read
as **node zero** — not as the negative marker that ends a walk. The guest does not fault
and does not leave the buffer; it silently scans node zero another sixty-four times. The
damage is bounded, and nothing reports it.

The 128 bytes is therefore 64 entries. Anything that walks the table needs the count
and not the byte size, and the two numbers are different.

The node table is the only enumeration of nodes either guest has. Linux walks it from
entry zero and stops at the first negative entry, with no fallback whatever; OpenBSD
skips negative entries and computes how many to consider from the directory entry's size.
Entry zero must therefore be the master node's identifier and entry one must already be
negative on a single-node machine.

**Version two or higher is a requirement.** A guest that finds the wrong signature or a
version below two abandons the structure entirely, sets the master node to zero and the
node count to zero. For a single-node machine that degradation happens to land on the
right answer, which is precisely what makes it dangerous: the machine boots, nobody
notices, and the first second node exposes it. Write version two.

The guest computes how many nodes to scan from the *size* field of the directory's
global-data-area entry: it subtracts the node table's offset of `0x80` in unsigned
arithmetic and then **divides by the two-byte element width**, so the count is
`(size - 0x80) / 2`. Two clamps follow, in order — first to the **reader's own** declared
capacity, which is 128 entries in OpenBSD and is larger than anything firmware writes,
then to **64 when the machine is in M mode**, which ours is. Only the second binds.

**Write `0x400`, the full size of the region the directory entry describes.** That
yields 448 before clamping and 64 after, so the guest considers 64 entries — exactly the
number firmware writes — finds entry zero equal to the master it has already scanned and
the other 63 negative, and stops. Any size from `0x82` upward is safe, because the M-mode
clamp bounds the count to the entries that exist whatever the size field says.

**The one value that is not safe is a size below `0x80`**, which underflows to an
enormous number, survives both clamps as 64, and sends the guest walking board lists
at 64 bogus node addresses. So the failure mode is specifically an undersized or
zeroed size field, not a merely inexact one.

The master field is worth a second look. The firmware packs a node identifier and a
processor number into those sixteen bits; OpenBSD reads the whole field as a node
identifier and stores it in a signed variable, so a value with the top bit set would come
out negative and poison every node address it computes. Our master is node zero,
processor zero, so the packed value is zero and the ambiguity never bites — but write
zero deliberately rather than by accident.

The field order above is the one to implement. The same headers that declare the
structure also carry a set of symbolic byte offsets for assembly-language use in which
the master-node and firmware-operation fields are swapped relative to the structure; the
structure is what every C consumer compiles against, so the structure wins.

## The launch blocks

Two blocks at offset `0x2400`, `0x200` apart, one per processor on the node. Not on the
banner path — they matter when we start a second processor — but the directory advertises
them from the start, so they get written from the start.

| Offset | Width | Field |
|---|---|---|
| `0x00` | 8 | signature, `0xaddbead2addbead3` |
| `0x08` | 8 | busy flag |
| `0x10` | 8 | address of the function for the slave to call |
| `0x18` | 8 | one's complement of that address, as a validity check |
| `0x20` | 8 | the single parameter passed to it |
| `0x28` | 8 | stack pointer for the slave |
| `0x30` | 8 | global pointer for the slave |
| `0x38` | 24 | three exception-handler addresses for the slave |

The protocol is that the master fills a block and interrupts the target processor; the
target only calls the address if the complement matches, and clears the complement before
transferring control so that a spurious interrupt cannot re-enter it. The slave sets the
busy flag and the idle loop clears it on return.

**The measured mechanism, from the release we boot.** The layout above is what the block holds;
this is how the two ends drive it, from disassembly plus live measurement of the 6.5.30 PROM.

- **The slave's wait proceeds two ways.** A parked slave runs a launch loop (entry `0xbfc04884`
  in the running image) that watches its own processor-interrupt pending register `PI_INT_PEND0`,
  bit `10 + cpu`, for the master's poke — and **also proceeds when the block's busy word (at
  block `+8`) is set**, which would cover an interrupt that was missed or coalesced, though that
  purpose is a reading rather than a measured fact. On either it re-reads the block and validates
  before calling: signature `0xaddbead2addbead3`, the busy flag set, and the call address
  matching its stored one's-complement (their XOR all-ones). Only then does it transfer control,
  taking the stack and global pointers from the block.
- **The master's poke is a pending-bit set.** The poster (at `0xbfc04bd0`) fills the target's
  block in that node's memory, then writes `(10 + cpu) | 0x100` to the target Hub's
  processor-interrupt pending-modify register at `0x90` — the set-a-pending-bit form — raising
  the `PI_INT_PEND0` bit the slave watches.
- **The block is located through a node-relative pointer.** The slave reads its block pointer
  from `0x9600000000002000 + (nasid << 32) + 8` — processor B's from a further `0x28` on — a
  node-relative cached location that folds the node number into its high half. This is the
  pointer's location, not the block's own address; the block itself is the static-layout entry
  above.
- **Per-CPU progress is on the panel.** Each processor writes its boot-status display codes — the
  values `0x45` through `0x48` observed through the launch — to its own per-processor junk-bus
  display register (Hub-local `0x220080 + cpu*0x20` on this machine class), so the front panel
  shows each processor's progress independently.

## Behavioral contract

This is the order in which OpenBSD's kernel touches the structures on the way to its
banner. Each step names what must already be correct.

1. **Locate the parameter block.** The guest reads processor identification to decide
   which address window to use, and on this machine uses the compatibility window, at
   `0x1000`. It then tries the 32-bit block layout first and the 64-bit layout second.
   Requires: the parameter block readable through compatibility space, and the signature
   written as a full 64-bit field.
2. **Print the firmware-identification line.** "ARCS64 Firmware" — the first output the
   machine ever produces, through the write routine on handle 1. Requires: the call
   vector pointer in the parameter block, and a working write routine.
3. **Identify the machine.** The guest calls the system-identifier routine, then asks for
   the root component's child. **That call must return null**, and then the guest checks
   the hardware directory's configuration-graph entry signature and concludes it is on
   this machine family. Requires: a non-null system identifier, a null child, and a valid
   directory signature.
4. **Cache the shutdown routines.** The guest reads the halt, power-down, restart, reboot
   and interactive-mode entries out of the call vector as raw pointers.
5. **Walk the memory descriptors.** Detailed below. Requires: the ordering rules.
6. **Read the environment.** Console output, graphics, keyboard, ethernet address,
   console baud rate and load options, then the load partition a little later. A missing
   variable is tolerated: the guest checks for null and falls back — the baud rate
   defaults to 9600 if the value is outside 50 to 115200.
7. **Print the machine-identification line.** "Found SGI-IP27, setting up."
8. **Read the boot-flash configuration block** and print the product name, "Origin 200".
   Requires: the signature and machine-type field in the flash region.
9. **Read the configuration graph header**, then the hub's network-interface status
   register to decide the addressing mode, and print "Machine is in M mode."
   **This is a hub register read, not a firmware structure.** The banner needs the hub's
   register window to respond, and specifically needs the more-nodes bit of the
   network-interface status register to read as zero — if it reads as one, the guest
   switches to the other addressing mode and every node base, memory base and widget
   address it computes afterwards is wrong. The processor interface's processor-number,
   region-present and address-alias-size registers are read around the same point, and
   the last two are written back. **The banner therefore does need one device to
   respond**: the boot plan (citation: `docs/plans/archive/mvp-boot.md`) is right that no serial port, I/O
   bridge or crossbar model is required, and wrong that no device model at all is — the
   hub's register window must exist and read as zero. Its contract belongs in the hub
   datasheet, not this one, but the dependency is real and it is on the banner path.
10. **Read the global data area** through the directory pointer, check its signature and
    version, and take the master node and node count.
11. **Walk the configuration graph**, board by board and component by component, taking
    the boot processor's clock and cache from the first processor component, memory
    above 32 MB from the memory-bank components, and the hub's widget number from the
    crossbar component.
12. **Print the memory summary.** Banner complete.

Linux exercises a completely separate path with **no firmware calls at all**, and this is
structural rather than incidental: its Origin port is built with the command-line half of
the firmware library and nothing else, so the call vector is never even resolved. It
takes its command line from an argument count and vector left in the first two argument
registers at kernel entry, its node list from the global data area's node table, its
processor count and clock and its memory bank sizes from the configuration graph, and its
early console from the console descriptor. Two of the argument strings are special: the
load-partition variable becomes the root-device argument and the load-options variable is
appended verbatim, both moved to the front of the command line, while the other five
standard variables are recognised and dropped. So a Linux boot needs the argument vector
built and the graph, directory and global data area correct, and needs neither a valid
parameter block nor a working call vector.

That independence is what makes Linux the better check of the graph — and it comes with a
warning about how little either guest verifies. Linux checks **nothing**: not the
configuration graph's signature, not the directory entries' signatures, not the global
data area's signature or version, even though the constants to do so are defined right
next to the structures. OpenBSD checks exactly two things: the directory's
configuration-graph entry signature, which is how it identifies the machine, and the
global data area's signature and version. **Neither guest ever checks the configuration
graph's own signature**, which is worth knowing before someone concludes that a booting
guest proves the graph well-formed.

**And the third guest we care about checks it no more than these two.** IRIX references
the graph header's magic constant in exactly one place in its kernel, and that place is
an operator-invoked hardware-error dump handler, reached only from panic handling and
gated behind a build flag and a runtime enable flag — never from the routines that walk
the board and component lists at boot. The graph's version field is declared and read by
nothing at all, in any of the three kernels this project reads. So a malformed
configuration graph — a corrupt link, a wrong signature, a version nobody wrote — passes
silently through all three guests' boot paths, and IRIX's only defence against it never
runs unless an operator asks for it after the machine has already failed some other way.
**Our own test is the only thing that will ever prove the graph well-formed**, and this
is stronger evidence for that sentence than it had before: not two guests indifferent to
the signature, but three.

## Memory descriptors

The guest asks for descriptors one at a time, passing back the pointer it was last given
and getting the next, until the routine returns null. Each descriptor is 24 bytes: a
32-bit type at offset zero, four bytes of padding, a 64-bit base page at offset 8, and a
64-bit page count at offset `0x10`. The page size is 4096 bytes regardless of what the
guest's own page size is.

Because the guest hands us back the address we returned, the descriptors have to be a
real array at a fixed stride in guest-readable memory, and the routine is index
arithmetic on the incoming address. Put that array — along with the block the
system-identifier routine returns and the environment block — in the firmware-owned
region behind the boot-flash window rather than in node memory. Then nothing in the
low-memory map has to reserve space for them and the guest cannot allocate over them.
Putting them where compatibility space reaches them directly also sidesteps a hazard the
real machine has: on real hardware the firmware's own data is only reachable while the
firmware's page mappings are still installed, which is why a guest that wants to remember
something from them copies it out early.

**The type enumeration is SGI's, not the ARC specification's, and they differ.** Ours:

| Value | Type |
|---|---|
| 0 | exception block |
| 1 | firmware parameter block page |
| 2 | free contiguous |
| 3 | free |
| 4 | bad |
| 5 | loaded program |
| 6 | firmware temporary |
| 7 | firmware permanent |

The ARC specification puts free-contiguous last and shifts everything between up by one.
Guests carry both orderings and select by platform, so emitting the wrong one relabels
free memory as bad memory and vice versa.

**The ordering rules that decide which machine the guest thinks it is on.** OpenBSD
distinguishes this machine from its successor by the base page of the **first
free-memory descriptor**: below page `0x20` means this machine, at or above means the
successor, and it will then confidently describe itself as an Origin 300 and configure
memory wrongly. First free memory on this machine is at offset `0x19000`, which is page
`0x19` — comfortably below the threshold, but only if the descriptor list actually
reports it and reports it first among free entries.

Second, the guest **stops walking at the first firmware-permanent descriptor**, and
before stopping rewrites that descriptor's page count so the entry spans to the 32 MB
mark. It ignores everything after it. It also does not manage the first 32 MB at all
beyond what these descriptors say; all memory above 32 MB comes from the memory-bank
components in the configuration graph. So the descriptor list is a description of the
first 32 MB and nothing else.

**What real firmware actually emits**, captured from an Origin 200 by a kernel
instrumented to print each descriptor as it read it (the capture is catalogued with the
real-machine boot logs (citation: `docs/library/boot-logs.md`)):

**Check the machine before comparing this table against the source page, because that page
carries two descriptor lists and the other one is a different machine.** The same author
posted an Octane dump a week earlier, in the same format, with the same field names — and
it appears **first**, so anyone arriving to verify this table meets the wrong list before
the right one. The two are told apart by the kernel the capture booted and by its entry
address: ours is the Origin 200 list, from the later post, booting the IP27 kernel with an
entry address whose page bits are low. The Octane's entry address sets a bit high in the
same field, and it boots an Octane kernel.

**The mis-comparison has a specific and misleading outcome, which is why this warning is
here rather than in a footnote.** The Octane list has a descriptor covering page `0x2`.
Reconciled against ours, that makes the undescribed range below look like an omission in
our table or in the capture — and it is neither. **The gap is a real property of the
Origin 200 list**, and someone "fixing" it would be correcting a correct table to match a
different machine.

| Type | Base page | Page count |
|---|---|---|
| exception block | `0x0` | `0x1` |
| parameter block page | `0x1` | `0x1` |
| free | `0x19` | `0x7` |
| loaded program | `0x20` | `0x328` |
| free | `0x348` | `0xfb8` |
| firmware temporary | `0x1300` | `0x100` |
| free | `0x1400` | `0x100` |
| firmware temporary | `0x1500` | `0x300` |
| firmware temporary | `0x1800` | `0x200` |
| **firmware permanent** | `0x1a00` | `0x600` |

Two things about that list are worth copying rather than improving on. **Pages `0x2`
through `0x18` are described by nothing at all** — the real list jumps from the
parameter-block page straight to first free memory, leaving the range holding the
directory, the vectors and the configuration graph undescribed. And the
**firmware-permanent entry starts at page `0x1a00` and runs `0x600` pages**, ending exactly
at page `0x2000`, the 32 MB mark, so the guest's rewrite of that count changes nothing.

A list for our machine, following the real one's shape:

| Type | Base page | Page count | Covers |
|---|---|---|---|
| exception block | `0x0` | `0x1` | the exception handlers |
| parameter block page | `0x1` | `0x1` | the firmware parameter block |
| **free** | `0x19` | to the loaded kernel | first free memory |
| loaded program | where we loaded the kernel | its size in pages | the directly-loaded kernel image |
| **free** | after the kernel | to page `0x1a00` | the rest of the first 26 MB |
| **firmware permanent** | `0x1a00` | `0x600` | the top of the first 32 MB, where the real I/O-board firmware lives |

The free entry's base page is the load-bearing number. The permanent entry's count will
be rewritten by the guest to reach 32 MB whatever we put there, so it costs nothing to
make it correct — but its base page must not be above the 32 MB mark, or the rewrite
computes a negative count in unsigned arithmetic and the reserved-memory figure in the
banner becomes nonsense. And the free range has to contain the address the kernel is
loaded at: the OpenBSD kernel we are aiming at links at physical `0x40000`, comfortably
inside the range above, and the guest carves its own image back out of the free list
afterwards.

Omit the firmware-permanent entry entirely and the guest walks the whole list and claims
everything in it, including the regions we told it were ours.

## Environment variables

The routine takes a name and returns a pointer to a null-terminated value, or null if
there is no such variable. Store them as a block in guest memory the way QEMU's existing
machines build environment blocks — an index of pointers plus inline strings — and serve
lookups from the machine's properties so the environment and the machine cannot disagree.

The standard set is the console input and output devices, the loader, the load partition,
the load filename, the load options and the system partition. The additions this machine
family uses are the console baud rate, a graphics-present flag, a keyboard-present flag,
the built-in ethernet address, a diskless flag and a verbosity flag. For the banner, the
console output device, the graphics and keyboard flags, the ethernet address, the baud
rate, the load options and the load partition are the ones read; every one of them
tolerates a null answer, so an incomplete environment degrades gracefully rather than
failing.

## Hardware the firmware must leave programmed

The structures above are what the firmware *writes into memory*. It also has to leave
some hardware in a state the guest expects, and a guest finding that undone fails in a
way that looks nothing like a firmware-structure problem.

**Program big-window translation entry six to name widget zero, before the guest runs —
knowing that on this machine the write changes nothing.** The Hub's small window zero is
unreliable through a defect on the processor-interface side, so real firmware redirects
widget zero onto the last big window and points software at that instead. Neither guest we
boot programs the entry: OpenBSD skips it by index in the routine that hands out the other
big windows, and reads through it as though it were small window zero; Linux's copy of the
write sits behind a conditional that is never defined. IRIX does program it, in live code
for this machine family, which is where the value this section specifies comes from. The
entry is at Hub offset `0x40_0160` plus eight times six; its layout is specified in
[the Hub I/O interface datasheet](hub-io-interface.md).

**This section previously predicted a failure mode that cannot occur, and the correction
matters more than the original claim.** It said that skipping the write leaves a guest
reading through big window six and reaching nothing — the crossbar's identification
register being among the first things a guest looks for — so the failure would present as
an unresponsive crossbar and be debugged in the wrong place.

**There is no such failure.** The entry resets to zero, and zero decodes to widget zero,
slice zero, memory space, which is field for field what the redirection writes. The write
puts back what is already there. That follows from the register's reset value and the field
layout rather than from anything about our model, so it holds on the real machine too. A
machine whose firmware never performed this write is indistinguishable, in this register
and in what a guest reads through the window, from one that did.

**Keep the write anyway, and keep it labelled.** Real firmware performs it, and the
coincidence belongs to this machine's reset values rather than to the mechanism — a
machine whose I/O bridge sat behind a different widget would need the write for real. But
nobody should read a successful boot as evidence that it happened, and no test on this
entry can tell the two states apart.

**The wrong version was worse than an absent one**, which is why it is recorded rather than
quietly replaced: it named a specific symptom, in a specific register, and pointed at where
the debugging would go wrong. Someone chasing an unresponsive crossbar would have found
this paragraph, recognised their symptom, and gone looking for a firmware step that was
never missing.

**This requirement was asserted in the Hub I/O interface datasheet and was missing here,
which is the reason it is worth stating twice.** That document says our synthesized
firmware performs the write; this document specifies what the firmware does and never
mentioned it, and nothing implements it. A claim about the firmware belongs in the
firmware's own specification — anywhere else it is a description of something nobody
has agreed to build.

## The PROM cache-as-RAM phase and its boot-status bit

**Status: proposed, not yet reviewed — added 2026-08-27 to close a standing cleanroom
gap; reconciled 2026-08-30; amended 2026-08-31.** The private-window filter in `hw/mips/sn0.c`
(`sn0_promdata_is_private`) is in the model today with no datasheet behind it; this section is
the specification it should have rested on, and it needs a reviewer and James before anything
leans on it further. **The 2026-08-30 pass added the exit contract and corrected a wrong
framing:** the earlier text held that the phase bit *persists set* across kernel handoff, and
built a level-vs-edge re-arm discriminator on that. Reading the exit operands showed the PROM
**clears the bit itself**, before handoff, at the later of the boot's two uncached switches;
the bit is therefore clear during loaded-kernel operation, the level test is sound on the
normal path, and the correction below supersedes the persist framing throughout. What the bit
does *not* give is a tight residency bound — it stays set past the point the window stops being
the live stack — so the filter still needs the execution-range test, for the reason the exit
subsection gives. **The 2026-08-31 amendment corrected that execution-range test:** it had
listed the flash and the RAM copy as separate *virtual* aliases, but the PROM runs the phase
from one fixed mapped virtual address whose TLB backing it re-points by ASID, so the test must
translate the PC through the MMU and match the resulting *physical* against the flash/copy
ranges — surfaced when the promdata implementation found the model wedging at a mapped-flash PC
the alias list missed. A further 2026-08-31 pass restated that test as an **arming latch**
rather than an instantaneous condition: the window is resident cache lines, and since neither
clearing the bit nor moving the PC is a cache operation, the phase must arm on entry and hold
until reset or OS handoff — clearing the bit or moving the PC cannot end it.

Before it initialises DRAM, the PROM has no main memory and uses the R10000 primary cache as
its working RAM — the standard firmware technique of **cache-as-RAM**. It holds a small
private data window — its C stack and per-processor local data. The **PROM** places this
window at physical `0x10_0000` and sizes it to exactly the R10000 primary data cache, 32 KiB,
so it fits wholly in that cache; our model mirrors those as `SN0_PROMDATA_PADDR` and
`SN0_PROMDATA_SIZE`. The window is pinned in the cache so it is not evicted to the memory that
does not yet exist. (The R10000
mechanism that pins it is the **dirty-exclusive** cache state — that is the one place the ISA
term belongs; the behaviour is cache-as-RAM.) The PROM records that it is in the cache-as-RAM
phase in a **boot-status bit**: it repurposes floating-point register 0 as a boot-status
register and sets bit 1 to mark the phase. Our model's filter keys on exactly that — the bit
set **and** the program counter in the PROM.

### The boot-status word

The cache-as-RAM flag is bit 1 of a wider **boot-status word** the PROM keeps in
floating-point register 0. The model defines only that one bit
(`SN0_PROM_BOOT_STATUS_CACHE_AS_RAM`); the rest are unmapped on our side and are
recorded here so a future bit the model needs lands under the same
`SN0_PROM_BOOT_STATUS_*` name with a known meaning rather than as another lone
constant. Every bit is PROM-internal — meaningful only while PROM code runs, gone once
an operating system takes over — and neither Linux nor OpenBSD references the word, so
there is no public bit map; this is read from the IRIX 6.5.7 PROM and stated in our own
words.

| Bit | Meaning |
|---|---|
| 0 | Which processor of the node's pair this is. |
| 1 | **Cache-as-RAM phase active** — the only bit the model uses. |
| 2 | Uncached-memory mode: the POD uncached memory mode, the counterpart to bit 1's cache mode. |
| 3 | Halt into POD in the cache-as-RAM memory mode. |
| 4 | Halt into POD in the cached memory mode. |
| 5 | NVRAM contents are trustworthy. |
| 6 | Declared for "system controller passed arbitration", but the firmware never sets or reads it — dead. The election is recorded as the master index in the PROM configuration, not in this bit. |
| 7 | Running against the system-controller console UART (the manufacturing / console-controller path). |
| 8 | Skip diagnostics. |
| 9 | This processor has already served as master — do not re-elect it. |
| 10 | Memory tests were requested. |
| 11 | The KLCONFIG configuration block has been initialised. |
| 12 | Unused. |
| 13 | Also use the global-data-area symbol table. |
| 14 | Display the kernel's non-maskable-interrupt registers. |
| 15 | Unused. |
| 16 | Verbose console output. |
| 17 | DIP-switch settings are available. |
| 18 | Kernel-debug entry requested. |
| 19 | This reset was a power-on reset. |
| 20 | Halt into local (this-node) POD mode. |
| 21 | Halt into global (all-node) POD mode. |
| 22 | Every node has premium memory modules. |
| 23 | The debug switches are non-zero. |
| 24 | The machine uses the coarse memory-region size. |
| 25 | The machine is partitioned. |
| 26 | The node interconnect is fully configured. |
| 48–63 | The sixteen virtual-and-physical DIP-switch settings. |

Bits 27–47 are undefined. The ones most likely to matter to the model next are bit 0,
which distinguishes the two processors of a node, and bit 2, which with bit 1 selects
which of the POD memory modes — cache-as-RAM, uncached, or cached — the operator puts
the machine in.

**The load-bearing question is whether that bit is one-shot for the machine's lifetime, and
it is not.** It is set **once per PROM entry**, not once per boot:

- **Set.** On entering the PROM, after the cache diagnostics and after making the private
  window in cache, the PROM sets the boot-status bit, then transfers to its C entry
  point. This is the "before DRAM is available" phase the existing gate's commit names.
- **Cleared before handoff — the PROM clears it itself, on the normal path.** The bit does
  **not** linger set into the running kernel. The boot makes two switches out of cached
  execution: an **early** one that ends the cache-as-RAM phase proper — the stack leaves the
  cache window for real memory — and a **later** one, after global-master arbitration, that
  precedes the Node-ID change. The early switch leaves the bit **set**; the PROM carries it set
  through low-memory setup, topology discovery and arbitration, and **clears it explicitly at
  the later switch** (a clear-of-bit-1 on the boot-status word), well before it hands control
  to the loaded program. So during loaded-kernel operation, and during an ordinary OS-to-PROM
  callback, the bit reads **clear**. (The earlier statement here — that the bit persists set
  until the kernel happens to overwrite the floating-point register — was inference from not
  having found the clear; the clear is explicit and runs on every normal boot.)
- **But the bit is set for longer than the window is the live stack — this is the one that
  matters for the filter.** The bit is set from entry until that later switch, whereas the
  window stops being the cache-resident stack at the **early** switch: everything between the
  two runs from real cached RAM with the window already vacated. So the bit **over-covers** the
  true residency and cannot, by itself, mark "the window is the active private stack." The
  program counter — equivalently, the stack pointer still pointing into the window — is what
  bounds the real residency. And the PROM's mapping is what makes the PC test subtle: for most
  of the phase the PROM executes from a **fixed mapped virtual address** (`0xc00000001fc00000`)
  whose wired TLB entry is re-pointed — by an **ASID switch** — from the flash to the RAM copy
  (uncached, then cached). The "run from RAM" transitions are TLB re-points, *not* changes of
  the program counter, so a filter that matches the PC against a list of *virtual* aliases
  misses the mapped execution and flips a window access to *forwarded* although the phase never
  changed. The classifier that holds still is an **arming latch**, not an instantaneous test:
  **bit set AND the PC (translated through the MMU) resolving physically into the flash or the
  PROM's RAM copy** (equivalently, SP within the window) **arms a per-processor phase**, and that
  phase then **holds until reset or the operating system runs**. The reason is hardware-truth:
  the window is resident cache lines, and neither clearing the bit nor moving the PC is a cache
  operation, so no later bit-clear or PC move may re-route a window access — the lines stay until
  an actual cache invalidate. The window *is* read again between that invalidate and the OS handoff — on PROM 6.156 a
node's first processor makes 28,673 doubleword accesses to it after its stack switch, a
memory-test pass, measured through the model's filter on the eight-processor Origin 2000
(2026-09-14) — but those are ordinary memory accesses to a window whose lines the exit has
discarded, and the held latch serves them from the private copy rather than RAM. **Inferred,
not observed:** nothing the PROM does afterwards depends on what that pass wrote. The boots
this was measured on all reached IRIX; nothing traces what the memory test's own result feeds,
so the claim rests on IRIX booting rather than on a check of that pass's consumers. Holding
the latch until reset or OS handoff remains safe on that basis, and the bit-clear no longer
has to make the filter inert on its own; the latch's release at OS handoff does that.
- **Re-armed only through the PROM's entry sequence.** The
  pin-the-window-then-set-the-bit sequence runs again whenever the machine
  re-enters the PROM to run cache-as-RAM — when the OS **reboots or returns to the PROM
  monitor** (the boot-exception-vector restart entry), and when a **PROM-context exception
  drops to the POD debugger** in its cache-as-RAM memory mode (the panic handler, taken when the
  faulting instruction is inside the PROM). A **kernel-context** exception instead drops to an
  **uncached** POD mode and does **not** set the bit, and a normal firmware call the OS makes
  during running does not run the entry sequence, so it does not re-arm the bit. Crucially,
  the reboot and monitor-return re-entry begins with the register-initialisation step that
  **clears the boot-status register to zero** (as it zeroes the processor's registers),
  *before* the bit is set again — so that re-arm always presents a genuine clear-then-set
  transition, whatever the bit held on entry.

**Level and edge — which applies where.** On the normal boot-and-install path the **level**
test — "bit 1 set and the PC in the cache-as-RAM execution range" — is sound, because the bit
is **clear** during loaded-kernel operation: an ordinary OS-to-PROM callback runs with the bit
clear, so the level test does not fire on it. (The earlier account had a level detector firing
on callbacks with a stale set bit; that followed from the persist claim above and does not hold
once the explicit clear is accounted for.) The **edge** — the bit's clear-to-set transition —
is what a detector needs only for the **re-entry** paths, where the phase genuinely re-arms: a
reboot or monitor return re-runs the entry sequence, and that sequence begins by forcing the
boot-status register to zero before setting the bit, so a real re-arm always presents a
clear-to-set transition that no firmware callback produces. A model that must
stay correct across reboot and monitor return keys the overlay's re-instatement on that
transition; on the normal path, the level test plus the residency PC-range above is enough.

**One re-entry path is weaker, and it is a terminal one.** The panic-to-POD path (a
PROM-context crash) re-enters cache-as-RAM mode **without** first running the
register-clearing step — it sets the bit onto whatever was there — so if the bit was already
set it presents no clear-to-set edge. This matters little in practice: that path is the POD
debugger reached on a crash, a halting state the running system does not return from, so the
private-window semantics there are not correctness-critical for a booting or installing
guest. A detector that wants to cover it as well cannot rely on the edge and must key on the
private window being re-initialised at POD entry, or on the POD entry itself.

**Consequence for a model that removes the private-window overlay.** Removing the overlay
one way is not sufficient by itself: after the cache-as-RAM phase ends, an OS reboot or a
return to the monitor re-enters the phase and re-uses the private window, so the overlay — or
its per-access hook — must be re-instated on the **clear-to-set edge** above, not on the bare
level. During normal loaded-kernel operation, such as a miniroot install, the phase does
**not** re-arm — the kernel does not re-enter the cache-as-RAM sequence — so the overlay is
inert then and removing it is safe for that window. The re-arm handling is for correctness
across reboot and monitor return, with the crash-to-POD path the weaker, terminal case noted
above.

### The exit from cache-as-RAM, and what anchors the private-window filter

**Added 2026-08-30, proposed, not yet reviewed.** How the phase ends, for the multi-node
promdata work. The exit is the PROM's own explicit copy — the model performs no data transfer
of its own at the boundary; what it must do is hold its private/forwarded classification
**stable** for the whole span a processor's stack is cache-resident. As the bit-lifecycle
discussion above establishes, that span is bounded by the phase bit paired with the
execution range, not by the bit alone — the bit stays set past the residency.

**The exit relocates to a fresh per-processor real stack; it does not write the window back in
place.** When main memory is up, the PROM leaves the phase in a fixed, progress-instrumented
order. Its *code* stays at the fixed mapped virtual address throughout; what moves is that
entry's TLB backing, re-pointed by ASID from the flash to the **uncached RAM copy** of the PROM
text and later to the **cached** copy — so "run from RAM" is a TLB re-point, not a jump. Its
*data* is carried out explicitly: it copies the crucial persistent structures from the cached
window into **uncached real memory** (deliberately uncached, so they still work if the machine
later re-enters cache-as-RAM for POD), and moves the stack to a **per-processor real-memory
stack** at a *different* region — node-local physical `0x1be_0000` plus the processor number
shifted by 16, a 64 KiB slot per processor, not the `0x10_0000` window — then invalidates the
caches, with the stack now on the cached per-processor slot. The system-controller scratch area
moves the same way, to its own per-processor real slot. So the cache-resident data is carried
out to per-processor real memory *before* the cache is discarded — nothing is abandoned or
written-back-in-place, and the model needs no exit-time transfer of its own.

**Both processors exit together; a parked slave is never cache-resident.** The two processors
of a node run the whole phase in lockstep on hub barriers and leave it together — the firmware
runs the jump-to-RAM unconditionally for both, "both CPUs use the same copy of the text," each
landing on its own per-processor slot. This exit runs **early** — before the permanent
low-memory structures, before CrayLink topology discovery, and before global-master
arbitration. (A *second*, separate switch to uncached memory with a full cache flush happens
much later, only to change the Node ID after global arbitration — do not conflate the two.)
Because the exit precedes arbitration, every processor has left cache-as-RAM before any slave
reaches the launch-block park the multi-node section describes: a parked slave's stack is in
per-processor real memory, never in the cache. The stack's cache-residency is bounded by this
early exit — the stack leaves the window here — even though the phase bit is not cleared until
the later Node-ID switch (see the bit-lifecycle above). Parking happens after the exit, so a
parked slave is outside the residency and is not a third state for the filter.

**The window is one region split by processor.** Both processors of a node use the **same**
32 KiB window at physical `0x10_0000`, split by half: processor 0 (slice 0) takes the upper
half, processor 1 the lower half — its stack pointer simply starts half a stack lower. The
selector is boot-status bit 0 (which processor of the pair). Each processor holds **its** half
in **its own** primary data cache, so there is no cross-processor aliasing even though the two
halves are adjacent addresses in one window: the accessing processor's number, not the
address, says which half is whose.

**The mode bit is a floating-point-register bit, not a CP0 bit.** Boot-status bit 1 is bit 1
of floating-point register 0, written with a doubleword move to coprocessor 1 (the FPU) and
read the same way; the PROM repurposes `$f0` as its boot-status register, so this is an
FPU-register access, **not** a CP0 access, and a model reads it from the FP register file it
already stores — no CP0 storage to add. How a private-window filter combines it with the
execution-range test — and why the bit must be paired with that test rather than used alone —
is the bit-lifecycle discussion above: the bit over-covers the true residency, so the stable
classifier **arms a per-processor phase** on **bit 1 set AND the PC (translated through the MMU)
resolving physically into the flash or the PROM's RAM copy** (equivalently, SP within the
window), and then holds it until reset or OS handoff — the arming-latch reasoning is in that
discussion above. Matching the *physical* rather than a virtual-alias list is what makes the
arming condition robust: the PROM runs the phase from a fixed
mapped virtual address (`0xc00000001fc00000`) whose wired TLB entry it re-points across the
flash and the RAM copy by an ASID switch, so any such mapping resolves correctly, and doing the
translation at full 64 bits means a truncated PC cannot misplace it. Bit 0 of the same word
selects which processor of the pair, and so which half of the window an access belongs to.

**Provenance (IRIX-derived, read to learn and written here in our own words).** The
lifecycle is read from the IRIX 6.5.7 IP27 PROM sources — the boot entry and
exception-vector paths, the POD/cache-as-RAM re-entry routine and the three
execution-mode routines that enter and leave the phase, the phase-boundary sequence in the
boot mainline, the boot-status-register bit positions from the PROM's own header, and the
private-window and per-processor real-stack addresses from the public SN0 address map.
Cache-as-RAM is standard firmware practice, and the R10000
dirty-exclusive cache state that implements it here is a public architectural term; the
boot-status-register convention and the re-entry paths are PROM behaviour, stated
behaviourally here and not transcribed. Cite the release, not only the file — PROM
revisions differ, and this is 6.5.7.

**Model record — the resident window served as RAM (added 2026-09-14, proposed, not yet
reviewed; fork branch `promdata-window-trunk-20260914`).** The filter as first built was an I/O
region for the whole phase, so every stack access the PROM makes while its stack is
cache-resident was an MMIO dispatch under QEMU's big lock: 1.92 to 1.96 million per node's
first processor and 95 to 131 thousand per second processor between the latch and the stack
switch, counted on the eight-processor Origin 2000 with PROM 6.156. The model now keeps the
filter's three decisions but takes the common case off the lock:

- **Cached access while resident:** at the latch the processor's private 32 KiB block is
  mapped as RAM above the filter in its cached view, so a translated load or store resolves to
  a host pointer. Same storage as the filter uses, so nothing the phase stored is lost when the
  filter takes over again.
- **Uncached access to the same address:** the processor has a second address space —
  the cached view with the filter alone at the window, selected by the reference's uncached
  attribute — so it still forwards to node RAM whatever sits above the filter in the cached
  view. On 6.156 no access of any kind reaches the filter's forward path during the PROM run
  (none before the latch, none uncached, none after the exit), so this view is kept for the
  contract, not for anything the PROM does.
- **Release at OS handoff:** tested on each cached filter access and, since 2026-09-26, by
  the node's 10 ms poll as well, on every node; see
  [releasing the phase at OS handoff](#releasing-the-phase-at-os-handoff) below. The mapped
  block must still be gone before the OS's first window access. The model unmaps it at the
  exit's stack switch, recognised as **the stack pointer leaving the window**, sampled every
  10 ms of guest time on the processor's own thread. On every measured boot (four boots, all
  eight processors) the stack pointer left for the region this document names as the real
  per-processor stack, `0x1be_0000` and the 64 KiB slots above it; that confirms the 6.5.7
  reading for 6.156, down to the exact slot. At the stack switch the stack pointers
  `0xc00000001fdefe80` and `0xc00000001fdffe80` translate through TLB index 61 (`lo0 0x68016`,
  `lo1 0x6c016`, `pagemask 0x1fe000`, so 1 MiB pages to `0x1a00000` and `0x1b00000`) to
  physical `0x1befe80` and `0x1bffe80` — slot 0 for slice 0 and slot 1 for slice 1.

**Approximations this carries, and their limits.**

- *The exit is recognised late by up to one sample.* A stack that has left the window stays
  mapped for at most one poll period more — but the period is measured on `QEMU_CLOCK_VIRTUAL`,
  which under translation without instruction counting advances with host wall-clock time, so
  a main loop starved of host time stretches the real bound to one period plus host timer
  latency, not a fixed 10 ms. The boots this was measured on ran at host loads of 65 to 122 on
  a 32-processor host. A cached access in that span still reaches the same block the filter
  would serve, so only the two items below can observe it. A sample that found the stack
  outside the window early would only restore the filter sooner — today's slower path. A PROM
  that never moved its stack out of the window before handing off would leave the block mapped
  into the OS for that same stretched bound; 6.156 moves it about 40 s before.
- *A cached instruction fetch from the window while the block is mapped runs the block and
  does not end the phase.* The filter's execution release only sees fetches that reach it: the
  uncached alias always does, the cached one only once the filter is back. The PROM never
  fetches from the window, and the OS — which does, on the second processor at handoff on
  IRIX 6.5.30 — runs long after the switch. The branch test pins this as a stated limitation
  (`jr-kseg0-cache-as-ram-resident`: the block's code runs, the RAM snippet's does not) and
  pins today's behaviour with the fast path off.

**What would settle the open parts.** An instruction-fetch trace of the window across the
phase on each PROM revision the project boots would show whether any firmware fetches from
the window while resident; a review of this exit section, which none of the above changes the
status of.
### The same window on IP35 (Origin 300)

**Independently reviewed 2026-09-14 for the 32 KiB window at physical
`0x10_0000`, FPR 0 bit 1, the fixed stack address and the resulting per-processor
private-storage model policy.** A bounded reading of the stock IP35 PROM 6.210
listing reproduces those facts. The live corruption and boot-progression narrative
below has no retained trace and was not independently reproduced. The stock
IP35 PROM keeps the same boot-status word in FPR 0 and the same bit 1 for the cache-as-RAM
phase, set during client-init (observed at PROM offset `0x1144` relative to the reset
entry) and cleared at the phase's exit points (observed at offsets `0x523c` and `0x5360`).
Both Origin 300 processors run their cold-boot client-init stack and per-processor
client-state block at the **same** physical address as IP27's window, `0x10_0000`,
32 KiB — but unlike IP27's bit-0-selected per-processor half, IP35 does not split the
window: both processors' state lives at the identical address, so the model needs one
private 32 KiB buffer per processor rather than one shared 32 KiB window split in half.
The original investigation recorded that, with the window modelled as ordinary shared RAM,
the two processors' concurrent client-init stacks and per-processor state blocks
overwrote each other, corrupting a return address into a firmware string table and
producing a Reserved-Instruction ("General Exception") crash a few instructions after
the corruption; modelling a per-processor private window (`hw/mips/sn1.c`,
`SN1PromData`/`sn1_promdata_*`, mirroring `hw/mips/sn0.c`'s `SN0PromDataState`) measurably
changed the boot's behaviour — it now runs past that crash site into later boot stages
(local-IO discovery, NUMAlink discovery, global-master election, BASEIO PROM load)
before hitting a later, unrelated fault. Those trace-specific details are context,
not independently cleared contract evidence.

**Simplified from the IP27 model: arm/release on the boot-status bit alone, no PC-in-PROM
check.** `sn0_promdata_is_private()` also confirms the PC is inside the flash or the PROM's
RAM copy, because on IP27 the bit alone over-covers the phase — it stays set well after the
stack has left the window. Whether the IP35 PROM has the same over-coverage is not
established; the port omits the PC check as a deliberate simplification, on the reasoning
that over-covering (shadowing the window a little longer than the phase strictly needs)
cannot itself cause the two processors' cache-as-RAM state to alias, which is the only
failure this window exists to prevent. This should be revisited if IP35 turns out to reuse
the window's physical address for something else while the bit is still set.

**Not yet resolved: the window fixes the corruption, but not the whole boot.** After this
fix, a 2-CPU boot still reports `peer_pled failed: Other CPU on this node died.` early in
the boot walk, before the later fault above. Live tracing (qtest breakpoints, and register
reads against a running boot) found the secondary spinning indefinitely, well past the
window's own phase, in a **separate** mutual-exclusion wait unrelated to this window: a
Bedrock I/O-interface "scratch 0" register (`0x40_0150`, `docs/datasheets/
bedrock-io-interface.md`, currently modelled as a plain 64-bit scratch cell) where each
processor sets a per-processor busy bit and a firmware helper (reached from many call
sites) clears it; the master sets its own bit during client-init and, on the path a live
trace observed, never reaches a call site that clears it before the secondary's own
client-init blocks on it. That the datasheet's own confidence marker for this register is
`unknown` — never verified against real hardware or a public driver — leaves open whether
real Bedrock hardware gives it additional semantics (per-processor banking, a hardware
doorbell) that this contract does not yet capture. This is being chased separately from
the cache-as-RAM window described above.

### The same cache-as-RAM window on Bedrock/IP35

**Reviewed scope:** The retained stock IP35 PROM 6.210 listing independently
establishes a 32 KiB early data window at physical `0x0010_0000`, FPR 0 bit 1,
and a fixed stack address within that window. Both Origin 300 processors use
the same address before ordinary RAM is ready. The model must therefore give
each processor private storage for the full window. This is the same
cache-as-RAM mechanism as the SN0 processor's, implemented once in
`hw/mips/sgi/sn-promdata.c`; the processor and window contract do not change
with the board topology. The bounded independent evidence review (citation: `docs/reviews/2026-09-14-origin300-cache-window-port-review.md`)
records the exact stock-image listing revision and instructions. Private
emulator storage is a model policy justified by overlapping early stacks,
not a measurement of every real cache access or coherence transition.

**Origin 300 execution-home placement:** The same listing rewrites the fixed
mapped firmware virtual address's wired TLB pair to physical
`0x01a0_0000` and `0x01b0_0000`, spanning 2 MiB. On its first exit path
that rewrite precedes both the virtual stack move and the later FPR 0 bit
clear. The flash-backed mapping of that virtual address spans
`0x1fc0_0000`–`0x1fdf_ffff`. The [Origin 300 address map](ip35-address-map.md)
also records a PROM copy starting at `0x01a0_0000`. These locations differ
from SN0 firmware's execution homes, but they are placement parameters to
the one shared cache-window implementation, not a second processor mode.
The listing does not prove every boot takes that path or that all 2 MiB
contain a complete copy of the firmware image.

**Phase and release remain approximated:** The listing shows bit 1 stays set
after the two observed virtual stack moves, so bit-only service can outlive
the direct early stack. The shared model arms on the bit while the translated
PC is in the flash or configured RAM execution home, holds private storage
while armed, and releases it at a later stack/firmware handoff. The static
listing alone does not establish every reachable bit transition, the moved
stack's physical backing, every access to the early window after the move,
or a safe physical release event. The PC-plus-bit latch and release timing
are therefore model approximations, not reviewed IP35 hardware facts; the
review above explicitly withholds their clearance. A two-processor stock-PROM
trace bound to its payload digest and QEMU revision must record each CPU's
FPR 0 bit, stack pointer, virtual PC, translated physical fetch address and
post-move accesses to `0x0010_0000`–`0x0010_7fff` to distinguish this policy
from bit-only service. A synthetic qtest must separately demonstrate that
the RAM execution home keeps private writes out of shared DRAM after the bit
clear. Until that evidence and independent review settle the release, no
claim of a hardware-accurate exit or completed OS boot follows from this
window model.

### Releasing the phase at OS handoff

**Added 2026-09-26 with QEMU pull request 873; proposed, needs independent evidence review.**
This describes a model approximation, not a hardware fact. It applies to both node
generations, because they share one implementation in `hw/mips/sgi/sn-promdata.c`.

**What the phase holds, and for how long.** While a processor is latched into the
cache-as-RAM phase, the model keeps two physical ranges local to that processor's own node,
whatever node number the address names:

- the 32 KiB data window at `0x0010_0000`, through the `promdata-filter` region;
- the PROM's RAM-copy range, `0x01a0_0000`–`0x01bf_ffff` on the Origin 300, through the
  `prom-copy-local` region. The SN0 node (Origin 200 and Origin 2000) uses the same base,
  sized to its 1 MiB flash.

The phase ends, and both regions are disabled, when the processor has cleared FPR 0 bit 1
and its PC, translated through the MMU, is outside both the flash and the RAM-copy range.
That is the handoff test the earlier subsections describe; this amendment does not change
it, only when the model applies it.

**Why the RAM-copy range is kept local.** *Inferred.* The PROM runs from its own copy in
RAM, and on the Origin 300 its wired mapping points at `0x01a0_0000` and `0x01b0_0000`
([the Bedrock/IP35 subsection above](#the-same-cache-as-ram-window-on-bedrockip35)). Each
node's PROM reaches its copy as a low address with node field zero, and that means the
reader's own memory only until the node takes a number other than 0
([the node-ID change](#the-node-id-change-and-the-low-memory-re-base)). The routing lets a
processor still in the phase reach its own copy even after its node's claim on node field
zero has moved on. No capture shows the PROM touching the range through a bare address after
renumbering, so whether the PROM needs the routing at all is unknown. The model keeps it
because removing it has not been tested against a boot.

**When the model applies the handoff test.** *Established for the model* (QEMU
`sgi-origami`, from the commit that lands pull request 873):

- on each cached access by that processor that reaches the filter, as before. An uncached
  access forwards without applying the test, and while the fast path maps the block, cached
  accesses do not reach the filter. The access that triggers the release is itself still
  forwarded to the processor's own node;
- by the node's poll, every 10 ms of guest time, on the processor's own thread, for every
  processor still in the phase, except one the fast path still maps after the straggler
  bound below has run out. Every node polls, whether or not `promdata-fast` is set. Before
  this change only a node with `promdata-fast` set had the poll, and `sgi-sn1-node` defaults
  it off.

Without the poll, a processor that never touched the data window after handoff kept the
phase, and both local routings, for the rest of its run.

**Approximations and their limits.**

- *Release lags handoff.* The poll releases a processor up to one poll period after it
  hands off. The period is `QEMU_CLOCK_VIRTUAL` time, which follows host time when the
  guest runs without instruction counting, so a loaded host stretches it further. Until
  then, that processor's accesses to either range with node field zero still reach its own
  node's memory, so a store meant for node 0's memory there lands in its own. For the boot
  processor, the handoff comes before the kernel. Every other processor stays in the PROM's
  slave loop, and so in the phase, until the kernel launches it
  ([the multi-node boot sequence](#the-multi-node-boot-sequence)); its release therefore
  lags its first kernel instructions. Whether IRIX touches either range through node field
  zero in that span is *unknown*; the qtest below checks only a store made after the lag.
- *Polling can release earlier than the per-access test did.* It can sample the moment a
  RAM-loaded program runs with the bit clear, before any window access. In the IP35 PROM
  6.210 disassembly (`tools/origami disasm show`), the two bit-1 clears, at reset-relative
  offsets `0x523c` and `0x5360` in `func_1fc0518c` and `func_1fc0525c`, both return into
  PROM code and are reached only from the execution-mode switch `func_1fc05608`. So once the
  bit is clear, the PC stays in the flash or the RAM copy until the PROM leaves its own code.
  *Reported by the first review of the pull request; not re-read for this amendment.*
- *A processor that never hands off can be sampled for good.* A processor that stays in the
  phase without its block mapped (a secondary parked in the PROM, or, on a node without the
  fast path, any processor the PROM disables) is sampled at 100 Hz of guest time for as long
  as it stays. On a node with the fast path, a processor that never reaches its stack switch
  stays mapped, and the existing straggler bound stops its sampling.
- *A processor that never latches keeps the window local.* The filter is enabled from reset
  and disabled only when the phase ends or the processor executes from the window, so an
  access to the window from a processor that never entered the phase still goes to its own
  node's memory. *From the model code.* No supported guest is known to hit this.

**Evidence.**

- *Observed on the model, before the change:* run `irix-20260926T072449-b9ed8b`, QEMU
  `b10c101480`, two Origin 300 chassis with 4 processors each, IRIX 6.5.30 online
  diagnostics. `pandora` reported 90 memory miscompares from four subtests, all on CPUs 4
  to 7. Every reported physical address, 20 on nasid 0 (for example `0x1b75e58` and
  `0x1a3ae58` from `memOPCAddrRandDgenBias1` on CPU 7) and 70 on nasid 1, lies at offset
  `0x1a0_0000`–`0x1bf_ffff` of its node. That fits node-1 processors whose node-field-zero
  accesses to that range reached their own node's memory: node 0's pages missing their
  stores and node 1's pages at the same offsets receiving them. *Inferred:* the console
  does not say which node each CPU number is on, and the run does not show which
  processors still had the phase latched.
- *Discriminating test:* `sn1/cross-node/prom-copy-range-leaves-at-handoff` in
  `tests/qtest/sn1-cross-node-test.c`. A node 1 processor enters the phase the way the PROM
  does, hands off to a RAM loop that touches the data window only with its uncached marker
  store, and after node 1 is renumbered stores with node field zero to the RAM-copy range and, uncached, to the data
  window. Both stores must reach node 0's memory. On the parent revision without the poll,
  `fb4d6d3e3b`, the test fails: node 0's copy of the window still reads zero.
- *Not yet observed:* a two-chassis `pandora` pass with the change.

**What would settle it.** A two-processor stock-PROM trace on each supported PROM revision
recording when each processor clears the bit and leaves the PROM's code, set against the
model's release time. That would bound the lag. A two-chassis online-diagnostics run with
the change would show whether `pandora` still fails.

## The multi-node boot sequence

**Status: proposed, not yet reviewed — added 2026-08-28.** Read to settle how the PROM
brings up a two-node machine, for the SMP-correctness and remote-boot work. Needs a
reviewer and James.

A two-node machine has four processors, two per node, and the PROM brings them up in a
definite order. The single-node cache-as-RAM phase described above is each processor's
starting point; this section is what surrounds it.

**Every processor runs the early phase independently and at the same time.** Each powered
processor, on each node, runs the PROM entry path — the cache-as-RAM phase included — over
its **own** node-local private window, concurrently, not one after another. There is no
master-first ordering for the early phase; ordering appears only afterwards, at arbitration.
So the per-processor private window the cache-as-RAM section describes must be **per node as
well as per processor**.

**Arbitration is two-tier: local master, then global master.** Within a node the two
processors rendezvous on a pair of progress registers and the lower-numbered processor
(slice 0) becomes the node's **local master**, the other a **local slave**. Across nodes, the
local masters then arbitrate for one **global master** — the highest-priority node — and that
node's local master is the global-master processor that drives the rest of the boot. A
processor that fails to check in is handled by a timeout: the survivor becomes a single
master and disables the missing one.

**Each node copies the PROM into its own memory, from its own flash.** Running out of flash
is very slow, so early on each node relocates the PROM text into fast RAM. This is a
**self-copy, not a master push**: each node copies the PROM text from **its own local
boot-flash window** into **its own node-local RAM**, at the same node-local offset on every
node (the PROM's RAM base within that node's memory). The master does not push code into the
other nodes' memory during boot.

**The remote-node flash window is I/O only — no processor fetches through it.** A per-node
window reaches *another* node's boot flash. It is used only to **read and write** a remote
node's flash — for the maintenance operation that replicates a good node's PROM image into the
other nodes' flash chips (the PROM code handles up to eight, a family-wide capability rather
than this two-node machine's topology), and as a defensive copy-source fallback — never as an
instruction source. No processor executes across the fabric through this window during
bring-up; every node fetches from its own local flash and then its own RAM copy. So the
remote-flash window can stay a plain I/O window and does **not** need a fast execution path.
(This settles the parked remote-boot question.)

**Slaves park in their own node's launch block until the kernel takes them.** After
arbitration, every processor that is not the global master — the local slaves, and the local
masters of the non-global-master nodes — enters a slave loop and spins polling its slot in the
**per-node launch block** (in that node's global data area, the launch blocks this datasheet
describes elsewhere), in its own node-local memory. When the kernel runs on the global master
it launches each slave by writing an entry point and a busy flag into that slave's launch
slot; the slave loop sees it and jumps into the kernel. Slaves wait in their **own** node's
memory, never the master's.

**Provenance (IRIX-derived, our own words).** Read from the IRIX 6.5.7 IP27 PROM boot,
arbitration, memory-copy and slave-launch paths and the launch-block layout — stated
behaviourally, in our own words, with no vendor file structure transcribed. Release:
IRIX 6.5.7.

## The cross-node barrier is advisory: it proceeds on timeout

**The load-bearing fact first: the multi-node barriers do not block. On timeout a barrier warns
and proceeds, returning success as if it had been met.** A model, and anyone testing against the
multi-node boot, must treat every barrier as a synchronization the firmware abandons under
load — because that abandonment is what turns a slow node into an out-of-sync one, and a
scheduler-starvation problem into a fault one phase later.

**The mechanism.** A barrier is a software mailbox in a Hub network-interface scratch register
(NI scratch at offset `0x600108`, the register the public Linux name identifies). At each barrier
call site a node posts a **per-call-site barrier id** into the register's bits 39:24, locally;
the other nodes **vector-read** that register across the fabric and **strict-compare** the id
against their own for the same call site. A node waits until its peers show the matching id — or
until it times out.

**On timeout it does not fail.** It emits a warning, rate-limited to ten prints across the whole
boot, and returns success, letting the caller enter the next phase. The warning's wording reads
like a PROM-revision mismatch, but that reading is hard-coded into the message — **no revision
field is read anywhere in the routine** — so it is editorial, not a diagnosis. A reader who sees
a revision-mismatch warning here must not take it as evidence of an actual revision check.

**Why it is the hazard.** Every phase boundary in the multi-node bring-up rests on these
barriers, and each proceeds on timeout. So a node that is merely slow — starved of emulated
cycles while a peer runs ahead — is not held at the barrier; it is released past it,
unsynchronized, and the divergence surfaces later as a fault in whatever the next phase assumed
was already done — the measured instance is a configuration record a peer had not yet written,
and the trap is general to any state the next phase takes as already in place. The barrier hides
the starvation and relocates its symptom one layer down. A test that asserts a
barrier blocks will not match the firmware; making the multi-node boot robust means supplying the
scheduling fairness the barrier declines to enforce, not tightening the barrier.

**Provenance (observed on the release we boot).** From disassembly of the barrier routine plus
live measurement on the 6.5.30 PROM (the routine at `0xbfc39124` in the running image); the
scratch-register name is the public Linux name for the Hub network interface. Stated
behaviourally. The 6.5.7 source carries the same multi-node barrier structure as the cross-check.

## The node-ID change and the low-memory re-base

**Status: proposed, not yet reviewed — added 2026-09-02.** How the PROM keeps addressing its own
low-memory working set (its executing code, its stacks, its config header, its device bases)
across the moment a node stops being node 0. Read for the multi-node bring-up and the Hub/NI
model. Needs a reviewer and James.

**The pre-assignment contract.** Out of reset every node answers as **nasid 0**: the node-id
field (`NSRI_NODEID`) of the Hub network-interface status register (`NI_STATUS_REV_ID`) reads 0
on every node. So a **bare, unqualified** low physical address — `IP27PROM_BASE`,
`IP27PROM_PCFG`, the two PROM stacks, `SLAVESTACK`, reached through the cached/uncached kseg
windows with **no nasid in the high bits** — lands in *the running node's own* memory on every
node, because that node *is* nasid 0. Every node runs the whole early phase (cache-as-RAM,
self-copy to RAM, arbitration) over these node-0 forms in parallel, each hitting its own memory.
It is position-independent by construction: the same address is correct on all nodes precisely
because none of them has a distinct identity yet.

**The change makes those addresses ambiguous.** When the local masters assign real nasids, each
node writes its **own** `NSRI_NODEID` to its assigned value. From that instant a bare nasid-0
address no longer means "this node" — it means node 0 specifically, which is remote for every
node except node 0. So everything the PROM keeps in low memory and still needs — code, stack,
config header, device bases — must move to **nasid-qualified** forms (`TO_NODE(nasid, …)`: the
address with `nasid << NASID_SHFT` in the high bits) that name the node's own copy explicitly.

**Two re-base mechanisms, applied in a fixed order around the change.**

*Code and stack — one wired-TLB entry, re-pointed.* The PROM does not fetch code or touch its
stack through bare physical addresses during this window. It runs at a **fixed kernel-virtual
address** covered by a **wired TLB entry** — a 2 MB mapping that spans the whole PROM RAM image
at once: code, the config header, and both PROM stacks. Re-basing the entire code-plus-stack
working set is therefore a **single act — rewrite that one wired entry's page frames** from the
bare/local physical base to the nasid-qualified base, and resume at the same virtual address.
Because the stack pointer is that fixed virtual address, it is **not** rewritten on its own;
only the physical page behind it changes. This is the wired-TLB page-frame rewrite measured on
the running image (the block at `0xbfc52ac8–0xbfc52b18`).

*Everything else — re-qualified pointer by pointer.* Data the PROM reaches by explicit address
rather than through the wired mapping — the config-header pointer, the system-controller and
console-UART register bases, the memory-directory region it must re-initialize — is
**recomputed** with the node's now-real nasid (`TO_NODE(nasid, …)`) right after the change. The
config header in particular is re-fetched as `TO_NODE(nasid, IP27PROM_PCFG)`; its address
"moves" only in that the node's own copy now sits at a different absolute address.

**The ordering, relative to the node-ID write.**

1. **Switch low-memory access to uncached and flush the caches — before touching the id.** The
   node re-points its fixed-VA mapping so low-memory access becomes **uncached**. The 6.5.7 source
   names this the UALIAS (the position-independent reader-local uncached window); the 6.5.30 image
   realizes it by **cache attribute, not address** — the wired entry keeps plain-physical page
   frames at the PROM RAM base (measured `EntryLo` PFNs `0x01a00000`/`0x01b00000`) and sets the
   **uncached** attribute over them. A TLB PFN field cannot hold an xkphys UALIAS address, so in a
   TLB context "the uncached alias" *means* the uncached-attribute variant over the bare physical
   PFNs; both realize the same routing — uncached access to low physical resolves to the reader's
   own node. The node then flushes and invalidates all caches, and from here to the end of the
   transition it runs **uncached**, so nothing depends on a cached line tagged with a
   soon-to-be-wrong address and nothing depends on the coherence fabric while the node's fabric
   identity is in flux.
2. **Change the node id, in a synchronized quiet window.** All nodes rendezvous on a barrier, the
   local master writes the new id into its `NSRI_NODEID`, and they rendezvous again. The write is
   issued as **three barrier-separated stores** — the first two carry the *old* value and the
   third the new one, trial passes that settle the path before the real change — each store fenced
   by a sync and a short delay. The firmware marks this a time-critical region (no console
   output), holds the slaves in a tight loop, and keeps the nodes closely synchronized
   specifically to keep inter-node message traffic out of the window while identities flip.
3. **Re-qualify the data pointers** (config header, device bases, memory directory), still
   uncached.
4. **Re-base the code/stack mapping and return to cached execution.** Install the nasid-qualified
   wired entry and resume — now fetching and stacking from the node's own physical copy, cached,
   with the node id part of every address.

**What protects the window — explicit sequencing, not a cache-residency gamble and not a timer.**
There is no hardware transition timer and no assumption that the working set "happens to be"
cache-resident across the change. The window is safe because (a) execution runs **uncached
through the bare/local alias**, which stays correct right up to the id write because nasid-0 still
means local until that write lands; (b) the id write sits inside a **barrier-bracketed,
traffic-free** interval; and (c) the wired-TLB rewrite that finishes the re-base runs while
executing **unmapped from the physical boot-flash image**, so no instruction is ever fetched
through the very entry being edited. A faithful model needs no bounded-time mechanism; it needs
the Hub/NI to (1) read a node-id field of 0 on every node out of reset, (2) let a node rewrite
its own `NSRI_NODEID` and thereafter route "local" accesses and answer discovery as the new
nasid, tolerating the three-write settle pattern, (3) decode `nasid << NASID_SHFT` so that both
the bare/local (nasid-0) form before the change and the qualified form after it land on the
intended node's memory, and (4) keep the reader-local faces this transition leans on — and they
are not the same kind of thing. The **xkphys UALIAS window** is a hardware alias, reader-local
unconditionally in every era. The **bare-physical low form carrying the uncached attribute**
(kseg0, or a TLB-mapped bare PFN) is reader-local here **only because the node is still nasid 0** —
it is the same nasid-0-is-local decode as (3), not a second permanent alias. During this transition
the firmware routes by that attribute face, so a model that lets bare-physical-uncached fall to
node0 *too early* — before the node has actually taken its new id — misses what the transition
rides on; an address-face-only model left it wedged. There is no discretionary or timed residency
to reproduce; the protection is sequencing *through* forms that are reader-local while the node is
unnumbered.

**Where the attribute face stops — do not carry it into the OS-era contract.** The
bare-physical-uncached form is reader-local *only during the pre-assignment window*, while the node
is still nasid 0. What it becomes once ids are assigned splits by carrier:

- **Unmapped `TO_UNCAC`** (the `UNCAC_BASE`/`0x96` window): **node0-absolute, not reader-local.**
  Settled — a symbolic identity (`TO_UNCAC` == `TO_NODE_UNCAC(0)`), and the model's own
  attr-3-to-window-zero decode agrees.
- **A TLB-mapped bare low PFN carrying the uncached attribute:** the model routes this by a
  **per-entry attribute** — the mapping's uncached cache-attribute field (the R10000 `EntryLo`
  encoding) selects the carrier, one value routing reader-local (the hub-special carrier) and
  another node-absolute (window zero), consistent with the xkphys attribute table. So there is **no
  model routing gap**: a mapped uncached reference is reader-local or node-absolute per the attribute
  the mapping carries, and the PROM's own wired restores carry the reader-local value. What remains
  **open is a hardware-fidelity question only** — whether real SN0 silicon routes a mapped uncached
  reference by that same attribute — which the public sources do not pin (it lives in the non-public
  Local Resources chapter) and which guest-dependence from a running kernel can still settle, though
  no model change now hangs on it. (An earlier draft asserted node0-absolute for the mapped case as
  if settled; that was inference from the `TO_UNCAC` identity, corrected here.)

The unmapped reader-local faces are unaffected either way: the xkphys UALIAS window and the cached
CALIAS within its small hardware bound stay reader-local. And the PROM transition never depended on
a node0-routing coincidence: its wired restores carry the reader-local attribute, so after the id
write the mapped code fetches route to the reader's own node — there is no node0 routing in play, so
no byte-identical-copy argument is needed to make it safe. (An earlier draft here reasoned from a
node0 routing that does not occur, and leaned on the byte-identical PROM copies to excuse it; both
were artifacts of that wrong premise. The per-node data is still re-qualified explicitly, which is
the real mechanism.) Read the pre-assignment scoping above as applying to the UNMAPPED nasid-field
forms; the mapped form is attribute-routed, and the only clause still open is the hardware-fidelity
one in the mapped bullet.

**Does any low DATA stay unqualified after the change?** Not on the bring-up path read here.
After the id write, low-memory data is reached either through the re-based wired mapping (code,
stacks, and the config header all live inside it) or through an explicitly nasid-qualified
pointer; the config header is additionally re-fetched in qualified form. This confirms the
temporal reading — **bare/local before the change, qualified after** — for the paths that make up
the transition. It is not an exhaustive proof over every helper the later boot may call: a bare
nasid-0 data reference used after the change would be a defect of the same kind, and is the thing
to watch for if a post-change access lands on node 0's memory.

**Provenance (IRIX 6.5.7 IP27 PROM bring-up, stated behaviourally; cross-checked on 6.5.30).**
Read from the 6.5.7 node-id-transition and TLB-remap path and stated in our own words — no code,
paths, or comments transcribed. The register and macro names (`NI_STATUS_REV_ID`, `NSRI_NODEID`,
`NASID_SHFT`, `TO_NODE`, the `IP27PROM_*`/`SLAVESTACK` map symbols, and the alias set — `UALIAS`,
`CPU_UALIAS`, `LBOOT`, `IALIAS`, and the cached `CALIAS` at `CAC_BASE`) are the public SN0/Linux
names for those objects; the position-independent alias map is defined in the public Linux
`asm/sn/addrs.h`. The running-image re-base address `0xbfc52ac8–0xbfc52b18` is the block
measured on the 6.5.30 PROM. The uncached re-point's **attribute face** — plain-physical PROM-base
page frames carrying the uncached cache attribute rather than an xkphys UALIAS address — is measured
on 6.5.30: the wired entry's `EntryLo0` = `0x00068016` and `EntryLo1` = `0x0006c016` decode to
1 MB-page PFN bases `0x01a00000`/`0x01b00000`, cache attribute `0b010` (uncached), dirty and valid
set (from the four-CPU stream's access-list disassembly of the running image). The 6.5.7 source
expresses the same routing as the named UALIAS window — the two faces of one alias. The
fixed-VA and wired-entry indices, the page size, and the exact three-store settle count are
6.5.7-source values to confirm against the 6.5.30 measurement — flag any delta there.

## The handoff to the kernel

Loading the kernel directly from an executable file is the primary path, so the emulator
also stands in for the loader, and the register state at the kernel's first instruction is
part of this contract. The first two argument registers carry an argument count and an
argument vector; the third carries the end of the symbol table if there is one.

The two guests want opposite things here. Linux's Origin port takes its **entire** command
line from that vector: the load-partition string becomes its root-device argument and the
load-options string is appended verbatim, both moved to the front, while the five other
standard variables are recognised and dropped. So a Linux boot needs a real vector.
OpenBSD walks the same vector for boot flags but decides how to read it from whether the
firmware advertised itself as 32-bit — which, for a 64-bit parameter block, means it reads
the entries as pointers. Passing a non-zero count with anything that is not a real pointer
array dereferences garbage. **Pass a count of zero when loading an OpenBSD kernel
directly**, and a real vector when loading Linux.

## The unhandled-exception path: a deferred flag, not a live trap

**When the firmware takes an exception it has no handler for, it does not stop. It records the
event in a flag and returns, and the console message that follows is a later report, not the
trap.** This matters for debugging: the familiar console line reporting that the machine has
taken an exception and is continuing is emitted by a reporting loop that reads the flag
*afterwards*, so by the time it appears the faulting context is already gone. It is not a
breakpoint on the fault, and it does not mark where the fault happened.

**The flag.** The general exception handler's no-registered-handler path sets a single bit —
bit 53 — in the processor's local Hub network-interface scratch register, and returns to the
interrupted flow. There are seven identical store sites, one per exception class, all writing
the same bit (store site at `0xbfc02a30` in the running image). A separate loop later reads the
bit and prints the continuing-after-an-exception line. So that line is a summary that an
exception happened at some earlier point, decoupled from the trap by however long it took the
report loop to run.

**The save area is one doubleword slot, and it does not even hold a coherent triple.** The
handler stores Cause, EPC, and BadVAddr all to the *same* hub-local address (`0x1000020`), with
no increment between the three stores — three stores, one slot — so within a single exception
each store overwrites the previous and only the last survives. That last store is **BadVAddr**;
Cause and EPC are clobbered by their own exception's later stores before any second exception
exists. So the slot never usefully holds Cause or EPC at all — at most the BadVAddr of the most
recent unhandled exception. For a debugger it is not a record of what faulted, only one fragment
at one address.

**The live record is a processor's coprocessor-0 state — but read it with its condition.** cp0
is authoritative only while that processor has taken **no subsequent exception**, because cp0 is
itself a single-slot store. An idle processor with interrupts masked has taken none, which is
exactly why cp0 read from such a processor still holds the faulting Cause, EPC, and BadVAddr; a
processor still running may have trapped again and overwritten them. Read cp0 from a quiesced
processor, not a live one.

**Provenance (observed on the release we boot).** From disassembly plus live measurement on the
6.5.30 PROM (the store site at `0xbfc02a30`, the save area at `0x1000020`, in the running image);
the scratch-register and coprocessor-0 names are the public ones. Stated behaviourally; the
6.5.7 source carries the same structure as the cross-check.

## Interrupts

None. Nothing described here asserts or acknowledges an interrupt. The launch-block
protocol is driven by an inter-processor interrupt the hub delivers, but the blocks
themselves are plain memory; that interrupt belongs to the hub's contract, not this one.

## DMA

None. Every structure here is written by the emulator into guest RAM before the first
instruction executes, and read by the guest with ordinary loads. Nothing here is behind
the I/O bridge, so nothing here uses address translation or byte swapping. The write
routine's buffer argument is a guest address the dispatcher reads with the ordinary
guest-memory accessors.

## Out of scope

- **The real firmware's internal work** — built-in self test, processor arbitration,
  memory sizing and directory initialisation, error-correction setup, flash timing, the
  one-wire board-identity reads, the non-volatile log, the network discovery that finds
  routers and assigns node identifiers. None of it produces anything a guest can observe
  except its output in the structures above. This is the entire argument for synthesis.
- **The system controller.** On Origin 200 the real firmware skips it: module number,
  partition, and debug switches come from a log in non-volatile memory rather than from
  controller traffic, and the front-panel display calls are skipped. Choosing Origin 200
  removes that subsystem from the problem.
- **Remote board records** in the configuration graph, the router board class,
  and the node table beyond entry zero are outside the current `origin200` QEMU
  target. The Origin 2000 projection (citation: `docs/machines/origin2000.md`) records Router,
  Crossbow, multi-module, and multi-chassis hardware as a reference model. It
  does not claim that the emulator synthesizes the corresponding firmware records.
- **The relocating loader and the private vector's inventory entries.** Written down
  above so the contract is complete, implemented when we go after the SGI standalone
  shell and IRIX.
- **Firmware-served block device access** — open, seek and read against a real disk. The
  SGI standalone shell needs it because it implements filesystems itself and expects the
  firmware to provide the device driver; we will serve those from QEMU's block layer
  directly, which is easier than routing them through a modelled disk controller and lets
  an IRIX experiment start before that controller works.
- **Loading a real firmware image** instead of synthesizing. Kept as a diagnostic path.
  It is not the supported path: the image is copyright with no redistribution grant, so a
  capability gated on it is a capability almost nobody has.

## Notes from the sources

- **The child-getter must return null, and this is stronger than it looks.** The guest's
  machine-identification routine only reaches the directory-signature fallback when the
  root component has no child. If a child *is* returned, the guest matches its identifier
  string against a table of machine names — a table that contains every SGI machine of
  the previous generation and does **not** contain this one. A helpful synthesized
  firmware that returns a root component named after this machine therefore lands in the
  "unrecognized system" path, which prints a complaint and halts. The earlier
  firmware boot analysis (citation: `docs/research/11-firmware-and-boot-entry.md`) recorded this as
  "may return null"; reading the identification routine closely makes it "must".
- **A guest reads these structures through more than one window.** The parameter block is
  read through compatibility space, deliberately, because the flat 64-bit window is
  sub-spaced on this machine. The directory, the configuration graph and the global data
  area are read through the node's *uncached* window. The global-data-area directory
  entry holds an absolute address that the guest dereferences as-is. All of these must
  land on the same bytes.
- **Two consumers, one layout.** Every structure here is declared independently by IRIX
  and by at least one of Linux and OpenBSD, and the declarations agree field for field —
  including the boot-flash configuration block, which OpenBSD declares with the same
  fields at the same offsets under different names. Where only IRIX declares something —
  the private call vector's entry order, the capability ladder, the parameter block's
  version and revision values — that is called out in the sources line at the top.
- **Documentation conflicts found while writing this.** The memory-map comment block in
  the Linux copy of the directory header describes the error spools as 32 KB running to
  the first free page, while the directory constant in the same file says 16 KB; the
  constant is what the directory advertises and the IRIX original's comment agrees with
  the constant, so 16 KB is right and the Linux comment is stale. The launch block is
  declared with fields and padding summing to 240 bytes while a constant in the same
  header calls it 256; the stride is `0x200` either way, so nothing observable depends on
  which is meant, but a builder should write the declared fields and zero the rest of the
  slot. The global data area's symbolic assembly offsets disagree with its own structure
  about the order of the master-node and firmware-operation fields.
- **The reset-vector entry count was wrong twice before it was measured.** The firmware
  headers name ten entry points; an early reading of a shipped image reported eight,
  having decoded only the first 64 bytes. Decoding the whole table in both shipped
  revisions gives **twelve**, bounded by the configuration block that begins immediately
  after. The two unnamed slots appear in no header we hold. The
  analysis of the real image (citation: `docs/library/ip27prom-analysis.md`) has the details.
- **The Origin 200 has no crossbar, and the two constants that used to be explained as a
  restricted one are explained better without it.** The Hub's crosstalk link goes straight
  to the Bridge, so nothing routes by widget number and every widget address reaches the
  one chip. **Widget 0 reporting itself as an I/O bridge is that**, not a reduced crossbar
  answering on a bridge's behalf. The guest's default assumption when it has seen no
  crossbar component — that the hub is at widget `0xa`, the hardwired interrupt widget —
  is the ordinary path here. The base-I/O Bridge's widget 8 identity is a firmware
  convention on this direct Origin 200 link rather than a switched location.
  Origin 2000 is different: its separate BaseIO card reaches a module Crossbow.
  The reviewed public topology figures establish that switched path, but they do
  not establish one numeric Crossbow widget identity for BaseIO across enclosure
  and board revisions. Keep that identity unresolved. A component contract may
  transfer between these machines. A widget number observed in one Origin 2000
  configuration does not.
- **Where the parameter block and vectors live is machine-family-specific.** On this
  family they sit at `0x1000`, `0x2800` and `0x2c00` in node memory. On SGI's other
  ARC-style machines the two vectors sit inside the parameter block's own page instead —
  the parameter block at `0x1000`, the call vector at `0x1800`, the private vector at
  `0x1c00`. GXemul's firmware emulation, which is the design we are copying for the trap
  mechanism, puts both tables in the flash window at `0x1fc80000` and `0x1fcb0000`
  instead, because it targets machines that load kernels into low memory and it wanted
  the space. Both alternatives are wrong for this machine. Take the trap mechanism from
  GXemul and the addresses from here.
- **How the vector entries become callable.** Each entry holds the address of a distinct
  word inside a small reserved region; executing that word traps into our dispatcher,
  which identifies the call from the low bits of the program counter and returns by
  jumping to the return-address register. Because a 64-bit table spaces entries eight
  bytes apart while the call numbering is natural, a dispatcher that keys on the
  low bits has to halve the offset it computes before comparing — the single most common
  way to get this design subtly wrong. The private vector is a second region and must be
  recognised before that halving, not after.
- **The vector has thirty-five entries on SGI machines and thirty-seven on the generic
  ARC machines it descends from**, which append a Unicode-character test and a display
  status call. Linux declares all thirty-seven unconditionally and simply never calls the
  last two on SGI hardware. Advertise thirty-five in the parameter block's length field,
  which is what SGI firmware does.
- **The honest implementation budget.** GXemul's equivalent is about 3,100 lines, but a
  large part of that is a text-console emulator, partition-table parsing and file access
  against disk images, and a component tree for a machine family we do not care about.
  Trimmed to what this machine needs, and counting the whole eventual job rather than the
  banner subset, the comparable figure is roughly a thousand lines: the vectors, the
  parameter block and the trap plumbing are small; the component tree, the environment
  and argument handoff, and the dozen call handlers that matter are where the work is.

## qtest checklist

**Before adding to this list: an invariant you have not checked against a capture is a
claim about the hardware wearing a test's clothes.** Every item below asserts something
about a real machine, and inventing a property that *ought* to hold is how two defects
got into this checklist from opposite directions — one item asserted a property so weak
that a broken console base satisfied it, and another asserted that a board's component
count equals its number of component offsets, which real firmware does not do. From
inside the check the two are indistinguishable: both look like reasonable properties of a
well-formed structure, and neither reports anything until a capture disagrees.

So the entry price for a new item is knowing which captured value would break it, and
saying so. An item nothing could have disproved is not testing the machine; it is
recording an opinion about the machine in a place where it will be trusted.

The firmware model's qtest must assert, on a freshly reset machine with no guest code
run:

- The parameter block at `0x1000` has the signature as a full 64-bit field with the first
  four bytes zero; its length, version and revision are as specified; its call-vector and
  private-vector lengths are `0x118` and `0x88`; and both vector addresses dereference to
  the expected node offsets.
- Every one of the thirty-five call-vector entries is non-zero and points into the trap
  page, and the halt, power-down, restart, reboot and interactive-mode entries are at
  offsets `0x18` through `0x38`.
- The private vector's seventeen entries are non-zero, and the capability entry, when
  called, returns 3.
- The hardware directory's sixteen entries: the six the firmware fills carry the
  signature and the specified offset, size, count and stride; the global-data-area entry
  carries a dereferenceable pointer and size `0x400` and a zero offset; the three that
  belong to the guest are entirely zero.
- The configuration graph header has the right signature, its board-list offset points at
  the first board, its console offset is `0x18`, and the console descriptor's memory base
  matches the address the machine gave the I/O controller — the field Linux's early
  console uses and the one whose failure mode is silence.
- The console descriptor's serial-port base **equals the whole expected 64-bit value**,
  compared as one quantity, and its baud rate is inside 50 to 115200. **Do not assert a
  property of it.** Every narrower check on this field has been passed by a wrong value:
  the low twelve bits select the I/O controller generation and `0x178` satisfies them
  while reaching no register; the low twenty bits give the chip-relative offset and a base
  missing its Bridge device window satisfies those while resolving to the wrong bus. A
  byte-for-byte comparison of the whole field has no weaker property left for a wrong
  value to pass on, which is why the generated-model gate caught what two of this
  datasheet's own assertions did not.
- The console descriptor's memory base is the IOC3 device window,
  `0x9200_0000_0860_0000`. Its configuration base is device 2's Type 0
  configuration aperture, `0x9200_0000_0802_2000`, and its PCI-device byte is
  `2`. These three relationships are checked together because different guests
  consume them.
- Walking the board list terminates on a zero next-board offset, visits exactly the
  expected boards in order, and every board has the local-board bit set in its record kind
  and a node identifier of 0. **Do not assert that a board's component count equals its
  number of non-zero component offsets — real firmware violates that.** An earlier version
  of this item did, and the captured graph from a real Origin 200 disproves it: the
  midplane reports a count of **zero** while its component array holds a
  module-serial-number entry, and the other two boards match their counts exactly. A test
  asserting the invariant would fail against the machine it is meant to model.
- Each board's record version is **exactly** the value SGI's firmware writes for its
  kind, not merely at least two: three on the node board, two on the midplane and the
  base I/O board. **Do not assert a lower bound.** A bound of "at least two" is satisfied
  by the node board carrying the midplane's value, by every board carrying the same
  wrong version, or by a version far above what real firmware ever writes — none of which
  is a property IRIX's board-naming fallback cares about, which fires on anything below
  two regardless of what is above it.
- Every component has the enabled flag set, the failed flag clear, and a
  firmware-component back-pointer that is either zero or an address inside the region
  where firmware components live — the assertion that catches an uninitialised builder.
- The processor component reports the machine's processor clock and cache size, and its
  identification field is **sixteen bits holding `0x0900`**, with the sixteen bits above
  it holding the floating-point value rather than the top half of a 32-bit write.
- The memory-bank component's eight bank sizes **each** match what the machine populated,
  and every slot the machine did not populate reads zero. Assert the slots individually
  and not only that they sum to the memory size: a sum is satisfied by any distribution,
  and the slot an N-mode guest would misread as the directory attribute is one of the
  trailing ones, so proving those are zero bounds the damage if the hub's mode bit and
  this encoding ever disagree.
- The midplane board carries a module-serial-number component and **no crossbar
  component**, which is what real firmware emits on this machine. Do not assert a
  crossbar component's contents; there is none to assert, and a test that looks for one
  either fails or quietly passes over an absent record.
- The Hub component's port descriptor node identifier reads as the invalid marker, all
  ones, since this machine's Hub names no crossbar link. **A zero here is a wrong value,
  not an absent one**: it is a live node identifier that a stricter guest follows into a
  board lookup, so the assertion has to check the specific bit pattern rather than only
  that the field is non-garbage.
- The global data area has the signature, version 2, master node 0, node-table entry 0
  equal to 0 and every other entry negative — entry 1 in particular, since that is where
  Linux stops counting. **Read the table as 64 signed sixteen-bit entries**, because the
  wrong element width is what this assertion has to catch: a byte-wide table read a byte
  at a time is also a zero followed by all ones, so a test that reads bytes passes on
  exactly the encoding that hangs the guest.
- Both launch blocks carry the launch signature and are otherwise zeroed.
- The memory-descriptor walk returns the descriptors in the specified order, terminates,
  the first free-memory descriptor's base page is **exactly `0x19`** as real firmware
  emits, the sole firmware-permanent descriptor is the last one and starts at page
  `0x1a00`, and a free or loaded-program range contains the address the kernel is loaded
  at.
- Reading the configuration block returns its signature and a machine-type field of 1,
  and the three clock fields read as specified — **through both windows**, the
  compatibility view at `0x1fc00060` and the hub special-space view at `0x10000060`.
- All twelve reset-vector addresses contain a branch rather than zero, the first eleven
  being jumps and the twelfth a call, and the word immediately after the last one is the
  configuration block's first field.
- Calling the write routine on handle 1 with a known string produces exactly that string
  on the character device, and the returned count equals the length written.
- Asking for a variable that does not exist returns null; asking for each variable the
  machine was configured with returns its value.
