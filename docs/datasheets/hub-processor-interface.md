# Datasheet: Hub processor interface

References rendered as citation identities name material outside this document
set. Those targets are not included or cleared for redistribution. Review and
runtime statements retain their recorded dates and scope.

**Status:** register map reviewed (coordinator with codex, 2026-08-09).
Nonzero PI error latching is blocked: the stock-PROM sequence is captured, but
the value real Hub silicon latches for that fault is not established.
The [graphics flow-control section](#graphics-flow-control), added 2026-09-27
and rewritten 2026-09-28 as the live credit contract, is not yet
independently reviewed.
**Fork file(s):** `hw/misc/sgi-hub.c` — the interrupt registers, the real-time
counter and its comparators, and the rest of this unit's register window all
live in the one Hub device. No separate interrupt-controller file exists or is
planned: the register window interleaves interrupt state with non-interrupt
state a split device could not cleanly own (the counter that raises hardware
interrupt 2 is also just a timer), and a second sysbus device bought nothing
against this project's own reset lesson — a device with no bus of its own
never gets reset, and answers with logic before it has ever received one.

**Sources used:** the IRIX 6.5.7 machine-readable Hub register table, together with
the table's own compiler and access-mode dictionary and the IRIX SN0 Hub
processor-interface register header; Linux `arch/mips/include/asm/sn/sn0/hubpi.h`,
`asm/sn/intr.h`, `asm/sn/sn0/hubio.h`, `arch/mips/include/asm/mach-ip27/irq.h`,
`arch/mips/sgi-ip27/ip27-irq.c`, `ip27-timer.c`, `ip27-init.c`, `ip27-berr.c`,
`ip27-memory.c` and `arch/mips/pci/pci-xtalk-bridge.c` (public, GPL); OpenBSD
`sys/arch/sgi/xbow/hub.h`, `sys/arch/sgi/sgi/ip27_machdep.c` and
`sys/arch/sgi/xbow/xbridge.c` from the `the-machine-hall/openbsd-sgi` tree
(public, ISC). OpenBSD retired its SGI port, so those files exist on no
checked-out branch: the reference clone's working tree holds only a README, and
the mainline OpenBSD clone beside it has no SGI architecture directory at all.
Read them without checking anything out, and **cite the commit rather than a branch**:
`git -C refs/openbsd-sgi show 2df91d80c1:sys/arch/sgi/sgi/ip27_machdep.c`. That commit is
the head of the branch matching OpenBSD 7.9 GENERIC-IP27, the kernel this project boots,
and everything here was read from it.

**This page previously described how that branch differs from the tree's other branch, and
the description was wrong by the time anyone could check it.** It said two files differed.
**Thirty-seven do** under the SGI architecture directory, including the one this datasheet
is built on. The differences it listed were real, and the count was not — a branch advances,
and a sentence comparing against one is out of date the moment it is written, without
anything marking it.

**The failure mode is worse than staleness: the comparison named no commits, so a reader
could not check it and could not tell that they could not.** That is why the citation is now
a commit. It also removes the need for the comparison at all — a pinned commit is the whole
of what a reader needs, and the reassurance that some other branch agrees was only ever
answering a question the pin makes moot.

For the record, since the original claim is the kind someone may have relied on: the file
this page most depends on does differ between those two branches, in a timecounter field, a
removed deferred-clock-interrupt block, and a change to how a secondary processor enters the
scheduler. **The deferred-clock block is not purely timekeeping** — it concerns delivering a
clock interrupt that was masked, which is this unit's own interrupt behaviour. Nothing cited
on this page rests on it, but the earlier sentence filed it under timekeeping and that
understated where it sits. Also used: the
measured real-time counter
rate recorded in
the firmware boot analysis (citation: `docs/research/11-firmware-and-boot-entry.md`). We only
read reference sources and never copy their code or file structure
(source-use policy, decision D-0002).

**Facts that are IRIX-derived.** Every reset value and every per-field access
type in the register map below comes from the IRIX machine-readable register
table, and nothing public carries either. The register offsets, the bit
assignments of the two interrupt-pending registers, the processor-line mapping
and the whole behavioural contract are dual-sourced against Linux and OpenBSD;
where the IRIX table and a public header disagree, the disagreement is called out
in the notes rather than silently resolved.

**How to read the access column.** The IRIX table's own dictionary defines the
codes, and this document uses them unchanged: *readable*; *writable*;
*readable and writable*; *writable, value ignored*, meaning the write is a
trigger and the data is discarded; *special action occurs on write*, meaning the
write does something other than store the data; *readable, and a write clears
every bit written as one*; and *readable, clears on read*. A dash in the reset
column means the register table declares no defined value at reset.

**One case needs stating, because the register table appears to contradict these pages and
does not.** Every protection-override register in the chip — this unit's, the I/O
interface's, the network interface's and the memory unit's — carries the unlock string
`0x5347_4972_756c_6573` in the table's **reset** column, asserted rather than dashed. These
datasheets show no reset value for them, and that is correct: **the field's access type is
*special action occurs on write*, so it has no readable state for a reset value to describe.**

**What the column holds there is the value to write, not a state to read.** The reading is
settled by the table's own contents rather than by argument: other fields with the same
access type — the interrupt-modify register's, the timeout register's — carry **zero** in
that column, because no single value is the one you write to them. Where exactly one value
is meaningful, the table records it. **And a protection mechanism that powered up already
holding its own unlock string would be no protection at all.**

So a comparison against the register table will flag these four as missing reset values.
**They are not missing; they are the one place the column means something else**, and these
pages say *none, the field is write-only* rather than a dash so that the distinction is
visible without consulting this paragraph.

## Role

The processor interface is the face the Hub turns towards the two R10000
processors on a node board. It does three jobs that the first operating-system
boot depends on and a handful more that it does not.

It owns the machine's real-time counter: a free-running 52-bit counter ticking at
1.25 MHz, one per node, with a comparator for each of the node's two processors.
Every Origin operating system uses it, either as its clock source or as its
timekeeping reference, and it is the only timer on the machine that is not
internal to a processor.

It is the machine's interrupt controller. Everything that is not generated inside
a processor — every device behind the I/O bridge, every cross-processor call,
every Hub error — arrives as a bit in one of two 64-bit pending registers, is
gated by a per-processor mask, and comes out on one of the processor's hardware
interrupt lines. Devices do not have wires to the processor: a device interrupt
arrives at the Hub as a *write* to a register in this unit, sent through the
Hub's I/O interface and cross-connected queues by the I/O bridge.

This ownership stops at the pending banks. The Hub I/O interface owns the
crosstalk path for device and Hub-I/O causes, and the Hub network interface owns
remote-node transport. The processor interface records and delivers the resulting
interrupt write. It does not choose the source route or perform endpoint DMA.

### Message path through the Hub

Each of the Hub's four interfaces has an input FIFO and an output FIFO. Each
FIFO is logically split again into a request queue and a reply queue. The
interfaces fan these queues out to one another. The manual draws no separate
internal crossbar block, so this datasheet treats the queue connections as the
contract and does not invent a fifth Hub unit.[^theory-hub-pi]

```mermaid
flowchart LR
    CPU[R10000 processor] --> PI[Processor interface]
    PI --> PIIN[PI input FIFO<br/>request | reply]
    PIIN --> IIOUT[II output FIFO<br/>request | reply]
    IIOUT --> II[I/O interface] --> XIO[Local XIO device]
    XIO --> II --> IIIN[II input FIFO<br/>request | reply]
    IIIN --> PIOUT[PI output FIFO<br/>request | reply]
    PIOUT --> PI --> CPU
    PIIN -. memory request .-> MD[Memory/directory queues]
    PIIN -. remote request .-> NI[Network-interface queues]
```

For local programmed I/O, the processor interface converts the processor's
transaction into the Hub's internal request form. The request crosses to the
I/O interface, which converts it to XIO. The response returns on the separate
reply path. Empty FIFOs may bypass storage to reduce latency. Keeping request
and reply capacity separate is part of the deadlock boundary, not an optional
performance optimization.

It also records processor-bus errors — parity, timeouts, uncorrectable data — in
a set of status registers and, optionally, spools them into a ring in memory.
Neither Linux nor OpenBSD relies on that machinery working; both disable it early.

For a first banner on a synthesised firmware, none of this needs to be live: the
banner is printed through a firmware call that our model implements as a C
function. This unit becomes load-bearing one step later, when a real device
interrupt has to reach a real driver, which is the classic place an emulated
machine boots and then hangs. Modelling it explicitly and testing it before any
device depends on it is the whole point of writing this down now.

## Register map

All offsets are from the base of the Hub register window, which the processor
reaches at its own node's crosstalk I/O base plus `0x0100_0000` or at a named
node's crosstalk I/O base plus `0x0180_0000`. Every register is 64 bits wide and
every access is a 64-bit load or store; the guests use nothing narrower.

Registers whose name ends in A belong to the first processor of the node and
those ending in B to the second. A processor learns which of the two it is by
reading the processor-number register.

### Real-time counter and profiling timer

| Offset | Name | Width | Reset | Access | Behavior as guest drivers use it |
|---|---|---|---|---|---|
| `0x03_0100` | Real-time counter | 52 | 0 | readable and writable | Free-running upcounter at 1.25 MHz. Linux zeroes it at boot and then reads it as its clock source and its scheduler clock. OpenBSD reads it as a timekeeping reference and never writes it. |
| `0x00_0108` | Counter compare, processor A | 52 | 0 | readable and writable | When the counter reaches this value the counter interrupt for processor A becomes pending. |
| `0x00_0110` | Counter compare, processor B | 52 | 0 | readable and writable | The same for processor B. |
| `0x00_0118` | Profiling compare | 32 | 0 | readable and writable | Raises the profiling interrupt on both processors when it matches the counter. |
| `0x00_0120` | Counter interrupt pending, processor A | 1 | 0 | readable and writable | Set by the comparator match. The handler acknowledges by writing zero. |
| `0x00_0128` | Counter interrupt pending, processor B | 1 | 0 | readable and writable | The same for processor B. |
| `0x00_0130` | Profiling interrupt pending, processor A | 1 | 0 | readable and writable | Same shape as the counter pending bit. |
| `0x00_0138` | Profiling interrupt pending, processor B | 1 | 0 | readable and writable | The same for processor B. |
| `0x00_0140` | Counter interrupt enable, processor A | 1 | 0 | readable and writable | Gates the counter interrupt onto that processor's line. Linux writes one here for both processors during Hub initialisation. |
| `0x00_0148` | Counter interrupt enable, processor B | 1 | 0 | readable and writable | The same for processor B. |
| `0x00_0150` | Profiling interrupt enable, processor A | 1 | 0 | readable and writable | Linux explicitly writes zero here for both processors. |
| `0x00_0158` | Profiling interrupt enable, processor B | 1 | 0 | readable and writable | The same for processor B. |
| `0x00_0160` | Counter clock control | 17 | see note | mixed | Bit 16 selects the Hub's internal clock rather than the network-wide clock and resets set. Bit 15 and bits 14:8 observe the network clock and are read-only. Bits 7:1 hold a maximum count and bit 0 enables the network clock, both writable. |
| `0x00_0168` | Counter clock filter control | 41 | 0 | mixed | Counts and masks glitches on the network-wide clock. Multi-node machinery only. |

The counter's rate is 1,250,000 Hz, and its supports are of three different
strengths rather than three of a kind.

- **The firmware's own arithmetic, which is the strongest of them.** Its time
  helper converts a counter reading by shifting the value left by three and
  dividing by ten — a multiply by 0.8, which is the 800 ns tick expressed as
  arithmetic the firmware performs rather than as a constant somebody wrote down.
  Read from the firmware's own instructions by the executed-trace stream. It is
  still software's treatment of the rate rather than the hardware answering for
  itself, but it is the vendor's own firmware for this platform and not a third
  party's belief about it.
- **The configuration block in the shipped 6.5-base IP27 firmware image states
  1,250,000 Hz.** The image is the platform's, not a board's.
- **OpenBSD registers its Hub timekeeping source at exactly 1,250,000**, in
  `sys/arch/sgi/sgi/ip27_machdep.c` on the 7.9 release branch. **Do not read this
  value off a `tc_frequency` search alone**: the same tree sets `tc_frequency` to
  12,500,000 in `sys/arch/sgi/xbow/xheart.c`, ten times larger, for a different chip
  on a different machine.

**Linux's 800 ns tick is not a fourth support, and an earlier version of this
passage counted it as one.** Linux does hard-code `NSEC_PER_CYCLE 800` in
`arch/mips/sgi-ip27/ip27-timer.c`, which is the same number. But the comment
immediately above that constant calls the values a hack, says they really need to
be figured out dynamically, explains that 800 ns was chosen because it *works well
across* Hub frequencies of 360 to 400 MHz, and then asks outright which clock rate
feeds the counter. **A source that records not knowing the answer cannot
corroborate the answer.** Reading the constant without the comment inverts what it
is evidence of, which is why the comment is described here rather than cited by
line.

**Why the rate does not vary by board**, which is the question the firmware-image
wording used to leave open: it is a property of the Hub's counter, and the Hub is
one of the parts shared across the Origin 200, the Origin 2000 and the Onyx2.
OpenBSD's single family-wide kernel configuration fixes the frequency
unconditionally and never reads it from anywhere, which it could not do if the rate
were per-board.

An 800 ns tick and a 52-bit counter give a wrap period of a little over 114 years,
so nothing on our path will ever see it wrap.

**The clock control register is now implemented, as plain masked storage over its
writable bits.** Measured demand: real firmware writes it twice with two distinct
values, `0x10000` (bit 16 alone -- selecting the internal clock, which is also the
reset state) and `0x10047` (bit 16, a maximum count of 35 at bits 7:1, and bit 0
enabling the network clock). The read-only network-clock-observation bits (15 and
14:8) are outside the writable mask and always read zero -- this is a single-node
machine with no crosstalk-connected network clock to observe, the same reasoning
already applied to every other status field nothing on this model ever sets.

**This does not change the counter's own rate, and the three-way corroboration
above is why that is a defensible choice rather than an unexamined one.** The
counter still free-runs at the fixed 1,250,000 Hz `PI_RT_HZ` this datasheet already
established, regardless of what gets written here. Real firmware, OpenBSD and Linux
all assume that exact rate unconditionally, before any of them read this register
back -- so a write here changing the counter's actual frequency would contradict
all three at once, not just this datasheet. **What is not yet chased down**: whether
selecting the internal clock (bit 16) and the specific divisor (35) written to bits
7:1 together imply, from the Hub ASIC's own clock-generation logic, a rate other
than 1,250,000 Hz. That would need the Hub's internal clock-tree documentation, not
the guest-facing register table this page draws on, and nobody has gone looking for
it yet. If it turns out the divisor implies a different rate, this is a live timing
defect and not just an unmodelled control bit; until then, storing the value without
deriving behaviour from it is the honest state of what has actually been checked.

### Interrupt pending, modification and masks

| Offset | Name | Width | Reset | Access | Behavior as guest drivers use it |
|---|---|---|---|---|---|
| `0x00_0090` | Interrupt modify | 9 | 0 | special action occurs on write | Bits 6:0 name an interrupt number and bit 8 chooses what to do with it: one sets the corresponding pending bit, zero clears it. Numbers 0 through 63 address the first pending register and 64 through 127 the second. This is the only way to set or clear a software-allocated pending bit, and it is the register a device interrupt arrives at; the cross-call set and clear registers also drive first-bank bits 5 and 6. |
| `0x00_0098` | Interrupt pending, first bank | 64 | 0 | readable | Bit *n* is pending interrupt *n*. |
| `0x00_00a0` | Interrupt pending, second bank | 64 | 0 | readable | Bit *n* is pending interrupt *n* + 64. |
| `0x00_00a8` | Interrupt mask, first bank, processor A | 64 | 0 | readable and writable — see the conflict note | A one enables that pending bit to reach processor A. |
| `0x00_00b0` | Interrupt mask, second bank, processor A | 64 | 0 | readable and writable | The same for the second bank. |
| `0x00_00b8` | Interrupt mask, first bank, processor B | 64 | 0 | readable and writable | The same for processor B. |
| `0x00_00c0` | Interrupt mask, second bank, processor B | 64 | 0 | readable and writable | The same for processor B. |
| `0x00_00c8` | Cross-call set, processor A | 64 | 0 | writable | Raises a cross-processor interrupt at processor A. |
| `0x00_00d0` | Cross-call set, processor B | 64 | 0 | writable | The same for processor B. |
| `0x00_00d8` | Cross-call clear, processor A | 64 | 0 | readable and writable | Reads the pending cross-calls for processor A and clears them. |
| `0x00_00e0` | Cross-call clear, processor B | 64 | 0 | readable and writable | The same for processor B. |
| `0x00_00e8` | Cross-call mask | 64 | 0 | readable and writable | Gates the cross-call sources. |

Seven bits of the first pending bank and six of the second are fixed by hardware,
thirteen in all; the rest are assigned by software, which is why the firmware and
each operating system have to agree on a numbering convention rather than
discover one. Linux's Origin interrupt header names first-bank bits 0 through 6,
so software allocation in the first bank starts at bit 7 — a firmware or a model
that starts allocating at bit 6 collides with the cross-call bit for the second
processor.

| Bank and bit | Meaning |
|---|---|
| first bank, bit 0 | Reserved. Linux's own header asks, in a comment, what it is for. |
| first bank, bits 1 and 2 | Graphics interrupt from each of the two graphics channels. |
| first bank, bit 3 | Page migration. |
| first bank, bit 4 | The Hub's own serial port and node microcontroller. |
| first bank, bits 5 and 6 | Cross-call pending for processor A and processor B. |
| second bank, bit 58 | Clock dropout. The IRIX register table calls this a real-time-counter dropout and both Linux and OpenBSD call it a clock error; they are the same bit. |
| second bank, bits 59 and 60 | Correctable system error from processor A and from processor B. |
| second bank, bit 61 | Correctable memory or directory error. |
| second bank, bit 62 | Network interface error. |
| second bank, bit 63 | System shutdown request from the module controller. |

Software conventions layered on top of those: Linux uses first-bank bits 7
through 10 for its own reschedule and function-call cross-processor interrupts,
one pair per processor, and OpenBSD uses first-bank bits 7 through 10 the same
way for its inter-processor interrupts. OpenBSD then treats first-bank bits 11
through 63 and second-bank bits 0 through 35 as the pool it allocates device
interrupts from. The firmware reserves a block in the second bank for its own use
and the guests avoid it.

### Processor status and control

| Offset | Name | Width | Reset | Access | Behavior as guest drivers use it |
|---|---|---|---|---|---|
| `0x00_0000` | Processor protection | 64 | all ones | readable and writable | One bit per memory region; a zero bit stops that region's processors writing this Hub's registers. |
| `0x00_0008` | Protection override | 64 | **none — the field is write-only** | special action occurs on write | Writing the eight characters `SGIrules`, which is `0x5347_4972_756c_6573`, unlocks the protection registers. |
| `0x00_0010` | Interrupt-write protection | 64 | 0 | readable and writable | One bit per memory region; a zero bit stops that region writing the interrupt-modify register. |
| `0x00_0018` | Region present | 64 | all ones | readable and writable | Which memory regions exist. OpenBSD writes all ones during setup; Linux writes a computed mask with the low bit forced on. |
| `0x00_0020` | Processor number | 1 | 0 | readable | Which of the node's two processors is reading. Linux and OpenBSD both read this to pick between the A and B copies of everything else. |
| `0x00_0028` | Cached-alias size | 4 | 15 | readable and writable | Size code for the cached alias onto the reader's own node memory. OpenBSD writes zero; Linux writes zero on the boot node and a code meaning 8 KB on any additional node. |
| `0x00_0030` | Maximum coherent-request timeout | 8 | `0x80` | readable and writable | Coherence bookkeeping. |
| `0x00_0038` | Coherent-request timeout scale | 16 | 0 | readable and writable | Coherence bookkeeping. |
| `0x00_0040` | Processor A present | 1 | 0 | readable | Whether the first processor socket is populated. |
| `0x00_0048` | Processor B present | 1 | 0 | readable | Whether the second processor socket is populated. |
| `0x00_0050` | Processor A enabled | 1 | 0 | readable and writable | Whether that processor is allowed to run. |
| `0x00_0058` | Processor B enabled | 1 | 0 | readable and writable | The same for the second processor. |
| `0x00_0060` | Reply level | 64 | 0 | readable and writable | Coherence bookkeeping. The IRIX processor-interface header and Linux both give this offset; the IRIX machine-readable register table alone gives `0x01_0060`, and we take the header (source-authority decision). |
| `0x00_0070` | Non-maskable interrupt to processor A | 64 | 0 | writable, value ignored | The write is the trigger; the data is discarded. |
| `0x00_0078` | Non-maskable interrupt to processor B | 64 | 0 | writable, value ignored | The same for processor B. |
| `0x00_0080` | Soft reset | 64 | 0 | writable, value ignored | Resets both processors on the node. |
| `0x02_0068` | Reset-survivor flag | 1 | 1 | readable and writable | A bit hardware sets and software clears, used to tell a soft reset from a cold start. |

### Error and status

| Offset | Name | Width | Reset | Access | Behavior as guest drivers use it |
|---|---|---|---|---|---|
| `0x00_0400` | Error interrupt pending | 25 | 0 | see note | Bits 23 down to 0 pair off, odd bits for processor A and even for processor B, covering system-state tag parity, uncorrectable data on an uncached load, a failed error spool, command and address parity in both address and data phases, write-buffer write and timeout errors, a spurious message and a spool-count match. Each of those is readable, and writing a one clears it. Bit 24 reports an uncorrectable memory error, is read-only here, and must be cleared in the memory-directory unit instead. |
| `0x00_0408` | Error interrupt mask, processor A | 25 | 0 | readable and writable | Which error sources reach processor A's error line. Linux writes zero. |
| `0x00_0410` | Error interrupt mask, processor B | 25 | 0 | readable and writable | The same for processor B. |
| `0x00_0418` | Error spool address, processor A | bits 31:3 | 0 | readable and writable | Where in memory to spool error records. Linux writes zero. |
| `0x00_0420` | Error spool address, processor B | bits 31:3 | 0 | readable and writable | The same for processor B. |
| `0x00_0428` | Error spool size | 4 | 0 | readable and writable | Zero disables spooling; a code *n* means 2 raised to the power *n* plus eleven bytes, so code one is 4 KB. Linux writes zero to disable it. |
| `0x00_0430` | Error status 0, processor A | 64 | — | readable | Bit 63 valid, bit 62 overrun, bits 61:25 the failing address, bits 24:17 the message command, bits 16:6 supplemental data, bits 5:3 the request number, bits 2:0 the error type. |
| `0x00_0438` | Error status 0, processor A, clearing | 64 | — | readable, clears on read | The same register; reading it also clears it. |
| `0x00_0440` | Error status 1, processor A | 64 | — | readable | Bits 63:53 the message source, bits 52:43 buffer status, bit 42 read versus write buffer, bits 41:39 the buffer number, bits 38:29 an invalidate count, bits 28:21 a timeout count, bits 20:0 the spooled-record count. |
| `0x00_0448` | Error status 1, processor A, clearing | 64 | — | readable, clears on read | The same register, clearing on read. A 64-bit zero store ends processor A's whole error record, status 0 included (inferred; see the behavioral contract). |
| `0x00_0450`, `0x00_0458`, `0x00_0460`, `0x00_0468` | The four processor B copies | 64 | — | as above | Identical layout, 32 bytes above the processor A set. |
| `0x00_0470` | Spool compare, processor A | 21 | — | readable and writable | Raises the spool-count-match error when the spooled-record count reaches it. |
| `0x00_0478` | Spool compare, processor B | 21 | — | readable and writable | The same for processor B. |
| `0x00_0480` | Timed-out request buffers, processor A | 12 | 0 | special action occurs on write | Bits 11:4 the eight write buffers, bits 3:0 the four read buffers. |
| `0x00_0488` | Timed-out request buffers, processor B | 12 | 0 | special action occurs on write | The same for processor B. |
| `0x00_0490` | Processor-bus checking enable | 6 | 0 | readable and writable | Six independent enables for generating and checking error-correction, data-quality, address parity, command parity and state parity on the processor bus. |
| `0x00_0498` | Force bad check bits, processor A | 8 | 0 | readable and writable | Diagnostic error injection. |
| `0x00_04a0` | Force bad check bits, processor B | 8 | 0 | readable and writable | The same for processor B. |
| `0x00_04a8` | Negative-acknowledge count, processor A | 21 | 0 | readable and writable | Bit 20 enables counting, bits 19:0 are the count. |
| `0x00_04b0` | Negative-acknowledge count, processor B | 21 | 0 | readable and writable | The same for processor B. |
| `0x00_04b8` | Negative-acknowledge compare | 20 | 0 | readable and writable | Threshold the counts are compared against. |

Registers this unit also contains but that nothing on our path touches — the
built-in self-test controls at `0x00_0200` through `0x00_0230` — are listed
here only so a reader knows the address range is accounted for. They get a
logging catch-all.

### Graphics flow control

**Not yet independently reviewed.** Added 2026-09-27 and rewritten on
2026-09-28 as the contract a live credit counter must follow. How a
graphics head reaches these registers is the
PI graphics credit convention (citation: `docs/designs/sgi-pi-graphics-credits.md`).
The Origin 2000 graphics products attach SI Viz and InfiniteReality pipes,
and both IRIX drivers use this block. These graphics credits are distinct
from the crosstalk link-level and Crossbow credits, which are per link and
cover all traffic; those are out of scope here.

Ten registers pace one processor's uncached stores to a directly attached
graphics pipe. The processor A set starts at `0x00_0300` and the processor
B set `0x28` above it. **The two sets belong to the node's two processor
slots, not to graphics contexts.** IRIX picks the set from the running
processor's slot and moves a pipe's accounting from one processor to
another by saving the counter at detach and writing it back at attach.

| Offset (A, B) | Name | Width | Reset | Access | Behaviour |
|---|---|---|---|---|---|
| `0x00_0300`, `0x00_0328` | Graphics page | bit 40 enable; page address in bits 39:17 (inferred) | unknown; model 0 | readable and writable | The physical page whose uncached stores from this processor are counted. |
| `0x00_0308`, `0x00_0330` | Credit counter | 12 (inferred) | unknown; model 0 | readable and writable | Free credits: falls by one per counted store, rises by one per credit returned. A write replaces the count. |
| `0x00_0310`, `0x00_0338` | Bias | 12 (inferred) | unknown; model 0 | readable and writable | Low-water threshold for the processor stall and the flow-control interrupt. |
| `0x00_0318`, `0x00_0340` | Interrupt counter | 26 (inferred) | unknown; model 0 | readable and writable | Counts clocks of credit starvation toward the compare. |
| `0x00_0320`, `0x00_0348` | Interrupt compare | 26 (inferred) | unknown; model 0 | readable and writable | Starvation count at which the processor's graphics interrupt is sent. |

Three registers outside the block take part: first-bank pending bits 1
and 2 (the graphics interrupts for slots A and B), the I/O interface's
graphics node-widget map at `0x40_0140` and `0x40_0148`, which routes
returned credits, and offset `0x02_0068`, whose meaning conflicts (below).

**Sources.** Linux `include/asm-mips64/sn/sn0/hubpi.h` lines 119 to 133,
`sn0/hubio.h` lines 116 to 134, `sn0/hubni.h` lines 90 to 91 and
`sn/intr.h` lines 86 to 87, all at `linux-history` tag `v2.5.40` (commit
`7570df54ef8c`); Linux `arch/mips/include/asm/sn/sn0/hubpi.h` at tag `v4.0`
(commit `39a8804455fb23f09157341d3ba7db6d7ae6ee76`) for the same offsets;
the SN1 headers of the succeeding chip at the same `linux-history` tag,
`include/asm-ia64/sn/sn1/hubpi.h` (register comments at lines 1053 to 1057,
2025 to 2031, 2067 to 2077, 2108 to 2111, 2144 to 2155 and 2188 to 2193;
the credit-mode register at lines 105 and 3925 to 3930) and
`sn1/hubio.h` lines 1290 to 1296. Disassembly, with no vendor source read,
of IRIX 6.5.30 `IP27boot` objects from the installation media:
`gfxs.a(gfx_flow.o)` (SHA-256 `70f668a5a117d389f093a524a8a349c061698c7caa3b7d106946e14e09f21af0`,
the committed record
`gfx_flow.o-gfxs.a-IP27boot` (citation: `docs/analysis/gfx_flow.o-gfxs.a-IP27boot/capstone-linear@70f668a5a117.json`)),
`mgras.a(mgras.o)` (SHA-256 `6a65e70d3cdcb9822b1cf5a840431b5d971e39957a350ab52b87cf620ffb6620`),
`kona.a(kona_flow.o)` (SHA-256 `0704a4d34a99aa6d0f867fea4466d52ccbc50d1abd2720aa3758a6abd0896bc3`),
and the 6.5.30 `unix.IP27` kernel for the per-processor data layout.
Offsets below are object offsets from `llvm-objdump -dr`. US Patent
6,622,182 B1 corroborates the general shape only, as recorded before.

#### Graphics page

- **Established:** the offsets, names and the enable in bit 40 (both SN0
  headers above). IRIX's attach routine, `gfx_flow_attach_me`, writes the
  pipe's page address with bit 40 set into the running processor's page
  register (`gfx_flow.o` `0x78c`); its detach routine, `gfx_flow_detach_me`,
  writes `0xdeadbabe`, which has bit 40 clear (`0x8f0`). The page address
  comes from the driver: MGRAS passes its widget base plus `0x12_0400`.
- **Established for SN1, inferred for SN0:** the page address occupies
  bits 39:17 and the other bits are reserved (the SN1 register union), and
  uncached writes into the graphics page made with the uncached attribute
  of I/O go out as graphics single-write commands (GFXWS) that the credit
  counters track, in place of ordinary uncached writes (SN1 comment at line
  2027). The counter comment also names a second command, GFXWL, which the
  page comment does not tie to any store; a gathered block write is
  **inferred**. Neither is stated for SN0.
- **Unknown:** the reset value, and whether a store outside the enabled page
  but to the same widget is ever counted.

#### Credit counter

- **Established, IRIX dependence:** the accessor
  `gfx_flow_credit_count_get` (`0x668`) loads 64 bits from the local form
  (`0x9200_0000_0100_0000` plus `0x308` plus `0x28` times the slot) and
  shifts the value left by three, converting a count of doublewords to
  bytes; the total and bias accessors shift the same way. The slot
  is the byte at per-processor data offset `0x520`, which `alloc_cpupda`
  in `unix.IP27` fills from `get_cpu_slice` (`0xc000_0000_001f_6470`), so it
  is 0 or 1. Attach writes the flow's saved count into the counter
  (`0x770`) after writing zero to the bias (`0x754`). Detach reads the
  counter back into the saved count (`0x93c`). `gfx_flow_reset` sets the
  saved count to the flow's total, so the first attach after a pipe reset
  writes the total. On a Hub whose revision is below 3, attach first
  masks error interrupt 8 with `err_int_block_mask` (`0x7f8`) and detach
  unmasks it (`0x970`). The driver therefore depends on a write replacing the
  count and a later read returning it; before the model stored these
  registers, the counter read zero and SI Viz hung.
- **Established, IRIX dependence:** the totals and biases `gfx_flow_alloc`
  sets from the byte size the driver passes. In doubleword mode, the mode an
  unmodified kernel runs (see the credit-mode conflict): total = bytes / 8,
  at most 101; bias 68, or 8 when the total is below 69 (`0x508` to
  `0x568`). In transaction mode: total = ((bytes − `0x220`) / 128) + 20, at
  most 27; bias 20 (`0x304` to `0x334`). MGRAS passes 1024 bytes
  (`mgras.o` `0x1ea4`), so it runs with 101 credits and bias 68.
- **Established, IRIX dependence:** how drivers compare. InfiniteReality's
  `kona_flow_await_returned_credits` (`kona_flow.o` `0x754`) waits while
  count is below bias, up to 1000 polls one microsecond apart, then calls
  `kona_die` and `kona_reset_adaptor`; once count reaches bias it reads the
  count again and calls `kona_die` if it exceeds the total (`0x7f4`). MGRAS
  compares count against bias after every attach, in ten functions of
  `mgras.o` including `MgrasPcxSwap`, `MgrasResume` and
  `MgrasValidateEramRes`, and below bias either calls its flow-control
  handler `mgras_hq4_hiwater_intr` or, in `MgrasResume` (`0x59ec`), polls at
  raised priority with no time limit until count reaches bias or a board
  status bit sets.
- **Inferred for SN0, established for SN1:** the counter falls by one for
  each doubleword sent to graphics through the page and rises by one for
  each doubleword acknowledgement from graphics (SN1 comment at line 2069).
  One uncached store of four bytes or fewer counts as one doubleword; the
  model uses that rule for every store up to eight bytes.
- **Inferred:** a credit returns when the pipe frees the FIFO space the
  word held. The totals match the pipe FIFOs (MGRAS: 1024 bytes, 128
  doublewords, capped at 101), which only makes sense if a credit stands
  for a free FIFO slot.
- **Unknown:** the reset value; what the counter does below zero or above
  `0xfff`. The SN1 interrupt-counter comment tests the credit counter for
  "less than or equal to zero", which suggests a signed count, but the
  field is twelve bits and IRIX compares it unsigned. **The model wraps
  modulo 4096 and logs the wrap as a guest error, its choice.** Either way
  IRIX tears the pipe down, since a wrapped count reads above the total.

#### Bias

- **Established, IRIX dependence:** attach writes zero, then
  `gfx_flow_attach_me_finish` writes the flow's bias (`0x84c`); detach
  writes zero (`0x8e4`). The driver compares against its own copy of the
  bias, not the register.
- **Established for SN1, inferred for SN0, internally inconsistent:** the
  counter's comment says that when the counter is below the bias the
  processor's write-ready signal is withheld, an interrupt is sent, and
  write-ready is then allowed again; the bias's own comment says a
  flow-control interrupt is sent when the counter is at or below the bias.
- **Unknown:** which threshold, below or at-or-below, and how long the
  processor is held.

#### Interrupt counter and compare

- **Established, IRIX dependence:** `gfx_flow_init` writes 10000 to both
  interrupt compares of every node through the node-named window
  (`0xa0`, `0xe8`, `0xfc`). The graphics interrupt handler zeroes the
  running processor's interrupt counter (`0x58`), and so does detach
  (`0x8f8`). Nothing else writes the counter.
- **Established for SN1, inferred for SN0:** the interrupt counter is
  written by software; is loaded from the compare when an interrupt, NMI or
  soft reset reaches the processor, which stops a second interrupt; is
  zeroed when the credit counter rises above the bias; and counts one per
  clock while the credit counter is at or below zero. An interrupt is sent
  when it reaches the compare. IRIX's handler zeroing it fits the reload
  rule: the handler re-arms the delay.
- **Unknown:** which clock it counts and at what rate, so 10000 has no
  known duration; and whether it counts while the credit is at or below
  zero, as the comment says, or below the bias, as the stall rule suggests.

#### Graphics interrupt

- **Established:** first-bank pending bit 1 is slot A's graphics interrupt
  and bit 2 slot B's (Linux `GFX_INTR_A`, `GFX_INTR_B`); the SN1 comment says
  hardware sets them and software clears them. IRIX connects bit 1 plus
  the slot on every enabled processor to `gfx_flow_delayed_hiwater_intr_prehandler`,
  which calls the attached flow's callback and zeroes the interrupt
  counter. MGRAS registers `mgras_hq4_hiwater_intr` as that callback: it
  waits up to ten microseconds for a board status bit, then marks the
  process suspended, starts a FIFO timer and reschedules. Detach clears
  pending bit 1 plus the slot through the interrupt-modify register and
  reads the first pending bank back (`0x900`, `0x904`).
- **Unknown:** whether the interrupt comes from the bias test, from the
  interrupt counter reaching the compare, or both. The routine's name, a
  delayed high-water handler, and its zeroing of the interrupt counter
  point at the counter, which is **inferred**.

#### Return routing: the graphics node-widget map

- **Established:** the layout (Linux `IIO_IGFX_INIT`: widget 3:0, node
  12:4, processor slot 16, valid 20). IRIX's attach writes valid, the slot,
  the attaching processor's node and the pipe's widget number into the map
  entry the flow owns on the pipe's master node (`0x794` to `0x7e0`); detach
  writes zero (`0x86c`). With transaction credits off IRIX allows one pipe
  per node and uses only the first entry; with them on, two (`0x41c` to
  `0x42c`).
- **Established for SN1, inferred for SN0:** the map matches write
  responses from a graphics widget to the processor that started the
  operation, and the responses become that processor's credits. The
  [Hub I/O interface datasheet](hub-io-interface.md) carries the same
  reading for these offsets.
- **Unknown:** what the I/O interface does with a response from a widget no
  valid entry names.

#### The credit-mode conflict at `0x02_0068`

IRIX's flow initialisation reads each Hub's chip revision (network
interface status bits 7:4, Linux `NSRI_REV`) and clears its
transaction-credit flag when any Hub is below revision 4 (`0xcc` to
`0xe4`). The flag starts at zero in the shipped object and nothing in it
sets the flag. **Established for the shipped IRIX 6.5.30 `unix.IP27`**
(SHA-256 `cbdb02ea5d334ecac949a6524a363e6c945f94baf438d41e8d9248eda0377bdf`):
the flag's initial data is zero, and its only stores through the global
pointer are the clear in `gfx_flow_init` (`0xc000_0000_0040_62ec`) and a
store of zero in `gfx_flow_alloc` (`0xc000_0000_0040_6488`), so that kernel
runs doubleword mode. **Inferred** for a kernel relinked on an installed
disk: a system tunable would change the initial data, and none was
checked. With the
flag clear, IRIX stores zero to `0x02_0068` on every Hub of revision 4 or
later (`0x1d0` to `0x218`), and skips older Hubs. The SN1 header names
this offset the graphics credit mode, bit 0 selecting transaction mode.
This datasheet's processor table calls it the reset-survivor flag, from the
IRIX register table, and SN0 Linux calls it the hard-reset bit.
**Inferred:** from revision 4 the offset carries the credit-mode bit.
**Unknown:** its reset value on those Hubs. The model's Origin 200
topologies report Hub revision 3, so IRIX leaves the offset alone there;
its Origin 2000 topologies report revision 5, so IRIX stores zero to it on
every Hub (QEMU `hw/mips/sgi/sn-catalogue.json`, `ni-revision`). Either way
the flag stays clear and IRIX runs doubleword mode, the only mode this
contract requires. The contract has the model take a zero store there
without comment, log any other value as unimplemented, and answer a read
as an unimplemented register.

#### What the model must do

This is the contract for the live counter, stated so a test can fail it.

1. Store the ten registers at the widths above and reset them to zero
   (model choice). Page bit 40 clear means nothing is counted.
2. A processor store of up to eight bytes through the uncached I/O space,
   issued by the processor in slot S of this Hub, whose physical address
   bits 39:17 equal slot S's page bits 39:17 while page bit 40 is set,
   decrements slot S's counter by one. The count belongs on the issuing
   processor's own I/O path, where the Hub sees the whole physical address
   and the slot; a head's register handler sees neither. The slot is the
   one the processor-number register reports to that processor, so a
   processor on another node is charged on its own Hub. Stores through the
   other uncached spaces are not counted. The processor model marks
   uncached and uncached-accelerated stores alike, so both count; which one
   IRIX's drivers use for the page is unknown. The model has no write
   gatherer, so every store is a single command and none is a GFXWL.
3. A credit returned by a widget goes to the node and slot the valid map
   entry for that widget names, and increments that slot's counter by one
   per credit. The node may be another node, whose Hub is found by its
   node number. If both entries name the widget, the first (`0x40_0140`)
   wins (model choice; IRIX never programs both alike). No matching entry:
   drop and log a guest error. The map is graphics-named and nothing
   establishes that the Hub checks the widget type, so the model routes by
   the map alone.

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
5. Below zero or above `0xfff` the counter wraps and a guest error is
   logged (model choice while the hardware behaviour is unknown). The
   model never clamps at the driver's total.
6. The stall and the graphics interrupt are not modelled. A counted store
   that leaves the counter below the bias register logs `LOG_UNIMP` once per
   slot. Synchronous heads never reach this.
7. Migration carries all ten registers and the map.

**What the model does.** The model follows the contract above. The ten
registers keep the field widths given, the page register only its enable
and bits 39:17, and all reset to zero. Each processor's uncached I/O
requests pass through a per-slice region of the Hub over that processor's
view of the I/O space; there an uncached I/O store of up to eight bytes
into the slot's enabled page costs the slot one credit, and the store goes
on to its target marked as counted or not. A graphics head returns credits
by widget number, only for stores marked as counted; the mark is one-shot,
so a store split into two callbacks is repaid once, and a head that asks
off the storing processor's thread is told no and a guest error is logged.
The node-widget
map sends them to the named slot, on this Hub or on the machine's Hub
carrying the named node number. The counter wraps with a guest error at
either end; a counted store that leaves it below the bias register logs,
once per slot, that the stall and the graphics interrupt are not modelled;
a zero store to `0x02_0068` is taken and any other value is logged as
unimplemented. The block, the page and the map migrate. No graphics head
returns credits yet: until the MGRAS and InfiniteReality models adopt the
interface, a guest's count holds what the driver wrote and the guest runs
without backpressure.

## Behavioral contract

**Setting and clearing a pending bit goes through one register.** Nothing writes
the pending registers directly; they are read-only. A write of `0x100` plus the
interrupt number to the interrupt-modify register sets that bit, and a write of
the number alone clears it. Linux expresses this as a pair of macros that send
and clear an interrupt on either the local or a named Hub, and OpenBSD as a
matching pair of set and clear functions; both compose exactly the same word.

That covers every bit software allocates, but it is not the only producer of
pending bits. The cross-call set and clear registers at `0x00_00c8` through
`0x00_00e0` drive first-bank bits 5 and 6, the two cross-call pending bits this
document's hardware-assignment table names. Nothing on the first-boot path uses
them, and a model written literally from "the interrupt-modify register is the
only way" would wire those four registers to nothing.

**Every clear is followed by a read.** Linux clears a pending bit and then
immediately reads the first pending register back, with a comment saying the read
is there to make sure the clear reached the Hub before anything else happens.
OpenBSD does the same after writing a mask and after clearing. Our model must
make the read return the post-write state, which it will naturally, but the
pattern is worth knowing: a guest that appears to lose interrupts is often a
model that reordered these.

**Masking is per processor, pending is per node.** The two pending banks are
node-wide state. Each processor has its own pair of masks. The output to a given
processor is the pending bank ANDed with that processor's mask for that bank.
Both guests rely on this to steer a device interrupt at a chosen processor by
setting the bit only in that processor's mask, and OpenBSD additionally
implements its interrupt priority levels by rewriting the masks on every level
change. The model must recompute the outgoing lines whenever either a pending bit
or a mask changes.

**Interrupt service masks aggressively.** OpenBSD's handler reads the pending
bank and the mask, writes back the mask with every currently-pending bit
*removed*, services what it found, then restores the mask. Linux's handler
carries a comment explaining the same technique and why: without it a
still-asserted source re-interrupts during its own handler and recurses until the
stack is gone. Nothing in the Hub model has to do anything special for this, but
a model that ignored mask writes while an interrupt was in service would break
both guests.

**Boot state is masks clear.** OpenBSD explicitly writes zero to all four mask
registers early in setup, then reads both pending registers, before it enables
anything. Linux builds its masks up from zero. So a model that resets its masks
to zero matches both, and the register table's reset value of zero agrees.

**The counter is acknowledged by writing zero.** Linux's counter handler writes
zero to the counter-pending register for its own processor as its first action,
then runs the clock event. It arms the next tick by reading the counter, adding a
delta, writing the sum to its own compare register, and then re-reading the
counter to check the deadline has not already passed — if it has, it reports the
event as late rather than waiting for a wrap. A model whose counter does not
advance will hang Linux's timer, because the armed compare is never reached. But a
compare that does **not** fire when its written value is already at or behind the
counter does **not** hang Linux: that re-read is exactly the guard for the case —
`rt_next_event` returns `-ETIME` and the clockevent core retries with a fresh
delta, so Linux never waits on a compare it has already passed.

**A compare written at or behind the live counter waits for the wrap; it does not
fire at once — settled by guest dependence, IRIX 6.5.30.** The register table's "when
the counter reaches this value" describes counting up to equality and says nothing
about a write that lands at or behind the count; that was an open question. The
two-node IRIX 6.5.30 miniroot settles it: it quiesces its secondary processor's ticks
by writing the compare to zero **unguarded**, with the counter high, and depends on
the silence that follows. A model that fires a passed compare at once turns that write
into an immediate match, and the measured result is an interrupt storm — both
processors trapped in tick service at roughly a 30 µs period, boot hung. Real
Origin 200s boot this IRIX, so the silicon lets a passed compare wait for the wrap.
Linux's post-write `-ETIME` guard is the consistent reading from the other side: its
authors expected behind-writes and handled them, where IRIX's unguarded write depends
on the hardware handling them. (The model provisionally fired at once: fork commit
ff3119cf1f took match-at-once as a deliberate loud-failure choice while the answer was
unobserved, and predicted a livelock for an unguarded writer — the loudness worked as
designed, surfacing exactly this storm. It is being corrected to wait-for-wrap in fork pull
request 327.)

**Confirmed a second time, live-stepped, in a different configuration.** The same storm was
reproduced on a **single-node two-CPU** installed-disk boot with an added Ethernet adapter, and
caught under a live debugger rather than inferred: the secondary processor's compare register read
zero against a counter near 1.6 billion, its companion's compare was armed ahead and that processor
ran normally, and the secondary processor was stepped cycling through the timer-interrupt path with
no forward progress — the match-at-once model firing the behind-compare perpetually. Two
independent configurations (the two-node miniroot, and this single-node two-CPU boot) now show the
same mechanism, so it is reproduced across configurations rather than predicted and seen once. The
two-CPU requirement falls out of the register layout: only a second processor brings its own
compare — here left at zero — into play, while a single-processor boot uses only the first compare,
which is armed ahead.

**OpenBSD needs the counter but not the comparator.** OpenBSD drives its clock
from the processor's own count and compare registers and uses the Hub counter
only as a monotonic reference, reading it through the node-explicit half of the
Hub window and truncating it to 32 bits. So for an OpenBSD boot the contract
reduces to: the counter at offset `0x03_0100` must advance monotonically at
1.25 MHz and must be readable. Linux is what makes the comparator, the pending
bit and the enable bit load-bearing.

**The present and enabled bits are guest-observable and currently unbacked, and this is
confirmed rather than theoretical.** A real Linux boot on this fork reached memory
initialisation, read the four registers at `0x00_0040`, `0x00_0048`, `0x00_0050` and
`0x00_0058` (`ip27-init.c` tests bit 0 of each), got the reset value of zero from all
four because nothing currently backs them beyond the logging catch-all, and concluded
node 0 has neither a primary nor a secondary processor — while the configuration graph,
read moments earlier by the same guest, said one processor exists. The machine told one
guest two different things about the same fact in the same boot. **Present must report
the node's actual configured population, not the reset value** — it is read-only
hardware-sensed state on real silicon, never written by any firmware we hold, so a model
must derive it from the machine's own CPU count at construction rather than from
anything a guest writes. **Enabled resets to zero and real firmware sets it during CPU
bring-up** (`SD(LOCAL_HUB(PI_CPU_ENABLE_A), 1)` in SGI's own Hub bring-up code) — our
synthesized firmware must do the same for any processor it brings up. **Checked directly
against Linux's source and corrected: enabled is weaker-consequence than present, not
equal to it.** Both registers are read in exactly one place in the whole IP27 platform
code, the same two `ip27-init.c` print statements, and there present decides "a" versus
"no" CPU — a real, wrong conclusion if left unbacked — while enabled only appends ", CPU
is running" or nothing to the same line. A guest that reads present correctly but enabled
as zero prints a technically-incomplete line, not a false one. Firmware should still set
it, because a real machine does and an accurate diagnostic line is worth having, but the
failure mode is a missing suffix, not a second wrong conclusion. Full findings, including
the second, unrelated
defect the same boot found, are in
the first Linux boot findings (citation: `docs/library/linux-first-boot-findings.md`). Recorded here
as a behavioural requirement rather than left as a register-map curiosity, because the
register-map entries alone (correct as far as they go) do not say what a model owes
beyond their reset values.

**Hub initialisation, in the order Linux does it.** Linux sets the I/O interface's
crosstalk timeout registers, then enables the counter interrupt for both
processors, disables the profiling interrupt for both, zeroes the counter, and
clears both counter-pending bits. On any node other than the first it then copies
the exception vectors into that node's memory and switches the cached alias to
8 KB. Every one of those registers must accept a write at reset without any
unlock step, which matches the register table: the protection registers reset
permissive for processor writes.

**Error handling is disabled, not used.** Linux's bus-error setup clears every
error-pending bit for the faulting processor by writing an all-ones-in-the-right-
places pattern, zeroes that processor's error mask, zeroes its spool address, and
zeroes the spool size to disable spooling entirely. Its bus-error handler prints
the error-pending register and both error-status registers and then panics.

OpenBSD leaves the error machinery alone during setup, but it does write one of
these registers, and the write is not an error report. On its halt and reboot
path OpenBSD stores a PROM-operation word — a request code, a magic value, and
flags asking the firmware to skip diagnostics and memory initialisation — into
offset `0x00_0418`, the register this document calls the processor A error spool
address, immediately before it resets the machine through the network interface.
Its comment says the obvious home for that word, the firmware-operation field in
the global data area, is not where the firmware actually looks. Nothing on the
banner path touches it. It matters later: a synthesized firmware that ever
implements reboot must watch this Hub offset rather than the structure field, and
the register must survive being written right up to the reset.

So the contract our model owes is narrow: the registers must exist, must accept
those writes, and must read back zero when no error has occurred. The Linux and
OpenBSD paths described here do not require the model to generate an error
report. The stock-PROM workaround that follows does encounter a real fault.
Its issuing-slice selection, status 0 `VALID` and address fields, persistence,
and clearing operation are established. Only the other status fields and any
Bridge-side error record remain unknown.

The status 0 clearing aliases have this reviewed write contract:

| Offset | Processor | Established write-clear behavior |
|---:|---|---|
| `0x00_0438` | A | A 64-bit store of zero clears processor A status 0. |
| `0x00_0458` | B | A 64-bit store of zero clears processor B status 0. |

The status 1 clearing aliases carry an inferred write contract:

| Offset | Processor | Inferred write-clear behavior |
|---:|---|---|
| `0x00_0448` | A | A 64-bit store of zero ends processor A's error record, status 0 included. |
| `0x00_0468` | B | A 64-bit store of zero ends processor B's error record, status 0 included. |

The exact PROM 6.156 instructions use those stores to discard a stale record
immediately before the deliberate probe. This establishes the zero-store case
only. Neither the PROM nor the reviewed specification establishes whether a
nonzero value or a narrower store has the same effect. The model and tests must
not generalize the rule beyond a 64-bit zero store.

**The retained stock-PROM sequences differ.** During early Hub inspection each reads
processor A status 0 at `0x00_0430`, processor B status 0 at `0x00_0450`, and both status 1
clear-on-read aliases at `0x00_0448` and `0x00_0468`. All four reads return zero on the current
model. The retained early-inspection excerpts do not show reads of the status 0 clearing aliases
at `0x00_0438` and `0x00_0458`.

PROM 6.11 later reads processor A status 0 and performs an eight-byte load from virtual address
`0x9600_0000_ffff_ff00`, physical address `0xffff_ff00`, inside a fault guard. The current machine
has no device at that address and raises a bus error. After the handler returns, the firmware
writes `0x30_0000` to the error-pending register at `0x00_0400`.

PROM 6.156 takes a different path. It reads processor A status 0 and, when that record is not already
valid for the expected address, writes zero to `0x00_0438`. The exact PROM instructions and the
reviewed firmware contract establish this as a 64-bit clearing store. The PROM then issues one
64-bit load from architectural virtual address `0x9200_0000_0240_0208`, physical address
`0x0240_0208`, inside a fault guard. That address is a Hub register that exists to fail. Linux
`arch/mips/include/asm/sn/sn0/hubio.h` names offset `0x140_0208` from the Hub's register alias
`IIO_IIBUSERR`, "Reads here cause a bus error", and `sn0/addrs.h` makes
`LOCAL_HUB_ADDR(IIO_IIBUSERR)` the error-status workaround's address, `ERR_STS_WAR_ADDR`, whose
physical form is "used to match addr in error reg" (both at commit
`db2ddb87143519e20a95aa36c60b36107b736a58`). The register alias starts at widget 1's small window,
`0x0100_0000` (`asm/sn/addrs.h`, `IALIAS_BASE`), which puts the register at `0x0240_0208`. So the
Hub fails the read itself and nothing goes out on the XIO link (established that reads fail,
from the header; that the Hub answers it without a link transaction is inferred from its being
a Hub I/O-interface register). A model run bears the inference out: with the probe routed to
the Crossbow instead, the illegal-destination error it left on the Hub's port panicked IRIX
6.5.30 at boot on the one-node Origin 2000, where a real machine boots. **Corrected 2026-09-28:** this paragraph said the address reached Bridge offset
`0x40_0208`, the device-1 window, as two 32-bit PCI reads.

The special address is deliberately chosen not to complete normally. The public header names it
as the bus-error address, and the reviewed firmware contract says the load forces a Hub
read-response timeout. The processor takes a guarded bus-error fault. Hub latches status 0 for the
processor that issued the load. The fault handler then writes `0x30_0000` to the PI error-pending
register and returns past the guarded access. It does not clear the new status record. Later calls
first read status 0 and leave an already-valid record for this address untouched, which is how the
record persists until the kernel checks it. Processor A uses
`0x00_0430`/`0x00_0438`. Processor B uses `0x00_0450`/`0x00_0458`.

The retained current-model trace shows the incorrect contrast. Immediately after the eight-byte
status read and clearing store, it routes the load through `sgi-bridge.window1.mem` as two
four-byte reads, both returning zero, so the processor receives no fault and Hub receives no
status record. The CPU-facing `0x2_0002_400208` value used in earlier QEMU traces is a
QEMU-internal carrier, not an architectural address. The memory-region trace reports the
downstream carrier as `0x2000_400208`. Neither number describes the hardware address. The exact
PROM instructions at `0x1fc5_6548`–`0x1fc5_660c` and the black-box trace agree on the status-read,
clear, load ordering and access widths.

PROM 6.11's address, `0xffff_ff00`, is the public Linux header's old error-status workaround
offset, eight 512 MiB memory banks minus `0x100` (`OLD_ERR_STS_WAR_OFFSET`). PROM 6.156's is the
new one, `ERR_STS_WAR_ADDR`, the I/O interface's bus-error register above. **Corrected
2026-09-28:** this paragraph said the 6.156 address was the old offset and not the bus-error
register, which it placed at `0x40_0208`; the register's offset from the Hub alias is
`0x140_0208`. Linux commit `db2ddb87143519e20a95aa36c60b36107b736a58` names all of these in
`arch/mips/include/asm/sn/sn0/addrs.h`, `hubmd.h`, and `hubio.h`. Linux does not execute either
workaround, and OpenBSD 7.9 at commit `2df91d80c104636deeef8124252ddd5951b55fbf` does not define
them.
The access order comes from the stock-PROM PI capture (citation: `docs/library/origin200-hub-pi-capture.md`),
not from either guest source.

These sources establish the fault, processor selection, persistence, and the two status fields the
kernel checks. They do not establish a Bridge error record or the remaining PI status fields. The
model must not invent either.

**How the model produces the record.** Every I/O-space request a processor makes passes this
unit: the model places a Hub region per slice over the processor's uncached view of the I/O
space. That region fails a doubleword read at the bus-error register itself, on both machines,
and forwards everything else to the processor's own physical view. The match ignores the node
number above the node offset, so a doubleword read of any node's `0x0240_0208` fails at the
issuing Hub; the header gives only the local form, `LOCAL_HUB_ADDR`, so that is inferred. The workaround's read is a
doubleword and that is the only width modelled as failing there: whether a narrower read at the
register fails too is not stated, so it goes on down the link as before (on an Origin 200 into
the Bridge's device-1 window, where no device claims it,
[a master time-out](bridge.md#an-unclaimed-device-window-cycle-is-a-master-time-out-too)); a
narrow read by an executing processor at the register on a real Hub would settle it. When a
read fails, here or downstream, the region records it in that slice's status 0 — `VALID`, and
the processor's physical address bits 39:3 in bits 61:25. That any failed processor read records here, not only this one, is
inferred from the register's purpose; the type, request-number, supplemental and command fields
stay zero because no source says what the chip puts in them. A read that fails while the record
is still valid sets the overrun bit, 62, and keeps the first record. That too is inferred: the
Linux header says only that with overrun set the error stack may hold more, and keeping the first
record matches how this family's other error captures behave. An instruction fetch from the I/O
space records the same way (inferred). A debugger's read, a device's DMA and a qtest access do not
pass the processor path and record nothing. Before this, the model faked the
record with an aperture overlaid on each processor's view, outside the Hub's own decode; the
storm this address once caused is recorded in [the Bridge datasheet](bridge.md).

**IRIX 6.5.30 does not use a firmware revision field for its old-firmware
warning.** It checks the error-status record that the processor PROM is expected
to leave behind. For every processor selected to boot, the kernel reads that
processor's non-clearing status 0 register: `0x00_0430` for processor A or
`0x00_0450` for processor B. It accepts the PROM when both of these conditions
hold:

- status 0 bit 63, the public `VALID` field, is set.
- status 0 bits 61:25, decoded as physical-address bits 39:3, equal
  `0x48_0041`. This names architectural physical byte address `0x0240_0208`,
  the deliberate I/O bus-error probe reached through the local Hub address
  calculation.

The kernel ignores the status 0 error type, request number, supplemental and
command fields for this compatibility check. It also ignores status 1 and the
PI error-pending register. A failure of either tested field for any booting
processor selects the conservative PIO mode and later emits the single
old-firmware warning.

This predicate is independently present in the exact IRIX 6.5.30 `unix.IP27`
used by the witness boot. Its unstripped DWARF locates the check at
`0xc000_0000_0004_4304`–`0xc000_0000_0004_4380`. Disassembly shows the status
load, the comparison with `0x48_0041`, and the bit-63 test. The same behavior is
restated from the IRIX 6.5.7 kernel source. No firmware-structure or KLCONFIG
revision participates.

The witness run `irix-netinstall-20260827T060849-77cb3a`, using PROM 6.156 and
IRIX 6.5.30 on QEMU `e11b4cd8d626c78deee107191c286935a4e11dfa`, emits this
warning after the CPU- and Hub-revision warnings have disappeared. The current
model returns zero from status 0 when no PI latch has been implemented, which
fails both parts of the predicate. This makes the warning a direct acceptance
test for the PROM-created status record, not evidence that the configured PROM
revision number is too low.

**IRIX 6.5.30 clears the record after checking it, through status 1.** The
kernel's handling depends on the Hub revision it reads from the network
interface status register. Established from disassembly of the same
`unix.IP27`:

- The firmware check above runs first, at `0xc000_0000_0004_4404`, before the
  per-processor and per-Hub setup calls at `0xc000_0000_0004_4468` and
  `0xc000_0000_0004_4764`.
- The per-CPU setup at `0xc000_0000_0002_67e0` turns the Hub revision into
  workaround flags. Revision 2 gets `0x17`, revisions 5 and 6 get `0x8`, and
  revisions 3 and 4 get none.
- When the flags lack bit 1, which is every revision but 2, the per-Hub error
  setup stores 64-bit zero to `0x00_0448` and `0x00_0468` on the local Hub at
  `0xc000_0000_0003_2800` and `0xc000_0000_0003_2808`. It never stores to
  `0x00_0438` or `0x00_0458`.
- The routine that fetches the next error-stack entry reads status 0 and
  status 1 from the non-clearing registers when the memory spool is empty. If
  status 0 is valid it stores 64-bit zero to that processor's status 1
  clearing alias at `0xc000_0000_0003_3ffc` and hands the record on. Its
  caller at `0xc000_0000_0003_45bc` fetches again after each record it
  handles, and in dump mode after every record. It stops only when a fetch
  reports nothing, which with an empty spool means status 0 reads not valid.
- Revision 2 takes the other path. A clock-tick routine at
  `0xc000_0000_0003_5034`, called at `0xc000_0000_0004_6248` when flag bit 4 is
  set, repeats the guarded probe of `0x0240_0208` whenever the local
  processor's status 0 is not valid. The stack walker skips the live status
  registers under flag bit 1. On that revision the record is kept valid on
  purpose and never read as an error.

The hardware effect of the status 1 store is inferred. The Linux header names
`0x00_0448` "clear on read" and says nothing about stores. But the fetch loop
above ends only if that store makes status 0 read not valid, so the kernel
depends on the store ending the whole record, status 0 included. The model
follows that inference for a 64-bit zero store and, as with the status 0
alias, not for narrower or nonzero stores. What would settle it: on a real
Hub 2.1 or later, after the PROM has left its record, read `0x00_0430`,
store 64-bit zero to `0x00_0448`, and read `0x00_0430` again.

Before the model honored the store, it logged both stores as writes to an
unimplemented register. A four-node Origin 2000 boot
(`irix-20260925T063510-2a7a1c`) dropped all eight, so every Hub kept the PROM
record. Any later walk of the error stack took that record as a live read
error. The model latches error type 0, which is not a defined read error
type, so the walk reported "Invalid RRB in error stack. PhysAddr 0x2400208"
and panicked. Runs `irix-20260925T060238-09a4e7` (after a graphics
context-switch timeout) and `irix-20260924T153250-c3b376` show it.
With the fix, run `irix-20260925T165841-bf8a04` (QEMU `8f046761f3`, the
change on `sgi-origami` `b52bc8eac6`) booted a four-node Origin 2000 with
`-d guest_errors,unimp` to login. It logged no writes to unimplemented
register `0x448` or `0x468`. The independent reviewer read status 0 as
`0x8000_9000_8200_0000` on all eight processors at the PROM menu and zero on
all eight after IRIX came up. Those register values come from the reviewer's
gdb reads. The run directory keeps the gdb scripts but not their output.

A hang capture taken while the stock PROM is still running can show every
processor with Cause ExcCode 7, EPC `0x1fc5_65f0` and BadVAddr
`0x9200_0000_0240_0208`. That is the stale exception state of the one
deliberate probe: the load sits in the delay slot of the jump at
`0x1fc5_65f0`, inside the probe sequence above. Status has EXL clear, so no
exception is in progress. It does not mean the processor is in a bus-error
loop.

**Protection registers are storage, not enforcement.** Neither guest writes the
interrupt-write protection register, which resets to zero — meaning, read
literally, that no region may write the interrupt-modify register. The firmware
is what opens it on real hardware. Our model should store these registers and not
enforce them, and should log if a guest ever reads one back expecting a value it
did not write. Enforcing a protection register that our synthesised firmware
forgot to open would produce a machine that takes no device interrupts at all,
with no diagnostic.

## Interrupts

### From this unit to the processor

The Hub drives five of the processor's six external hardware interrupt lines. The
mapping is stated identically by the IRIX header, by Linux's Origin interrupt
header, by Linux's Origin interrupt-number header, and by OpenBSD's setup code,
which registers its two bank handlers on the first two hardware lines.

| Processor hardware interrupt | Cause register bit | Source |
|---|---|---|
| 0 | 10 | Any unmasked bit of the first pending bank. |
| 1 | 11 | Any unmasked bit of the second pending bank, and the cross-call sources. |
| 2 | 12 | The real-time counter comparator for this processor. |
| 3 | 13 | The profiling comparator. |
| 4 | 14 | Hub errors, from the error-pending register gated by this processor's error mask. |
| 5 | — | Not driven by the Hub. The processor's own count and compare occupy the highest line. |

The two software interrupt bits below these belong to the processor and the Hub
does not touch them. All five Hub-driven lines are level-triggered: the line
follows the state of the pending bits and masks, and drops as soon as software
clears the last pending bit it can see. Recomputing the five lines on every
change to a pending bank, a mask, an enable or an error bit is the whole of the
model's output logic.

### From a device to this unit

A device does not have a wire to the Hub. It has a level input to the I/O bridge,
and the bridge turns a rising level into a crosstalk write that reaches the
processor interface through the Hub's I/O interface and cross-connected queues.

```
device level input
  → I/O bridge interrupt status, gated by its interrupt enable
  → bridge issues a crosstalk write to the address in its interrupt-destination
    registers, carrying the interrupt number configured for that pin
  → the write lands on this unit's interrupt-modify register at offset 0x90
  → the named bit of the first or second pending bank is set
  → pending AND that processor's mask
  → processor hardware interrupt 0 or 1
  → exception
```

The address the bridge writes is the crosstalk view of this unit's
interrupt-modify register: crosstalk address bit 47 set, the destination node
number at bit 38, and the Hub register offset `0x0180_0090` — that is, widget 1's
small window, the node-explicit half, plus `0x90`. Linux computes that constant
directly when it describes the bridge to itself. OpenBSD computes it by
subtracting the node's crosstalk base from the address of the interrupt-modify
register in the node-explicit half and then adding the node number at bit 38.
The two expressions look nothing alike and produce the same number, which is the
strongest confirmation available that the constant is right.

The bridge's per-pin interrupt register holds the top two bits of that address in
its bits 17:16, the destination node number in bits 15:8, and the interrupt
number in bits 7:0; the rest of the address comes from the bridge's
widget-wide interrupt-destination register pair. Linux and OpenBSD's older-bridge
path build that word identically. How the bridge splits that word between the
address and the data of the write it issues is the bridge's contract, not this
one, and belongs in the bridge datasheet. What this unit guarantees is narrow and
sufficient: a write to offset `0x90` whose data names an interrupt number in bits
6:0 and carries bit 8 sets that pending bit, and the same write without bit 8
clears it.

Clearing works the same way in reverse. Linux enables a per-pin mode bit in the
bridge that makes it send a clearing packet when the device's level input falls,
with a comment noting that IRIX also sets address bits the bridge documentation
calls reserved. So a device level that drops can clear its own Hub pending bit
without the processor doing anything.

### The Hub's own outbound interrupt

The Hub's I/O interface has an interrupt-destination register of its own at
offset `0x40_0138`, which is how the Hub reports its own crosstalk errors. Bits
16:8 name the destination node, bits 6:0 the interrupt number, bit 24 enables it,
and bit 28 reports that an interrupt has been sent. It resets to zero throughout,
so the Hub reports nothing until firmware programs it.

The seven-bit number field is what makes this unit's whole 128-number space
reachable from the Hub's own error reporting, so our synthesized firmware may
allocate the Hub error interrupt in either pending bank. That width is not
obvious from the headers, which describe the register two incompatible ways: a
set of masks that would make the field six bits and reach only the first bank,
and a bit-field structure in the same file that makes it seven and reaches both.
The structure is right, and the full argument is set out in
[the Hub I/O interface datasheet](hub-io-interface.md).

The register is described here because it is the second producer of pending bits
in this unit and a reader tracing the interrupt chain will otherwise wonder where
Hub errors enter it. It is specified in full, with its reset values and the rest
of the unit around it, in
[the Hub I/O interface datasheet](hub-io-interface.md).

## DMA

The processor interface performs no transfers of its own. Its only interaction
with the transfer path is that the error spool, when enabled, writes error
records into node memory at the address in the spool-address register. We disable
spooling, exactly as Linux does, so the model performs no memory access from this
unit at all.

## Out of scope

- **The error spool ring.** Linux disables it and OpenBSD never enables it. The
  spool address and size registers are storage; no record is ever written. If a
  guest enables spooling we log it as unimplemented rather than silently
  ignoring it.
- **Built-in self test.** Seven registers of scan-chain control that only the
  real firmware's power-on diagnostics drive. Logged catch-all.
- **The graphics stall and graphics interrupts.** The processor stall and the
  graphics interrupt stay unmodelled until their trigger is settled, as
  [the graphics flow-control section](#graphics-flow-control) states. Credit
  counting and return are modelled under that section's contract.
- **The network-wide clock.** The counter clock control and filter registers
  exist to synchronise counters across nodes over CrayLink. A single-node machine
  uses the internal clock, which is the reset state of the selector bit. We model
  the counter as free-running at 1.25 MHz regardless of what is written to these
  registers, and log a write that tries to select the network clock. This is
  where a reviewer should push back if multi-node timekeeping is ever wanted.
- **Coherent-request buffers, timeouts and negative-acknowledge counting.** The
  model has no coherence protocol to time out. These registers are storage.
- **Protection enforcement.** Explained in the behavioural contract: storage, not
  enforcement.
- **Non-maskable interrupt and soft reset.** Both are triggers whose data is
  discarded. Deferred until the second processor exists, because the guest paths
  that use them are the debugger's stop-all-processors path and the crash-dump
  path.

## Behavioral notes

**Interrupt numbers are software policy, not hardware.** Only thirteen of the 128
bits are hardware-assigned — seven in the first bank, six in the second.
Everything else is an agreement between the firmware,
which allocates numbers when it programs the I/O bridge, and the operating
system, which allocates the rest. Our synthesised firmware and our machine
description have to make the same choices, which is the argument for driving both
from one description of the machine rather than two.

**Two register pairs behave the opposite way round.** The counter-pending bits
are acknowledged by writing zero. The error-pending bits are acknowledged by
writing one. Getting these backwards produces a machine that either never clears
its timer interrupt or never clears an error, and both failures look like a hang
rather than a wrong value.

**The counter's offset is a real decode and an odd one.** It is at `0x03_0100`,
not at the `0x00_0100` slot its neighbours would suggest. Three independent
sources agree. Two other registers in this unit share the same pattern of a
non-zero value in address bits 19:16. What those bits select is not explained by
anything read for this document, and the model should decode the three offsets
guests use and log anything else rather than inventing a rule.

**One conflict between the IRIX register table and the public headers.** The
IRIX table marks all four interrupt mask registers read-only. That is wrong:
Linux writes them on every interrupt enable, disable and affinity change, and
OpenBSD writes them on every interrupt priority change. The fields also carry the
names of the *pending* register's fields rather than mask names, which suggests
the table's mask entries were copied from its pending entries and never
corrected. We take the registers as readable and writable. This is worth
recording because it is a concrete demonstration that the register table's
per-field access types, which are the most valuable thing it gives us, are not
infallible and want a cross-check wherever a guest actually touches the register.

**One anomaly in the register table's reset values.** The counter clock control
register's maximum-count field is declared as bits 7:1, seven bits, with a reset
value of `0x100` — a value that does not fit in seven bits either as a field
value or as a positioned one. Either the field is really bits 8:1, or the reset
value is wrong. Nothing on our path reads it, so we reset the whole register to
the selector bit set and everything else zero, and note the discrepancy here.

**One naming difference worth not tripping over.** The second pending bank's bit
58 is a clock dropout in the IRIX register table and a clock error in both Linux
and OpenBSD. Same bit, same meaning, two names.

**The processor-number register is written by real firmware, and the write is not
storage.** Both the IRIX release whose firmware we boot and current mainline Linux
define exactly one register at this offset, found by searching on the offset value
rather than a name, so the negative that nothing else lives there is bounded by the
source rather than by vocabulary. Real firmware's own headers document writing this
register as a deliberate debug convention: one macro stores a fixed pattern here
annotated for a logic analyzer, and a low-level header pairs a store here with a
comment pointing at register-transfer-level simulation logs as where the write is
meant to be seen. IRIX's own PCI bridge and Hub I/O drivers use the register the
same way, storing marker and diagnostic values with nothing ever read back
afterward. That fits the register's own nature: it can only report which processor
is asking, so a value written here is never retrievable regardless of what the
write meant. The model refuses the write and continues to report the processor
number, which is what real hardware does — logged at the unimplemented tier rather
than as a guest error, since a guest following its vendor's own documented
convention is not misbehaving.

## qtest checklist

- Reading the real-time counter twice, with the emulated clock advanced in
  between, returns an increasing value, and the increase matches 1.25 MHz within
  the test's tolerance.
- The counter is 52 bits: writing all ones and reading back returns only bits
  51:0.
- Writing zero to the counter resets it, and the next read is small rather than
  wrapped.
- With the counter interrupt enabled for processor A and its compare set a short
  way ahead, advancing the clock past the compare value sets the processor A
  counter-pending bit and raises processor hardware interrupt 2.
- Writing zero to that pending bit lowers hardware interrupt 2.
- With the counter interrupt disabled for processor A, the same comparator match
  sets the pending bit but raises no line.
- Writing `0x100` plus interrupt number 12 to the interrupt-modify register sets
  bit 12 of the first pending bank and nothing else.
- Writing interrupt number 12 alone to the interrupt-modify register clears that
  bit again.
- Writing `0x100` plus interrupt number 70 sets bit 6 of the *second* pending
  bank and leaves the first bank untouched.
- A pending bit with the corresponding mask bit clear raises no processor line;
  setting the mask bit afterwards raises the line without any further write to
  the pending register.
- Setting a first-bank bit in processor A's mask only raises the line for
  processor A and not for processor B.
- Unmasked bits in the first bank raise hardware interrupt 0 and unmasked bits in
  the second bank raise hardware interrupt 1, and the two are independent.
- The interrupt-pending registers reject writes: writing all ones to either bank
  leaves it unchanged and is logged.
- Reading the processor-number register returns 0 for the first processor of the
  node.
- The reset values of the mask registers, both pending banks, the counter and all
  four counter and profiling enable and pending registers are zero, and the
  cached-alias size register resets to 15.
- Once a reviewed trigger establishes a nonzero error-pending value, writing a
  one to that bit clears it and writing a zero leaves it alone. This test is
  blocked with error latching; reset-zero reads alone cannot prove
  write-one-to-clear behavior.
- Reproduce the PROM 6.156 compatibility sequence for each processor slice:
  create a stale status 0 record through a real reviewed fault, issue a 64-bit
  zero store to that slice's clearing alias, and prove the non-clearing status
  register is zero. Then issue the architectural 64-bit probe at physical
  `0x0240_0208`. Require a processor bus-error fault and a status 0 record for
  the issuing slice with `VALID` set and address `0x48_0041`. Read the record
  repeatedly to prove that the non-clearing register preserves it, then issue
  another 64-bit zero store to the matching clearing alias and prove it is
  zero. Run the A and B cases independently and prove that neither operation
  changes the other slice's status. Do not use a test-only seed or QOM property.
- After the same probe, a 64-bit zero store to a slice's status 1 clearing
  alias (`0x00_0448` or `0x00_0468`) leaves that slice's status 0 reading zero
  and the other slice's record intact. Narrower and nonzero stores leave it
  valid, and a fresh probe latches again.
- A crosstalk write directed at Hub register offset `0x90`, issued the way the
  I/O bridge issues one rather than by a processor store, sets the named pending
  bit — this is the assertion that proves the device-to-processor chain end to
  end, and it must exist before any device model depends on an interrupt.

Graphics flow control. Counted stores come from processors running a small
test firmware image, as the PI error tests do: a qtest access has no
processor and does not pass the processor's I/O path, so it is never
counted. Register setup and reads may use plain qtest accesses. The head is
a qtest-only stub that returns one credit per counted store it takes.

- The credit counter and bias keep bits 11:0 of a write, the interrupt
  counter and compare bits 25:0, the page register bit 40 and bits 39:17.
  All ten read zero after reset.
- With slot A's page set to a head's command-port page with bit 40, the
  counter at 100 and the node-widget map's first entry routing that head's
  widget to slot B, one store to the port leaves `0x00_0308` at 99 and
  `0x00_0330` at 1 more than before. This separates the decrement from the
  return, which a synchronous head otherwise makes invisible.
- With the map routing to slot A, a burst of stores leaves `0x00_0308`
  where it started: every counted store's credit comes back.
- A store from slot A while slot A's page bit 40 is clear, or to an
  address whose bits 39:17 differ from the page, leaves `0x00_0308`
  unchanged: the store is not counted.
- A head that returns one credit for each counted store it takes leaves
  `0x00_0308` where it started over a burst into its port, and a store
  into its port while the page is disabled neither costs nor returns a
  credit, so the count never rises above what the driver wrote.
- A store through an uncached space other than I/O to an address inside
  the page is not counted.
- On a two-node machine, with the head's master node's first map entry
  naming node 1, slot A, and node 1's slot A page covering the port, a
  store from a node 1 processor charges node 1's `0x00_0308` and the
  return lands there too; node 0's counters do not move. With the entry
  naming node 1 while node 0's processor issues, node 0 loses the credit
  and node 1 gains it.
- With both map entries naming the head's widget, the first entry's slot
  gets the credit (model choice).
- A map entry with valid bit 20 clear, or naming another widget, routes
  nothing: the store's slot loses one credit and no slot gains one, and a
  guest error is logged.
- A write of 50 to `0x00_0308` followed by one routed return reads 51: a
  write replaces the count and returns add to it.
- A counted store with the counter at 0 reads `0xfff` and logs a guest
  error (model choice); a return that carries the counter past `0xfff`
  wraps the same way.
- A counted store that leaves the counter below the bias register sets
  neither first-bank bit 1 nor bit 2 and logs the unmodelled stall once
  (model choice, pinned until the interrupt trigger is settled).
- A 64-bit store of zero to `0x02_0068` is accepted, as IRIX's flow
  initialisation issues it on revision-5 Hubs, and counting goes on
  unchanged after it.
- The counters, page, bias, interrupt counter and compare, and the map
  survive migration with their values.

## Open questions

- **The graphics stall and interrupt trigger.** The SN1 header gives three
  thresholds (credit below the bias, at or below it, at or below zero) and
  no clock for the interrupt counter. A published Hub or Bedrock
  specification would settle it. Short of that, on a real Origin 2000 or
  Onyx2 with a graphics pipe: attach a flow, set the bias above the count,
  stop the pipe from draining, and read the interrupt counter, first-bank
  bits 1 and 2 and the counter at intervals.
- **Credit counter wrap.** Whether the twelve-bit counter wraps, saturates
  or carries a sign below zero. The same real-hardware setup settles it by
  writing a count of 1 and issuing two stores to the page.
- **`0x02_0068` on Hub revision 4 and later.** Whether it is the
  credit-mode register the SN1 header describes, and its reset value. A
  read on a revision-5 Hub before IRIX's flow initialisation settles it.
- **The PI fields outside the IRIX compatibility predicate remain open.** The
  exact IRIX 6.5.30 predicate establishes the required status 0 valid bit,
  address field and per-processor register selection. It does not establish the
  error type, request number, supplemental value, command, status 1 contents,
  pending bit, repeated-error behavior, whether a 32-bit alias access clears
  a 64-bit report, or directly what a store to the status 1 clearing alias does
  in hardware. IRIX's own loops require that store to end status 0 too, and the
  model follows that inference. The error type in particular matters: IRIX
  reads type 0 in a read record as invalid, and the reviewed firmware contract's
  read-response timeout would be the public read time-out type 3.
- **A read of a status 1 clearing alias is only partly modelled.** The register
  table says `0x00_0448` and `0x00_0468` clear on read. The model reads status 1
  as zero, and that read does not clear status 0. No guest reading those
  aliases has been observed to depend on either effect. Whether a read of the
  status 1 alias also ends the status 0 record is unknown. A real-machine register capture or an independently reviewed
  public specification must settle those details. Do not add a test-only seed
  or QOM property to manufacture the missing state.

- **`NI_STATUS_REV_ID` is unimplemented.** Confirmed absent from `hw/misc/sgi-hub.c`
  on current `sgi-origami` trunk (not merely unsearched). The vendor PROM reads it in
  more than one place: the KLCONFIG board-bring-up path extracts a board revision field
  from it (IRIX 6.5.7), and low-level exception code extracts this node's own NASID from
  it. A read of an unimplemented register in this model returns 0, which happens to
  be the correct NASID on this fork's single-node machine — so this gap has not been
  observed to cause a wrong guest-visible value yet, but it means nothing backs the
  board-revision field either, and any guest or firmware path that reads it for a
  different reason gets a value that means "revision zero" rather than "not
  modeled." Surfaced while investigating an unrelated data-bus fault (a different
  session's NASID-in-a-TLB-entry bug) that turned out not to trace back to this
  register. Register layout: the IRIX 6.5.7 SN0 Hub network-interface register header,
  for the `NSRI_REV_MASK`/`NSRI_REV_SHFT` and `NSRI_NODEID_MASK`/`NSRI_NODEID_SHFT`
  field names.

[^theory-hub-pi]: *Origin and Onyx2 Theory of Operations Manual*, SGI
    `007-3439-002`, revision 002, Figure 3-2 and its accompanying Hub message
    description on printed pages 78-79, PDF pages 92-93. SHA-256
    `f6f30191b662aeef6c9888f109c4997d58ac9c4a77acd24263937c6392113309`.
