# Datasheet: Bedrock processor interface (PI)

References rendered as citation identities name material outside this document
set. Those targets are not included or cleared for redistribution. Review and
runtime statements retain their recorded dates and scope.

**Status:** reviewed 2026-09-03 by an independent second reader
(`docs/reviews/bedrock-pi-lb-ni-datasheet-review.md`): the register map is cleared position for
position against the SN1 header and both MIPS consumers, with the review's seven
findings applied in this revision. The open items stay open. A partial isolated
implementation, including timers, is saved through fork commit `ea2f5028bc`,
with nine passing PI qtest subtests.
It does not establish a complete processor interface or machine acceptance.
The [graphics flow-control section](#graphics-flow-control), added 2026-09-28, is not
yet independently reviewed.
**Fork file(s):** `hw/misc/sgi-bedrock-pi.c` and
`include/hw/misc/sgi-bedrock-pi.h`. The parent `hw/misc/sgi-bedrock.c` composes
two processor interfaces with the local-block and network-interface devices.
**Evidence cutoff:** 2026-09-03, Origami trunk `506836b9dd`.

**Sources used:** the SGI-generated Linux SN1 register headers at tag `v2.5.40`
(commit `7570df54ef8c`) in `refs/linux-history`: `include/asm-ia64/sn/sn1/hubpi.h`
(88 registers, every field as a 64-bit union), `hubpi_next.h`, `sn1/intr.h`,
`sn/intr.h`, `sn1/bedrock.h`, `sn1/arch.h`, `sn/clksupport.h`; the Itanium
consumers in the same tree, `arch/ia64/sn/io/sn1/{hub_intr.c,ml_SN_intr.c,huberror.c}`,
`arch/ia64/sn/kernel/{irq.c,setup.c}`, `arch/ia64/sn/kernel/sn1/{error.c,sn1_smp.c}`,
`arch/ia64/sn/io/sgi_io_init.c`, `sn1/ip27config.h` for the counter frequency.
OpenBSD at `2df91d80c1`: `sys/arch/sgi/xbow/hub.h`, `sys/arch/sgi/sgi/ip27_machdep.c`,
`sginode.c`, `ip27.h`, `intr_template.c`, `sys/arch/mips64/mips64/clock.c`. The
out-of-tree MIPS port `refs/linux-ip35` at `9f613a321ea0`: `arch/mips/sgi-ip35/{ip35-init.c,ip35-irq.c,ip35-timer.c}`,
`arch/mips/include/asm/sn/intr.h` and its `sn1/hubpi.h` (byte-identical to the
Itanium one apart from its guard). The Origin 3000 System Architecture 108-0240-002
printed page 2-12 for the unit's role. Reading preserved in the SN1 research notebook.

**Facts that are IRIX-derived:** only in the
[graphics flow-control section](#graphics-flow-control), which adds what IRIX 6.5.30
does with that block, read from disassembly of the shipped binaries; no vendor source
was read. Elsewhere every offset and field comes from the public
SN1 header, which is SGI's own generated description of the chip; every behavior
comes from that header's narrative or from a named public consumer. **Reset values:
the header states none for this unit.** It says only which registers survive a soft
reset and which are not cleared at reset. The reset column below is therefore
`unknown` unless a consumer's behavior implies a requirement, and the model's reset
state is an emulator hypothesis to be corrected by observing the stock PROM.

## Role

The Bedrock carries two processor interfaces, PI0 and PI1, each facing two
processors, A and B, over the system bus (Linux SN1 `sn1/arch.h`: four processors per
node, two per interface). Each interface is the processors' path to memory and I/O and
holds the interrupt pending banks and per-processor masks, the real-time counter and
its comparators, the region and cached-alias controls, the protection registers and
the error capture for its two processors. PI1's registers are PI0's at hub offset
`+0x200000`, reached through the node-named register window; the crosstalk local
form (`0x9200_0000_0100_0000`, no node number, no interface offset) is assumed by
OpenBSD to reach the requesting processor's own interface, which no header states
(open, below). An [actual two-CPU IP45_2CPU inventory](https://nekonomicon.irixnet.org/forum/14/16726611/1.html)
reports fitted processors in slices A and C. On this observed board, PI0/A and
PI1/A are fitted; PI0/B and PI1/B are empty. The owner also reports PROM 6.210
and IRIX 6.5.29m. That capture does not establish the placement of every board
revision or the reset value of a PI register. Four-CPU products populate both
slices of both interfaces.

The PI is the unit a guest touches first (its own slice number) and most (every
interrupt), so it is the first Bedrock unit to model, with the local block's scratch
registers and the network interface's status and reset beside it.

## Register map

Offsets are hub-relative within PI0; add `0x200000` for PI1. Every register is 64
bits wide. Fields are stated least-significant-bit first, restated from the header's
big-endian unions. "Consumer" names the first public reader or writer.

### Identity, presence and control

| Offset | Name | Fields | Reset | Access | Consumer | Behavior |
|---|---|---|---|---|---|---|
| `0x00` | CPU protect | 63:0 one bit per region | survives soft reset | R/W | none | Permits CPU-initiated register access to this interface per region; a denied access gets an error reply (`hubpi.h` lines 557 to 564). No public consumer writes it; every consumer assumes the PROM leaves its region permitted. |
| `0x08` | protection override | 63:0 pattern | unknown | write, pattern-gated | none | A write of a "special data pattern" sets the writer's own region bit in CPU protect (lines 583 to 585). The pattern is not named for PI; the local block's override takes the 64-bit value `0x53474972756c6573` (`bedrock.h` line 15). Open whether PI takes the same. |
| `0x10` | I/O protect | 63:0 per region | survives soft reset | R/W | none | Permits I/O-initiated interrupt sets per region (lines 604 to 611). |
| `0x18` | region present | 63:0 per region | survives soft reset | R/W | OpenBSD writes all ones to PI0 and PI1 at setup (`ip27_machdep.c` lines 338 to 344) | A zero bit blocks speculative coherent reads to a region; a coherent read to an absent region returns an error to the processor with no error register change; partial accesses are not checked; cached-alias accesses are unaffected (lines 630 to 640). |
| `0x20` | CPU number | 0 cpu (0 = A, 1 = B); 1 pi (0 = PI0, 1 = PI1); 63:2 reserved | unknown | read | OpenBSD's first hub access, kept as the slice (`ip27_machdep.c` line 159; `ip27.h` lines 96 to 97); the Itanium port's slice macros agree (`sn1/arch.h` lines 79 to 85) | Identifies the reading processor's slot and interface (lines 660 to 689). |
| `0x28` | cached-alias size | 3:0 size code; 63:4 reserved | survives soft reset | R/W | OpenBSD writes 0 to both interfaces (lines 339 to 344) | Code 0 disabled, 1 = 4 KB, doubling to 15 = 64 MB (`hubpi_next.h` lines 200 to 215); every interface in a system must carry the same size (`hubpi.h` lines 701 to 702). |
| `0x30` | maximum request-buffer timeout | 7:0 maximum timeout; 63:8 reserved | unknown | R/W | none | Selects the timeout-counter value at which a valid coherency request buffer times out (`hubpi.h` lines 735 to 765). The model stores the low eight bits and reads reserved bits as zero. This readback policy is an emulator choice; the public header gives no reset value or programmed value for IP45. |
| `0x38` | request-buffer timeout scale factor | 23:0 scale factor; 63:24 reserved | unknown | R/W | none | Selects how often a valid coherency request buffer's timeout counter increments (`hubpi.h` lines 772 to 802). The model stores the low 24 bits and reads reserved bits as zero. This readback policy is an emulator choice; the public header gives no reset value or programmed value for IP45. |
| `0x40`, `0x48` | CPU present A, B | 0 present; 63:1 reserved | unknown | set by hardware | none | The interface sets the bit when it sees the processor's first transaction (lines 811 to 813). |
| `0x50`, `0x58` | CPU enable A, B | 0 enable; 63:1 reserved | survives soft reset | R/W | none | While clear the interface ignores the processor's bus request and never grants it the bus (lines 883 to 887); the only register-level way to hold a processor off the bus. Whether the PROM parks slaves with it is open. |
| `0x70`, `0x78` | NMI A, B | 63:0 | unknown | write | none cited | A write issues a non-maskable interrupt to that processor (lines 961 to 994); the exact trigger predicate is in no public source. The stock PROM stores that were once read as NMI stores go to the Hub-special remote register window, not to this register (see the NMI behavioral note). |
| `0x80`, `0x88` | not defined | | | | the Itanium port writes its inter-processor interrupts here (`sn1_smp.c` lines 364 to 385) | No public header names a register at these offsets; the Itanium port alone uses them, through raw addresses (`sn1_smp.c` lines 367 and 374). Out of scope for a MIPS machine. |

### Interrupts

| Offset | Name | Fields | Reset | Access | Consumer | Behavior |
|---|---|---|---|---|---|---|
| `0x90` | interrupt pending modify | 5:0 bit; 6 bank (0 = pending 0, 1 = pending 1); 7 reserved; 8 value (1 set, 0 clear); 63:9 reserved | unknown | write | every consumer: OpenBSD set `0x100 \| n`, clear `n` (`hub.h` lines 99 to 100); Linux SN1 `sn1/intr.h` lines 122 to 143 and linux-ip35 `sn/intr.h` lines 24 to 47 identical; a bank-1 bit is written as 64 + n so bit 6 comes out set | Sets or clears one bit of a pending bank (lines 1001 to 1006). The only PI register an I/O-issued write may reach, checked against I/O protect; processor writes are checked against CPU protect (lines 1007 to 1013). Every consumer follows a clear with an uncached read of pending 0 as a write-posting fence, not a register rule. |
| `0x98` | interrupt pending 0 | 0 reserved; 1 graphics A; 2 graphics B; 3 page migration; 4 UART (the L1 controller); 5 and 6 the coherency-pending results A and B; 63:7 general | unknown | read-only | read by every consumer's dispatch | Level 2 pending bank. Bits 1 and 2 are set by hardware and cleared by software (lines 1053 to 1093). Consumers put their inter-processor interrupts at bits 7 to 10 and allocate widget interrupts from the rest; OpenBSD allocates downward from 63 to 11 here, then 35 to 0 in bank 1 (`hub.h` lines 168 to 171). |
| `0xa0` | interrupt pending 1 | 53:0 general; 54 crossbar error; 55 local-block error; 56 and 57 processor nack A and B; 58 clock error (named in three ways, below); 59 and 60 correctable error (A/B naming conflicts, below); 61 memory correctable error; 62 network error; 63 system shutdown | unknown | read-only | as above | Level 3 pending bank unless remapped. Bits 54, 55, 62 and 63 only reflect other units and cannot be changed by a modify write; bits 56 to 60 are set by hardware and cleared by software (lines 1104 to 1109). |
| `0xa8`, `0xb0` | interrupt mask 0 A, mask 1 A | same layouts as the pending banks | unknown | R/W | OpenBSD writes 0 to all four at setup, then per-processor enable masks at every priority change (`ip27_machdep.c` lines 311 to 314, 1013 to 1018); the Itanium port zeroes all eight masks of both interfaces (`ml_SN_intr.c` lines 1052 to 1068); the out-of-tree port never writes a mask at init and writes them only when an interrupt is connected or disconnected (`ip35-irq.c` lines 43 to 73) | Mask pending bank 0 (to the level 2 line) and bank 1 (to level 3; bits 31:0 of bank 1 may be remapped to level 2) for processor A (lines 1161 to 1218). Only one processor per Bedrock should enable page migration, network, local-block, crossbar and memory-correctable errors. |
| `0xb8`, `0xc0` | interrupt mask 0 B, mask 1 B | as above | unknown | R/W | as above | For processor B; the A-to-B stride is `0x10` (`hubpi_next.h` line 29). |
| `0xc8` to `0xe8` | coherency-pending set A and B, clear A and B, mask | 63:0 | unknown | set on store; write-one-to-clear; R/W mask | none | A processor's store sets its region's bit; the results drive pending 0 bits 5 and 6 (lines 1313 to 1408). |
| `0xf0` | interrupt pending 1 remap | 3:0 remap; 4 error forward; 63:5 reserved | unknown | R/W | none | Redirects bank 1 bits 31:0 from level 3 to level 2; bit 4 forwards memory and unit error interrupts to the I/O interface's interrupt destination (lines 1427 to 1435). |
| `0x180` | interrupt pending modify alias | identical to `0x90` | unknown | write | none | Same semantics and checks; why a second address exists is not stated (lines 1980 to 2015). |

The processor lines: bank 0 raises the processor's hardware interrupt 0 (level 2 in
the header's numbering), bank 1 interrupt 1 (level 3), the real-time comparator
interrupt 2 (level 4) with a conflict noted below, the profiling comparator interrupt
3 (level 5), and the error masks interrupt 4 (level 6) (`sn1/intr.h` lines 163 to
170; linux-ip35 `intr.h` lines 53 to 61). The same shape as the Origin 200's Hub,
which the architecture document's interrupt table already records.

The fixed bits of bank 1 (OpenBSD's "level 1", the header's level 3), as the two
consumers that boot IP35 hardware name them (OpenBSD `hub.h` lines 120 to 147; Linux
SN1 `sn1/intr.h` lines 196 to 230). They agree on 63 to 54 and on 42 to 39; on 53,
44, 43 and 38 to 36 only Linux SN1 names a use, and OpenBSD's file marks 44 and 43
available and folds 53 into the PROM range.

| Bit | Linux SN1 | OpenBSD | generated union (`hubpi.h` lines 1121 to 1131) |
|---|---|---|---|
| 63 | system-controller panic | system-controller error | system shutdown |
| 62 | network error | network error | network error |
| 61 | memory correctable | memory correctable | memory correctable |
| 60, 59 | correctable B, A | correctable B, A | correctable A, B (naming only) |
| 58 | clock error | clock error | performance-counter overflow; the narrative says clock drop-out |
| 57, 56 | nack B, A | nack B, A | nack B, A |
| 55 | local-block error | local-block error | local-block error |
| 54 | crossbar error | crossbar error | crossbar error |
| 53 | bridge error, set up by the PROM | "53 to 45 used by PROM" | general |
| 52 to 45 | PROM 0 to 7, entry 0 at 52 | PROM | general |
| 44, 43 | TLB flush B, A | "available" | general |
| 42, 41 | link failure B, A | link errors | general |
| 40, 39 | broadcast error B, A | network error B, A | general |
| 38 | I/O error, set up by the PROM | "38 to 36 IP35 PROM" | general |
| 37, 36 | debug B, A | PROM | general |
| 35 to 0 | available | available | general |

The out-of-tree MIPS port's bank-1 file is byte-identical to mainline's Origin 200
header, so it carries the Origin 200's assignments for 54 to 57 and the PROM range in
reverse order; its slice is forced to zero and its hub data is single-node
(`ip35-timer.c` lines 38 and 63; `ip35-init.c` lines 45 and 49), and it is not the
assignment.

### Real-time counter and comparators

| Offset | Name | Fields | Reset | Access | Consumer | Behavior |
|---|---|---|---|---|---|---|
| `0x30100` | real-time counter | 54:0 count; 63:55 reserved | unknown | R/W | OpenBSD reads PI0's copy on the master node as its timecounter, truncated to 32 bits, 1,250,000 Hz assumed, in multiprocessor kernels only (`ip27_machdep.c` lines 104 to 114, 1147); linux-ip35 writes 0 at init and reads 52 bits (`ip35-timer.c` lines 133 to 140, 175); the Itanium port reads node 0's copy for timestamps | 55 bits, wraps to zero, increments on each rising edge of the global clock the local block supplies, in its own 64 KB page (`hubpi.h` lines 3963 to 3967). Linux SN1 `sn1/ip27config.h` line 50 defines the real-time clock as 1,250 kHz (an 800 ns cycle) and line 245 puts it into the configuration block's `freq_rtc` field (line 62) at boot-window offset `0x60` (line 23); OpenBSD's `ip27.h` lines 74 to 90 carry the same block with the field at line 80, read on its IP27 branch only. Every MIPS consumer assumes 1,250,000 Hz, and the model uses it. |
| `0x108`, `0x110` | real-time compare A, B | 54:0 compare; 63:55 reserved | unknown | R/W | linux-ip35 arms A on every clock event with the slice forced to 0 (`ip35-timer.c` lines 37 to 45); OpenBSD never arms either (its clock is the processor's count and compare); the stock IP35 PROM 6.210 writes progress codes and then the pair liveness value `0x24` into each processor's own register and polls the partner's, before it has written either real-time enable register (disassembly and access trace, the presence-race notebook (citation: `docs/notebooks/origin300-copy-phase-stall-presence-race-20260924.md#the-partners-safe-park-2026-09-25`)) | When the counter equals the compare, the matching real-time interrupt pending bit is set (lines 1477 to 1480). A processor's first write to its own compare register after its presence join (its first LB scratch 1 write after the reset barrier's release has landed) is taken as its liveness post, whatever the value; the first of a pair to post is halted until its partner posts, or for two seconds of virtual time (`sgi_bedrock_rt_compare_written()`, `hw/misc/sgi-bedrock.c`). The value is stored first and nothing guest-visible changes. That the first such write is the `0x24` post at `0x1fc69274` is established for PROM 6.210 from disassembly; the park is the model's approximation of the pair running off one clock. See known limitation: the presence-publication join (citation: `docs/known-issues.md#the-presence-publication-join-approximates-same-edge-reset-at-the-first-barrier`). |
| `0x118` | profile compare | 31:0 compare; 63:32 reserved | unknown | R/W | none | When the low 32 bits of the counter match, both profile pending bits are set (lines 1551 to 1553). |
| `0x120`, `0x128` | real-time interrupt pending A, B | 0 pending; 63:1 reserved | unknown | read; any store clears | linux-ip35 writes 0 to acknowledge (`ip35-timer.c` line 68) | With the matching enable set, drives the processor's timer interrupt line; any store to a register clears that register's own bit (lines 1587 to 1591). |
| `0x130`, `0x138` | profile interrupt pending A, B | 0 pending | unknown | read; any store clears | none | Both set on a profile match, cleared independently (lines 1663 to 1670). |
| `0x140`, `0x148` | real-time interrupt enable A, B | 0 enable | unknown | R/W | linux-ip35 writes 1 to both at init (`ip35-timer.c` lines 171 to 172) | Enables the real-time interrupt to that processor (lines 1745 to 1746). |
| `0x150`, `0x158` | profile interrupt enable A, B | 0 enable | unknown | R/W | linux-ip35 writes 0 | Enables the profiling interrupt (lines 1815 to 1816). |

The single-node model runs the counter at a fixed 1,250,000 Hz. Writes to
the local block's clock controls do not change that rate. Physical clock
selection and programmable synchronization remain open hardware questions.
The timer contract review (citation: `docs/reviews/bedrock-pi-timer-contract-review.md`)
confirms this implementation boundary and the existing interrupt routing.

For deterministic model reset, the counter, both real-time compares, the
profile compare, and all timer pending and enable bits initialize to zero.
The counter then advances at the fixed model rate. These are emulator choices,
not established hardware reset values.

### Errors

| Offset | Name | Fields | Reset | Access | Consumer | Behavior |
|---|---|---|---|---|---|---|
| `0x3f8` | error interrupt pending, writable | 34:0 the error bits | not cleared at reset | direct write except the memory-uncorrectable bit | none | The writable view of the next register (lines 2430 to 2441). |
| `0x400` | error interrupt pending | 34:0. Bits 0 to 23 are twelve B/A pairs, B even and A odd, in the header's order: spool complete, spurious message, the two write-buffer error kinds, bus-state parity, bus data ECC, bus address ECC, bus command data parity, bus command address parity, spool error, uncached uncorrectable, bus-state tag. Bit 24 is memory uncorrectable, with no pair. Bits 25 to 34 are five pairs with the parity reversed, **B odd and A even**: bad data 25 and 26, cached uncorrectable 27 and 28, packet length 29 and 30, request-buffer error 31 and 32, request-buffer timeout 33 and 34 (lines 2566 to 2603 little-endian, 2609 to 2647 big-endian; `hubpi_next.h` lines 257 to 266) | not cleared at reset | read; write-one-to-clear | the Itanium port writes all ones at I/O init (`huberror.c` lines 40 to 52) | Each set bit may raise the error interrupt line to the processor its mask permits (lines 2543 to 2555). The model sets bit 24 on both interfaces when the memory unit latches an uncorrectable error, an approximation by the same precedent as the node-wide error sources in the mask rows ([the memory-directory datasheet](bedrock-memory-directory.md#interrupts)); the writable view neither sets nor clears it and reads it, a write of it here does clear it. The sMDK monitor reads it through the writable view against the processor's mask, both from its idle loop and from its handler for the error line. |
| `0x408`, `0x410` | error interrupt mask A, B | 34:0 | unknown | R/W | none | Masks the error line per processor (lines 2659 to 2663). |
| `0x418`, `0x420` | error stack address A, B | 32:3 address; rest reserved | survives soft reset | R/W | OpenBSD on IP27 only writes its reboot request here (`ip27_machdep.c` line 703); the Linux SN1 tree still names it the PROM operation register and never writes it | The address of the next error-stack write, incremented after each write within the size (lines 2735 to 2739). On IP35 OpenBSD moves the reboot request to the local block's scratch register 2; see the local-block datasheet and the hand-off register decision packet. |
| `0x428` | error stack size | 3:0 | survives soft reset | R/W | none | Bytes are 4096 shifted by size minus one; 0 disables (`hubpi_next.h` lines 33 to 34). |
| `0x430` to `0x468` | error status 0 and 1 for A and B, each with a clear alias at +8 | status 0: 63 valid, 62 overrun, 61:25 address, 24:17 command, 16:6 supplemental, 5:3 request number, 2:0 type; status 1: spool, timeout and invalidate counts, buffer number and status, source (`hubpi_next.h` lines 109 to 151) | not cleared at reset | read; a write to the clear alias clears both of that processor's status registers | the Itanium port writes all ones to both clear aliases at I/O init | Error capture (lines 2855 to 3225). |
| `0x490` | system-bus error-check enable | 0 ECC generation; 1 data-quality generation; 2 SysAD parity check; 3 command parity check; 4 state check; 5 data-quality check; 63:6 reserved | unknown | R/W | none | Controls system-bus error checking and forwarding (`hubpi.h` lines 3424 to 3465). The model stores the low six bits and reads reserved bits as zero. This readback policy is an emulator choice; the public header gives no reset value or programmed value for IP45. |
| `0x4a8`, `0x4b0`, `0x4b8` | nack count A, B; nack compare | 19:0 count, 20 enable; 19:0 compare | unknown | R/W | none | Counts consecutive negative acknowledgements while enabled; at the compare value the processor gets an error reply instead and pending 1 bit 56 or 57 is set (lines 3552 to 3559). |
| `0x4c0` | spool mask | 5:0 | unknown | R/W | none | Which errors are spooled (lines 3672 to 3674). |
| `0x4c8`, `0x4d0` | spurious header 0, 1 | 63 valid | not cleared at reset | R/W | the Itanium port writes 0 at I/O init | Spurious-message capture. |

The remaining registers (debug select at `0x160`, the force-bad-check-bit pair at
`0x498` and `0x4a0`, reply level, the performance counters, spool compare and error
injection) are touched by no public consumer and are out of scope. The graphics
flow-control block and the graphics credit mode, which IRIX uses, are specified in
the next section.

### Graphics flow control

**Not yet independently reviewed.** Added 2026-09-28 as the contract a live
credit counter must follow. How a graphics head reaches these registers is
the PI graphics credit convention (citation: `docs/designs/sgi-pi-graphics-credits.md`).
The Odyssey (VPro) driver, on Fuel and on the Origin 300 composition, uses
this block, and so does the InfiniteReality driver in the same IP35 kernel.
These graphics credits are distinct from the crosstalk link-level and
crossbar credits, which are per link and cover all traffic; those are out of
scope here.

Ten registers per interface pace one processor's uncached stores to a
directly attached graphics pipe. PI0 and PI1 each carry the full block; PI1's
is at `+0x200000`. Within an interface the slot A set starts at `0x300` and
the slot B set `0x28` above it. **The sets belong to processor slots, not to
graphics contexts.** IRIX's register accesses carry only the slot, bit 0 of
the running processor's slice, and reach the interface through the local
form (see [local-form dependence](#local-form-dependence)); slice bit 1
appears only in the node-widget map's PI field. IRIX moves a pipe's accounting
between processors by saving the counter at detach and writing it back at
attach.

| Offset (A, B) | Name | Fields | Reset | Access | Behaviour |
|---|---|---|---|---|---|
| `0x300`, `0x328` | graphics page | 16:0 reserved; 39:17 page address; 40 enable; 63:41 reserved | unknown | R/W | The physical page whose uncached stores from this processor are counted. |
| `0x308`, `0x330` | credit counter | 11:0 | unknown | R/W | Free credits: falls by one per counted doubleword, rises by one per doubleword acknowledged. A write replaces the count. |
| `0x310`, `0x338` | bias | 11:0 | unknown | R/W | Low-water threshold for the processor stall and the flow-control interrupt. |
| `0x318`, `0x340` | interrupt counter | 25:0 | unknown | R/W | Counts clocks of credit starvation toward the compare. |
| `0x320`, `0x348` | interrupt compare | 25:0 | unknown | R/W | Loaded into the interrupt counter on an interrupt, NMI or soft reset; the interrupt is sent when the counter reaches it. |
| `0x20068` | graphics credit mode | 0 transaction mode; 63:1 reserved | unknown | R/W | Counts transactions instead of doublewords when set. In its own 64 KB page so it can be mapped to user space. |

Two registers elsewhere take part: pending 0, whose bits 1 and 2 are the
graphics interrupts for slots A and B (in the interrupt table above), and the I/O interface's
graphics node-widget map at `0x400140` and `0x400148`
([I/O interface register map](bedrock-io-interface.md#register-map)), which
routes returned credits.

**Sources.** The SN1 header `include/asm-ia64/sn/sn1/hubpi.h` at
`linux-history` tag `v2.5.40` (commit `7570df54ef8c`): offsets at lines 105
and 332 to 383, register comments at lines 1053 to 1057, 2025 to 2031, 2067
to 2077, 2108 to 2111, 2144 to 2155, 2188 to 2193 and 3925 to 3930;
`sn1/hubio.h` lines 104 to 115 and 1290 to 1296; `sn1/hubio_next.h` lines
93 to 109; `sn1/intr.h` lines 191 to 192. Disassembly, with no vendor
source read, of IRIX 6.5.30 `IP35boot` objects from the installation
media: `gfxs.a(gfx_flow.o)` (SHA-256
`2908dd361cfa0ac90885ee35563674f7f07cb891e363d28b127e7a2316a01850`) and
`odyssey.a(odsy_flow.o)` (SHA-256
`c4c87485c608dea4531f8fbcaeaa9bd2d16dae279bf381da1bad9db8de837d62`), at
object offsets from `llvm-objdump -dr`; and the IRIX 6.5.30 IP35 kernel
relinked with the Odyssey driver on the installed Origin 300 disk (SHA-256
`b86b069a90b9abb249576c9a28e06b79096a5a398a7923c069e6f73ecd3d8115`), at its
own addresses, for the per-processor data layout, interrupt connection and
driver arguments. The IRIX facts in this section are disassembly-derived.

#### Graphics page

- **Established:** the layout, and that uncached writes into the enabled
  page made with the uncached attribute of I/O go out as graphics
  single-write commands (GFXWS) that the credit counters track, in place of
  ordinary uncached writes (SN1 comment at line 2027). The counter comment
  also names GFXWL, which no comment ties to a store; a gathered block write
  is **inferred**. IRIX's attach routine, `gfx_flow_attach_me`, writes
  the pipe's page address with bit 40 set into the running processor's page
  register (`gfx_flow.o` `0x5ec`); detach, `gfx_flow_detach_me`, writes
  `0xdeadbabe`, which has bit 40 clear (`0x748`).
- **Unknown:** the reset value, and whether stores made with another
  uncached attribute are counted.

#### Credit counter

- **Established:** decremented for each doubleword sent to graphics
  through the page, incremented for each doubleword acknowledgement from
  graphics (SN1 comment at line 2069).
- **Established, IRIX dependence:** the accessor `gfx_flow_credit_count_get`
  (`0x4ec`) loads 64 bits from the local form `0x9200_0000_0100_0000` plus
  `0x308` plus `0x28` times the slot, and shifts the value left by three.
  The slot is bit 0 of the per-processor data byte at `0x528` (`0x504`),
  which `alloc_cpupda` fills from `get_cpu_slice` (kernel
  `0xc000_0000_0029_336c`). Attach writes zero to the bias (`0x5b4`), then
  the flow's saved count to the counter (`0x5d0`). Detach reads the counter
  back into the saved count (`0x768`, `0x76c`). `gfx_flow_reset` sets the
  saved count to the flow's total, so the first attach after a pipe reset
  writes the total. Before the model stored these registers the counter
  read zero, IRIX tore the board down and the X server could not open a
  rendering node.
- **Established, IRIX dependence:** `gfx_flow_alloc` sets the total to the
  driver's byte size divided by eight, at most 256 (`0x320`), and the bias
  to 68 (`0x2f0`). Odyssey passes eight times the kernel variable
  `odyssey_dwcredits`, 100 in the shipped kernel, or `0x800` bytes on the
  board types it selects by an explicit test (kernel `0xc000_0000_0047_1a2c`
  to `0xc000_0000_0047_1a9c`), so it runs with 100 or 256 credits and bias
  68.
- **Established, IRIX dependence:** `odsy_flow_await_returned_credits`
  (`odsy_flow.o` `0xbc`) runs before each of the driver's 31 command-issue
  sites. Unless a software flag has disabled flow control, it waits while
  the count is below the bias, polling up to 25000 times one microsecond
  apart (`0x108`), and leaves the wait at once if a second software flag, at
  offset `0x68` of the board's driver structure (the argument, not the flow
  structure), is set on any poll (`0x114`). It then reads the count again and, if the count exceeds
  the total (`0x16c`), logs "Credits stuck above maximum". On either failure
  ("gfx credits below bias" for the timeout) it raises priority, detaches,
  tears the board down, waits for the board's FIFO level to reach zero,
  resets the flow, attaches and finishes the attach. InfiniteReality's
  `kona_flow_await_returned_credits` has the same shape with 1000 polls.
- **Inferred:** one uncached store of four bytes or fewer counts as one
  doubleword; the model uses that rule for every store up to eight bytes.
  A credit returns when the pipe frees the FIFO space the word held.
- **Unknown:** the reset value; what the counter does below zero or above
  `0xfff`. The interrupt-counter comment tests the credit counter for "less
  than or equal to zero", which suggests a signed count; IRIX compares it
  unsigned. **The model wraps modulo 4096 and logs the wrap as a guest error,
  its choice.** Either way IRIX tears the board down, since a wrapped count
  reads above the total.

#### Bias

- **Established, IRIX dependence:** attach writes zero, then
  `gfx_flow_attach_me_finish` writes the flow's bias (`0x6a4`); detach
  writes zero (`0x73c`). The driver compares against its own copy of the
  bias, not the register.
- **Established, internally inconsistent:** the counter's comment says
  that when the counter is below the bias the processor's write-ready signal
  is withheld, an interrupt is sent, and write-ready is then allowed again;
  the bias's own comment says a flow-control interrupt is sent when the
  counter is at or below the bias.
- **Unknown:** which threshold, and how long the processor is held.

#### Interrupt counter and compare

- **Established, IRIX dependence:** `gfx_flow_init` writes 20000 (`0xc0`) to
  both interrupt compares of PI0 and of PI1 on every node, through the
  node-named window (`0x128`, `0x150`). The graphics interrupt handler
  zeroes the running processor's interrupt counter (`0x5c`), and so does
  detach (`0x750`).
- **Established:** the reload, zero and count rules in the table and the
  SN1 comments: written by software; loaded from the compare on an
  interrupt, NMI or soft reset, which stops a second interrupt; zeroed when
  the credit counter rises above the bias; incremented once per clock while
  the credit counter is at or below zero.
- **Unknown:** which clock and at what rate, so 20000 has no known duration;
  and whether the count condition is at or below zero, as written, or below
  the bias, as the stall rule suggests.

#### Graphics interrupt

- **Established:** pending 0 bits 1 and 2 are slots A and B, set by hardware
  and cleared by software (SN1 comment at line 1055; `sn1/intr.h`). IRIX's
  `install_gfxintr` connects bit 1 plus the slot on every enabled processor
  (kernel `0xc000_0000_0006_0d88`) to
  `gfx_flow_delayed_hiwater_intr_prehandler`, which calls the attached
  flow's callback, `odsyHiwaterInterrupt` for Odyssey, and zeroes the
  interrupt counter. Detach clears bit 1 plus the slot through the local
  interrupt-modify register and reads pending 0 back (`0x758`).
- **Unknown:** whether the interrupt comes from the bias test, from the
  interrupt counter reaching the compare, or both. The handler's name, a
  delayed high-water handler, and its zeroing of the counter point at the
  counter, which is **inferred**.

#### Credit mode

- **Established:** the register and its bit (SN1 lines 105 and 3927).
  `gfx_flow_init` stores zero to it on PI0 (`0xe8`) and PI1 (`0x108`) of
  every node, so IRIX runs doubleword mode.
- **Unknown:** the reset value.

#### Return routing: the graphics node-widget map

- **Established:** the map's layout (widget 3:0, PI 4, node 12:5, processor
  slot 16; `hubio_next.h` `IIO_IGFX_INIT`), and that it matches write
  responses from a graphics widget to the processor that started the
  operation and returns them to that processor's interface as credits
  (`hubio.h` line 1292). IRIX's attach writes the slot, the interface (slice
  bit 1), the attaching processor's node and the pipe's widget number into
  the entry the flow owns on the pipe's master node (`0x648`); detach writes
  zero (`0x6c4`).
- **Unknown:** what the I/O interface does with a response from a widget no
  entry names. The SN1 map has no valid bit, so a zeroed entry names widget
  0, node 0, slot A.

#### Local-form dependence

Every per-slot access in IRIX's flow control, including the interrupt-modify
clear, uses the local form with only the slot bit, never the interface. For
a processor on PI1 that is correct only if the local form reaches the
requesting processor's own interface, the consumer expectation this
datasheet already records as open under
[two processors per interface](#behavioral-contract). IRIX 6.5.30's flow
control is a second consumer that depends on it.

#### What the model must do

This is the contract for the live counter, stated so a test can fail it.

1. Store the block on both interfaces at the field widths above and reset
   it to zero (model choice). Page bit 40 clear means nothing is counted.
   Store the credit-mode bit.
2. A processor store of up to eight bytes through the uncached I/O space,
   issued by the processor in slot S of interface P, whose physical address
   bits 39:17 equal the page bits 39:17 of slot S on interface P while page
   bit 40 is set, decrements that slot's counter by one. The count belongs
   on the issuing processor's own I/O path, where the Bedrock sees the whole
   physical address and the slot; a head's register handler sees neither.
   The slot and interface are the ones the CPU-number register reports to
   that processor, so a processor on another node is charged on its own
   Bedrock. Stores through the other uncached spaces are not counted. The
   processor model marks uncached and uncached-accelerated stores alike, so
   both count; which one IRIX's drivers use for the page is unknown. The
   model has no write gatherer, so every store is a single command.
3. A credit returned by a widget goes to the node, interface and slot the
   map entry for that widget names, and increments that slot's counter by
   one per credit. The node may be another node, whose Bedrock is found by
   its node number, but only this node and the Bedrock at the far end of
   its cable are reachable: a return naming any other node is dropped and
   logged as a guest error. If both entries name the widget, the first (`0x400140`)
   wins (model choice). No matching entry: drop and log a guest error. The
   model routes by the map alone and checks nothing about the widget.

   **Over-return and the teardown risk.** A return is simply added: the
   model does not track how many credits a slot was charged and never caps
   a return at that number or at the driver's total, which IRIX would read
   as "stuck above maximum" and tear the pipe down. What keeps the count
   in bounds is the head rule: a head returns credit only for a store the
   issuing processor's interface marked as counted, so a store its page
   did not cover earns nothing (model choice; whether hardware returns a
   credit for an ordinary uncached write to the pipe is unknown, and the
   SN1 comment distinguishing graphics writes from ordinary ones makes
   "no" the **inferred** reading). IRIX's own sequence then cannot
   over-return: attach enables the page before it writes the map entry and
   detach zeroes the map entry before it disables the page, so no store is
   counted while its credit has nowhere to go, and a store before attach
   or after detach (page `0xdeadbabe`, bit 40 clear) is neither charged
   nor repaid. An over-return needs the map to name a slot other than the
   one charged, which IRIX never programs because attach writes both for
   the running processor; the model would then add the credit to the
   named slot and log nothing.
4. A guest write replaces the count. A read returns the live count.
5. Below zero or above `0xfff` the counter wraps and a guest error is logged
   (model choice). The model never clamps at the driver's total.
6. Doubleword and transaction mode count alike: one per store, since the
   model forms no multi-doubleword transactions.
7. The stall and the graphics interrupt are not modelled. A counted store
   that leaves the counter below the bias register logs `LOG_UNIMP` once per
   slot. Synchronous heads never reach this.
8. Migration carries the block, the credit mode and the map.

**What the model does.** The model follows the contract above. Both
interfaces carry the block at the field widths given and the credit-mode
bit, all reset to zero; the mode is kept and changes nothing, since the
model counts one per store in either mode. Each fitted slot has a region
of the Bedrock over its processor's uncached view of the I/O space, a view
the node builds for every processor in its default mode (the PROM data
filter's fast mode builds it itself, and that mode currently aborts at
startup, a separate problem); there an uncached I/O store of up to
eight bytes into the slot's enabled page costs the slot one credit, and the
store goes on to its target marked as counted or not. A graphics head
returns credits by widget number, only for stores marked as counted; the
mark is one-shot, so a store split into two callbacks is repaid once, and a
head that asks off the storing processor's thread is told no and a guest
error is logged. The node-widget map sends them to the named interface and slot, on this
node or on the Bedrock at the far end of its cable when that carries the
named node number; any other node number is dropped with a guest error.
The counter wraps with a guest error at either end, and a counted store
that leaves it below the bias register logs, once per slot, that the stall
and the graphics interrupt are not modelled. The block, the mode and the
map migrate. No graphics head returns credits yet: until the Odyssey and
InfiniteReality models adopt the interface, a guest's count holds what the
driver wrote and the guest runs without backpressure.

## Behavioral contract

**What a guest does, in order.** OpenBSD (`ip27_machdep.c` lines 148 to 360): reads
its slice from CPU number; during its node walk reads region present and
cached-alias size through the local form and only prints them (`sginode.c` lines
105 and 107); writes all four masks of PI0 to zero and reads both
pending banks, then the same for PI1 through the node-named window on an IP35
machine; writes region present all ones and cached-alias size zero to both interfaces;
computes the crosstalk interrupt-send address from the pending-modify register's
node-named address; launches the other processors of its node through the PROM
entry; then at every priority change writes the running processor's two masks and
reads the pending banks back. At run time a widget interrupt is cleared by a modify
write of its bit through the processor's own interface followed by a pending read,
and an inter-processor interrupt is sent by a modify write of `0x100 | (7 + slice)` to
the target's interface. The out-of-tree MIPS port additionally clears pending 0 bits 0
to 12 and 63 and pending 1 bits 39 to 63 at init, enables both real-time interrupts,
zeroes the counter, and arms compare A for every clock event (`ip35-init.c` lines 29
to 40; `ip35-timer.c` lines 37 to 68, 171 to 177). The Itanium port zeroes all eight
masks of both interfaces and clears every set pending bit, clears the error registers,
and, when blocking an interrupt, spins until the mask reads back the written value
(`ml_SN_intr.c` lines 614 to 616): a mask that discards writes hangs it.

**The pending-modify contract.** A write with bit 8 set sets the named bit, without
it clears; bit 6 chooses the bank. Bits 54, 55, 62 and 63 of bank 1 cannot be changed
this way. Whether a crosstalk write with bit 8 clear can clear a bit is not excluded
by the header. A crosstalk write to the node-named address `0x1800090` (bit 47, node at
39) carries the same value word a processor would write.

**Lines.** Each processor's level 2 line follows pending 0 and its mask 0; level 3
follows pending 1 and mask 1 (with the remap for the low half); the timer line follows
the real-time pending bit and enable; the error line follows the error pending bits
and mask. All level-triggered, recomputed on every change, as the Origin 200 model
does.

**Two processors per interface.** Which mask pair a processor uses is its slot bit;
which interface, its slice's bit 1. Every consumer selects by those bits. The
crosstalk local form resolving to the requester's own interface is a consumer
expectation (OpenBSD dispatches through it from either interface's processors)
that no header states. For the two-CPU A/C machine, a PI1 requester must read its
own CPU number as slice 2 through the crosstalk local form, whereas node-named PI0
and PI1 accesses stay distinct. This is an emulator approximation pending a
hardware comparison of local and node-named reads from slice C, not established
Bedrock decoder behavior.

**sMDK reaches its L1 UART through the local Hub-special register window.** sMDK,
the Origin 300 field-diagnostic package's diagnostic monitor, is confirmed running
against this emulator to reach its L1 UART through the local Hub-special register
window (the [L1 controller datasheet](l1-controller.md#register-map) has that
evidence). No consumer, public or inferred, reaches PI through either Hub-special
register-window form (local or remote), and the [address
map](ip35-address-map.md#hub-special-space) records that neither window is a view
of this register file: the offsets that would be interrupt mask 1 B and the
cross-call-pending registers there are the per-processor LEDs.

**Pending bank 0 bit 4 now reaches both interfaces ("hw/mips/sgi/sn1:
deliver the L1 UART interrupt to the CPU", QEMU PR #665).** The register
map above already named this bit UART from the public SN1 header; nothing
modeled its assertion until this L1 UART receive/THRE interrupt gap left
sMDK unable to accept typed input at its own
`smdk>` prompt (the [L1 controller datasheet's interrupt
section](l1-controller.md#interrupts) has the full account). The model now
takes the UART's existing standalone IRQ output through a new "uart-irq"
input on the `sgi-bedrock` composite device and OR's it into bit 4 of both
PI0's and PI1's own pending bank 0, the same broadcast this device's
`sgi-bedrock-pi.c` already uses for local-block, network-interface and
crossbar errors reaching both interfaces from their own single node-wide
source. No public source establishes whether real silicon broadcasts this
line to both interfaces or targets one; the broadcast is carried here as an
emulator approximation by structural precedent, not an observed fact.

**PROM startup consumer.** PROM 6.210 reads a separate startup scratch word for
each A-to-D position before its failed-start recovery. A value of one means that
position started; zero enters recovery. This predicate does not read the
processor-number register or another presence field. The observation establishes
the firmware consumer, not a reset value or a physical presence contract for the
processor interface. In particular, an emulator must not report that absent B and
D processors started merely to avoid the recovery path. The two-CPU model instead
uses the reviewed CPU boot-flash state (citation: `docs/datasheets/ip35-boot-flash.md#two-cpu-population-environment`)
that the PROM itself writes after repeated failed starts.

## Interrupts

Covered above. The Bedrock has no separate interrupt controller; the pending banks
and masks are this unit's, as on the Hub.

## DMA

None in this unit.

## Remaining implementation and qualification

Protection and permission enforcement, coherency-pending behavior, graphics
flow control (counting and return are modelled; the stall and the graphics
interrupt are not), performance counters,
error spooling and injection remain unqualified beyond the specifically
implemented behavior. Permissive register values do not
qualify protection checks. Complete the required behavior and tests before
claiming firmware or SGI diagnostic coverage. Undefined offsets `0x80` and
`0x88` are retained Itanium consumer observations, not cleared MIPS registers.

## Behavioral notes

**Reset state is a hypothesis.** The public header gives no reset value for any PI
register. A model that starts with masks zero, pending banks zero, region present all
ones, cached-alias size zero, protection registers all ones and the counter running
satisfies every public consumer; the stock PROM trace will correct it. The only reset
observation the family has is on a different chip: the reviewed Origin 200 Hub
datasheet found its interrupt-write protection resetting to zero
(`hub-processor-interface.md`), so "protection all ones" is a hypothesis, not a
family rule.

### Selected Origin 300 PI1/A boot grant

PI1/A initial enable on the selected two-CPU A/C Origin 300 is a model
approximation. The [published Linux SN1 PI header](https://git.kernel.org/pub/scm/linux/kernel/git/history/history.git/tree/include/asm-ia64/sn/sn1/hubpi.h?h=v2.5.40)
states that a clear CPU-enable bit denies that processor a bus grant and that
the register survives soft reset. It gives no power-on or cold-reset value.
The [observed IP45_2CPU inventory](https://nekonomicon.irixnet.org/forum/14/16726611/1.html)
establishes a fitted slice C, not its reset enable bit. In two owner-managed
runs with the same QEMU source
`9b0eab45d56fb4418d7f8cf53af658ed48879426`, the unmodified run
`prom-20260915T085740-7783d6` reported one CPU enabled and C failed early
init. In the recorded managed intervention (citation: `docs/boot-logs/firmware.boot.2026-09-15T02:01:45-07:00`)
(`prom-20260915T090144-b4ff2e`), a managed debugger wrote and read back
PI1/A CPU-enable 1 before the CPU-population phase. The console reported
two CPUs enabled and `ALL DIAGS PASSED`. These runs constrain this emulator's
boot state. They neither establish the silicon's initial bit nor show that
firmware never writes PI1/A later.

The two-CPU A/C QEMU configuration initializes PI0/A and PI1/A CPU-enable to 1
when Bedrock is realized, and empty PI0/B and PI1/B remain disabled. Before
2026-09-25 PI0/A reset to 0 and took its enable from the PROM store loop described
below. Guest writes and migration override the
initial value. `sn1/address-map/origin300-cpu-c-initial-enable` distinguishes
this narrow boot policy from enabling empty sockets. The CPU C migration check
loads an explicitly cleared value over the destination's initial grant. A
published cold-reset specification or measurement on a matching IP45_2CPU
board is still needed to replace the approximation. The passing emulator run
retains empty-socket B/D PLL retries and a configuration warning. It is
not IRIX or full SGI diagnostic acceptance.

A four-processor Origin 300 fits every slot on both processor interfaces.
Intervention run `prom-20260918T174912-4df6ba`, at QEMU `4ff73490e9` with
test-only trace events, traced every processor-interface write and every
CPU-enable read across a four-processor boot. It recorded the boot master's
register store loop (PROM listing base `0xc00000001fc00000`, loop `0x1fc66164` to
`0x1fc66228`) as writing interface 0's CPU-enable A and B, and no write to either
CPU-enable register of interface 1. A managed-debugger write of 1 to interface 1's
CPU-enable B, made before the CPU-population phase, gave "4 CPUs on 1 nodes found",
"No. of CPUs enabled = 4" and `ALL DIAGS PASSED`.

| boot flash | model grant | result |
|---|---|---|
| stock seed | C only (before QEMU change 694) | 3 CPUs, CPU D disabled |
| stock seed | C and D (694) | 3 CPUs, CPU D disabled |
| disable records erased | C only | 3 CPUs, CPU D disabled |
| disable records erased | C and D (694) | 4 CPUs, `ALL DIAGS PASSED` (three runs) |

That loop does not write the processor interface. It stores to the Hub-special
remote register window at `0x9000_0000_3000_0000 + (node << 33)`, offsets `0x00`
to `0x78`; the model used to pass those offsets through to this register file,
which is how interface 0's CPU-enable registers came to hold the loop's last
value, `0x29` (bit 0 set), along with `0x2b` in region present and cached-alias
size. The [address map](ip35-address-map.md#hub-special-space) infers that the
window is not a view of this register file. Read that way, the stock PROM's boot
path never sets a CPU-enable register: its only CPU-enable stores there write
zero, to disable a partner that fails the early liveness check (listing
`0x1fc68944`, `0x1fc69328`, `0x1fc693d8`). Its interactive monitor can set or
clear one, through the node-named form, only on an operator's CPU enable or
disable command (the routine at `0x1fc64300`, store at `0x1fc64588`). With the pass-through removed and only interface 1 granted, five
four-processor boots on 2026-09-25 each reported "2 CPUs on 1 nodes found" from the
BaseIO PROM and then a TLB refill exception, and three of them first printed
`peer_pled failed: Other CPU on this node died.`: the two processors on interface 0
were not counted (runs `prom-20260925T165103-9c19af`, `prom-20260925T165840-759e3a`,
`prom-20260925T165903-8f1066`, `prom-20260925T165948-c05ec0`,
`prom-20260925T170028-d67f0c`; the model code of QEMU `84fc4a5f07`, branch
`o300-led-decode-evidence-no-pi0-grant`, built from an uncommitted tree that each
`run.json` records as `32e4238a6e`, dirty).

The model rule is now that every fitted slot, on both interfaces, is granted at
reset, and empty slots are not. After that change, three four-processor boots
each reached "4 CPUs on 1 nodes found" and "No. of CPUs enabled = 4" (two with
`ALL DIAGS PASSED`, the third with a memory-bank self-test failure reported
against bank 3, a failure other runs showed before this change, for example
`prom-20260923T160441-04bc5c`), and two two-processor boots reached "2 CPUs on 1 nodes found"
and `ALL DIAGS PASSED` (runs `prom-20260925T171117-c622aa`,
`prom-20260925T171139-02bb1a`, `prom-20260925T171145-5ad32c`,
`prom-20260925T171155-d981ed`, `prom-20260925T171203-8c0649`; the model code of
QEMU `0611190532`, branch `o300-led-decode-evidence-pi0-grant`, which differs from
the tree those runs were built from only in comments). This remains a
model approximation: the silicon's cold-reset value for CPU-enable is still
unknown, and a firmware that never sets the bit is evidence that fitted
processors come out of reset granted, not a measurement of it.
`sn1/address-map/origin300-four-cpu-initial-enable` and
`sn1/address-map/origin300-cpu-c-initial-enable` pin this reset state as qtests.

**The NMI registers (`0x70`, `0x78`).** The header ties a write to a per-processor
non-maskable interrupt but names no trigger predicate and no reset or programmed
value. No cited consumer stores to them through the crosstalk form. An earlier
revision of this datasheet cited the stock PROM's per-node store loop (PROM 6.210,
listing base `0xc00000001fc00000`, loop `0x1fc66164` to `0x1fc66228`) as a store of
`0x100` to both NMI registers. That loop writes the Hub-special remote register window at
`0x9000_0000_3000_0000 + (node << 33)`, offsets `0x00` to `0x78`, which the model
then passed through to this register file; the address map infers that it is not
a PI access at all (the
[address map](ip35-address-map.md#hub-special-space) and the measurement
notebook (citation: `docs/notebooks/origin300-hub-special-led-decode-20260925.md`) have the
decode). The trigger predicate stays unknown, so the model delivers no NMI from a
store here, logs the store as unimplemented, and takes the L1 controller's external
assertion (its `nmi` command and the front-panel button, per the L1 datasheet) as
the NMI path. Settle the predicate by observing what drives an NMI on real hardware.

**Conflicts, each with its settling observation.**
- Which processor line the real-time compare raises: the header says level 4 at the
  compare and level 5 at the pending register; both consumer headers say level 4
  (interrupt 2), and linux-ip35 dispatches its timer there. The model raises hardware
  interrupt 2, as its choice, labelled so. Settle by arming the compare on real
  hardware and reading Cause.
- The correctable-error bits: consumer headers put A at 59 and B at 60, the generated
  union the reverse. Naming only; no consumer distinguishes them.
- Bit 58 is named clock error, performance-counter overflow and real-time-clock
  dropout in three places; the local-block datasheet's clock-loss error is the same
  event.
- The counter frequency: the SN1 header's default is 1,250,000 Hz and the firmware's
  configuration block carries the value the PROM believes; both MIPS consumers assume
  1,250,000 Hz, the linux-ip35 author calls the value a hack, and the local block
  makes the period programmable. Settle by reading the held PROM's configuration-block
  frequency field, then by timing the counter against the processor's count on real
  hardware and recording the local block's clock control register.
- Whether the crosstalk local form addresses the requester's own interface: settle by
  reading CPU number through the local and both node-named windows from a processor
  on PI1.
- What the silicon's protection, region-present, cached-alias and CPU-enable registers
  hold at hand-off, now that the PROM store loop once read as writing them is known to
  address the Hub-special window instead: settle by reading them at PROM hand-off
  before a guest writes them. Until then the model keeps the cached-alias size across
  every reset, a system reset included, as it keeps region present and the protection
  registers: a model choice, since the value at power-on is unknown. On the
  two-chassis Origin 300 the size bounds the processors' cached-alias window, so a
  value left from before a reset stays in force until the guest writes it.

**Consumer defects, recorded so nobody encodes them.** linux-ip35 clears a bank-1
interrupt with a bank-0 value (`ip35-irq.c` line 171); the Itanium error handler
writes complemented masks to the modify register (`error.c` lines 74 to 177). Neither
is a specification.

## qtest checklist

- CPU number read through PI1's node-named window returns bit 1 set and the slot in bit 0; the test states which slot its accesses are taken to come from.
- CPU enable A and B retain writes. CPU present A reads 1 for a configured processor and 0 for an unpopulated slot at reset: the model's presence rule, since a qtest issues no processor transaction.
- Region present and cached-alias size retain writes; the size field is four bits wide.
- Pending 0 and pending 1 are read-only: a direct write to `0x98` or `0xa0` changes nothing.
- A modify write of `0x107` sets pending 0 bit 7 and `0x007` clears it; `0x14a` sets pending 1 bit 10 and `0x04a` clears it; `0x177` does not change pending 1 bit 55, and neither does a direct write; the alias at `0x180` behaves identically.
- The remap register retains bits 4:0.
- Each mask retains a write; with pending 0 bit 7 set and mask 0 A bit 7 set, processor A's level 2 line asserts and drops when the mask is cleared; the same for B with its pair and for PI1's processors.
- The real-time counter takes a written value and increases from it between reads, and PI1's copy reads consistently with PI0's.
- Compare A retains 55 bits; with enable A set and the compare passed, real-time pending A sets and the timer line asserts on hardware interrupt 2, the model's choice while the header conflict is open; any store to real-time pending A clears its bit and the line follows.
- Profile compare retains 32 bits and a match sets both profile pending registers.
- A direct write to `0x3f8` sets error-pending bits; a write of the same bits to `0x400` clears them.
- Error interrupt masks retain 35 bits; the error stack address keeps bits 32:3.
- On the two-processor A/C population, CPU enable reads 1 for PI0/A and PI1/A and 0
  for PI0/B and PI1/B after reset; on the four-processor population all four read 1.
  Both pin the model's reset grant, not a measured silicon value.
- A crosstalk-form store to NMI A or B delivers no NMI, the model's choice while the trigger predicate is unknown; the modelled NMI line is asserted by the L1 controller.
- With CPU protect's bit for the test's region first cleared and read back clear, the protection override with the model's chosen pattern (the local block's, the PI pattern being open) restores it; a wrong pattern does not.
- CPU protect, I/O protect, region present, cached-alias size, CPU enable, the error stack address and its size keep their values across a network-interface local reset, the model's equation of that reset with the header's soft reset.
- A crosstalk write of `0x100 | n` to the node-named modify address sets pending 0 bit n and a packet aimed at another node arrives nowhere.

Graphics flow control. Counted stores come from processors running a small test
firmware image: a qtest access has no processor and does not pass the processor's
I/O path, so it is never counted. Register setup and reads may use plain qtest
accesses. The head is a qtest-only stub that returns one credit per counted store it
takes.

- On PI0 and PI1 the credit counter and bias keep bits 11:0 of a write, the
  interrupt counter and compare bits 25:0, the page register bits 40 and 39:17, and
  the credit mode bit 0. All read zero after reset.
- With PI0 slot A's page set to a head's command-port page with bit 40, the counter
  at 100 and the map's first entry routing that head's widget to PI1 slot A, one
  store to the port leaves PI0 `0x308` at 99 and PI1 `0x200308` 1 higher. The map's
  PI bit alone moves the return between interfaces, and the test separates the
  decrement from the return, which a synchronous head otherwise makes invisible.
- The same with the map routing to PI0 slot B moves the credit to `0x330`.
- With the map routing to PI0 slot A, a burst of stores leaves `0x308` where it
  started: every counted store's credit comes back.
- A store from PI0 slot A while its page bit 40 is clear, or to an address whose bits
  39:17 differ from the page, leaves `0x308` unchanged: the store is not counted.
- A head that returns one credit for each counted store it takes leaves the counter
  where it started over a burst into its port, and a store into its port while the
  page is disabled neither costs nor returns a credit.
- A store through an uncached space other than I/O to an address inside the page is
  not counted.
- On the two-chassis machine, with the head's master node's first map entry
  naming node 1, PI0, slot A, and node 1's page covering the port, a store
  from a node 1 processor charges node 1's `0x308` and the return lands
  there; node 0's counters do not move. With the entry naming node 1 while a
  node 0 processor issues, node 0 loses the credit and node 1 gains it.
- With both map entries naming the head's widget, the first entry's slot gets
  the credit (model choice).
- A map entry naming another widget routes nothing: the store's slot loses one
  credit, no slot gains one, and a guest error is logged.
- A write of 50 to `0x308` followed by one routed return reads 51.
- A counted store with the counter at 0 reads `0xfff` and logs a guest error (model
  choice); a return past `0xfff` wraps the same way.
- A counted store that leaves the counter below the bias register sets neither
  pending 0 bit 1 nor bit 2 and logs the unmodelled stall once (model choice).
- Stores to the credit mode at `0x20068` and `0x220068` are kept and change no
  count.
- The block, the credit mode and the map survive migration with their values.

## Open questions: graphics flow control

- **The stall and interrupt trigger.** The header gives three thresholds (credit
  below the bias, at or below it, at or below zero) and no clock for the interrupt
  counter. A published Bedrock specification would settle it. Short of that, a
  physical test on an Origin 300-class system with a graphics pipe, which needs the
  owner's agreement first: attach a flow, set the bias above the count, stop the
  pipe from draining, and read the interrupt counter, pending 0 bits 1 and 2 and the
  counter at intervals.
- **Counter wrap and reset values.** Whether the twelve-bit counter wraps,
  saturates or carries a sign, and what the block and the credit mode hold at
  reset. A read before IRIX's flow initialisation and a count of 1 followed by two
  counted stores settle both.
- **The local form on PI1.** Already open above; a read of CPU number through the
  local form from a PI1 processor settles it for flow control too.
