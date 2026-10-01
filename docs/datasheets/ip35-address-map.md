# Datasheet: IP35 physical address map (Origin 300, board IP45)

References rendered as citation identities name material outside this document
set. Those targets are not included or cleared for redistribution. Review and
runtime statements retain their recorded dates and scope.

The 2026-09-05 identity correction (citation: `docs/reviews/origin300-identity-correction.md`)
withdraws unsupported IP45 register assignments and scopes inherited controller
names. The original clearance remains recorded for the unchanged contract.

**Status:** the base map was reviewed 2026-09-03 by an independent second reader
(`docs/reviews/ip35-address-map-review.md`): cleared for the register map, the
XIO windows, the Hub special layout, the interrupt-send address, the hand-off
register and the IP35 low-memory map, position for position against the public
sources, with the review's nine findings applied in this revision. The
board-identification compatibility contract added from 2026-09-09 PROM evidence
has a separate independent review (citation: `docs/reviews/origin300-board-identification-contract-review.md`).
The remaining open items stay open; nothing here is implemented or tested.
The 2026-09-09 independent BDDIR review (citation: `docs/reviews/origin300-bddir-contract-independent-review-20260909.md`)
clears the exact hashed delta for the M-mode transform, one 64-bit directory
word per 128-byte line on a 16-byte address stride, standard and premium masks,
zero initialization as model policy, aligned 64-bit access policy, migration,
fitted-memory holes, the exact 512 MiB carriers, and the stated machine qtest.
It excludes page-counter layout and behavior, a hardware directory reset value,
active directory transitions, coherence, ECC, access protection, a nonzero
memory-configuration word, and completion of `go cac`. It does not claim that
the directory region or qtest has been implemented or run.
The present `+8` delta identifies the public Bedrock page-counter address and
records a narrow firmware-facing model policy. It remains unreviewed and must
receive independent evidence review at an exact revision before implementation.
The 2026-09-25 Hub-special register-window delta (the LED registers, the
unidentified block at `0x00`, and the removal of the processor-interface pass-through)
changes a reviewed contract and needs its own independent evidence review before the
model change lands.
**Fork file(s):** none yet; the planned machine file is `hw/mips/sn1.c`.
**Evidence cutoff:** 2026-09-09. The reviewed map used Origami trunk
`506836b9dd`. The independently reviewed BDDIR delta used baseline
`918d4deae569b6dd31888c85486649707c6f8481`.

**Sources used:** OpenBSD at commit `2df91d80c104636deeef8124252ddd5951b55fbf`
(branch `sgi-is-alive-at-7.9`, the OpenBSD 7.9 GENERIC-IP27 kernel this
project boots on the Origin 200): `sys/arch/mips64/include/cpu.h`,
`sys/arch/sgi/include/mnode.h`, `sys/arch/sgi/xbow/hub.h`,
`sys/arch/sgi/xbow/xbow.c`, `sys/arch/sgi/xbow/xbridge.c`,
`sys/arch/sgi/sgi/ip27.h`, `sys/arch/sgi/sgi/ip27_machdep.c`,
`sys/arch/sgi/sgi/sginode.c`, `sys/arch/sgi/sgi/l1.c`,
`sys/arch/sgi/conf/GENERIC-IP27`. Read as
`git -C refs/openbsd-sgi show 2df91d80c1:<path>`; cite the commit, never the
branch. The Linux SN1 port at tag `v2.5.40` (commit `7570df54ef8c`) in
`refs/linux-history`: `include/asm-ia64/sn/sn1/addrs.h`, `sn1/arch.h`,
`sn1/bedrock.h`, `sn1/hubpi.h`, `sn1/hubpi_next.h`, `sn1/hubio.h`,
`sn1/hubio_next.h`, `sn1/hublb.h`, `sn1/hublb_next.h`, `sn1/hubni.h`,
`sn1/hubni_next.h`, `sn1/hubxb.h`, `sn1/hubmd.h`, `sn1/synergy.h`,
`sn1/ip27config.h`, `sn/addrs.h`, `sn/kldir.h`, `sn/gda.h`, `sn/io.h`,
`sn/xtalk/xtalkaddrs.h`, `sn/sn_cpuid.h`, `arch/ia64/sn/io/sn1/ip37.c`,
`arch/ia64/sn/io/hubspc.c`. This is
SGI-written GPL code generated from SGI's register database: a public
register-definition source and an Itanium reference consumer, not a MIPS
guest and not independent corroboration of anything else SGI wrote. The
earlier Linux tag `v2.5.0` (commit `381ab1f538162`) retains the common
`include/asm-ia64/sn/addrs.h` directory-backdoor formulas removed before
`v2.5.40`. The
out-of-tree MIPS Linux IP35 port in `refs/linux-ip35` (commit
`9f613a321ea0`, 2015, base 3.19-rc4, target Origin 350): `arch/mips/include/asm/mach-ip35/spaces.h`,
`arch/mips/include/asm/sn/sn1/addrs.h`, `arch/mips/include/asm/sn/addrs.h`,
`arch/mips/sgi-ip35/Platform`, `arch/mips/sgi-ip35/ip35-l1.c`,
`arch/mips/pci/pci-sgipic.c`; GPL, public, a reference consumer that nothing
records booting. Linux mainline `arch/mips/include/asm/mach-ip27/spaces.h`
and `asm/sn/sn0/addrs.h` at commit `db2ddb871435` for the IP27-only low/high
directory comparison. US patent 6,751,698 B1, retained as
`docs/library/cache/patents/US6751698-sn1-bedrock.pdf`, original PDF pages 1
and 14, corroborates the Bedrock memory/directory unit but does not define its
backdoor addressing. The reconciliation these were
read in is preserved in the SN1 research notebook.

No facts in this document derive from restricted IRIX source. The stock IRIX
6.5.30 PROM observation below is software-requirement evidence, not a hardware
address-map source. A clean implementer may read this file.
Where a value is confirmed by two trees that do not share code it is marked
*established*; where one tree states it, *observed*; where it is this
document's own arithmetic on cited constants, *derived*; where the sources
disagree or are silent, *open*, with the observation that would settle it.
OpenBSD's node header says its definitions were partly inspired by Linux source
(`mnode.h` line 34), so two-tree agreement is strongest where OpenBSD executes the
access (the L1 UART, the board-identification read, the network status, the scratch register)
and weakest for constants it defines and never touches; the rows below say which.

## Role

An IP35 board presents the same shape of address space as the Origin 200:
one flat physical space shared by every node, with the node number in the
high bits so each node's memory, Bedrock registers, XIO windows and
boot window appear at a fixed stride, plus position-independent aliases that
always resolve to the reader's own node. Three things move. The physical
address is still 40 bits, but a node now owns 8 GB rather than 4 GB, so the
node number sits at bit 33 and is seven bits wide in the normal (M) mode.
The Bedrock's units are rearranged inside the 8 MB register window and a
second processor interface and a local block appear. Hub special space
splits the Origin 200's single 256 MB boot window into a 128 MB local
register window followed by a 128 MB boot window, and the local register
window carries the L1 system controller's UART. An OpenBSD board-identification
read remains consumer evidence rather than a cleared IP45 register block.

What does not move is the mechanism by which a processor chooses a space.
Every public consumer that runs a MIPS processor on IP35 selects Hub
special, I/O space, memory special or uncached memory with the
R10000-family uncached attribute in virtual-address bits 58:57, exactly as
on the Origin 200. A four-bit selector field at physical bits 47:44 with an
eleven-bit node number, which the project's flat Origin 300 model records,
matches no public consumer this datasheet cites, MIPS or Itanium: the Itanium
tree keeps its space nibble at bits 43:40 and a seven-bit node at bit 33, and
its only eleven-bit node field is in its SN2 branch at bit 38. The correction
is set out under behavioral notes.

A single Origin 300 chassis is one node in M mode, so the parts of this map
that depend on N mode or on a second node are recorded for completeness and
marked out of scope for the first implementation.

## Register map

### Processor-side address spaces

The processor selects a space with two bits beside the address, not inside
it. An R10000-family processor takes the uncached attribute from
virtual-address bits 58:57 of an uncached (cache algorithm 2) xkphys
address and drives it on the system bus with the 40-bit physical address
(OpenBSD `mips64/include/cpu.h` lines 92 to 113; the out-of-tree Linux port
says the board uses the R10000's uncached-attribute feature,
`mach-ip35/spaces.h` lines 13 to 16, and its SN1 address header says the
bases would become processor-dependent if that changed, `sn1/addrs.h` lines
19 to 22). The Bedrock header carries the same four-entry attribute table
(`sn1/bedrock.h` lines 40 to 44).

| Space | Attribute | Unmapped base an R10000-family guest uses | Confidence |
|---|---|---|---|
| cached memory | none (cached algorithm) | `0xa800_0000_0000_0000` | established: OpenBSD `mnode.h` line 40 with `cpu.h` line 87, which lies under the coherent-target option `GENERIC-IP27` line 28 sets, and `GENERIC-IP27` line 20 states the base outright; linux-ip35 `spaces.h` line 17; the same value as the Origin 200 |
| Hub special | 0 | `0x9000_0000_0000_0000` | established: OpenBSD `cpu.h` line 98, `mnode.h` line 46; linux-ip35 `spaces.h` line 19 |
| I/O | 1 | `0x9200_0000_0000_0000` | established: `cpu.h` line 99, `mnode.h` line 47, `ip27_machdep.c` line 161; `spaces.h` line 20 |
| memory special | 2 | `0x9400_0000_0000_0000` | established: `cpu.h` line 100, `mnode.h` line 48; `spaces.h` line 21 |
| uncached memory | 3 | `0x9600_0000_0000_0000` | established: `cpu.h` line 101, `mnode.h` line 49; `spaces.h` line 22 |

There are exactly four uncached spaces on the MIPS side: OpenBSD's extractor
masks two bits (`cpu.h` line 113) and linux-ip35 defines four uncached bases
(its `MAP_BASE` at `0xc000…` is the mapped-kernel segment, not a space). The
Itanium port's seven bases, with a space nibble at bits 43:40 of an Itanium
region address and two byte-swizzled variants (`sn1/addrs.h` lines 44 to 51),
describe what an Itanium sees through Synergy and do not apply to this
machine.

**QEMU consequence.** The Origin 200's 42-bit machine-bus carrier, a two-bit
window number above the 40-bit physical address, fits this generation as it
stands. The window rotation that puts memory at window zero is unchanged.
This retires the worry recorded on the Origin 300 machine page that seven
spaces would need a third carrier bit.

### The node number in the physical address

| Item | M mode | N mode | Source |
|---|---|---|---|
| node field | bits 39:33, 7 bits, mask `0x7f` | bits 39:32, 8 bits | established: linux-ip35 `sn1/addrs.h` lines 50 to 71; Linux SN1 `sn1/addrs.h` lines 71 to 92 and `sn/sn_cpuid.h` line 114; OpenBSD `sginode.c` line 102 (shift 33 or 32 on IP35) |
| node window | 8 GB | 4 GB | established (same lines; OpenBSD `mnode.h` line 55 with the run-time shift) |
| maximum nodes | 128 | 128 | established: `sn1/arch.h` `MAX_NASIDS` 128 in both Linux trees; OpenBSD `mnode.h` line 438 sizes the GDA table at 128 and `sginode.c` lines 125 to 127 cap M mode at 64 nodes, an OpenBSD choice |
| physical mask | 40 bits | 40 bits | Linux SN1 `sn1/addrs.h` line 51; OpenBSD's generic mask keeps 36 bits (`cpu.h` line 107) but its node arithmetic uses the shift directly (`mnode.h` lines 55 to 71) |
| Origin 200 for comparison | bits 39:32, 8 bits, 4 GB | bits 39:31, 9 bits, 2 GB | mainline `sn0/addrs.h` lines 53 to 74 |

The OpenBSD header's comment that a node owns 4 GB in M mode (`mnode.h` line
53) is the Origin 200 value; its code uses the IP35 shift.

One first-party observation sits beside the consumers: the vendor's Origin 300
field-diagnostics manual prints a sample configuration record for an Origin 3000
pair in which node 1's cached memory base is `0xa800000200000000`, 2^33 bytes above
node 0's (108-0264-004 printed page A-2). That is the vendor's own tool running on
IP35 hardware, independent of every header and kernel cited above, and it agrees
with the seven-bit field at bit 33. A real two-chassis Origin 300 shows the
layout in use: OpenBSD's trap on the second node reads its KLCONFIG at
`0x9600000200032728`, node 1 at bit 33 (the openbsd-sgi list capture in
`docs/library/boot-logs.md`).

**How a guest learns the mode, and the conflict.** OpenBSD reads the network
port status register at hub offset `0x680000` and takes bit 18 as "more
nodes than fit in M mode" (`sginode.c` lines 100 to 103, `hub.h` lines 217 to
218), the Origin 200 rule carried over. The Bedrock register file defines
only bits 2:0 of that register (Linux SN1 `sn1/hubni.h` lines 1015 to 1033),
and the Itanium port reads the system-size field of the local block's
revision register instead, bits 47:46, where 2 means N mode
(`sn1/hublb_next.h` lines 19 to 32; `io/sn1/ip37.c` lines 34 to 38). **Observed:** bit 18 reads clear on real Origin 300s, one chassis and two cabled
together, since OpenBSD prints "Machine is in M mode." on both (the openbsd-sgi
list capture of 2014 in `docs/library/boot-logs.md`), so the model's zero there
is the hardware's answer for every Origin 300 configuration seen. What an N-mode
Bedrock returns in that bit, and which register the PROM itself consults for the
system size, stay unobserved; OpenBSD's IOC3 driver notes that IP35 systems
always run in M mode (`ioc.c` lines 114 to 118). The revision register's region-
size field the Itanium port reads is unobserved on a MIPS board; the model
returns zero there as its choice.

### Node memory

Memory is reported per bank in 1 GB steps: OpenBSD appends each bank's size
at 1 GB intervals from the node base (`sginode.c` lines 549 to 590) and the
Linux SN1 headers give a 1 GB slot shift with a 64 MB minimum
(`sn1/arch.h` lines 53 to 54; eight slots per node at line 47). OpenBSD
does not manage the first 64 MB of a node on IP35, against 32 MB on IP27
(`sginode.c` lines 566 to 575 and 519 to 528). The Origin 300 User's Guide
(007-4365-002 printed page 11) describes four DIMM slots as two pairs, each
pair two banks, 512 MB to 4 GB per chassis, with the pairs allowed to
differ in size; the IP45 assembly layer owns that population.

The firmware's low-memory map for IP35, as the Linux SN1 tree draws it
(`sn/kldir.h` lines 164 to 283 for the diagram, 310 to 361 for the constants),
differs from the Origin 200's and is what
a synthesized firmware would have to reproduce. Offsets are within node
memory. Only the entries a guest reads by fixed offset are load-bearing;
the rest are reached through the low-memory directory.

| Offset | Contents | Fixed-offset readers |
|---|---|---|
| `0x0000` | processor 0 exception handlers; cache-error frame at `+0x400`; early cache-exception stack at `+0x800` | firmware |
| `0x1000` | ARCS system parameter block, 1 KB | every ARCS guest |
| `0x2000` | low-memory directory (kldir), 1 KB, 16 entries of 64 bytes | OpenBSD `mnode.h` line 90; both Linux trees |
| `0x2400` | launch area, one entry per processor, four entries of `0x100` | firmware; OpenBSD launches through the PROM entry instead |
| `0x2800` and `0x2c00` | ARCS transfer and private vectors, master node only | ARCS guests |
| `0x3000` | NMI handler page, four entries of `0x40` | firmware |
| `0x8000`, `0x10000`, `0x18000` | processors 1, 2 and 3: handlers, cache-error frame, early stack, same layout as processor 0 | firmware |
| `0x9000` | NMI register save area, `0x400` per processor | OpenBSD `ip27.h` lines 29 to 31 hard-codes it |
| `0xa000` | NMI exception-frame area, `0x400` per processor | OpenBSD `ip27.h` line 31 |
| `0xb000` | global data area, 1 KB | via kldir slot 3 |
| `0x11c00` | per-partition data area, 8 KB | firmware |
| `0x20000` | processor-interface error spools, 64 KB | via kldir slot 6 |
| `0x30000` | configuration graph (KLCONFIG), 64 KB | via kldir slot 1; the PROM itself prints `init_klcfg: nasid 0 start 9600000000030000 size 10000` on real Origin 300, Origin 350 and Tezro machines |
| `0x40000` | first free memory; non-debug kernel load address | the map's constant (`sn/kldir.h` line 336); linux-ip35 loads at `0xa800_0000_0004_0000` (`Platform` line 11). OpenBSD links at the same address (`GENERIC-IP27` line 19), but that is one kernel for both boards and shows only that it sits above both maps |
| `0x4c000` | symmon stacks, four of `0x8000` | debug only |
| `0x310000` | debug kernel | debug only |

The Origin 200's corresponding fixed offsets (NMI save area `0x11400` with
`0x200` per processor, GDA `0x11000`, KLCONFIG `0x4000`, free memory and
non-debug kernel `0x19000` in the out-of-tree port's own Origin 200 diagram,
`kldir.h` lines 64 to 65 and 175) do not apply; `0x1c000` is where mainline
Linux loads its IP27 kernel (`arch/mips/sgi-ip27/Platform` line 14), not the
map's value. The out-of-tree MIPS port still carries those Origin 200
constants in its kldir header; its IP35 code never uses them.

The kldir slot numbers are unchanged from the Origin 200: 0 launch,
1 KLCONFIG, 2 NMI, 3 GDA, 4 free memory, 5 symmon stack, 6 PI error,
7 kernel variables, 8 kernel cross-partition, 9 partition id (OpenBSD
`mnode.h` lines 91 to 100; both Linux trees). The kldir entry layout and
magic are also unchanged. OpenBSD reads the directory uncached; the Itanium
port reads it through the cached alias.

### I/O space windows

| Item | Value | Source |
|---|---|---|
| small window | 16 MB per widget, widget `w` at node I/O base `+ w << 24`, widgets 8 to 15 for devices | established: OpenBSD `hub.h` lines 22 to 26, `ip27_machdep.c` lines 488 to 489; both Linux trees' `sn/addrs.h` |
| big window | 512 MB in M mode, 256 MB in N mode; window `k` at node I/O base `+ (k + 1) << 29` (28 in N mode) | established: OpenBSD `ip27_machdep.c` lines 69, 236 to 237, 493 to 497; linux-ip35 `sn1/addrs.h` lines 65 and 51; Linux SN1 `sn1/addrs.h` lines 86 and 72 |
| translation entries | seven, at hub offsets `0x400160` to `0x400190` in 8-byte steps; software numbers them 0 to 6, hardware manuals 1 to 7 | established: OpenBSD `hub.h` lines 191 to 193; Linux SN1 `sn1/hubio.h` lines 132 to 174 and `sn1/hubio_next.h` lines 132 to 133 |
| entry encoding | bit 12 space (1 device, 0 memory), bits 11:8 widget, low bits the XIO address above the window size | established: OpenBSD `hub.h` lines 195 to 201; Linux `sn/io.h` lines 20 to 46 (Linux masks a 5-bit offset, OpenBSD 8; no consumer needs more than 5) |
| space bit consumers write | memory (0), in every consumer that programs a big window | OpenBSD `ip27_machdep.c` lines 568 to 569 (with a remark that the author does not know why); Linux SN1 `io/io.c` line 253 |
| widget 0 | reached through big window 6, which the PROM programs for it; OpenBSD never allocates entry 6 | OpenBSD `ip27_machdep.c` lines 478 to 486 and 535 to 536; both Linux trees resolve widget 0's small window to big window 6 (`sn1/addrs.h`, `sn1/hubio_next.h` lines 141 to 151) |

**Open:** whether the widget-0 remap is still needed on a Bedrock. The Linux
SN1 header asks that question in as many words (`sn1/hubio_next.h` line 144),
OpenBSD applies the remap unconditionally, and the out-of-tree MIPS port
reads widget 0 through the raw small window (`sgi-ip35/ip35-xtalk.c` lines
49, 86 and 169) without a record of it working. Settle by reading entry 6
after a stock PROM boot on the emulated machine, which observes what the vendor
PROM programs; probing widget 0 both ways afterwards tests the model, not the
hardware. Until then the model must answer both forms, which is what the
Origin 200 model already does for its widget 0.

OpenBSD's window allocator (`ip27_machdep.c` lines 500 to 585) scans entries
0 to 5 through the remote register form, treats a zero widget field as free,
reuses an entry whose widget and offset cover the request, otherwise writes
the first free entry and reads it back, and never releases an entry.

### Hub register space

The Bedrock's registers are XIO widget 1 of its own node. The local,
position-independent alias is small window 1 of node 0, 8 MB; the
node-named form adds `0x800000`, which is address bit 23 (remote, access
checks apply); the second processor interface is reached by adding
`0x200000` on the node-named form. OpenBSD expects the local form to reach
the requester's own interface; whether Bedrock actually selects the interface
this way remains unmeasured. The modeled two-CPU board has a requester on each
interface, so this assumption needs a discriminating PI0/PI1 local-read test.

| Form | Address | Source |
|---|---|---|
| local register `r` | `0x9200_0000_0100_0000 + r` | established: OpenBSD `hub.h` lines 28 to 29; both Linux trees' `IALIAS_BASE` |
| remote register `r` on node `n` | `0x9200_0000_0180_0000 + (n << 33) + r` | established: `hub.h` lines 30 to 31; `sn/addrs.h` `REMOTE_HUB_ADDR` |
| second processor interface, register `r` | remote form `+ 0x200000` | established: `hub.h` lines 32 to 34 and 110 to 114; `sn1/hubpi_next.h` lines 16 to 19 |

The unit bases inside the 8 MB window. Every generated Linux SN1 header
gives hub-relative offsets, so a register's hub offset is unit base plus
unit-relative offset; OpenBSD keeps the bases as separate constants and
adds them at the call site.

| Unit | Base | Source | Origin 200 |
|---|---|---|---|
| processor interface 0 | `0x000000` | established: `hub.h` line 85; `sn1/hubpi.h` first register at 0 in both Linux trees | `0x000000` |
| processor interface 1 | `0x200000` | established: `hub.h` lines 110 to 114; `sn1/hubpi_next.h` line 18 in Linux SN1, line 17 in linux-ip35 | memory/directory |
| I/O interface | `0x400000` | established: `hub.h` line 189; `sn1/hubio.h` line 26 | `0x400000` |
| local block | `0x600000` | established: `hub.h` line 208; `sn1/hublb.h` line 26 | network interface |
| network interface | `0x680000` | established: `hub.h` line 215; `sn1/hubni.h` line 24 | |
| crossbar | `0x700000` | established: `hub.h` line 233; `sn1/hubxb.h` line 24 | none |
| memory/directory | `0x780000` | established: `hub.h` line 180; `sn1/hubmd.h` line 25 | `0x200000` |

No public source conflicts with this layout. A comment in the Linux SN1
XIO header describing register bits 22:21 as selecting PI, MD, IO and
NI in that order is Origin 200 text left in the SN1 tree; the same tree's
register definitions govern.

The register offsets within the processor interface are unchanged from the
Origin 200 for everything a boot touches (region present `0x18`, CPU number
`0x20`, cached-alias size `0x28`, CPU present and enable `0x40` to `0x58`,
interrupt-pending modify `0x90`, pending banks `0x98` and `0xa0`, the four
mask registers `0xa8` to `0xc0`, the real-time counter at `0x30100`). The
Bedrock unit datasheets own the register contracts.

### Hub special space

| Region | Offset from the node's Hub special base | Size | Source |
|---|---|---|---|
| uncached alias of the reader's own node memory | `0x0000_0000` | 256 MB | observed: linux-ip35 `sn/addrs.h` lines 119 to 133, text inherited from the Origin 200 headers; no IP35 consumer uses it |
| local register window | `0x1000_0000` | 128 MB | established: OpenBSD `hub.h` lines 63 to 64; Linux SN1 `sn/addrs.h` lines 123 to 125 |
| boot window | `0x1800_0000` | 128 MB | observed: Linux SN1 `sn/addrs.h` lines 126 to 128; OpenBSD defines the constants (`hub.h` lines 65 to 66) and never uses them; the PROM copies itself from `0x9000000018000000` into node memory at offset `0x1a00000`, length `0x1686a8` (the Tezro verbose trace, with the same line truncated by the posters on the Origin 300 and Origin 350 traces) |
| primary software PROM read address | boot-window offset `0x0000_0000` | observed: Linux SN1 defines the LBOOT base and a two-PROM stride; the static machine carrier maps the exact stock payload here |
| second PROM selection | boot-window offset `0x0200_0000` | established: Linux SN1 `sn/addrs.h` defines a 32 MiB stride as two PROMs on 32 MiB boundaries; the model maps the upper 8 MiB of the 16 MiB backing here with command state independent of the first selection |
| remote register window of node `n` | `0x3000_0000` | 128 MB | established: OpenBSD `hub.h` lines 48 to 49 with callers passing register-window offsets; Linux SN1 `sn/addrs.h` line 148; linux-ip35 `sn/addrs.h` lines 170 to 172 |
| remote boot window of node `n` | `0x3800_0000` | 128 MB | observed plus derived: Linux SN1 `sn/addrs.h` line 153; linux-ip35 `sn/addrs.h` lines 174 to 176; OpenBSD never forms this address |
| directory backdoor | `0x1c000_0000` | 1 GB aperture, with 16-byte carrier slots only for fitted memory | observed: Linux `v2.5.0` `sn/addrs.h` maps one Bedrock directory word at `+0` and names `+8` as the page-counter address |

Inside the remote register window:

| Block | Offset from the register window | Source |
|---|---|---|
| unidentified 16-doubleword device | `0x0000_0000` to `0x0000_0078` | observed: the stock PROM 6.210 stores `0x2000`, then `0x100`, then (after enabling memory refresh) `0x2b` at `0x00` to `0x38` and `0x29` at `0x40` to `0x78`, through the remote form of the node it is initializing (listing `0x1fc66164` to `0x1fc66228`); no public source names anything here. Inferred, not established: DIMM mode-register commands, from the order and values (measurement notebook (citation: `docs/notebooks/origin300-hub-special-led-decode-20260925.md#the-stores-at-remote-offsets-0x00-to-0x78`)). Unknown: what a load returns |
| L1 system controller UART, register `r` at `+ 8r` | `0x0000_0080` | established: OpenBSD `hub.h` lines 68 to 73 and `l1.c` lines 97 to 142 (64-bit accesses, low byte significant, through the remote form); linux-ip35 `sgi-ip35/ip35-l1.c` lines 28 to 39. The model now answers the same eight registers at the same `+0x80` offset in the LOCAL register window too (below); that half is an inferred consumer requirement, not established by any of these three sources |
| per-processor LED, slice `s` (0 to 3) | `0x0000_00c0 + 8s` | established: Linux `v2.5.40` `asm-ia64/sn/leds.h` lines 17 to 19 (`LED0` is the Hub-special base plus `0x1000_00c0`, `LED_CPU_SHIFT` 3) and `arch/ia64/sn/kernel/setup.c` line 371 (one LED per slice); the stock PROM 6.210 stores progress codes at `0x9000_0000_1000_00c0 + 8s`, `s` read from the PI CPU-number register (listing `0x1fc6b54c`), and at the remote-form equivalent (`0x1fc6b594`); IRIX 6.5.30 stores heartbeat values through the remote form (observed). Inferred: the register is 8 bits wide, from the four 8-bit LED banks the memory/directory unit reads back (`sn1/hubmd.h` lines 139 to 151 and 1106 to 1132) and SN2's "junk bus LED" data field at bits 7:0. Unknown: what a load through the window returns; the only public statement is the successor's, that its LED addresses are write-only and the LEDs are read through separate registers (`sn2/shub.h` lines 26 to 28) |
| Origin 300 board-identification compatibility register | `0x0400_0000` | established consumer requirement: OpenBSD reads the remote form and the stock IRIX 6.5.30 IP35 PROM reads the local form; the bounded model response is documented below |

Inside the local register window, the same offsets hold the same blocks: sMDK
reaches the UART, and the stock PROM the LEDs and the board-identification
register, through the local form, and the public Itanium header defines the LED
through the local form
(the [L1 controller register map](l1-controller.md#register-map) owns the UART
evidence). The Itanium tree also places the Synergy registers at `0x0400_0000` and
`0x0500_0000` of this window (`sn1/synergy.h` lines 60 to 66); the Origin 300 has
no Synergy, and the board-identification entry above is the only thing modelled
there.

**Neither window is a view of the processor-interface registers (inferred).** No
consumer reaches a PI register through either Hub-special window form; every PI
access every cited source makes goes through the I/O-space form
(`0x9200_0000_0100_00xx`, node-named `0x9200_0000_0180_0000 + (n << 33)`, see
"Hub register space" above). The public header routes PI, MD, NI and II registers
through the I/O-space alias and the UART, LEDs and Synergy through this window
(`sn/addrs.h` lines 159 to 166 describe the I/O-space alias as the path for PI,
MD, NI and II registers; the UART and Synergy macros and the literal LED address
use this window instead), and OpenBSD's list of what the window holds names only
the UART and the Synergy registers, in a list its own comment calls very incomplete (`hub.h` lines 56 to 75). Were the window a PI
view, the LED offsets would be PI interrupt mask 1 B and the cross-call-pending
set A, set B and clear A registers, and every LED update by the PROM and by the
SGI kernels would rewrite an interrupt mask or raise and clear a cross-call; and
the PROM's first two stores at offset `0x50` of its own node's remote window
would clear the boot processor's own CPU enable (the boot processor being slice A
of interface 0). No
public source states the window's decode outright, so this is inferred, not
established. A hardware store to local offset `0xc0` followed by a crosstalk-form
read of PI interrupt mask 1 B would settle it.

Model policy for the two windows, one decode for both forms:

- `0x00` to `0x7f`: an explicit unmodelled block. Stores are accepted and
  ignored, loads return zero, and every access is logged as unimplemented. The
  PROM's measured stores need somewhere to land; what the device does with them
  is not modelled. This is an approximation of an unidentified device, not a
  claim about it.
- `0x80` to `0xbf`: the L1 UART.
- `0xc0` to `0xdf`: four LED registers, one per slice, shared by all processors
  and both forms. A store keeps bits 7:0; a load returns the kept byte (the
  model's choice while the load behavior is unknown; the successor chip's
  window LEDs are write-only). Only the aligned 64-bit access every observed
  consumer makes is answered; other widths are refused with a bus error and
  logged. Reset clears them (the reset value is unknown). They touch no
  interrupt state. The memory/directory unit's LED read
  registers (`0x7a0010` to `0x7a0028`) are not yet tied to them.
- `0x0400_0000`: the board-identification compatibility register below.
- Every other offset in either window is not decoded: an access fails with a bus
  error and is logged, as the remote form already did above `0x80_0000`. The
  measurement behind this covered offsets `0x00` to `0x7e_0047` of both windows;
  no access above that, other than the board-identification register, has been
  observed or ruled out.
- Access to either window follows the current-cell rule
  ([memory-directory datasheet](bedrock-memory-directory.md)), as before.

Before 2026-09-25 the model passed every offset outside the UART through to the PI
register file. The measurement
notebook (citation: `docs/notebooks/origin300-hub-special-led-decode-20260925.md`) counts what
that did: every IRIX 6.5.30 LED update from slot A rewrote slot B's interrupt
mask 1, slot B's and D's updates raised and cleared cross-call pending bits, and
the PROM's memory-initialization stores at `0x00` to `0x78` landed on PI0's CPU
enable, region-present and cached-alias registers.

For node zero, ordinary memory line address `pa` maps to MIPS uncached
directory address `0x9000_0001_c000_0000 | ((pa >> 3) & 0x3fff_fff0)`.
The low four zero bits place one 64-bit directory word on a 16-byte address
stride for each 128-byte memory line. The inverse shifts the masked
directory-word offset left by three.
The same public header says Bedrock has one directory word rather than the
low/high pair used by IP27, and names the canonical `+8` address as a page
counter. It leaves the counter fields and raw alias behavior undefined. Stock
PROM run `prom-20260909T053344-67ac81` established the baseline. An ad hoc
follow-up recorded in Origami issue 846, comment 13845, then observed 11,648
8-byte writes at `+8 mod 16` from offset `0x340008` through `0x36d7f8`. The
next 8-byte read at MIPS address `0x9000_0001_c034_0008`, carrier
`0x101_c034_0008`, raised the next data-bus error. The memory-directory
datasheet defines the bounded opaque page-counter-word policy required to answer
that firmware access without claiming hardware counter semantics.
The [memory-directory datasheet](bedrock-memory-directory.md#directory-backdoor)
owns entry formats, reset, access, migration, and fitted-memory bounds.

The three cited public consumers (OpenBSD, both Linux SN1 headers) access only the
remote form; none of them reaches the L1 UART through the local register window.
The independent register-window review (citation: `docs/reviews/ip35-l1-local-remote-window-review.md`)
read those consumers, found none reaching the local form, and concluded the model
must not add a local alias without one; that finding is superseded (see the
review's own status note). sMDK, the Origin 300 field-diagnostic package's
diagnostic monitor, is the local-form consumer that finding did not have: booted
headless under the stock PROM's `console=d`, it polls the UART's line-status
register through the local form at the same relative offset the remote form
already answers. That is a software requirement observed running against this
emulator, not a measured IP45 decoder fact; the [L1 controller
datasheet](l1-controller.md#register-map) carries the evidence and its limits, and
a discriminating hardware read of Hub special local offset `0x80` on a physical
Origin 300 or Origin 350 is what would settle whether real silicon answers there.
Window sizes and current-cell protection rules do not establish identical device
populations beyond the UART.

OpenBSD reads register-window offset `0x0400_0000` through the remote form and
interprets bits 15:12 as the product code. The stock IRIX 6.5.30 IP35 PROM reads
the same offset through the local form, masks those bits and recognizes product
codes 8 and 4. The model therefore exposes one stateless compatibility register
through both forms. An aligned 64-bit read returns `0x8001`: product code eight
and PSC configuration selector one for the fixed 500 MHz R14000 machine.
This remains an explicit compatibility approximation, not a measured IP45
strap value. The supplied PROM interprets the low byte as a configuration
selector. Its selector zero describes a 400 MHz CPU, whereas selector one
describes the modeled 500 MHz CPU and 200 MHz Hub. Both the CPU PROM and its
embedded BASEIO image contain that mapping. The
configuration-input investigation (citation: `docs/research/origin300-flash-cpu-identity.md#prom-6210-configuration-input-paths`)
records the identified binaries, table locations and live inputs.
The remaining bits stay zero without claiming their hardware meaning.
This response neither reports empty processors as present nor changes the
PROM's empty-socket PLL recovery decision. Writes, other widths, adjacent offsets and offset
`0x0500_0000` remain unmapped and observable. Reset leaves the fixed response
unchanged, and the device has no migratable state.

This contract does not identify the physical IP45 decoder or establish an IP45
instance of the ASIC named by public Itanium headers. The
board-identification evidence (citation: `docs/library/origin300-identity-evidence.md`)
records the independent consumer paths, manuals and limits of the approximation.

The Origin 200 differs here in kind: it has a single 256 MB boot window at
`0x1000_0000`, no register window, its firmware configuration block at boot
window `+0x60`, and its bring-up UART inside the memory/directory unit.
OpenBSD reads the configuration block only on the IP27 branch and
identifies an IP35 machine from the board-identification read instead. The Itanium port
does read an IP35 configuration block at boot window `+0x60` for the PROM
version (`sn1/ip27config.h` lines 23 to 28; `io/hubspc.c` lines 149 to 150),
so the block exists on IP35; whether a MIPS guest depends on it is open and
nothing public says it does.

The processor reset vector is `0x1fc0_0000` through the uncached
compatibility segment (OpenBSD `mips_cpu.h` line 202). No public IP35 source
explains how that request reaches physical flash. The
stock-PROM reset-carrier decision (citation: `docs/decisions/D-0024-origin300-stock-prom-reset-carrier.md`)
therefore maps one supplied raw payload at the reset address and aliases the
same region at the local boot-window base, the bounded high local boot address
`0x100_1fc00000`, and the node-zero remote boot-window base. This is emulator
compatibility policy, not a hardware decoder claim. Only the exact supplied
length answers through each view; the rest of the 128 MiB boot windows remains
unmapped. The
bounded reset-alias investigation (citation: `docs/library/origin300-reset-alias-evidence.md`)
preserves the unresolved physical question.

### The cached alias

The cached-alias size register in each processor interface (`0x28`) takes a
four-bit code, 0 disabled and 1 to 15 for 4 KB doubling to 64 MB (Linux SN1
`sn1/hubpi_next.h` lines 199 to 215; OpenBSD `hub.h` lines 89 to 90). OpenBSD
writes 0 to both interfaces and all-ones to both region-present registers
at setup (`ip27_machdep.c` lines 338 to 345). The IP35 memory map gives each
of four processors its own exception block at `0x0`, `0x8000`, `0x10000`
and `0x18000`, so the Origin 200's per-processor flip of the bottom 128 KB
of the uncached alias is not stated for IP35 by any public source. Open;
settle by observing where a stock PROM places each processor's handler
copies.

### The interrupt-send address

A widget raises a Bedrock interrupt by writing to an XIO address that
names the node, the Bedrock's own widget 1, the remote form and the
interrupt-pending-modify register. OpenBSD computes it as bit 47, the node
number shifted by 39 on IP35 (38 on IP27), and `0x0180_0090`
(`ip27_machdep.c` lines 350 to 353). The Itanium port's constant sums to the
same value (`sn/xtalk/xtalkaddrs.h` lines 66 to 91) and adds `0x200000` when
the target processor is on the second interface (`io/sn1/hub_intr.c` line
145).

| Term | Meaning | Source |
|---|---|---|
| bit 47 | XIO register access rather than memory | `xtalkaddrs.h` lines 27, 44, 64 to 66; OpenBSD comment at line 350 |
| node `<< 39` | destination node on SN1 (SN0 used bits 46:38) | `xtalkaddrs.h` lines 45 and 68; OpenBSD line 351 |
| `0x0100_0000` | small window 1, the Bedrock itself | `xtalkaddrs.h` lines 48 to 50 |
| `0x0080_0000` | bit 23, the checked remote form | `xtalkaddrs.h` lines 51 to 52 |
| `0x90` | interrupt-pending modify: writing `0x100 | level` sets, `level` alone clears | OpenBSD `hub.h` lines 98 to 100; Linux SN1 `sn1/intr.h` lines 122 to 143 |

The value is written into the bridge's interrupt-destination registers
with the Bedrock's crossbar widget number in bits 51:48 (OpenBSD
`xbridge.c` lines 2177 to 2191), which comes from the configuration graph's
crossbar record and defaults to `0xa` (`sginode.c` lines 159 to 160 and 338).
**Open:** for a Bridge-class bridge on a node other than 0, OpenBSD packs
XIO bits 47:38 into per-pin register bits 17:8, placing the node at
bit 9, while linux-ip35 writes the node at bit 8 (`xbridge.c` lines 1393 to
1394; `pci/pci-sgipic.c` line 77). A one-node machine cannot tell them
apart; out of scope until a second node exists.

### Composing an XBridge device address

A device register behind the XBridge composes as: I/O space,
plus the node's offset, plus the bridge's widget window (widget `<< 24`),
plus the bridge's device window for the PCI device, plus the register's
offset inside the device. The XBridge keeps the Bridge's device-window
layout: two 2 MB windows from `0x20_0000` and six 1 MB windows from
`0x60_0000`; the XBridge datasheet owns that and its evidence.

**The widget numbers are observed on real Origin 300s.** The XBridge
specification (rev 0.5, printed pages 17 and 44 to 46) gives the chip two modes
chosen by a mode pin: in XBridge mode it is a Crossbow whose two PCI bridges sit
on internal ports `0xe` and `0xf` behind six external XIO ports 8 to
`0xd`; in Super Bridge mode the crossbar is disabled and each bridge is an
external widget. Three owners' inventories of real Origin 300s report widget 0
as `ASIC XBOW Rev 2` with `ASIC BRIDGE Rev 2` at widgets 14 and 15, the IO-8's
SCSI, IOC3 and USB at PCI 1, 4 and 5 behind widget 15 and the option slots
behind 14, and a verbose PROM trace from one prints `HUB at 0x0 attached as
widget 0xa`, `xbow_base= 0x9200000000000000` and `Check_master: link 10 is
master` (the Origin 300 captures in `docs/library/boot-logs.md`). So the
crossbar is at widget 0, the bridges at `0xe` and `0xf`, and the Bedrock enters
at port `0xa`; the model carries all three as observed hardware. What the PROM
prints as the crossbar's base is this map's I/O space base for node 0.

The Origin 200 rule that a Bridge identity at widget 0 means a restricted
topology probed at widget 8 alone is a rule OpenBSD applies from the
identity it reads, not from the board type (`xbow.c` lines 290 to 320); on
a crossbar identity it walks the configuration graph for a crossbar record
and a brick or base-I/O board to choose a probe order.

### Firmware hand-off and node reset

| Item | Value | Source |
|---|---|---|
| firmware operation request on halt or reboot | written to local block scratch register 2, hub offset `0x608010`; value is magic `0x0ead0000` or-ed with a command (halt `0x10`, power down `0x20`, restart `0x30`, reboot `0x40`, enter interactive monitor `0x50`) and options (no diagnostics `0x100`, no memory init `0x200`, no device init `0x400`) | OpenBSD `ip27_machdep.c` lines 680 to 704, `mnode.h` lines 456 to 466; the register name from Linux SN1 `sn1/hublb.h` line 155 |
| Origin 200 equivalent | processor interface `0x418` | OpenBSD line 703 |
| network port reset register | hub offset `0x680008`; bit 0 link reset, bit 1 port (warm) reset, bit 2 local Bedrock reset | established: OpenBSD `hub.h` lines 219 to 225; Linux SN1 `sn1/hubni_next.h` lines 28 to 30 |
| reset-enable register | hub offset `0x680010`, bit 0 = 1 allows the reset | established: `hub.h` lines 226 to 227; `sn1/hubni.h` line 35 with `hubni_next.h` lines 141 to 143 |
| halt sequence | for each other node in turn, write reset-enable 1 then the reset register; then the same pair on the local node last; the reset value is local plus link reset in OpenBSD (`0x5`) and local plus port reset in the Itanium port (`0x6`) | `ip27_machdep.c` lines 706 to 729; `io/sn1/ip37.c` lines 45 to 46 |

**Open:** both Linux SN1 trees still name the processor interface's
error-stack-address register (`0x418`) as the firmware-operation register
and never write it at run time, while OpenBSD, the only IP35 consumer that
exercises a reboot, writes the local-block scratch register. The out-of-tree
MIPS port writes the `0x418` register on its Origin 200 reset path
(`arch/mips/sgi-ip27/ip27-reset.c` lines 47 and 63) and has no reset code for
IP35 at all, so the `0x418` path is real on the Origin 200 and the IP35 side is
silence. Settle by tracing which register the held IP35 PROM reads on a warm
reset.

### Processor launch

OpenBSD starts secondary processors through the PROM's launch-slave entry
at compatibility address `0x1fc0_0038`, passing node, slice, function,
parameter, stack and global pointer (`ip27.h` lines 60 to 64;
`ip27_machdep.c` lines 1083 to 1085), unconditionally on IP35. No public
MIPS consumer uses a register-level launch path. The launch area at
`0x2400` holds four entries of `0x100` on IP35 against two of `0x200` on
the Origin 200, per the Linux SN1 map (`sn/kldir.h` lines 310 to 313); the
firmware-structures datasheet for this generation will own the entry
layout, and it is contract territory.

## Behavioral contract

What OpenBSD does with this map at setup, in order (`ip27_machdep.c` lines
148 to 360 and `sginode.c` lines 83 to 136):

1. reads its slice from processor interface register `0x20` through the
   local form;
2. reads the board-identification field of node 0, not of the master node, through the
   remote Hub special form and takes bits 15:12 as the product code;
3. reads the network port status through the local form and takes bit 18
   as the mode; sets the node shift to 33 (M) or 32 (N); then reads
   region-present and cached-alias size through the local form
   (`sginode.c` lines 105 and 107), values it only prints;
4. reads the configuration graph header through the kldir at `0x2000`,
   the GDA through kldir slot 3 (magic `0x58464552`, version at least 2,
   master node number, node table), and walks every node's board list for
   processors and memory banks;
5. pre-launches every other processor of its own node through the PROM
   entry;
6. takes the console UART address from the configuration graph header and
   uses its low twelve bits to tell an IOC3 (below `0x380`) from an IOC4;
7. writes zero to all four interrupt masks of both processor interfaces
   and reads both pending banks;
8. installs its NMI callback through kldir slot 2;
9. writes all-ones to region-present and zero to cached-alias size on both
   processor interfaces;
10. computes the interrupt-send address;
11. attaches the crossbar at widget 0 and probes widgets in the order the
    configuration graph implies.

Every one of those reads and writes must land on a register that answers,
or the boot stops before the first console line.

## Interrupts

The Bedrock processor-interface datasheet owns the pending, mask and modify
registers. This map contributes the address a widget writes and the
`0x200000` step to the second interface.

## DMA

OpenBSD constrains DMA-reachable memory to the low 31 bits of a node
(`ip27_machdep.c` lines 216 to 222), that is the bridge's direct-mapped
window, and never uses the XBridge's translation table. The out-of-tree
MIPS port's 64-bit direct-mapped attribute bits belong to the XBridge
datasheet. How the XIO address a bridge emits carries the node number
on IP35 (bit 39 by the interrupt-address rule) is stated only for
interrupts; for DMA it is open and out of scope on one node.

## Out of scope

- N mode, and every node number other than 0.
- The Itanium-side spaces, the swizzled variants and the Synergy-local
  space; they exist only behind a Synergy.
- The uncached-alias per-processor flip: not stated for IP35 by any public
  source.
- Which flash parts IP45 fits, how the two software PROM addresses select
  physical storage, what an absent part returns, and the complete IP45 decoder.
- Cross-partition and cell structures (kldir slots 8 and 9).

## Behavioral notes

**The flat model's address encoding is corrected, not decided.**
`docs/machines/origin300.yaml` declares a four-bit space field at bits
47:44, an eleven-bit node number at 43:33, seven spaces, and up to 2048
nodes, taking its facts from the source-map survey (`origin300.yaml` line 8).
Every public MIPS consumer contradicts it on all four points, and the Itanium
tree contradicts it too: its space nibble is at bits 43:40 with a seven-bit
node at bit 33 (`sn1/addrs.h` lines 44 to 50 and 85 to 90), and its only
eleven-bit node field sits at bit 38 in its SN2 branch (`sn/sn_cpuid.h` line
120). Because the public sources fix the answer,
the correction is reference-determinable and is filed in the Origin 300
model audit for the model owner, not as a decision packet.

**Two Linux trees carry undefined symbols for this map.** The out-of-tree
MIPS port references a local register base and size it never defines and
names the Origin 200's launch entry in a header it never compiles for
IP35; both Linux trees name a Hub-special UART base that is defined
nowhere. Those paths were never built for IP35, and the constants are
absent rather than evidence.

**The Itanium port's interrupt-address macro mislabels one term** (it calls
a `1 << 23` term the small-window field while the same file puts the small
window at bits 27:24), but the terms sum to the widget-1 remote address, so
there is no behavioral conflict.

## qtest checklist

- A read of processor interface register `0x20` through the local form
  (`0x9200_0000_0100_0020`) returns the reading processor's slice, and the
  same register through the node-named form with `0x200000` added
  addresses the second interface.
- The local block's revision register at hub offset `0x600000` returns a
  part number of `0x3012` in bits 27:12, the node number in bits 39:32 and
  a region-size field of 0 in bits 47:46 on a one-node machine.
- The network port status register at `0x680000` returns 0 in bit 18 on a one-
node machine, as real Origin 300s do (OpenBSD's mode line in the openbsd-sgi
list capture in `docs/library/boot-logs.md`).
- Small window `w` for widgets 8 to 15 decodes at `0x9200_0000_0000_0000 +
  (w << 24)`; big window `k` for `k` 0 to 5 decodes at `(k + 1) << 29` once
  translation entry `k` at `0x400160 + 8k` names a widget; entry 6 is
  reserved for widget 0 and widget 0 answers through big window 6.
- Widget 0's identification register also answers through the raw small
  window at node I/O base plus offset 0, the form the out-of-tree port reads,
  until the remap question is settled.
- The register-window ranges are 128 MB at Hub special `0x1000_0000`
  and `0x3000_0000`. These ranges alone do not establish which registers
  answer in both forms. Boot-window aliases require their separate evidence.
- An aligned 64-bit read of board-identification offset `0x0400_0000` returns
  `0x8001` through local address `0x100_1400_0000` and node-zero remote address
  `0x100_3400_0000`. The adjacent word and offset `0x0500_0000` do not answer,
  other widths do not answer, a write cannot change the value, and reset
  preserves it. Run the rejected accesses with guest-error logging and require
  one diagnostic carrying the attempted address and width for each one. For the
  narrow-read check, address the low 32-bit lane at `+4`, which would expose the
  `0x8000` nibble if QEMU split the access. These checks reject an empty
  register-window container, an ignored write, and an accidentally chunked read.
- The boot-window ranges are 128 MB at `0x1800_0000` and `0x3800_0000`.
  Without pflash, the static carrier maps only the reviewed payload and second
  software read views. With pflash, the reviewed
  Origin 300 boot-flash mapping (citation: `docs/datasheets/ip35-boot-flash.md`) places one complete
  Bedrock device region at each boot-window base. The device decodes the two
  software read addresses inside each region. The reset and bounded high local
  views remain limited to the payload length and reject writes. These choices
  do not settle the physical IP45 decoder or establish a repeating mask.
- On the 512 MiB Origin 300 model, Hub-space carrier address
  `0x101_c034_0000` reaches the directory word for memory line
  `0x01a0_0000`, and `0x101_c034_0008` reaches its opaque page-counter word.
  The two positions retain independent values. The last fitted slot answers at
  `0x101_c3ff_fff0` and `0x101_c3ff_fff8`. The first carrier past fitted memory
  at `0x101_c400_0000` does not. The MD qtest owns contents, reset, access
  widths, isolation, and migration.
- The L1 UART answers at register-window offset `0x80` with 8-byte stride, in
  both the local and the remote form, and the two forms reach the same device:
  a scratch byte written through one reads back through the other.
- A PI register written through the I/O-space form does not appear when read
  back at its same relative offset through either Hub-special window form;
  offsets `0x80` through `0xbf` of both windows answer only the UART.
- A 64-bit store of `0x5d` to local offset `0xc0 + 8s` reads back `0x5d` at
  the same offset in both forms (the read-back pins a model choice), and leaves PI0 and PI1 interrupt masks, the
  cross-call-pending words and every processor's interrupt lines unchanged; a
  store with bits above 7 set keeps bits 7:0 only; each slice's LED is
  independent; a reset clears all four.
- A store to remote offset `0x70` changes no PI register and reads back zero;
  a load at offset `0xe0` or `0x1000` of either window fails.
- A 64-bit write of `0x100 | n` to node I/O-space offset `0x1800090` (small
  window 1, the remote bit, then register `0x90`) from the XIO side sets
  pending bit `n`; `n` alone clears it.
- A write to hub offset `0x608010` is retained and readable (scratch
  register behavior) and a write of `0x0ead0040` there followed by the
  reset sequence resets the node.
- Writing 1 to `0x680010` then `0x4` to `0x680008` resets the node, and so do
  the consumers' values `0x5` and `0x6`.
