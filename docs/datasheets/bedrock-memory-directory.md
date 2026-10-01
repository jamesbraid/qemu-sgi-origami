# Datasheet: Bedrock memory and directory interface (MD)

References rendered as citation identities name material outside this document
set. Those targets are not included or cleared for redistribution. Review and
runtime statements retain their recorded dates and scope.

The 2026-09-05 identity correction (citation: `docs/reviews/origin300-identity-correction.md`)
withdraws unsupported IP45 register assignments and scopes inherited controller
names. The original clearance remains recorded for the unchanged contract.

**Status:** reviewed 2026-09-03 by an independent second reader
(`docs/reviews/bedrock-io-md-xb-datasheet-review.md`): all 45 registers cleared position for position
against the SN1 header, with the review's seven findings applied in this revision,
two of them field corrections. The open items stay open. Fork commit
`d1734b50a9de3435a390d77d9e78df2c502faf3f` implements the initial register
storage, masks, idle replies, reset retention and migration. Four MD qtests pass
in a combined run of 54 device subtests. This does not qualify memory operations,
memory protection, refresh progression, self-test execution or SGI diagnostics.
Parent composition is saved in `1058e83a80553de158f2846ceb1cdaed169f4899`: the
combined run has 55 passing subtests, including actual parent soft-reset paths
and MD child migration.
Parent commit `54ce1c3fabaec311812f94df4eb678c1b6122c35` adds current-cell
protection for an explicitly attached remote L1 UART. The restored combined
run passes 59 subtests, and a compiled protection-bypass mutation fails because
a denied read consumes a queued reply. This qualifies the attached UART path,
not memory protection or the other Hub special windows.
The 2026-09-05 expanded-register review (citation: `docs/reviews/bedrock-md-expanded-register-review.md`)
clears the formerly collapsed 23-register row for the explicit idle/storage
policy recorded here. It does not clear the omitted hardware operations.
The 2026-09-09 independent BDDIR review (citation: `docs/reviews/origin300-bddir-contract-independent-review-20260909.md`)
clears the exact hashed delta for the M-mode transform, one 64-bit directory
word per 128-byte line on a 16-byte address stride, standard and premium masks,
zero initialization as model policy, aligned 64-bit access policy, migration,
fitted-memory holes, the exact 512 MiB carriers, and the stated machine qtest.
It excludes page-counter layout and behavior, a hardware directory reset value,
active directory transitions, coherence, ECC, access protection, a nonzero
memory-configuration word, and completion of `go cac`. It does not claim that
the directory region or qtest has been implemented or run. The delta does not
change the open performance-counter behavior at `0x790010`.
The 2026-09-09 page-counter review (citation: `docs/reviews/origin300-bdcnt-contract-review-20260909.md`)
clears the `+8` carrier identity and bounded firmware-facing model policy. The
implementation review (citation: `docs/reviews/origin300-bdcnt-implementation-review-20260909.md`)
clears the corresponding QEMU change. Neither review clears hardware counter
fields, automatic counter behavior, alias decoding, or access protection. The
ECC-backdoor review (citation: `docs/reviews/origin300-bdecc-contract-review-20260909.md`)
clears the bounded five-eighths carrier policy at `097685a421`. Its
conflict-free replay at `369969f099` has the same stable patch ID.
The 2026-09-10 stock-PROM investigation (citation: `docs/notebooks/origin300-bedrock-mbist-and-protection.md`)
records the self-test sequence that times out in the first post-flash diagnostic
run and the protection-entry readback that raises the next exception. The
independent evidence review (citation: `docs/reviews/origin300-bedrock-protection-mbist-review-20260910.md`)
cleared only software bank 0 and its discriminating qtest at exact commit
`5c79f5a334860c1e5dccfbd57515ad56a560314a`. A configured-PROM run through
that implementation reached software bank 1 and proved that bound incomplete.
The corrected eight-bank extent below awaits independent evidence review. It
does not establish the self-test algorithm or complete protection semantics.
The directory-size mismatch investigation (citation: `docs/notebooks/origin300-directory-size-mismatch-20260910.md`)
records the next configured-PROM exception. It finds an ordinary directory
access beyond 512 MiB after firmware sees every software-bank position enabled
with size code 5. A no-SPD comparison reaches the same address, so the supplied
SPD set is not the producer. This mismatch does not expand the ordinary-directory
contract below.
The 2026-09-12 direct guest-error follow-up in the stock-PROM investigation
corrects the first memory-test ECC exception to machine carrier
`0x101_4400_0006`. The
independent correction review (citation: `docs/reviews/bedrock-ecc-carrier-correction-review-20260912.md`)
clears that carrier and its qtest at exact commit
`b7bab1051172027c349fec1850748602ad56cf43`.
The [directory ECC](#directory-ecc-the-codes-checking-on-a-read-and-the-error-record),
[memory ECC](#memory-ecc-the-code-checking-on-a-read-and-the-error-record)
and [memory special space](#memory-special-space-fetch-and-op) sections,
added 2026-09-23 from the Origin 300 diagnostics' own accesses, the public
register layouts and the public SN2 encoding, and the corrected halfword
lane order of the ECC backdoor, await independent evidence review.
The [directory state on processor requests](#directory-state-on-processor-requests)
section, added 2026-09-27 for QEMU pull request 926, reverses the earlier
decision not to model directory transitions. Its
independent evidence review (citation: `docs/reviews/2026-09-27-md-directory-state-review.md`)
cleared the first version's protocol marks and admission rule and held
the change on a four-processor pair regression. The change was reworked
before landing; the
rework's review (citation: `docs/reviews/2026-09-28-md-directory-rework-review.md`)
clears the section's evidence classes and leaves the change held on open
code findings; the implementer's runs show the four-processor pair
passing. A second-round review (citation: `docs/reviews/2026-09-28-o300-integration-reviews.md`)
clears those code findings and the qtests, and found this section of the
datasheet itself out of date against the fixes; this revision addresses
that.

**Fork file(s):** `hw/misc/sgi-bedrock-md.c`,
`include/hw/misc/sgi-bedrock-md.h`, and `tests/qtest/sn1-bedrock-md-test.c`;
for directory state also `hw/mips/sgi/sn1-node.c`, `target/mips/r10k.h`,
`target/mips/tcg/system/r10k_helper.c`, `target/mips/system/physaddr.c`,
`target/mips/tcg/system/tlb_helper.c`, `include/exec/memattrs.h` and
`tests/qtest/sn1-directory-state-test.c`.
**Evidence cutoff:** 2026-09-12. The reviewed register base used Origami trunk
`506836b9dd`. The independently reviewed BDDIR delta used baseline
`918d4deae569b6dd31888c85486649707c6f8481`.

**Sources used:** the SGI-generated Linux SN1 register headers at tag `v2.5.40`
(commit `7570df54ef8c`): `include/asm-ia64/sn/sn1/hubmd.h` (45 registers),
`hubmd_next.h`, `sn1/mmzone_sn1.h`, `sn/leds.h`, `sn1/addrs.h`, `sn/addrs.h`; the
Itanium consumers `arch/ia64/sn/io/{hubspc.c,l1.c}`,
`arch/ia64/sn/io/sn1/{huberror.c,mem_refcnt.c}`, `arch/ia64/sn/kernel/setup.c`.
Linux tag `v2.5.0` (commit `381ab1f538162`) supplies the earlier public
`include/asm-ia64/sn/addrs.h` backdoor formulas that were absent by `v2.5.40`.
Linux mainline commit `db2ddb871435` supplies the IP27-only low/high directory
pair in `arch/mips/include/asm/sn/addrs.h` and the corresponding 128-bit SN0
entry in `arch/mips/include/asm/sn/sn0/hubmd.h`. It is a product comparison,
not evidence for Bedrock. US patent 6,751,698 B1, retained as
`docs/library/cache/patents/US6751698-sn1-bedrock.pdf` with SHA-256
`ec521d758e070b04e387a92a3ca34624452c32d49c835ef30501bb3701e1d7b2`,
identifies separate memory-data and directory-data paths on original PDF page 1
and identifies the Bedrock memory/directory destination on PDF page 14,
sheet 11, figure 7F. Those pages do not define the backdoor address or counters.
OpenBSD at `2df91d80c1`: `sys/arch/sgi/xbow/hub.h`, `sys/arch/sgi/sgi/{ip27_machdep.c,sginode.c}`,
`sys/arch/sgi/include/mnode.h`. The out-of-tree MIPS port `refs/linux-ip35` at
`9f613a321ea0`: `arch/mips/sgi-ip35/ip35-memory.c`, `include/asm/sn/agent.h`. The Origin 3000 System
Architecture 108-0240-002 printed page 2-13 for the unit's role. Reading preserved in
the SN1 research notebook.

No facts in this document derive from restricted IRIX source. A stock IRIX
6.5.30 PROM observation supplies only the software access requirement recorded
under the directory backdoor. It does not establish hardware semantics. The
header states that the refresh enable bit is cleared on reset. The
memory-configuration register, the refresh
enable and threshold, and the directory-configuration register keep their contents
through a soft reset; the outgoing reply queue size register's read-only fields
default to 12 header and 24 data entries. A companion header carries a software
constant of `0x800` it calls the refresh register's reset default, with no consumer
that uses it. The expanded request-queue row records an additional default of four
entries in each of three fields, omitted from the earlier summary. Other reset
values remain unknown unless the register row states otherwise.

## Role

The memory and directory interface is the Bedrock's memory controller and coherence
directory. Per the Origin 3000 architecture book it has three blocks: issue, which
arbitrates requests and drives the DIMM address and control lines; memory, which
carries data and ECC; and directory, which holds coherence state. It supports page
migration, refreshes the DIMMs' DDR SDRAM, and has a built-in self test over data,
ECC and directory. A component fact for the chip; nothing in it says which parts an
Origin 300 fits.

**No public MIPS consumer touches any register of this unit on IP35.** OpenBSD's one
MD access is the Origin 200 LED write at the Origin 200's MD base, executed only when
the system is IP27 and the subtype is Origin 200 (`ip27_machdep.c` lines 426 to 432);
its IP35 MD base is defined and unused. The out-of-tree tree carries no SN1
memory-directory header at all; its only such names are the Origin 200's header and
one bank-count define (`agent.h` line 21). Every consumer takes memory size from the configuration records
the PROM leaves in memory (OpenBSD `sginode.c` through `mnode.h` lines 362 to 365;
`ip35-memory.c` lines 148 to 166), never from the memory-configuration register. So
the unit's boot-path contract is what the PROM programs, which no public source
records, plus the four error-clear registers the Itanium port reads at probe.

Two further facts make the unit matter to an emulator regardless: the current-cell
register is the access-rights vector for the Hub special windows that hold the L1
UART, and the junk-bus timing register is the only register in any unit that names
the UART, the flash PROM and Itanium-side controller timing fields. The Linux
header names the latter for Synergy. That names the header fields, not a device
fitted to IP45; SGI places that ASIC on IP37 in the
board-identity evidence (citation: `docs/library/origin300-identity-evidence.md`).

## Register map

Offsets are hub-relative; the unit starts at `0x780000`. Every register is 64 bits.
Line numbers are `hubmd.h` unless named.

| Offset | Name | Fields | Reset | Access | Consumer | Behavior |
|---|---|---|---|---|---|---|
| `0x780000` | current cell | 63:0 one bit per region | unknown | R/W | none | A region whose bit is set is inside the cell and uses the directory's local-access protection instead of per-region protection. **The same vector is the access rights for the Hub special spaces**: set means read and write, clear means read-only, except the local and remote register windows, which become inaccessible (lines 321 to 354). The text attributes region size to the network interface; the local block's revision register holds it. |
| `0x780008` | memory configuration | per DIMM bank d at 8d: 1:0 bank enables, 3 DRAM width, 7:4 size code; 33:32 bank select; 44 cell protection enable; 45 I/O protection enable; 46 ignore I/O protection; 47 ignore CPU protection; 48 to 53 and 58:56 timing; 60 and 61 the RCD and RP configuration bits | unknown; **survives soft reset** (line 376) | R/W; the tree calls the 58:56 timing field write-only with default 7 (`hubmd_next.h` lines 755 to 756) | none | To configure a bank software sets its size, width and enables; addresses beyond the size return errors; the OS must ask the system controller whether a DIMM is x4 or x8/x16 (lines 361 to 376). Size codes: 0 empty, 1 = 64 MB, 2 = 128 MB, 3 = 256 MB, 4 = 512 MB, 5 = 1 GB, bytes = 32 MB shifted left by the code (`hubmd_next.h` lines 20 to 28, a companion header whose preamble warns that much of it is unrevised from the Origin 200; the codes agree with the register header's 64 MB-times-a-power-of-two statement and the 1 GB bank). |
| `0x780040` | miscellaneous error 0 | message header fields and two-bit error flags | unknown | read | none | Lines 640 to 654. |
| `0x780048` | miscellaneous error 1 | 38-bit address and the same flags | unknown | read | none | Lines 700 to 743. |
| `0x780058` | miscellaneous error 1 clear | as error 1 | unknown | read (consumer) | Itanium `huberror.c` lines 54 to 57 | The header names it a clear register without saying whether a read or a write clears; the consumer reads and discards. |
| `0x780060` | outgoing reply queue size | six read-only fields: three 4-bit header-queue sizes at 11:8 (PI0), 19:16 (PI1) and 27:24 (the third queue), three 5-bit data-queue sizes at 36:32, 44:40 and 52:48 (lines 829 to 846) | **12 in each header field, 24 in each data field**, so the word reads `0x0018_1818_0c0c_0c00` | read; a write resets the credit counters to the defaults (lines 793 to 799) | none | |
| `0x790010` | performance counter 0 | 40:0 count | unknown | R/W | Itanium `io/l1.c` lines 208, 268, 295 to 303 | The Itanium L1 console code uses it as a 20-bit lock word by setting and clearing bits, a use the header does not sanction; a model must at least retain written bits within the 41-bit field. |
| `0x7a0000` | refresh control | 11:0 threshold in Bedrock clocks; 23:12 live counter; 63 enable | **enable 0** (lines 1014 to 1017); enable and threshold survive soft reset | R/W | none | Once memory configuration holds the DIMM information the hardware refreshes every bank; if a window expires with banks unrefreshed, all other traffic halts until they are; the enable is cleared on reset because the first DIMM operation must be a mode-register write; 4096 refreshes per 64 ms give a threshold of 3125 cycles (lines 1003 to 1052); the Origin 3000 book says the interface refreshes the DIMMs about every 8 µs (printed page 2-13). Firmware-programmed; no consumer touches it. |
| `0x7a0008` | junk bus timing | 7:0 flash PROM setup-hold; 15:8 flash PROM enable; 23:16 UART setup-hold; 31:24 UART enable; 39:32 Synergy setup-hold; 47:40 Synergy enable | unknown | R/W | Itanium `hubspc.c` lines 207 to 214 rewrite the two flash fields when a user maps the PROM | Read and write timing for the flash PROM, UART and Synergy "junk bus" devices (lines 1061 to 1097). |
| `0x7a0010`, `0x7a0018`, `0x7a0020`, `0x7a0028` | LED 0 to 3 | 7:0 data | unknown | read (header says "reads of 8-bit LED n") | none on IP35 | Each address allows the value on one 8-bit bank of LEDs to be read (lines 1106 to 1132). Whether a write here drives the LEDs is not stated. |
| `0x7c0000` | directory configuration | 0 premium directory; 1 ignore directory ECC | unknown; **survives soft reset** (lines 1454 to 1455) | R/W | none | Lines 1475 to 1482. |
| `0x7c0010` | directory error | 63 uncorrectable directory valid; 62 access-protection double-bit valid; 61 correctable directory valid; 60 access-protection correctable valid; 59 to 56 their overrun bits in the same order; 54:52 protection index; 51:48 bad protection; 46:40 syndrome; 32:3 Hub special address | zero (model) | read | `ndir` polls it for the directory errors it injects | Lines 1491 to 1514, 1550 to 1584; the model latches directory ECC errors as described under [directory ECC](#directory-ecc-the-codes-checking-on-a-read-and-the-error-record). |
| `0x7c0018` | directory error clear | as directory error | zero (model) | read (consumer) | Itanium probe; `ndir` reads it to clear | Read-to-clear is the model's choice. |
| `0x7c0020` | protocol error | directory state and format, pointer, message type, initiator, valid, overrun | unknown | read | none | Lines 1700 to 1743. |
| `0x7c0028` | protocol error clear | as protocol error | unknown | read (consumer) | Itanium probe | |
| `0x7c0048` | migration value threshold | 14:0 threshold; 34:32 threshold action; 37:35 saturation action; 62 local interrupt enable; 63 valid (lines 1961 to 1972) | unknown | R/W | Itanium `mem_refcnt.c` lines 31 to 32 read it through a 20-bit mask from the companion header (`hubmd_next.h` lines 45 to 46), over-reading five reserved bits harmlessly | Page-migration absolute threshold (lines 1923 to 1937). The companion header's own preamble says most of its contents were carried over from the Origin 200 and are wrong (lines 12 to 15); the register header governs. |
| `0x7e0000` | memory-block ECC configuration | 0 ignore data ECC | unknown | R/W | none | Lines 2116 to 2122. |
| `0x7e0010` | memory error | 63 differing-address flag; 61:60 read uncorrectable overrun and valid; 57:56 write uncorrectable overrun and valid; 53:52 read correctable overrun and valid; 47:40 syndrome; 32:3 address | zero (model) | read | `nmem` reads it after monitor call `0x18` for a read-correctable error | Lines 2131 to 2150, 2255 to 2262; the model latches read errors as described under [memory ECC](#memory-ecc-the-code-checking-on-a-read-and-the-error-record). |
| `0x7e0018` | memory error clear | as memory error | zero (model) | read (consumer) | Itanium probe; `nmem` reads it to clear | Read-to-clear is the model's choice. |

### Remaining register descriptions

The following rows expand the 23 addresses previously grouped as retained-write
storage. Bit layouts describe hardware fields; they do not establish that every
field accepts a guest write. In particular, captured status, error data, and
self-test results must not become writable storage merely because the header
defines their positions. Unspecified reset values are unknown.

| Hub offset | Register | Defined fields | Behavior and limits | Public header lines |
|---|---|---|---|---|
| `0x780010` | arbitration control | 3:0 reply guarantee; 7:4 write guarantee | Control fields are named; arbitration timing is not specified here. | 472–493 |
| `0x780018` | migration configuration | 9:0 interval; 23:16 node mask; 32 enable | Controls page migration. Interval units and complete scheduling are not supplied by these fields. | 508–533 |
| `0x780020`, `0x780028` | fetch-and-op cache status 0 and 1 | 32:6 address; 63 valid | Describes a captured cache entry. Guest write behavior is not established. | 551–612 |
| `0x790000`, `0x790008` | performance selection 0 and 1 | 1:0 mode; 7:4 activity; 14:8 source; 19:16 channel; 59:20 command; 63 interrupt | Complete event encodings and interrupt cadence remain unresolved. | 857–929 |
| `0x790018` | performance counter 1 | 40:0 count | Same 41-bit layout as counter 0. Field width alone does not establish the counting events. | 944–998 |
| `0x7a0030` | self-test control | 0 start; 1 stop; 2 reset; 4 bank; 6:5 DIMM | Controls a running test. Command precedence, self-clearing behavior, algorithm, and duration remain unresolved. A retained write cannot qualify a completed test. | 1245–1282 |
| `0x7a0038` | self-test data | 3:0 initial pattern; 4 nibble control; 5 byte control; 6 cycle control; 7 write control | The pattern-control names are provided, but their full transformation rules are not. | 1289–1325 |
| `0x7a0040` | self-test error address | 14:0 directory column; 30:16 memory column; 46:32 row; 48 memory error; 49 directory error | Captures the failing address and affected block. Guest writes and clear mechanism are unresolved. | 1332–1375 |
| `0x7a0048` | self-test status | 0 passed; 1 done | Reports test results. The model must not set either bit to simulate success without running the specified test. | 1380–1411 |
| `0x7a0060` | issue-block debug selection | Layout declares 1:0 | The prose says three bits while both endian layouts declare two. Width conflict remains open. Debug output selection meanings are not supplied. | 1418–1448 |
| `0x7c0030`, `0x7c0038` | migration candidate and candidate-clear | 20:0 page address; 31:21 initiator; 32 overrun; 33 threshold type; 34 local; 63 valid | On overrun the newest candidate replaces the earlier one. The clear operation is not established by the repeated description alone. | 1805–1868 |
| `0x7c0040` | migration difference threshold | 14:0 threshold; 34:32 threshold action; 37:35 saturation action; 63 valid | A valid threshold is reached when the difference is at least the threshold. Candidate capture and interrupt depend on the migration mode; action encodings remain unresolved. | 1877–1915 |
| `0x7c0050` | outgoing request queue size | 10:8 PI0; 18:16 PI1; 26:24 third request queue | Each field defaults to four and has a maximum of four. A header occupies one entry. Aggregate default is `0x0000000004040400`. Writes above four and queue-resize side effects are not specified. | 1978–2020 |
| `0x7c0058` | self-test directory error data | 31:0 failing directory word | Captured test failure data. Guest writes are not established. | 2025–2055 |
| `0x7c0060` | directory-block debug selection | 1:0 selection | Selector meanings require the separate design specification. | 2062–2090 |
| `0x7e0020`, `0x7e0028`, `0x7e0030`, `0x7e0038` | self-test memory error data 0 to 3 | 35:0 in each register | Four captured quarters of the failing memory line. Guest writes are not established. | 2291–2423 |
| `0x7e0040` | memory-block debug selection | 0 selection | Selector meaning requires the separate design specification. | 2430–2459 |

These fields were read through canonical Library MCP from public root
`linux-history`, `include/asm-ia64/sn/sn1/hubmd.h` at `7570df54ef8c` on
2026-09-05. The address map was read at lines 25–313. The bounded field reads
covered lines 430–639, 850–998, 1150–1448, 1800–1924, 1970–2114, and 2290–2476.
No restricted source or firmware was read. The source reader cannot implement MD.

### Stock-PROM self-test requirement

The public fields establish the register locations and bit meanings. A retained
stock IP35 PROM 6.210 disassembly adds a precise software requirement for one
probe mode. It uses only aligned 64-bit loads and stores for this interface.
On NASID zero the MIPS addresses are `0x9200_0000_01fa_0030` for control,
`0x9200_0000_01fa_0038` for data, `0x9200_0000_01fa_0040` for error address,
and `0x9200_0000_01fa_0048` for status.
For every selected DIMM and bank, it writes `0x4` to self-test control to reset
the engine, writes `pattern | 0x80` to self-test data, and writes
`((dimm << 5) & 0x60) | ((bank << 4) & 0x10) | 1` to self-test control to
start. The memory-probe caller supplies pattern `0x5a`, so the observed data
word is `0xda`.
The eight start words are `0x01`, `0x11`, `0x21`, `0x31`, `0x41`, `0x51`,
`0x61`, and `0x71` for DIMM-bank pairs 0/0 through 3/1.

The PROM polls self-test status bit 1, `done`, until it sets or a deadline
expires. The exact deadline increment is 625,000 in the PROM's scaled-time
domain. The time helper reads the 64-bit local-block real-time clock control at
MIPS address `0x9200_0000_0160_4200` and the 64-bit PI real-time counter at
`0x9200_0000_0103_0100`, then calculates
`counter * ((((control & 0x3ff0) >> 3) + 2)) / 200`. With the documented
1,250,000 Hz counter and the selected 200 MHz Bedrock clock, 625,000 scaled
units infer a 625 ms deadline. The immediate and comparison are established
firmware behavior; the time conversion is inferred from those two public clock
facts.

In the mode used by the memory-probe caller, the first `done` result causes the
PROM to write the original pattern `0x5a` without bit 7, write the same start
word again, and await `done` under a new deadline. It accepts the final result
only when status bits 1:0 are both set. It does not wait to observe `done`
clear after reset or either start command, so the hardware's intermediate
transition and self-clearing behavior remain unknown.
If `done` is set and `passed` is clear, it reads self-test error address
`0x7a0040`, directory error data `0x7c0058`, and the four memory error data
registers at `0x7e0020` through `0x7e0038`. If `done` remains clear, it writes
the stop command `0x2`, reports a hang, and returns `-2` without reading those
error-capture registers.

The memory-probe caller walks DIMMs 0 through 3 and banks 0 and 1. It invokes
each selection twice after a negative return. Run
`prom-20260910T164049-de445a` therefore prints two hang and `-2` pairs for
each of eight selections. Status staying zero explains the result exactly:
`-2` is the PROM's timeout result, not a self-test report of bad memory.

This sequence establishes what this PROM requires, not hardware command
precedence, self-clearing behavior, reset status, algorithm, or duration on a
physical Bedrock. A functional model still needs an independently reviewed
policy for the test operation. It must at least preserve the 64-bit access
contract and the visible `done` and `passed` distinction; unconditional success
would hide both timeout and failure paths.

### Functional self-test approximation

The first Origin 300 model uses a bounded command-state approximation so stock
firmware can distinguish configured memory from an absent bank. This is model
policy, not a claim about Bedrock's physical self-test algorithm or timing.

Only aligned 64-bit accesses are accepted. Bits 2:0 are command bits and bits
6:4 select the DIMM and bank. Reset is exactly `0x4` and stop is exactly `0x2`;
either command cancels a pending completion and clears `done` and `passed`.
A start has command bits equal to `0x1`, permits any selector in bits 6:4 and
has every other bit clear. It latches the selector, self-test data bit 7 and the
eventual result, clears the prior status, and completes after one microsecond of
QEMU virtual time. A new supported start replaces a pending operation and
restarts that interval.

Combined command bits, selector bits on reset or stop, and any reserved set bit
are unsupported. Such a write is logged and preserves the current result and
any pending completion. The one-microsecond interval is model policy chosen to
make the running state observable while remaining well below the PROM's
inferred 625 ms deadline. It is not a claim about physical Bedrock timing.

**Completion time.** The interval counts from the arrival of the start
command, and a read of status or error address at or after the completion
time returns the completed result, whether or not QEMU's main loop has run
the completion timer yet. This is model policy resting on established
firmware behavior. The PROM's disassembly shows each wait reading the clock
before it stores the data and start words. The PROM adds its deadline
increment to that sample, reads status once right after the start, and
reports a hang with result `-2` if `done` is still clear once its clock
passes the deadline. In the model, the zero-pattern phase below runs its
fill inside the start store, and the model's PI real-time counter, which
the PROM reads, advances with host time meanwhile.

Before this rule the interval started after the fill, and completion
appeared only when the main loop ran the timer. A fill longer than the
deadline therefore became a PROM hang whenever the processor polled before
the main loop ran. On a host at one-minute load average 75 to 190 (as
recorded at each boot's launch by `tools/origami test boot-repeat`), the
first fill of each 256 MiB bank took 1.7 to 12.3 s of host time and later
fills 66 to 400 ms; that the difference is the host faulting in RAM the fill writes
first is inferred, not measured. Runs `prom-20260925T085952-1dffa8` and
`prom-20260925T155403-efccb4`, with temporary MD tracing, show a 2.7 s and
a 3.0 s fill, a status read of zero right after it, then `MBIST hang` and a
bank swap. In the first run the timer published completion 8 ms after that
read. Under this rule, fills of up to 12.3 s were followed by a first read
of `done | passed`. None of this describes hardware timing: the
one-microsecond interval, and a fill that completes within the start store,
remain model policy. The qtest below covers publication on read only; under
qtest the fill takes no virtual time, so counting the interval from the
start rests on the traced boots above.

The selector identifies locational bank `2 * DIMM + bank`. The machine supplies
the set of installed locational banks from its configured SPD topology. A supported
start samples that population and the memory configuration at that instant. The
bank-select field maps the locational bank to its software bank by exclusive-or.
The selected bank passes only when it is installed, enabled, has size code 1 through
5, and its entire backing range fits in the attached RAM. A temporary
memory-configuration descriptor selects a bank for the test; it does not create
an absent bank or let several selectors reuse one dense backing range. Later
memory-configuration writes cannot change the latched result.

The selected two-DIMM, 512 MiB inventory contains 256 MiB modules at the
guide's positions 2 and 3, with positions 0 and 1 empty. The earlier
single-bank-0 approximation disagrees with this inventory. The model assigns
256 MiB to each locational selector 2 and 3 (installed mask `0x0c`). Equating
the guide's ordinals with locational selectors is an explicit approximation,
not a measured Bedrock rank map. The published inventory and fixed bank
windows support this choice; they do not establish the physical interleave
or cold-reset configuration. See the
selected population (citation: `docs/decisions/D-0023-origin300-initial-population.md`) and
memory decoder evidence review (citation: `docs/reviews/origin300-memory-decoder-evidence-review.md`).

Installed capacity is independent of temporary configuration words. With
bank-select zero, selectors 2 and 3 occupy separate 1 GiB windows, each with
256 MiB of storage. The existing XOR rule relocates those windows when
bank-select changes. Dense host backing follows installed locational-bank
order; a temporary enable cannot create another bank or consume its storage.

Stock PROM does not derive its memory-configuration enable bits directly from
the visible `passed` status. It interprets the detailed self-test error records.
Run `prom-20260912T091715-a30d6e` reports failure for selectors 1 through 7,
and its lower test routine returns a nonzero error count even when every detail
register is zero. Its caller nevertheless treats an all-zero error-address word
as a premium-memory classification. Error-address bit 48 or bit 49 is the
minimum branch-discriminating detail that prevents that classification. The
public register description names those bits as memory error and directory
error. The other captured fields do not control this branch.

For the absent-bank compatibility case, report error-address bit 48 as explicit
model policy and leave the directory-error word and four memory-error words
zero. This is the smallest value consistent with the public meaning and the
PROM branch; it does not define a physical Bedrock error address, algorithm, or
data syndrome. Successful selectors report status `done | passed` and all six
detail registers as zero. Failed selectors report `done`, clear `passed`, and
set error-address bit 48. The detail registers return to zero on reset and at
the next supported start.

These results still do not determine bank enables. Firmware's provisional-word
loop enables both banks in all four descriptors independently of the self-test
result. Its later address-based capacity probe removes absent banks. In the run,
aliased protection carriers make that probe return size code 4 for every
software bank, producing `0x2700_0000_4b4b_4b4b`. No final
memory-configuration word is specified by this self-test approximation. The
installed-bank rule and minimum error detail are stock-PROM compatibility policy,
not physical Bedrock measurements or proof of the exact SPD rank-to-Bedrock
descriptor mapping.

Every supported start eventually sets `done`. It also sets `passed` when the
latched validation succeeds; an absent, disabled, partly backed, or invalid
carrier completes with `passed` clear. Both phases selected by self-test data
bit 7 use the same validation. The self-test data register retains only its
published low eight bits. Result and error-capture registers remain read-only;
failed selectors set error-address bit 48, while the other five detail
registers report zero.

**Zero-pattern write phase.** A supported start whose self-test data is exactly
`0x80` (write control, initial pattern zero, no nibble, byte or cycle
control) and whose selected bank passes validation writes zero through the
bank's whole configured range at the start, and unmarks every doubleword
there that the ECC backdoor had marked ([memory ECC](#memory-ecc-the-code-checking-on-a-read-and-the-error-record)).
Inferred, not established: the Origin 3000 architecture manual
(108-0240-002, the MD's feature list in chapter 2) says the unit's
built-in self-test covers data, ECC and directory memory, and the data
register names bit 7 write control and bits 3:0 the initial pattern; that
pattern zero with no other control writes zero is the reading of those
names, and data zero with check byte zero is a codeword under any linear
code. Why it matters: the stock PROM runs such a phase over each bank before
and after its memory test, whose ECC-storage subtest leaves check bytes
that disagree with their data. On run `prom-20260924T174855-38c847` (QEMU
`8b80582993` plus tracing) the order for selector 3 was the zero phase,
1,812 backdoor writes, five pattern pairs (`0xc5`/`0x45`, `0xca`/`0x4a`,
`0x9c`/`0x1c`, `0x93`/`0x13`, `0xf9`/`0x79`) and a final zero phase. That
trace records register and backdoor writes only; a host watchpoint on one
marked doubleword (run `prom-20260924T173650-77776d`) recorded no data
write to it after the memory test. Without the fill the marks survive the
boot: `scct` and `ndir` read them before writing them
(see the memory ECC section). The phases with other patterns leave memory
unchanged, because the published names do not give their transformation
rules. The fill writes no directory word except that, like any data
write, it re-encodes the entry of each marked line it covers, which
unmarks the line; unmarked entries stay as they are. Whether the hardware
self-test leaves the directory zero is not established. A discriminating
test: mark a doubleword, run a `0xc5` phase, which must leave the mark,
then a `0x80` phase, which must clear it and zero the word, and a bank
outside the selector must keep its data.

This policy does not test DRAM patterns, directory contents, ECC, timing, stuck
bits, or coupling faults. A firmware memory probe can use it to accept or reject
the modeled bank configuration. SGI memory diagnostics that claim to exercise
those omitted failures do not pass merely because this state machine reports a
configured carrier.

A discriminating qtest must observe `done` clear before advancing virtual time
and `done | passed` after one microsecond for each data-bit-7 phase. The second
start must clear the first phase's result. Another case keeps the machine stopped, so
stepping the clock runs no timer: `done` stays clear one nanosecond before
the completion time and reads set at it, and a failed selector's error
address reads `1 << 48` when read before status. A model that publishes
completion only from its timer fails that case. Separate negative cases cover an
absent bank, a disabled bank, an invalid size code and a range only partly
backed by RAM, using a nonzero bank selector. Tests must also cover a new start
replacing a pending operation; control reset, stop and system reset cancelling
pending completion; unsupported writes preserving pending and completed state;
read-only status and error capture; and 64-bit access enforcement. Each cancel
case advances beyond the old deadline and verifies that a stale timer cannot
publish a result. The tests must fail if start reports immediate or
unconditional success.

The Origin 300 machine case must use 512 MiB of backing split between installed
locational banks 2 and 3. It cycles all eight selectors with configurations
within each installed bank's capacity, including both self-test phases.
Only selectors 2 and 3 report `done | passed`; absent selectors report
`done` with `passed` clear. It also confirms that an uninstalled bank's physical
window remains absent after configuration. A model that derives presence solely from each
temporary configuration, or maps every selector onto the same dense backing,
fails this test. For successful selectors, all six error-detail registers read zero. For
each failed selector, error address reads `1 << 48`; directory error data and
all four memory error data words read zero. A CPU-through-device test executes
the PROM's two self-test phases and requires its lower routine to return zero
for installed selectors and a nonzero detailed-error count for absent selectors.
It also rejects the all-zero failed-selector error address because that takes
the caller's false premium-classification branch. This test does not require a
final memory-configuration word; the separate capacity-probe contract owns
that result.

Migration during a running phase preserves the running flag, latched selector,
phase and result, plus the remaining virtual time. The destination must remain
incomplete until that saved interval expires. Migration also preserves a
completed success or failure result. Cold and system reset cancel the timer and
clear the running and result state; migration invokes neither reset policy.

### Cleared initial register policy

The expanded-register review clears the following deterministic model policy.
Unknown reset and access behavior is not presented as measured hardware behavior.

| Addresses | Initial behavior |
|---|---|
| `0x780010`, `0x780018`, `0x790000`, `0x790008`, `0x790018`, `0x7a0038`, `0x7a0060`, `0x7c0040`, `0x7c0060`, `0x7e0040` | Reset to zero and retain only the listed fields. The issue-block debug selector retains bits 1:0 pending the width conflict. Selections do not cause an unimplemented operation. |
| `0x780020`, `0x780028`, `0x7a0040`, `0x7c0030`, `0x7c0058`, `0x7e0020`, `0x7e0028`, `0x7e0030`, `0x7e0038` | Return zero while event generation is absent. After a completed functional self-test failure, `0x7a0040` follows the bounded policy above. Ignore and diagnose guest writes. Do not fabricate state for the other registers. |
| `0x7a0030`, `0x7a0048` | Implement the bounded functional self-test state machine above. Control reads zero. Status resets to zero and exposes only `done` and `passed`; guest writes to status are ignored and diagnosed. |
| `0x7c0038` | Read zero and diagnose ignored writes. An idle read does not prove clearing of a real candidate. |
| `0x7c0050` | Read fixed `0x0000000004040400` and diagnose ignored writes. This is the initial model policy, not an established hardware rule for reprogramming the queue. |

Qtests must reject missing storage, incorrect masks, register aliases, wrong byte
order, and lost state after migration. Status-zero checks cover idle presence
only. They cannot qualify error clearing, candidate generation, queue credits,
or self-test. Unsupported command tests must verify that success is not fabricated.
Soft-reset retention of memory configuration, refresh configuration, and directory
configuration remains separately required.

### Initial reset policy

Cold reset selects all ones for current-cell, the documented reply and request
queue defaults, and zero for other state. Current-cell and unspecified zero
values are model choices. Refresh enable zero is documented hardware behavior.

The model's soft-reset operation preserves memory configuration, refresh enable
and threshold, and directory configuration. It resets the refresh live counter
to zero, cancels a pending self-test and restores other state to the cold values.
The live-counter and other unspecified soft-reset values are model choices. VM
migration preserves state, including the self-test state defined above, and must
not invoke either reset policy.

Bank geometry the headers state: eight software banks of 1 GB per node, software
bank n at node offset n GB (`hubmd_next.h` lines 242 to 248; `mmzone_sn1.h` lines
39 to 58); four DIMM banks of two physical banks each; the bank-select field maps
locational banks to software banks by exclusive-or (`hubmd_next.h` lines 225 to
230). The Origin 300's four DIMM slots in two pairs, 512 MB to 4 GB, are the product
fact in the evidence matrix.

### Directory backdoor

The public SN1 address header at `v2.5.0` places the directory backdoor at
seven eighths of the node's Hub special space. For an ordinary physical memory
address `pa`, the M-mode address is:

```text
HSPEC_BASE | 7/8 * 8 GiB | (pa & NASID_MASK) |
    ((pa >> 3) & 0x3ffffff0)
```

The mask clears the low four address bits. Directory-word addresses therefore
use a 16-byte stride, with one 64-bit directory word for each 128-byte memory
cache line. The inverse directory transform takes the node bits unchanged and
shifts the masked directory-word address left by three. For node zero,
directory words begin at MIPS uncached address
`0x9000_0001_c000_0000`. The QEMU carrier for the same Hub-space address is
`0x101_c000_0000`.

Ordinary carriers answer only for installed, enabled memory. Apply the
transform separately to both 256 MiB intervals of the selected Origin 300
population, after bank-select relocation, and preserve the holes. Dense host
backing follows locational-bank order for main memory, directory and ECC alike.
A flat 512 MiB isolated fixture instead begins at `0x101_c000_0000` and ends
before `0x101_c400_0000`; that fixture is not the production memory layout.
The whole 1 GiB backdoor aperture must not become ordinary RAM.

The public companion header supplies zero as the software initialization value
for both formats. It does not state a hardware reset value. A standard
directory uses the low 32 bits. A premium directory uses all 64 bits. The
directory-configuration register selects the format. The initial model policy
stores those reviewed bits, resets fresh entries to zero, and preserves the
storage across migration. Directory words use 64-bit accesses aligned to their
`+0` carrier. Narrow or unaligned accesses fail as an explicit model policy.
A word's check field is regenerated on a write unless directory ECC is
ignored, and checked on a read of its memory line, as
[directory ECC](#directory-ecc-the-codes-checking-on-a-read-and-the-error-record)
records. Access protection and coherence side effects on other units are
not implemented. Directory transitions are followed for a bounded set of
lines, as the next section records.

#### Directory state on processor requests

On hardware the unit rewrites a line's directory word as it serves each
processor request: a read miss, a write or ownership request, a prefetch, a
write-back. The directory word visible through this backdoor therefore
follows the processors' cache misses. The model keeps no processor caches:
under TCG a cached load or store reaches memory as an ordinary RAM access.
With QEMU pull request 926 (the Origin 300 integration branch) the unit
follows the directory word for the lines of a bounded set of *watched*
pages. A line's page is watched once a processor has named the line in a
secondary-cache instruction and software then reads the line's word through
this backdoor, which is how a diagnostic checks a transition. For watched
pages the unit applies the protocol below to every coherent cached load and
store, R10000 secondary-cache instruction and prefetch that reaches them.
Every other line keeps the storage-only behaviour: its word changes only
through this backdoor and the ECC rules above. The earlier decision not to
model transitions at all, recorded here until 2026-09-27, is reversed by
this section; the
2026-09-27 review (citation: `docs/reviews/2026-09-27-md-directory-state-review.md`)
records the evidence for the first version, and the
2026-09-28 review (citation: `docs/reviews/2026-09-28-md-directory-rework-review.md`)
the evidence for the rework described here.

The one consumer that reads the transitions is `ndir`'s section 4, "Directory
State Verification Test". Before QEMU pull request 926 it failed at its
first transition. On run `prom-20260924T215719-e3cd21` (two R14000, 512 MB)
it passes sections 0 to 3 and fails with `Failed  0x000000e3
0x00000000 0x0001  0`, "UNOWNED -> (READ) -> EXCLUSIVE (CLEAN)", "Expect
Format: POINTER, Actual Format: UNOWNED", at memory `0xa800000000580000`,
directory carrier `0x90000001c00b0000`, word zero. What the section checks,
from its disassembly (routine `0x10006f84`, checker `0x10009964`):

- It starts each line with a cached store of zero followed by index
  write-back-invalidate operations on the line's primary data cache set
  (`cache 1`, both ways) and secondary cache set (`cache 3`, both ways),
  expecting UNOWNED (`0x100097dc`). "Flush" in the steps below is that same
  pair of index operations (`0x100098a4`).
- The single-processor sequence is, in order: UNOWNED -(read)-> EXCLUSIVE
  clean; EXCLUSIVE clean -(read)-> EXCLUSIVE clean; -(flush)-> EXCLUSIVE
  clean; -(write)-> EXCLUSIVE dirty; -(read)-> EXCLUSIVE dirty; -(write)->
  EXCLUSIVE dirty; -(flush)-> UNOWNED; UNOWNED -(write)-> EXCLUSIVE dirty;
  -(flush)-> UNOWNED; UNOWNED -(prefetch with read hint)-> SHARED; SHARED
  -(write)-> EXCLUSIVE dirty; -(flush)-> UNOWNED. A read is a cached `ld`, a write a cached `sd`, and the prefetch
  is `pref 6` (`0x1000034c`), which the diagnostic's messages call a
  prefetch with read hint.
- Its multi-processor steps, named by its own messages, are: UNOWNED or
  EXCLUSIVE -(all processors read)-> EXCLUSIVE or SHARED; SHARED or
  EXCLUSIVE dirty -(all write)-> EXCLUSIVE dirty; EXCLUSIVE dirty or SHARED
  -(all read)-> EXCLUSIVE dirty or SHARED; SHARED -(write)-> EXCLUSIVE dirty;
  EXCLUSIVE dirty -(all read)-> EXCLUSIVE dirty or SHARED; SHARED -(all
  flush)-> SHARED; SHARED -(write and flush)-> UNOWNED. The order and exact
  checks of these steps were not traced.
- A pointer-format entry is checked for its state field and its first
  pointer, which must name the requester. A shared entry is checked for its
  first vector or its first tag.

The encodings it reads, and how strongly each is known:

- Established, from the public SN1 header `hubmd_next.h` at Linux `v2.5.40`:
  format in bits 1:0 of both formats (0 unowned, 1 pointer, 2 shared fine,
  3 shared coarse for standard or shared sparse for premium); the pointer
  format's state in bits 4:2 (0 exclusive, 4 busy shared, 5 busy exclusive,
  6 wait, 7 poisoned, with 1 unowned and 2 shared as software-only values
  for the other formats); the first pointer in bits 15:8, extended in a
  premium word by bits 24:22 as pointer bits 10:8; the shared-fine format's
  first tag in bits 4:2 (premium adds 23:22) and first vector in bits 15:8
  (premium adds 31:24). A pointer carries its node above three device bits
  (`MD_PTR_NODE_SHFT` 3, `MD_PTR_DEVICE_MASK` 7). Device bit 1 marks an I/O
  owner and its absence a processor (`MD_PTR_OWNER_IO(x)` is `x & 2`,
  `MD_PTR_OWNER_CPU` its complement). A processor's slice is device bit 0
  plus device bit 2 shifted down one (`MD_PTR_SLICE`, from
  `MD_PTR_SUBNODE0_MASK` 1 and `MD_PTR_SUBNODE1_MASK` 4), so slices 0 to 3
  are device values 0, 1, 4 and 5. The decoders at `0x1000857c`,
  `0x100085e0`, `0x100085fc`, `0x100086d8` and `0x10008764` read exactly
  these fields, and `ndir` expects the requester's pointer to be
  `nasid << 3` plus 0, 1, 4 or 5 for processors 0 to 3, which agrees.
- Inferred from `ndir` alone: a shared-fine vector bit stands for one
  processor interface, index `nasid * 2 + processor / 2`, not one
  processor; a flush of a clean exclusive line sends nothing and leaves the
  owner in place, while a flush of a dirty line writes back and returns the
  line to UNOWNED; a read of an UNOWNED line grants EXCLUSIVE and a
  prefetch with a read hint grants SHARED. The Origin 3000 architecture
  book (108-0240-002, printed page 1-3) describes the unowned, exclusive
  and shared states and invalidation in general terms only.
- Unknown: the busy and wait states' use on Bedrock, what the unit does to
  a word for an I/O requester, and the premium-directory behaviour of each
  transition.

##### The protocol the model applies

Each item is marked by how strongly it is known.

- Established for the Origin 2000's Hub, from Laudon and Lenoski, "The SGI
  Origin: A ccNUMA Highly Scalable Server", ISCA 1997, section 3.2 (PDF
  page 4, the read, read-exclusive, upgrade and write-back flows): a read
  of an unowned line, or of one the reader already owns, makes the reader
  the exclusive owner; a read of a shared line adds the reader to the
  sharers; a read of a line another processor owns ends shared between the
  two after an intervention; a read-exclusive or an upgrade makes the
  requester the owner and invalidates every other copy; the owner's
  write-back returns the line to unowned. Clean-exclusive and shared copies
  are dropped without telling the directory (the paper's section 2, PDF
  page 2), so the processors send no eliminate request; the R10000 makes
  that request optional behind its PrcElmReq mode bit (manual printed page
  123).
- Established for the processor, from the MIPS R10000 Microprocessor User's
  Manual version 2.0, Table 4-2 (printed page 57), the cache-algorithm
  text on printed page 54 and section 4.6: under cacheable coherent
  exclusive on write (algorithm 5) a load miss issues a coherent block read
  shared, which "may be returned in either a Shared or Exclusive state";
  under cacheable coherent exclusive (algorithm 4) a load miss issues a
  coherent block read exclusive; under either a store miss issues a read
  exclusive and a store to a shared copy an upgrade. Cacheable noncoherent
  (algorithm 3) and uncached references make no coherent request. Index
  WriteBack Invalidate (S) and Hit WriteBack Invalidate (S) write a dirty
  block back and drop a clean one, and Hit Invalidate (S) drops the block
  without a write-back (manual sections 10.4, 10.16 and 10.13). `ndir`'s
  addresses (`0xa800...`) are algorithm 5, so its loads are reads shared,
  and the Origin 2000 read flow above grants an unowned line EXCLUSIVE, as
  `ndir`'s first transition expects.
- Implemented from those two sources: each processor reference carries its
  cache algorithm, and the unit serves only algorithm 4 and 5 references as
  coherent requests, an algorithm-4 load miss as a read exclusive and an
  algorithm-5 one as a read. A reference with no algorithm is taken to be
  a device's, such as the block transfer engine's.
- Inferred: that Bedrock serves the block transfer engine and other devices
  as I/O requesters; the public header defines only the I/O-owner bit.
- Approximated: a device's reference changes no word and no processor's
  copy. On hardware a DMA write to a line a processor holds must recall or
  invalidate that copy; what the unit writes to the word is unknown.
- Inferred from the manual: that a store-hint prefetch reads exclusive
  (Table 1-1's note that a store prefetch must obtain an exclusive copy),
  and that a load-hint prefetch under algorithm 4 does too, as the manual
  says of algorithm-4 load misses in general. That the unit leaves the word
  unchanged for an algorithm-3 reference is also inferred: the manual
  states only the processor's side, a noncoherent block read.
- Unknown: which read an instruction miss issues. Table 6-5 gives it its
  own read cause, and Table 4-2 speaks only of load and store misses. The
  model serves an instruction fetch from a watched page as a load under
  its algorithm, a read exclusive under algorithm 4; no test checks it.
- Inferred for Bedrock: that the Origin 2000 protocol above carries over to
  the Origin 300's memory unit. The Origin 3000 architecture book
  (108-0240-002, printed page 1-3) names the same states in general terms,
  and every transition `ndir`'s single-processor sequence expects is the
  protocol's. Components transfer between these machines only where the
  evidence matches, and here it is the diagnostic's expectation, not a
  Bedrock specification.
- Inferred from `ndir` alone, as the list above records: one shared-fine
  vector bit per processor interface, that a prefetch with a load hint of
  an unowned line is served SHARED, and that Bedrock, like the Origin 2000,
  takes no eliminate requests.
- Approximated: every request completes at the access that makes it. The
  busy-shared and busy-exclusive states, negative acknowledgements,
  interventions and invalidation acknowledgements never appear in the
  word. There is no capacity eviction: a processor's copy leaves only
  through a secondary-cache instruction or another processor's request,
  never through replacement. A rewritten word keeps its local-protection
  bit. The priority, second-pointer and critical-word fields of a pointer
  entry are written zero, which is model policy.
- Model policy, hardware rule not established: a rewritten word is written
  as a backdoor write is, so its check field is regenerated unless
  directory ECC is ignored, when the old field is kept. A word that is then
  not a codeword gets a directory check-error mark. With the directory's
  ignore-ECC bit set, a marked line's transitions continue like any other
  line's (`md_line_frozen`, QEMU `3c886ccc8e`, "keep lines changing while
  ECC checks are off"); with the bit clear, they stop.
- Left alone and logged as unimplemented: a word in a state the protocol
  does not produce (busy, wait, poisoned, shared coarse or sparse, an I/O
  owner, or a pointer or sharer the machine does not have), a sharer set
  that would span more than two tags, and a line carrying a directory
  check-error mark.

##### Which lines the model follows

The unit keeps each processor's copy of a line apart from the word, because
the word does not record it: whether the owner's copy is dirty, which
decides whether a flush writes the line back and unowns it or drops it
silently, and whether a processor has silently dropped a clean or shared
copy, which decides whether a Hit (S) instruction finds it, are processor
cache state. Following a line has two parts.

- Naming. An Index WriteBack Invalidate (S), Hit WriteBack Invalidate (S) or
  Hit Invalidate (S) marks the line at its address, on that line's home
  node, as named. A prefetch names nothing, since copy loops issue them by
  the thousand. The mark is one bit per directory entry. It is tested and
  then set atomically without the model's lock, because the firmware's
  cache-flush loops name the same lines from every processor.
  On the eight-processor pair, single runs with an unconditional atomic
  write, and separately with the lock taken on every instruction, missed
  the firmware's global barrier ("Global barrier failed at line 5300",
  then a General Exception on node 0); a run without either passed. That
  explanation is now doubtful: base run `prom-20260928T101446-25b8e2`,
  built before the system-request region's exemption from the re-entrancy
  guard (QEMU `121f7dd192`, "exempt system requests from the reentrancy
  guard"), reproduces the same barrier failure and
  exception directly from a board-ID read the guard refused, with naming
  unchanged. The re-entrancy exemption
  review (citation: `docs/reviews/2026-09-28-o300-integration-reviews.md#2-system-requests-exempt-from-the-re-entrancy-guard`)
  traces a near-identical failure to that guard by a QEMU backtrace and
  matching stderr, which is the better-supported cause. Neither this run's
  stderr nor a backtrace was checked to confirm it, and naming's own cost
  is not shown to matter; repeated pair boots at the landing revision
  would settle it.
- Watching. A `+0` backdoor read of a named line's word, other than a debug
  access, watches the line's 4 KiB page, 32 lines, in one of sixteen slots:
  a free one, or the least recently used, where a backdoor read of a named
  line on the page, or a processor load, store or request to the page,
  counts as a use.
  Backdoor writes and reads of unnamed lines, such as a directory walk's,
  watch nothing. A newly watched page's lines take their copies from their
  words the first time the model sees them: the owner a pointer names
  holds the line clean exclusive, and both fitted processors on an interface
  a sharer bit names hold it shared, since the vector does not say which. A
  dirty owner cannot be read from the word; clean is the choice that never
  changes the word by itself.
- A watched page gets an overlay on the memory window, the same mechanism
  as the ECC and directory marks, above both. The overlay serves the access
  through the existing checked memory paths, then applies the protocol for
  the requesting processor, named by the reference's requester ID. The
  node sets each processor's ID to its directory pointer, the node number
  above device bits 0, 1, 4 or 5 by slot, and updates it when the node
  number changes. The overlay also sits in the node's DMA address space,
  where references carry no algorithm and change nothing. QEMU handles a
  page holding any overlay on its slow path, and code on a watched page
  runs one instruction per translation block, since the overlay is an I/O
  region.
- Secondary-cache instructions and prefetches reach the memory unit
  through a per-node system-request region the processor reads, the way
  QEMU's generic CACHE model reaches its inter-thread-communication tags.
  A Hit WriteBack Invalidate (S) or Hit Invalidate (S) instruction acts on
  the processor's kept copy of the line whether or not its page is
  watched, as the processor's own cache would; Status.CH reports what that
  action found (`sgi_bedrock_md_processor_request`, QEMU `872ee38ab5`,
  "apply cache instructions whether or not watched"). An Index WriteBack
  Invalidate (S) names a set, not a way, and the same action reaches every
  line the processor holds whose address matches in the set bits, on the
  processor's own node and its one cabled peer, all an Origin 300 pair
  has, again whether or not each line's page is watched. A prefetch stays
  gated on the page being watched, since it is a request for the line like
  a load or store, not an action on a copy already held.
- Moving a slot to another page removes the overlay and nothing else. The
  word and the copies of every line ever followed stay as they were, and
  watching the page again takes them up where they were left. The move
  itself changes no word, copy or check field. What changes is how later
  accesses and cache instructions to that page are served; the limitations
  below list the consequences. System reset and the unit's soft reset empty
  the slots, the named marks and the copies; loading a migration stream
  clears them before it restores the saved ones.

##### Limitations

The main limit: the word only reflects processor loads, stores and
prefetches on lines whose page is watched, and a page is watched only
after a line on it has been named in a secondary-cache instruction and its
word read through the backdoor. Every line on a watched page is followed,
named or not, for the word. An access to a line on any other page leaves
the word where hardware would change it. A consumer that reads the word
without first flushing the line, or that relies on a word transition made
while the page was not watched, sees the storage-only behaviour. A
processor's own kept copy of a line does not share this limit: a
secondary-cache instruction acts on it whether or not the page is watched,
as [Which lines the model follows](#which-lines-the-model-follows)
describes. This rule for which lines the word follows is model policy
chosen to serve the diagnostic's flush-then-read pattern; it has no
hardware counterpart.

The 2026-09-28 review (citation: `docs/reviews/2026-09-28-md-directory-rework-review.md`)
found that the model is bounded by more than that one statement. Besides the
approximations the protocol list above records:

- Fixed: a secondary-cache instruction now acts on a processor's kept copy
  whether or not its page is watched (QEMU `872ee38ab5`, "apply cache
  instructions whether or not watched"), closing the gap the
  first-round review (citation: `docs/reviews/2026-09-28-md-directory-rework-review.md`)'s
  finding H1 reported.
- A line stays named until reset, but a naming is spent the first time a
  directory walk's backdoor read reaches it while its page is unwatched
  (QEMU `b1fe74bd84`, "spend a line's naming on the page it watches"): a
  repeat walk over a range firmware already flushed no longer watches any
  of its pages. The first walk after a flush still watches one page per 32
  lines it reads, each a memory-map change, since that walk is what spends
  the naming; how often that happens on the stock PROM has not been
  measured.
- Watched pages persist into whatever the PROM loads next, on QEMU's slow
  path with one instruction per translation block. The first version left
  sixteen lines on eleven pages from node address `0x4000_0000` at the
  PROM's `ALL DIAGS PASSED` (run `prom-20260927T192359-132243`); the
  rework's placement there has not been read.
- Copies are never removed from the table, even when every processor's
  copy of a line is invalid; the table grows with the lines ever followed
  until reset.
- More than sixteen holders of one line are not followed and are logged as
  unimplemented.
- The unit's soft reset discards the kept copies with everything else;
  whether hardware's processors lose their cache contents then is unknown.
- Cache algorithms 0, 1 and 6, reserved in the R10000 manual's Table 4-1,
  arrive as no algorithm and are served as a device's access.

##### Evidence

`ndir` passes every section with QEMU `162ba09656` ("watch only lines a
processor has named"), on the stock IP35 PROM 6.210, `tools/origami run
smdk --diag ndir`:

| Machine | Result | Run |
|---|---|---|
| one chassis, 2 R14000 | `Passed  0x000000e3 0x00000001 0x0000  100` | `prom-20260928T013255-a61a66` |
| one chassis, 4 R14000 | `Passed  0x000000fb 0x00000001 0x0000  100` | `prom-20260928T013254-4bdcbb` |
| pair, 4 R14000 (2 + 2) | `Passed  0x000000ad 0x00000001 0x0000  100` | `prom-20260928T013255-1f4d0f` |
| pair, 8 R14000 | `Passed  0x000000c5 0x00000001 0x0000  100` | `prom-20260928T013255-0c4d76` |

The four-processor pair, which failed node 1 in section 1 in two runs of
two with the first version of the change ("NodeSync Error: Timed out
waiting for 2 cpus to sync up", runs `prom-20260927T190736-7cbb68` and
`prom-20260927T192206-b32a54`), passes in the one run recorded here; it
has not been repeated.
Whether any of section 4's transitions are requests from a processor on
the other node is not established. Before the change section 4 failed at
its first transition (`Failed  0x000000e3 0x00000000 0x0001  0`, run
`prom-20260924T215719-e3cd21`).

This table's runs, and the ones above it and in the rest of this section,
predate the directory model's second-round fixes: the cache-instruction
and naming changes just described, ignore-ECC transitions continuing, and
the request region's big-endian read. `d9518955b6` ("mark instruction
fetch as an inferred load") is the model's last change on the landing
revision, `0d4a2c3b81`.

`ndir` also passed with those fixes applied, at QEMU `37464f43f1` (a
pre-rebase build of the same series, sharing `d9518955b6`'s subject as its
own tip, but before the system-request region's exemption from the
re-entrancy guard): 100% at two processors (run
`prom-20260928T034935-386f1a`) and, on the eight-processor pair, 100% on
a retry (run `prom-20260928T035103-3a79f2`) after a first attempt hit
"Barrier sync warning: local=5300" and a General Exception on `bay/02`
(run `prom-20260928T034935-b5af96`) — the same symptom the
[naming bullet](#which-lines-the-model-follows) now attributes to the
guard rather than to naming's cost, and consistent with that: a
guard-refusal race would not repeat every boot. Neither run reflects the
big-endian read fix, landed separately.

The second-round review (citation: `docs/reviews/2026-09-28-o300-integration-reviews.md#5-directory-model-second-round-fixes`)
built its own worktree at `0d4a2c3b81` and ran the qtests: they pass
there, and reverting the whole series back to `3bdc9091bd` fails the three
new assertions at their own checks (the H1 hit half's `STATUS_CH`, the H1
index half's `UNOWNED`, naming and ignore-ECC), while the existing
page-is-watched control still passes. Reverting only `b1fe74bd84` fails
naming alone; reverting only `3c886ccc8e` and its two follow-on comment
commits fails ignore-ECC alone.

The qtest `tests/qtest/sn1-directory-state-test.c` runs 26 cases with
processors executing: loads and stores, three readers, flushes, prefetch,
the premium format, uncached and debug accesses, an absent owner, the
algorithm-4 and algorithm-3 loads, the block transfer engine, the
ignore-ECC bit and its transitions continuing while the bit is set,
least-recently-used reuse, unnamed lines and backdoor writes watching
nothing, a flush of a released line, reset and snapshot load forgetting
watch state, whole-page watching, a naming spent on the page it watches,
copies taken from the word, and a line's state surviving its slot's reuse
and a migration. At `0d4a2c3b81` it passes 26 subtests, and the companion
`sn1-bedrock-md-test` passes 79.

A future guest that reads directory state for its own purposes, rather than
to verify the protocol, would need new evidence and its own contract.

#### Fitted-size boundary diagnosis

Configured-PROM run `prom-20260910T195020-fd8d06` supplied 512 MiB of RAM.
During memory initialization, the PROM's ordinary-directory retention test
wrote and then read device offset `0x0400_0000`: MIPS address
`0x9000_0001_c400_0000`, QEMU carrier `0x101_c400_0000`, and the directory
image of physical address `0x2000_0000`. The load raised Cause 7 at
`0xc00000001fc564bc` because the model classified that address as the
first ordinary-directory carrier beyond its 512 MiB backing.

Comparison run `prom-20260910T201258-aadded` omitted all SPD properties and
reached the same address. Instrumented no-SPD run
`prom-20260910T204026-edbabc` records all successful memory-configuration
writes. After 16 temporary probe writes and a restored zero population,
firmware writes `0x2700_0000_5353_5353` twice. Every low pair byte is `0x53`:
both software-bank enable bits are set and the shared size field is 5. The
helper decodes each active position as `0x0200_0000 << 5`, or 1 GiB. Current
QEMU qtests establish a zero system-reset value for this register.

Static control flow identifies routine `0xc00000001fc660b8` as the producer.
It sets both enables and default size code 5 in every pair byte before the first
final write at `0xc00000001fc6647c`, then may replace those fields with
per-bank probe results before the second write at `0xc00000001fc666a8`. The
identical writes show that this run retained every default field. The probe
input that causes that result is the capacity-probe readback described below.

After the default write, the producer calls the protection-carrier probe at
`0xc00000001fc2d18c` once for each software-bank position. Each call writes
and reads sixteen aligned 64-bit patterns at 64 MiB intervals, then reads back
the power-of-two positions to derive a size code. For a 512 MiB bank, the
decisive unfitted access represents physical `0x2000_0000`: protection device
offset `0x0400_0408`, MIPS address `0x9000_0001_c400_0408`, and machine carrier
`0x101_c400_0408`. If that carrier retains the just-written
`0x585a_585a_585a_585a`, the probe returns code 5 for 1 GiB. A nonmatching read
returns code 4 for 512 MiB. The producer inserts that nonzero return into the
descriptor before its second final write. The producer investigation (citation: `docs/notebooks/origin300-directory-size-mismatch-20260910.md`)
records the complete path, patterns, and branch conditions.

The retained static path derives the directory-test start and end directly from
this register. It performs aligned 64-bit pattern stores and readbacks over
changing address bits. This is ordinary directory storage, not the page
counter, protection carrier, ECC carrier, or Bedrock self-test registers.

This observation leaves the fitted-memory rule unchanged. The directory extent
must follow the memory actually attached to the Bedrock device. The linked
notebook records the call chain, arithmetic, retained runs, and exact producer
addresses.

#### Stock-PROM directory compatibility

Stock IP35 PROM 6.210 continues the directory test beyond fitted memory after
it records one-GiB sizes for every software bank. Public sources do not
establish how Bedrock handles these unfitted accesses. The initial model uses a
74-entry, first-seen, address-keyed store for aligned untranslated carriers and
classifies it as stock-PROM compatibility rather than hardware directory
storage. Its capacity comes from the retained PROM sequence, but admission is
capacity-based rather than an allowlist. An unrelated aligned untranslated
carrier can consume an entry. This order dependence is explicit model policy.

A one-GiB walk has two 25-address phases, but both include ordinary carrier
offset `0x0400_0000`. Their union therefore contains 49 distinct carriers.
Bank 0 contributes 25 untranslated carriers above the fitted 512 MiB boundary,
and bank 1 contributes 49, requiring 74 entries through the second bank. A
32-entry implementation fills its remaining seven slots at bank-1 offsets
`0x0800_0000`, `0x0800_0008`, `0x0800_0010`, `0x0800_0020`,
`0x0800_0040`, `0x0800_0080`, and `0x0800_0100`. Its first rejected write is
at `0x0800_0200`. Across all eight statically reachable bank positions, the
upper bound is `25 + 7 * 49 = 368`. That larger value describes this PROM path.
It is not a Bedrock hardware capacity or a requirement for the current boot
increment.

Compatibility entries use the same format rules as fitted carriers. A premium
ordinary directory word retains all 64 bits, a standard ordinary word retains
the low 32 bits, and a `+8` page-counter word retains all 64 bits under the
existing opaque-counter policy. Fresh entries read zero, system reset clears
them, and migration preserves them. Seventy-four entries cover only the exact
first two bank calls established by retained control flow. Reaching another
untranslated carrier after the store is full requires new evidence or a fix to
the incorrect population that caused firmware to test beyond fitted RAM.

#### The `+8` carrier

The public SN1 header rules out a second Bedrock directory half. Its IP35/SN1
conditional says that a Bedrock entry is one word with no low/high split and
maps both compatibility names to the same address during bring-up
(`v2.5.0` `sn/addrs.h` lines 197 to 211). In contrast, mainline IP27 places an
SN0 low word at one address and a high word 8 bytes later
(`arch/mips/include/asm/sn/addrs.h` lines 187 to 206). The SN0 layout does not
transfer to Bedrock.

The SN1 header names `+8` as `BDCNT_ENTRY`, the page-counter address. In M mode
it keeps the node and 16 KiB page bits, selects one of four interleaves 16 bytes
apart, and adds 8 (`v2.5.0` `sn/addrs.h` lines 213 to 214 and 268 to 273). For
memory address `0x01a0_0000`, the canonical counter address is therefore MIPS
address `0x9000_0001_c034_0008`, QEMU carrier `0x101_c034_0008`. The separate
protection accessor adds `0x408`, and the address classifiers distinguish that
space from counters by bits 10 and 3 (`sn/addrs.h` lines 218 to 223 and 331 to
333). The companion header says that page counters and protection entries have
four interleaves at a `0x10` stride and gives zero as the software
initialization value for both standard and premium counters
(`sn1/hubmd_next.h` lines 268 to 292 and 635 to 648). It does not define
counter fields, valid-bit masks, increment or saturation rules, hardware reset,
or the raw-address aliases implied by the canonical accessor.

The retained baseline is stock-PROM run `prom-20260909T053344-67ac81`. A later
ad hoc run recorded in Origami issue 846, comment 13845, served the `+0` word
and refused 11,649 accesses at `+8 mod 16`, all 8 bytes wide. The rejected range
was `0x340008` through `0x36d7f8`: 11,648 writes in a contiguous two-word sweep
and the read that raised the next data-bus error at
`0x9000_0001_c034_0008`. This proves that the firmware requires these addresses
to answer. It does not prove their hardware value or alias behavior.

The smallest stateful model that preserves both public and observed software
requirements treats `+8` in each fitted 16-byte carrier slot as an opaque
64-bit page-counter word. It accepts only aligned 8-byte accesses, retains all
64 written bits, initializes the word to zero on cold and system reset, and
preserves it across migration. Every page-counter word is independent of the
directory word at `+0` and of adjacent slots. Full-width retention avoids an
invented field mask. Per-address storage avoids an invented alias rule. This is
source-derived emulator policy, not a hardware-accurate page-counter model.
Automatic counting, saturation, threshold interaction, protection entries,
and page migration remain unsupported. Independent evidence review must clear
this policy before implementation. A hardware counter specification or a
controlled matching-revision measurement would be required to replace it with
hardware semantics.

#### The `+0x408` protection carrier

The public `BDPRT_ENTRY_ADDR` transform selects a protection entry by adding
`0x408` to the seven-eighths directory base. It preserves the physical page
bits selected by `BDDIR_PAGE_MASK`, uses physical bits 8:7 for one of four
16-byte interleaves, and uses region bits in the remaining selector field. A
protection entry is therefore distinct from both the directory word at `+0`
and the page counter at `+8`. The public access helpers treat it as a 64-bit
word; their complete update and alias behavior is not adopted here.

The stock PROM supplies a narrower software requirement. During memory-bank
configuration, the caller invokes the protection probe for all eight 1 GiB
software-bank positions in region zero. Within each position, the probe samples
sixteen physical addresses from 960 MiB down to the bank base in 64 MiB steps.
It therefore accesses 128 protection words.

For software bank `b` from 0 through 7 and descending sample `j` from 0 through
15, the exact addresses are:

```text
physical address = (b << 30) + 0x3c000000 - j * 0x04000000
device offset = 0x07800408 + b * 0x08000000 - j * 0x00800000
QEMU machine carrier = 0x101c0000000 + device offset
MIPS XKPHYS address = 0x90000001c0000000 + device offset
```

The device offsets cover `0x0000_0408` through `0x3f80_0408` in a
`0x4000_0000` protection aperture. Execution descends within one software bank,
then starts at the high sample of the next bank. The per-bank bounds are:

| Software bank | Low device offset | High device offset | Low machine carrier | High machine carrier |
| ---: | ---: | ---: | ---: | ---: |
| 0 | `0x0000_0408` | `0x0780_0408` | `0x101_c000_0408` | `0x101_c780_0408` |
| 1 | `0x0800_0408` | `0x0f80_0408` | `0x101_c800_0408` | `0x101_cf80_0408` |
| 2 | `0x1000_0408` | `0x1780_0408` | `0x101_d000_0408` | `0x101_d780_0408` |
| 3 | `0x1800_0408` | `0x1f80_0408` | `0x101_d800_0408` | `0x101_df80_0408` |
| 4 | `0x2000_0408` | `0x2780_0408` | `0x101_e000_0408` | `0x101_e780_0408` |
| 5 | `0x2800_0408` | `0x2f80_0408` | `0x101_e800_0408` | `0x101_ef80_0408` |
| 6 | `0x3000_0408` | `0x3780_0408` | `0x101_f000_0408` | `0x101_f780_0408` |
| 7 | `0x3800_0408` | `0x3f80_0408` | `0x101_f800_0408` | `0x101_ff80_0408` |

Each bank starts with pattern `0x599a_599a_599a_599a` and repeats the same
sixteen-pattern sequence. The first pass compares the alternating 16-bit fields
selected by `0x0000_ffff_0000_ffff`. A later verification first compares the
complete 64-bit word, then falls back to that mask. Later code clears the
sampled entries and reads them again.

Run `prom-20260910T164049-de445a` reaches that first store and load after all
sixteen self-test timeouts. Its trace first records nonfatal 4-byte Crossbow
register-write errors at offsets `0x1c` and `0x14`. It then rejects the
protection write and read at directory offset `0x0780_0408` as outside fitted
node memory. The load at PROM address `0xc00000001fc2d338` raises a data-bus
exception with Cause 7. The write is at the preceding instruction.

Configured-PROM run `prom-20260910T185636-db2501` at QEMU
`db8c94ee1c8ece7c401eba7160a8a4eac89c53ee` includes the cleared software-bank
0 storage. It completes that bank and next rejects a store and load at device
offset `0x0f80_0408`, the high sample of software bank 1. The load raises Cause
7 at the same PROM address. This progression establishes that the caller's
static eight-bank loop is active in the configured boot path.

Clipping every seven-eighths carrier to the 512 MiB ordinary-directory extent
cannot satisfy these accesses. The protection transform spans all eight fixed
software-bank positions even when the fitted memory is 512 MiB. Ordinary `+0`
directory storage remains limited to fitted RAM. Protection entries need their
own address classification and `0x4000_0000` aperture.

The protection aperture must decode aligned 64-bit region-zero carriers across
all eight software-bank positions so the capacity probe does not take a bus
exception. A populated bank's protection metadata follows the same missing
high DRAM address bits as that bank's memory. Addresses above the installed
density therefore mirror lower protection entries instead of naming
independent retained words. An absent bank accepts the marker store but its
following load must complete without echoing the marker in the two lanes the
PROM selects for that bank. The exact absent-bank value remains unspecified.
Presence and density use the installed population above, independently of the
temporary descriptor sizes. Apply the bank-select XOR before deciding whether
the locational bank exists. For the selected Origin 300 population, each of
the two installed banks mirrors at 256 MiB and retains separate storage.

Run `prom-20260912T063805-876eed` establishes the failed half of that
distinction with protection access logging. With no fitted protection alias,
bank 0 writes marker `0x599a_599a_599a_599a` at local offset `0x0780_0408`.
The model discards it and the following read returns zero. The PROM then clears
offsets from `0x0780_0408` down through `0x0400_0408`. Each other bank fails its
first marker in the same way, and the final memory configuration is
`0x2700_0000_0000_0000`. Its `qemu.trace` SHA-256 is
`3425fb39b192a41dac91054f68de212d5c4272ddeb27cf3b7526b3b0fb0745ad`.

Run `prom-20260912T061931-6bacb4` establishes the populated result after
missing high address bits are mirrored. Its trace records high protection
offsets sharing the same backing index, exact marker echo, and final memory
configuration `0x2700_0000_4b4b_4b4b`, size code 4 for every enabled
descriptor position. Its `qemu.trace` SHA-256 is
`13332fc233c375cc77a7b7528539d252d63d284ba85f0709343b96ac3df5a53a`.
These are emulator observations of the contract discriminator, not independent
measurements of Bedrock silicon.

The public transform maps one selectable carrier per 16 KiB physical page and
four interleaves at 16-byte spacing. Protection words remain opaque storage at
the mirrored physical position, initialize to zero on cold and system reset,
and survive migration. Other regions remain unsupported and observable until
a consumer requires them.

The smallest discriminating qtest runs `-M origin300 -m 512`. For each installed
bank it writes different patterns at the base protection entry and at the entry
whose inverse address is 256 MiB higher. The two carriers must name the same
storage, so the second value is visible through both, without changing the
other installed bank. Repeat after nonzero bank-select relocation. The same
high marker on an absent bank must not echo in the bank-selected lanes, and
neither access may raise a bus exception. An ordinary directory carrier beyond
an installed bank's capacity must still fail. This separates missing-address-bit mirroring from independent high
storage, a zero-open-bus policy applied to populated memory, shrinking the
protection decode, and expanding ordinary directory storage.

Hardware power-on values, protection bit meanings, access enforcement, the
exact absent-bank read value, aliasing across regions or interleaves, and
nonzero-region behavior remain unknown. A hardware specification or a
matching-revision measurement is required before assigning those semantics.

### ECC backdoor

The public SN1 address header at Linux `v2.5.0` places the backdoor ECC-entry
carrier at five eighths of an M-mode node. `BDECC_ENTRY(pa)` maps one ECC byte
for each physical 8-byte doubleword using:

```text
(pa & NASID_MASK) | 5/8 * 8 GiB | (((pa >> 3) & 0x3fff_ffff) ^ 7)
```

`BDECC_ENTRY_H(pa)` uses the same carrier with `^ 6`, producing an aligned
two-byte address for a 16-byte-aligned physical address. These are Hub-space
addresses. Node zero begins at QEMU machine carrier `0x101_4000_0000` after
adding the Origin 300 machine-space base `0x100_0000_0000`.

A flat 512 MiB isolated fixture requires a 64 MiB ECC-byte extent through
carrier offset `0x03ff_ffff`. The final byte and halfword forms begin at offset
`0x03ff_fff8`. The first forms beyond fitted memory are byte offset
`0x0400_0007` and halfword offset `0x0400_0006`. The production model maps
the transform of its two installed 256 MiB intervals after bank-select
relocation, retaining population holes and separate backing for each bank.

The stock IRIX 6.5.30 IP35 PROM first requires posted aligned 16-bit zero stores
while initializing memory. The five-eighths investigation (citation: `docs/notebooks/origin300-five-eighths-aperture.md`)
records that requirement and separates it from unknown hardware semantics. A
later retained run shows the same PROM writing two 64-bit main-memory words and
then a 16-bit ECC carrier before reading the ECC carrier and either data word.
At physical address zero it writes and compares `a55a`, `5aa5`, `9999`, and
`6666` at ECC offset `0x0000_0006`. Returning zero causes the four observed
third-field failures while both data words compare equal.

The same writer exposes a Bedrock-specific lane permutation as the walking
address changes. Focused run `prom-20260912T073516-216c47` records the
instruction, complete GPR value, target carrier and tested memory address. Its
`qemu.trace` SHA-256 is
`f7322b176e5e0e709e54252996eb2f01cbc24188291c5177e784c7d1c36e8054`.
That run establishes three cases. Run `prom-20260912T082826-d178cb` reaches
the last 16-byte record in the 512 MiB bank and supplies the fourth. Its
`qemu.trace` SHA-256 is
`e716c9c5cc83893e30959be8b2b1b86bfa24d9c0d8cf95c39e177cc8f2c7572b`.
The complete observed table is:

| Memory address modulo `0x40` | ECC carrier offset modulo 8 | Check word position in the source GPR |
|---:|---:|---:|
| `0x00` | `0x06` | bits 15:0 |
| `0x10` | `0x04` | bits 63:48 |
| `0x20` | `0x02` | bits 47:32 |
| `0x30` | `0x00` | bits 31:16 |

For the fourth case, physical address `0x1fff_fff0` maps to ECC carrier offset
`0x03ff_fff8`, whose position within its eight-byte carrier word is zero. The
trace records complete source values `0x0000_0000_a55a_0000`,
`0x0000_0000_5aa5_0000`, `0x0000_0000_9999_0000`, and
`0x0000_0000_6666_0000`. The existing selector takes the architectural low
16 bits and writes zero. The immediate carrier read also returns zero, and the
PROM reports the four check-word mismatches at physical `0x1fff_fff0`. This
establishes the source position as bits 31:16 for stock-PROM compatibility.
It does not establish the physical SysAD signaling or Bedrock wiring.

The PROM issues `sh at,0(v0)` in every case. It expects the positioned check
word to reach the ECC entry even when the architectural low 16 GPR bits are
zero. The public VR10000 and VR12000 instruction contract would store the low
16 bits for ordinary SH. Run `prom-20260912T073315-39f359` applied a global
R14000 rule that selected the GPR slice from the carrier address. It passed the
`0x20` quadrant and failed at `0x10`, proving that rule insufficient. The
required compatibility behavior belongs to the Bedrock ECC access path. It
must not change cached or uncached narrow stores to ordinary memory or other
devices.

QEMU may use an internal target-specific transaction abstraction to preserve
the full source value and enabled lanes until the Bedrock ECC path selects the
check word. The guest-visible result must match the table. Do not key a global
SB or SH rule on the R14000 model, PRId, instruction flags or cache attribute.
The exact R14000 SysAD presentation and Bedrock internal lane wiring remain
unknown, so this is a stock-PROM compatibility choice rather than a hardware
claim.

Run `irix-20260912T040051-9fa4b7` directly traces the first failing access in
the current 512 MiB configuration. Firmware has described software bank 0 as
1 GiB. Its paired-address phase writes main-memory words at physical
`0x2000_0000` and `+8`, half the configured bank size above its base. It then
writes and reads the corresponding size-2 ECC carrier at device offset
`0x0400_0006`, MIPS address `0x9000_0001_4400_0006`, and QEMU machine carrier
`0x101_4400_0006`. Both carrier accesses reject. The read raises a data-bus
exception at `0xc000_0000_1fc5_552c` before either data word is loaded.

Earlier run `prom-20260912T015657-7fd33c` at a different QEMU revision had
advanced into software bank 1 before rejecting offset `0x0800_0006`, the
carrier for physical address `0x4000_0000`, at the same PROM instruction. The
instruction address identifies the shared checker, not a fixed data address.
Both offsets are retained firmware requirements, with `0x0400_0006` first on
the current path.

Accepting that carrier removes only the observed ECC exception. The checker's
next instructions load the data words at physical `0x2000_0000` and `+8`, both
beyond the actual 512 MiB backing. The direct trace already records their
posted stores as reaching no device. This contract does not extend or alias
ordinary RAM. Full progress requires correcting the incorrect
memory-configuration producer or a separately reviewed compatibility policy.

Run `irix-20260912T041409-436bf0` confirms that boundary. After the
`0x0400_0006` ECC access succeeds, the PROM takes a data-bus exception on an
aligned 64-bit load from physical `0x2000_0000` at
`0xc000_0000_1fc5_5530`. This is the first required data-word readback in the
same paired-address data-and-ECC test, following two posted stores; it is not a
capacity probe. The address is `base + size / 2`, where memory configuration
descriptor `0x53` supplies size code 5 and the decoder returns 1 GiB for bank
zero. The source-reader record (citation: `docs/notebooks/origin300-bedrock-mbist-and-protection.md`)
records the exact branch path and evidence hashes. The run adds no basis for
making that address legal on a 512 MiB machine. The memory-configuration
producer must instead describe intervals the fitted RAM actually backs; the
exact corrected descriptor values remain unknown.

Public sources identify the carrier addresses and widths but do not establish
readable contents, power-on state, reset treatment, bank selection, or error
generation. The initial model therefore classifies ECC retention as stock-PROM
compatibility. It decodes the software-bank window, applies the reviewed
bank-select mapping and configured size, and translates fitted ECC bytes into
the same dense locational-bank order as main memory. The low packing transform
is undone with XOR 7 for a byte and XOR 6 for a halfword before the fitted bound
is applied.

Accepted byte and halfword accesses share one byte of storage per doubleword.
A halfword write is visible through its two byte carriers, and writes to both
byte carriers are visible through the overlapping halfword, in the order plain
big-endian byte addressing of the `^ 7` byte carriers gives: the halfword at
carrier `2k` (`k` an even doubleword index, so the carrier is `k ^ 6`) holds
the even doubleword's byte in its low half, at byte carrier `k ^ 7`, and the
odd doubleword's byte in its high half. The Origin 300's memory diagnostic
requires that order, and the hardware order is inferred from it: its injector
(`0x10013a80`) reads the halfword at
`carrier ^ 6`, replaces the lane `(carrier ^ 6) & 1` selects, low for an even
doubleword, and writes it back; with the halves the other way round the model
corrected the neighbouring doubleword and the diagnostic reported "Syndrome
and/or Address Miscompare" with the error register naming `0x506008` for an
injection at `0x506000` (run `prom-20260923T160527-6ae33b`). The earlier model
policy had the halves reversed; the stock PROM only reads back what it wrote
and cannot tell the two apart. Offset `0x0000_0006` uses installed
ECC backing when the selected bank maps there, including bank-select 2 for
the Origin 300 population. This increment admits only the two observed untranslated halfword
offsets `0x0400_0006` and `0x0800_0006`. Every other untranslated ECC carrier
rejects until separately evidenced. All three observed offsets must remain
distinct. Fresh storage reads zero, system reset clears it, and migration
preserves it. Retention does
not implement ECC generation, correction, error injection, or checking of
ordinary memory accesses.

An unaligned halfword, a width other than 1 or 2 bytes, or an access outside a
fitted or specifically admitted carrier is rejected, emits one diagnostic, and
changes no state. Validate width first, alignment second, and admission last
when more than one condition is invalid. Diagnostics use the offset from the
five-eighths carrier, lowercase hexadecimal with eight zero-padded digits, and
the decimal access size. These examples define the exact text and reasons:

```text
sgi-bedrock-md: ECC backdoor write offset 0x00000007 size 2 rejected: unaligned halfword
sgi-bedrock-md: ECC backdoor write offset 0x00000004 size 4 rejected: unsupported width
sgi-bedrock-md: ECC backdoor write offset 0x04000007 size 1 rejected: outside fitted memory
```

This is an explicit functional approximation for the documented carrier and
observed stock-PROM requirements. It does not establish physical ECC storage
semantics or diagnostic success. The independent correction review clears the
added `0x0400_0006` compatibility carrier for a clean implementation.

### Directory ECC: the codes, checking on a read, and the error record

A directory word carries its own check field, and the unit checks it when
the memory line the word describes is read. The public header gives the
record kept (`hubmd.h` lines 1491 to 1584): a valid bit for each of four
kinds, uncorrectable directory at bit 63, access-protection double-bit at
62, correctable directory at 61 and access-protection correctable at 60,
their overrun bits at 59 to 56 in the same order, the protection index and
bad protection value at 54:48 (protection kinds only), a seven-bit syndrome
at 46:40 and the Hub special address of the offending entry at 32:3; the
address and syndrome belong to the highest-priority kind latched,
uncorrectable above protection above correctable. The directory
configuration register's bit 1 ignores directory ECC (line 1478). The
Origin 300's directory diagnostic requires the rest, from which the
hardware behaviour is inferred (`ndir` section 2,
"Directory SECDED Verification"; the
2026-09-23 notebook entry (citation: `docs/notebooks/firmware-diags.md#2026-09-23-the-two-processor-smdk-failures-re-measured-on-boots-that-kept-both-processors`)
records the reading):

- The check field is bits 37:32 and 48 of a premium word (seven bits) and
  bits 21:16 of a standard word (six bits): its injector places the
  syndrome there (`0x100039a8`, `0x10003838`), its check-bit predicate masks
  those positions (`0x10005038`, `0x100050b0`) and its section 4 field
  printer extracts them (`0x10009340`, `0x100093f8`). The flavor is read
  from the configuration register's bit 0 and never written.
- The codes' column tables are the diagnostic's own, one column per word
  bit, at its `0x100187d8` (premium, 64 entries) and `0x100188d8`
  (standard, 32 entries):

Publication classification: the following ordered columns are retained as
factual ECC encoding layouts described by this contract. Their diagnostic
origin and limitations remain those stated here. No new derivation, rights
conclusion or physical-hardware acceptance is asserted.

```text
premium  1f 2f 37 3b 3d 3e 07 0b 0d 0e 13 15 16 19 1a 1c
         7f 4f 57 5b 5d 5e 67 6b 6d 6e 73 75 76 79 7a 7c
         01 02 04 08 10 20 23 25 26 29 2a 2c 31 32 34 38
         40 43 45 46 49 4a 4c 51 52 54 58 61 62 64 68 70
standard 1f 2f 37 3b 3d 3e 07 0b 0d 0e 13 15 16 19 1a 1c
         01 02 04 08 10 20 23 25 26 29 2a 2c 31 32 34 38
```

  Every column is distinct and of odd weight, the check positions' columns
  are the unit vectors, and the columns exclusive-or to zero, so the
  all-ones word is a codeword, which the diagnostic's ones phase relies on.
  A word's syndrome is the exclusive-or of the columns of its set bits; zero
  names a codeword, a column names the bit in error, and any other value is
  uncorrectable.
- One injection: the diagnostic writes zero to the word with ECC in force,
  sets the ignore bit and reads it back to confirm, writes the raw
  non-codeword, clears the ignore bit, reads the memory line once through
  uncached memory space, then reads the word back through the backdoor and
  expects the nearest codeword (`0x10003790` to `0x10003874`,
  `ndirSec2.c` line 623 on a miscompare). It requires the error register,
  captured by a polling process, to hold the injected syndrome at 46:40 and
  the word's Hub special address at 32:3 in place, compared as a 32-bit
  truncation, with the correctable valid bit for a single-bit error and the
  uncorrectable valid bit for a double-bit one, and it clears by reading the
  clear register. For a double-bit error it requires the uncached read to
  take a bus error; its handler then clears the processor interface's
  error-pending bits 20 and 21 and the error-status clear aliases. Its
  polling process (`0x100033c0`) blocks in monitor call `0x18` with
  argument `0x7e` (`0x10003474`), the memory diagnostic's wait for the
  correctable interrupt, and then reads this register and records it when
  bit 61, correctable valid, is set (`0x100035cc`); a second process
  (`0x100044d8`) blocks with argument `0x99` (`0x10004754`) and reads this
  register's bit 63, uncorrectable valid. `0x99` is consistent with
  error-interrupt bit 24 counted the way `0x7e` counts bank 1 bit 61 (128
  plus 24 plus 1), an inference.
- Sections 0, 1 and 3 write patterns with ECC in force and compare the
  read-back with the check field masked out, so a write with ECC in force
  regenerates the field (inferred from that and from section 2 ignoring ECC
  only to write its raw word; no source states it).

**Model.** A backdoor write of the `+0` word with the ignore bit clear
stores the word with its check field regenerated for the selected flavor;
with the ignore bit set it stores the word as written, and a non-codeword
marks its memory line checked. A read of any part of a marked line, unless
the ignore bit is set, computes the word's syndrome: a column corrects that
bit in the stored word, unmarks the line and latches a correctable error
with the syndrome and the word's node-relative Hub special address
(`0x1_c000_0000` plus the backdoor offset); any other nonzero syndrome
latches an uncorrectable error and fails the access, which a load reports
as a bus error, leaving the word as stored. A correctable latch raises
bank 1 bit 61 and an uncorrectable one error-interrupt bit 24 on both
processor interfaces, the memory errors' interrupts ([interrupts](#interrupts)).
The record follows the
header's priority for the two directory kinds; the protection kinds are
not generated. Reading the clear register returns the record and zeroes it;
writes to either register are ignored and logged. A data write to any part
of a marked line, through the memory window, the unit's own fetch-and-op
stores or the self-test's zero fill, rewrites the entry: the model keeps the stored word's bits,
regenerates its check field for the selected flavor and unmarks the line,
so the next read is not checked. That the unit rewrites the entry on a data
write is inferred, not established: it is the model's minimal way of
satisfying two consumers at once, the stock PROM's read of a line it wrote
after leaving its word a non-codeword, which must not fail, and `ndir`'s
uncached read of a marked line, which must be checked; no source states
it, and whether the hardware rewrites the entry, ignores the stale word,
or does something else, and what data bits a rewritten entry holds, are
not established. A backdoor read of the word after a data write on the
hardware would discriminate. The PROM's sequence: its memory test writes
182 directory patterns with the ignore bit set, leaves 46 non-codewords
behind at the lines whose numbers are powers of two and the last line,
clears the ignore bit, then writes the two doublewords at byte addresses
`0x2000` and `0x2008` of line 64 (its bytes `0x2000` to `0x200f`) and
reads the doubleword at byte address `0x2048` of it (run
`prom-20260924T032419-25a30e`, QEMU `3d4667f9df` plus tracing). A model
that checks that read fails it, and the PROM dies in a General Exception
at `0xc00000001fc54cb4` after "Copying PROM code to memory". Untranslated compatibility carriers
regenerate or store verbatim the same way but are never checked, having no
memory line. The marked set is bounded (1024 lines); a write past that
bound is logged and its line is not checked. The stock PROM's walk holds
46 marked lines at once on the two-bank Origin 300, the same 46 it leaves
behind (run `prom-20260924T030704-ffce66`, a peak slot index of 45 under
first-fit allocation); the earlier bound of 32 dropped the rest silently.
The bound of 1024 is a round power of two more than twenty times that
peak; the peak was measured on one population, two banks and 512 MB, and
is unmeasured for larger ones, which the bound may not cover. Cold and soft reset unmark every line and clear the
record. A read of a memory line whose word was never written through the
backdoor is not checked: the word is whatever the storage holds and the
model regenerates no check field for it, so the zero words a fresh
directory holds are codewords in both flavors. The stock PROM writes its
directory patterns with the ignore bit set (traced on run
`prom-20260923T153712-f77fc9`) and reads them back, which the model
satisfies either way; the earlier storage policy, which retained raw words
with ECC in force, is replaced. A doubleword that carries both a memory-ECC
mark and a directory mark is checked for its memory ECC only: the 8-byte
memory-ECC overlay sits above the 128-byte directory overlay, and the
decoder consults the ECC mark first. No diagnostic exercises the
combination -- the stock PROM leaves memory and directory marks on
different addresses, and `nmem` and `ndir` inject one kind at a time -- so
which error a hardware read reports first is not established; the model's
choice is the memory ECC. Directory state transitions on cached
accesses, which `ndir`'s section 4 checks, are followed for watched lines
only, as
[directory state on processor requests](#directory-state-on-processor-requests)
records.

### Memory ECC: the code, checking on a read, and the error record

The unit stores one check byte per doubleword of memory and checks it on a
read. Public sources give the record it keeps and nothing of the code. The
memory error register (`hubmd.h` lines 2131 to 2262) holds, in the
big-endian layout, a valid and overrun bit pair for each of three kinds,
read correctable at bits 52 and 53, write uncorrectable at 56 and 57 and
read uncorrectable at 60 and 61; an eight-bit syndrome at bits 47:40; a
30-bit address at bits 32:3; and a differing-address flag at bit 63. The
header's narrative: the valid bit of a kind is set whenever that kind
occurs; the address and syndrome always belong to the earliest error of the
highest priority, uncorrectable read above write above correctable read;
a kind's overrun bit is set when its address could not be recorded because
an error of the same or higher priority was already recorded; and bit 63 is
set when uncorrectable errors reached different cache lines. The clear
register at `0x7e0018` repeats the layout with no stated mechanism.

**The code is the Origin 300 memory diagnostic's own table.** `nmem`'s
section 2 ("Memory SECDED Verification") walks a 72-entry byte table at its
`0x10024eb8`, writing each entry through the ECC backdoor for a doubleword
it has just written, and its encoder at `0x10013b40` is the exclusive-or of
the entries of the data's set bits, so the table is the code's column table:
the syndrome each of the 64 data bits produces alone, then the eight check
bits' unit vectors. In order:

Publication classification: the following ordered columns are retained as
factual ECC encoding layouts described by this contract. Their diagnostic
origin and limitations remain those stated here. No new derivation, rights
conclusion or physical-hardware acceptance is asserted.

```text
13 23 43 83 2f f1 0d 07 d0 70 4f f8 61 62 64 68
1c 2c 4c 8c 15 25 45 85 19 29 49 89 1a 2a 4a 8a
51 52 54 58 91 92 94 98 a1 a2 a4 a8 31 32 34 38
16 26 46 86 1f f2 0b 0e b0 e0 8f f4 c1 c2 c4 c8
01 02 04 08 10 20 40 80
```

Every column is distinct and of odd weight, and no two differ by a column,
so a single-bit error is named by its syndrome and a double-bit error has a
nonzero syndrome that names no bit: a single-error-correcting,
double-error-detecting code of the odd-weight-column kind. Entry 0 belongs
to data bit 0, the least significant: the diagnostic writes a zero
doubleword, forces entry `b`, and expects the read to return `1 << b`
(`0x100068d0` to `0x100068dc`). This is read from the diagnostic's data and
code, not from any source.

**What the diagnostic requires of a read** (`nmem` section 2: its injector
at `0x10007250`, comparison at `0x100072d0`, register check at `0x10007010`
and polling process at `0x10006c50`; the
2026-09-23 notebook entry (citation: `docs/notebooks/firmware-diags.md#2026-09-23-the-two-processor-smdk-failures-re-measured-on-boots-that-kept-both-processors`)
records the reading):

- It writes the doubleword through uncached memory space, forces the check
  byte through the five-eighths backdoor, and reads the doubleword back
  uncached. For a data bit it expects the corrected word; for a check bit,
  the word unchanged.
- Its waiting process (`0x10006c50`) blocks in monitor call `0x18` with
  arguments `0x7e, 1` (`0x10006e60`) before each read of the memory error
  register, then requires a nonzero read-correctable field (bits 54:52),
  captures the register and reads the clear register. What the monitor
  call waits for is not established from the diagnostic; `0x7e` is
  consistent with a one-based wait for bank 1 bit 61 (64 + 61 + 1), an
  inference. Before the section it clears the memory-correctable mask bit,
  bank 1 bit 61, in every processor interface mask (`0x100093f0`, called
  from `0x10006758`).
- It requires the captured register's bits 47:40 to equal the forced entry
  and bits 32:3 to equal the doubleword's physical address in place, low
  three bits dropped.
- After the correcting read, it reads the check byte back through the
  backdoor and requires the code's byte for the corrected word: the
  correction is written back to the array, check byte included.
- Its third subsection forces even-weight syndromes and requires the
  uncached read to take a bus error: it installs a handler for signal 10
  at `0x100079d0`; the handler, at `0x10009070`, clears the processor
  interface's error-interrupt pending bits 20 and 21 (uncached
  uncorrectable) and the error-status clear aliases at `0x438` and
  `0x458`. Its three injection loops (`0x10008810` over the
  even-weight syndromes, `0x10008a00` and `0x10008d30` over pairs of
  columns) each read the word through an uncached pointer (top byte
  `0x96`) and build an error record if the read returns.
- Around a backdoor read of the check byte it may set the memory-block ECC
  configuration's ignore bit and clear it again. The stock PROM sets that
  bit for its memory test and leaves it clear at hand-off, with the
  directory configuration at `0x1` (traced on run
  `prom-20260923T153712-f77fc9`).

**The scrub carrier.** The public SN1 address header at `v2.5.0` also
defines `BDECC_SCRUB(pa)`, at four eighths of the node with the entry
carrier's index transform: `(pa & NASID_MASK) | 4/8 * 8 GiB | (((pa >> 3)
& 0x3fff_ffff) ^ 7)`. For node zero it begins at QEMU machine carrier
`0x101_0000_0000`. The header names it and nothing more. `nmem` uses it in
two ways: before it reads a corrected doubleword back it stores a zero
doubleword at the aligned scrub address of the doubleword (`0x100138f0`,
`sd $zero` at `0x10013944`), and its check-byte read routine (`0x10013950`)
reads the byte through the scrub carrier, `^ 7`, not the entry carrier. With
nothing mapped there the read took a bus error (`kl *` on run
`prom-20260923T161540-78dd1f`: `badVaddr 0x90000001000a0c07` at
`0x10013a04`). The model maps the carrier over the same bytes: a byte or
halfword read answers as the entry carrier does, and an aligned 64-bit
store scrubs the 64-byte block whose eight bytes the carrier word holds,
in doubleword order, checking, correcting, writing back and latching each
marked doubleword as a read would. Narrower or unaligned stores and blocks
outside fitted memory are refused with a diagnostic of the form
`sgi-bedrock-md: ECC scrub write offset 0x00000204 size 8 rejected:
unaligned doubleword`. What the stored value means, whether the chip
scrubs a block or a line, and whether a byte write through this carrier
overrides the byte as the entry carrier does are not established; the
diagnostic stores zero and reads bytes.

**Model.** The controller regenerates the check byte on every write, so a
doubleword's stored byte disagrees with its data only after a backdoor
write says so. The model keeps that set: a backdoor write whose byte
differs from the code's byte for the current data marks the doubleword
checked, and a write that agrees, or any data write, unmarks it. A backdoor
read returns the stored byte for a marked doubleword and the code's byte
otherwise. A read of a marked doubleword, by a processor through any
memory space or by a DMA engine, is checked unless the ignore bit is set:
a zero syndrome returns the data; a syndrome naming a data bit returns the
word with that bit corrected, writes the corrected word and its check byte
back and unmarks the doubleword; a syndrome naming a check bit returns the
data and writes the check byte back; either latches a read-correctable
error with the syndrome and the doubleword's address and raises bank 1 bit
61 on both processor interfaces; any other syndrome latches a
read-uncorrectable error, raises error-interrupt bit 24, and then depends
on the reference's cache attribute. A processor's uncached or
uncached-accelerated read (cache algorithm 2 or 7, which the processor
model marks on the access) fails, and the load reports a bus error. Any
other read, a processor's cached read or a DMA engine's, completes with
the word as stored and leaves it marked.

The distinction rests on two published sources. The NEC VR10000 Series
manual (U10278EJ4V0UM00 revision 4, `library/cache/mips/nec-vr10000-vr12000-um-r4-200103.pdf`,
printed pages 106 and 316) says the processor posts a Bus Error exception
when a double, single or partial-word read response carries the
uncorrectable-error indication, and a Cache Error exception instead when a
block response does. The public SN1 processor-interface header (Linux
history `linux-2.5.0`, `include/asm-ia64/sn/sn1/hubpi_next.h` lines 232 to
267) gives the error-pending register separate bits for an uncached
uncorrectable error (20 and 21), data of bad quality on the processor bus
(25 and 26) and an uncorrectable error during a cached load (27 and 28),
beside bit 24 for the unit's own. That the processor-bus behaviour applies
to the Origin 300's R14000 through the Hub is established for the
processor side and inferred for the Hub's: the same manual (printed page
316) makes a block read that gets an error completion post a Bus Error,
so a Hub that answered a cached read with an error completion rather than
data of bad quality would fault it. Section 4.1 of 108-0240-002 says all
SECDED errors cause interrupts handled by the cache-exception handler,
which fits the data-quality path. The model's cached read is an
approximation: it raises no Cache Error exception and neither bits 25 to
28, so software sees the error only through the record and bit 24. The
uncached read's fault does not raise bits 20 and 21 either
([interrupts](#interrupts)). The Origin 300's memory diagnostic requires the uncached fault (its
third subsection above); no diagnostic in the package reads a marked
doubleword cached once the self-test fill clears the PROM's leftovers.

Before that fill, two diagnostics did. `ndir`'s memory scrub (`0x1000ead0`,
the load at `0x1000eae8`, no bus-error handler) read physical `0x800000`
through cache attribute 5 and aborted at Life 1 on the model's bus error
(run `prom-20260924T054705-90f2d0`; history on run
`prom-20260924T061039-96f270`: data `0x0f0f0f0f0f0f0f0f`, code byte `0x3c`,
carrier byte `0x18`). `scct`'s basic-addressing walker read software
address `0x40800008` (raw `0x10800008`) through a mapped cached page, and
with the cached read completing, the latch and bit 24 made the monitor
abort it ("Aborted: see kernel or hardware error logs", runs
`prom-20260924T172956-8eda31` and `prom-20260924T174122-5bbe8a`). A host
watchpoint on that raw doubleword (run `prom-20260924T173650-77776d`)
recorded no write after the PROM's memory test left it with data
`0x0f0f0f0f0f0f0f0f` and carrier byte `0x81`; syndrome `0xbd` is exactly
`0x81` against the code byte `0x3c`. Both were the stock PROM's leftovers,
which the self-test fill above now clears. The discriminating test: force
an even-weight syndrome and read the doubleword through a cached reference
and an uncached one; the model returns the data for the first and faults
the second, and a model that faults both, or neither, fails.

The error record follows the header's priority and overrun
rules above. Reading the clear register returns the record and zeroes it,
the model's choice as for the other clear registers; writes to either
register are ignored and logged. The marked set is bounded (4096
doublewords); a backdoor write past that bound is logged and its
doubleword is not checked. The bound must clear the stock PROM's memory
test, which treats the check bytes as storage: with the ignore bit set it
writes patterns such as `0xa55a`, `0x8118` and `0xf00f` through the
backdoor beside data they do not encode, reads them back, and holds 260
marked doublewords at once on the two-bank Origin 300, leaving 258 in
place when it clears the ignore bit (run `prom-20260924T030704-ffce66`,
counted with a temporary trace; a peak slot index of 259 under first-fit
allocation). Before the self-test fill, by the time ndir's scrub read physical `0x800000` on a later
boot, 230 remained mismatched (run `prom-20260924T061039-96f270`); that
the rest were rewritten by the boot in between is inferred from the
counts, not traced. The bound of 4096 is a round power of two more than
fifteen times that peak, leaving an eight-bank node room; the peak was
measured on one population, two banks and 512 MB, and is unmeasured for
larger ones, which the bound may not cover. With the earlier bound of 128 the writes
past it were dropped, the read-back returned the code's bytes for the
data (`0x0000` for all ones, `0xaaaa` for `0xaa55...`, `0x5555` for
`0x6666...`), and the PROM reported "MAIN MEMORY ERROR" at the last block
of the first bank and the base of the second, disabled the bank and
swapped banks (runs `prom-20260923T160441-04bc5c` and
`prom-20260924T025457-e95980`). Cold and soft reset unmark every doubleword
and clear the record. Whether the correction is written back on the read path
or scrubbed later, whether an uncorrectable read leaves the array
unchanged, what a DMA read of an uncorrectable doubleword does, the Cache
Error exception and processor-interface bits a cached one raises, the
write-uncorrectable kind, which no consumer
produces, and which processor interfaces an uncorrectable read's
error-pending bit reaches ([interrupts](#interrupts)) are model choices or
unmodelled; a matching-revision
measurement would settle them.

### Memory special space (fetch-and-op)

The processor's uncached attribute 2 selects the memory special space at
unmapped base `0x9400_0000_0000_0000`
([the IP35 address map](ip35-address-map.md#processor-side-address-spaces));
the R10000-family model carries it to machine window 3, carrier
`0x300_0000_0000` plus the 40-bit physical address, and the Origin 300 node
maps that window at node offset zero. The space reaches ordinary node memory
through the unit's fetch-and-op operations rather than plain loads and
stores. The Bedrock header describes only the operation cache: two
fetch-and-op cache status registers hold a valid bit and a 27-bit address
field at bits 32:6 (`hubmd.h` lines 551 to 612), so a cached entry is one
64-byte block. The public SN2 successor header defines the variable and its
encoding for that chip's memory-special space (Linux
`arch/ia64/include/asm/sn/mspec.h`): a variable is 64 bytes, of which the
first 8 are the value and the remaining 56 are unaddressable because the
offset selects the operation; a load at offset 0 returns the value, a load at
8 fetches and increments, at 16 fetches and decrements, at 24 fetches and
clears; a store at 0 stores, at 24 ands, at 32 ors; a store at 56 clears the
cache entry.

The Origin 300's own diagnostics require two points of that encoding, from
which the Bedrock encoding at those points is inferred. `scct` synchronises its processors on a read at offset 8 of the
64-byte block at `0x940000000038c000`, and read the model as a bus error
there before the space was mapped (`kl *` on run `prom-20260923T070230-eddf5d`:
both processors frozen at `0x10002c60`, `ld $s2, 8($s5)`); with the encoding
below it completes all its sections (`Passed`, Life `0xac0`, Pass `0x64`, run
`prom-20260923T153006-3c8a7a`). `nmem`'s section 3 stores and loads patterns
at offset 0 of `0x9400000000506000` (`0x1000af18`, `sd $s0,($s2)` then `ld
$s3,($s2)`, run `prom-20260923T061817-98c125`). The remaining offsets are
inferred from the SN2 header, which the Bedrock header's block size agrees
with; no public Bedrock source states them.

`nmem`'s section 4 ("Memory Fetch and Op Test", entries at `0x1000ae10`,
`0x1000b750`, `0x1000bc10`, `0x1000c760`, `0x1000d2f0`, `0x1000d6e0` and the
word forms from `0x1000b350`) establishes more of the encoding: the
operation is address bits 5:3 (`and` with `~0x38` then `ori`, `0x1000b770`);
a store at the increment or decrement offset ignores its data and
increments or decrements (`0x1000b80c`, `0x1000bcd8`); a load at 24 returns
the value and clears it (`0x1000c938`); a store at 24 ands and at 32 ors
(`0x1000d3e0`, `0x1000d7d0`); and 32-bit loads and stores use address bit 2
to name the high word (0) or the low word (4) of the variable, with the
operation unchanged (`0x1000b46c` to `0x1000b47c`, `0x1000c140` to
`0x1000c154`, `0x1000c300` to `0x1000c314`, `0x1000cdb8`, `0x1000dbac`).
Offsets 40, 48 and 56 are never issued.

The model answers the space over the same bank decode as ordinary memory: the
variable is the doubleword at the base of the 64-byte block, and an
operation reads and writes it in node memory, so the value is visible through
the uncached and cached memory spaces. Reads at offsets 0, 8, 16 and 24
return the value before the operation (load, increment, decrement, clear).
Writes at 0, 24 and 32 store, and and or; writes at 8 and 16 increment and
decrement, ignoring the data. A 32-bit access treats the word bit 2 names
as a 32-bit variable of its own: every operation, increment, decrement and
clear included, acts on that word alone, a load returns that word of the old
value, an increment or decrement wraps inside the word without carrying into
the other, and the other word is unchanged. This is inferred from `nmem`'s
section 4, since no public source states a 32-bit encoding. Its word
increment/decrement test (`0x1000c100`) stores `0x80000000` in both words,
issues `0x10000` 32-bit increments at offset 8 on the high word and as many
32-bit decrements at offset 16 on the low word, and expects `0x80010000` and
`0x7fff0000`. It then reloads `0x80000000` into both words, issues `0x10000`
32-bit fetch-and-decrement loads on the high word and as many
fetch-and-increment loads on the low word, and expects the last old values
`0x7fff0001` and `0x8000ffff`. Its load-clear word test (`0x1000caa0`)
fills both words with ones, issues a 32-bit load-clear at offset 24 on one
word, and expects that word zero and the other still all ones. An earlier
model policy incremented and decremented the whole doubleword and cleared
both words; `nmem` failed it with a "Fetchop register compare error" in its "Fetchop
Load Increment/Decrement Word Test", expected `0x80010000`, actual
`0x80000000` (run `prom-20260924T190524-a65695`). Only an
aligned 64-bit or 32-bit access reaches a variable; a narrower or unaligned
access, and an aligned access at an offset that names no operation (including
the cache-clear store at 56, which no consumer has issued), is refused as a
bus error with a guest-error diagnostic of the form
`sgi-bedrock-md: fetch-and-op read at offset 0x000002020 rejected: no such
operation` (reasons: `no such operation`; `not a 64-bit or 32-bit access` and
`unaligned`, which also name the size), so an unmodelled operation is
observable rather than a silent alias of memory. What a 32-bit and or or does
to the unaddressed word is inferred the same way: the diagnostic's and/or word
test checks only the addressed word, and leaving the other unchanged is
consistent with the word forms above. A block in a bank the memory decoder refuses is refused the
same way, with the decoder's own diagnostic. The model keeps no operation
cache, so the two cache status registers stay zero and the cache-clear store
has nothing to do; cache coherence between the memory-special and cached
views is not modelled, which matches a processor that never caches these
lines, the discipline the SN2 header requires of software. Whether Bedrock
performs the operation in a cache and what an operation on a line held in a
processor cache does remain unknown.

**Where the LEDs and the UART really are.** The Itanium port drives the processor
heartbeat LEDs at Hub special local-register window offset `0xc0` plus 8 bytes per
processor slice (`sn/leds.h` lines 17 to 19; `setup.c` line 371), 64 bytes above the
L1 UART at `+0x80` that OpenBSD and the out-of-tree port use. The UART's registers
appear in no MD header. The MD registers time and guard those windows; which Bedrock
unit decodes them is not stated by any header, and the Origin 3000 book gives local
PIO to the local block (printed page 2-17). Recorded as open in the address-map
datasheet and the board-identification decision packet.

## Behavioral contract

For one chassis:

- Registers explicitly identified as writable retain their defined writable bits.
  Read-only status, captured errors, clear aliases, and command registers follow
  their own access rules. Missing access rules remain unresolved.
- Refresh control reads enable 0 after a cold reset. Memory configuration, refresh
  and directory configuration keep their values across the "soft reset" the header
  names without saying which reset that is; the model equates it with the network
  interface's local-reset bit, its own equation, settled by the same observation the
  local-block datasheet names (values across the PROM's own early local reset).
- The four error-clear registers read 0 on a machine with no errors, and reading one
  leaves its base error register 0. No clear register's narrative states a mechanism
  (each repeats its base register's text); read-to-clear is the model's choice.
- The outgoing reply queue size reads 12 in each of its three header fields and 24 in
  each of its three data fields.
- LED 0 to 3 read the current value of the corresponding LED bank. Tying bank n to
  the Hub special LED window's slice n at `0xc0` plus 8n is the model's choice; no
  source ties a bank to a slice or the window to this unit. Settle by a PROM trace of
  writes to the window and reads of `0x7a0010` to `0x7a0028`, or on hardware a write
  to the window followed by a read of the register.
- Performance counter 0 retains written bits.
- The current-cell register's reset value is unknown, and it is load-bearing: with a
  region's bit clear the local and remote register windows have no access at all
  (lines 337 to 338), so a zero reset would make the L1 UART unreachable through the
  remote window OpenBSD uses. The model returns all ones, its choice, which permits
  every Hub special access every consumer assumes. Settle by a trace of the held
  PROM's writes to `0x780000`, or a read after PROM hand-off. Qtests assert the
  explicit all-ones model policy without claiming a hardware reset measurement.

## Interrupts

Correctable memory errors raise level-1 bit 61 in the processor interfaces; the
Itanium port panics on it and OpenBSD's bit table agrees on the number (`sn1/intr.h`
line 198; OpenBSD `hub.h` line 126). Nothing on a one-node boot sets it; the
model sets it on both interfaces when it latches a read-correctable error,
and software clears it through the interface's pending-modify register, as the
processor-interface datasheet records for the bits hardware sets. The Origin
300's memory diagnostic clears the mask bit in every interface before its
correctable section and blocks in monitor call `0x18` before each read of
the error register; whether that call waits for this interrupt is not
established (the memory ECC section above records the evidence).

A directory error raises the same two bits: a correctable latch bank 1 bit
61, an uncorrectable one error-interrupt bit 24, on both interfaces.
Inferred from one consumer, not established: `ndir`'s processes wait in the
same monitor call as the memory diagnostic and read the directory error
register after waking (the directory ECC section above), and the header's
comment on bit 24 ("must be cleared in MD") names the unit rather than its
memory. Without the correctable interrupt `ndir` sat in section 2 at Life
`0x71` for the rest of a 600-second budget (run `prom-20260924T180036-49c95c`);
with it section 2 passes (run `prom-20260924T181833-f51d7b`, a tree with
the change uncommitted over QEMU `2209cf96b9`, and run
`prom-20260924T190521-17a097` on `0686813d04`, which went on to section 4).
Section 4.1 of 108-0240-002 says all SECDED errors cause interrupts,
without naming the bits.

Uncorrectable memory errors raise error-interrupt pending bit 24, memory
uncorrectable, in the processor interface
([the processor-interface datasheet](bedrock-processor-interface.md#errors)).
Inferred, not measured: the register header names the bit and keeps it out of
the writable view, and the Origin 300 memory diagnostic's SECDED verification
depends on it, as follows. The model raises the bit in both interfaces, an
approximation: whether the unit signals one interface or both is not
established, a memory read carries no requesting-processor identity through
the model, and the processor-interface datasheet's mask rows already treat
node-wide error sources as visible to both interfaces with one processor
enabling them. The per-processor uncached-uncorrectable pair, bits 20 and 21,
is not raised. The bit is raised from the memory error register's
uncorrectable latches, read and write alike, though only the read latch has a
path today: no model access latches a write uncorrectable error yet.

What the diagnostic does, from its disassembly (`nmem`, the Origin 300 sMDK
package): at entry to its uncorrectable phase (`0x100077a8` calling
`0x100094f0`) it clears bits 20, 21 and 24 in every present processor's error
mask through the node-named register window (the AND with `0xffff_ffff_fecf_ffff`
at `0x100095c0`), remembering which processor held bit 24; it installs a
bus-error handler (`0x100079d0`, handler `0x10009070`) that writes bits 20 and
21 back to the error-pending register and clears the two error-status aliases;
at exit (`0x10007e2c` calling `0x10009220`) it restores the three bits on that
one processor. Each injection (`0x10008810`, later `0x10008a00` and
`0x10008d30`) writes the doubleword, sets its check byte through the ECC
carrier, reads the word uncached and takes the bus error, then waits for a flag
that its watcher process sets after monitor call `0x18` with argument `0x99`
returns and the memory error register has been read (`0x10007fd0`, loop
`0x1000834c`).

What the monitor does with that call, from its disassembly (`smdk`, not
relocated, so file offset is the runtime address's low 20 bits): it runs its
own code, its idle loop included, with the interrupt-enable bit clear (Status
`0xb40000e0`), and creates a diagnostic process with interrupts enabled and
every interrupt-mask bit set (Status `0xb400ffe3` at `0xc000000000110b4c`,
the exception level bit there only for the return from exception; processor 1
ends inside `nmem` at Status `0xb400ffe1` in runs
`prom-20260924T152943-3bc426` and `prom-20260924T162845-a5c61f`). It
services the error-pending bits from two places, both through the routine
`0xc000000000101298`: its idle loop (`0xc000000000101664`, the call at
`0xc000000000101670`) and entry 0 of its exception table, the interrupt
handler, which on hardware interrupt 4, the error line in the
[processor-interface datasheet](bedrock-processor-interface.md#interrupts), calls it at
`0xc000000000111060`. The routine reads the error-pending register through
its writable view at `0x3f8`, ANDs it with the processor's own error mask
(`0x408` or `0x410` by slot), writes the result to `0x400` to clear those
bits, and on bit 24 calls `0xc00000000010d904`, which reads the memory and
directory error registers through their clear aliases (`0x7e0018`,
`0x7c0018`, `0x7c0028`). That this then wakes the process blocked in call
`0x18` is inferred from the sweep completing on the model, not traced. The
mask bit is therefore both the idle loop's polling interest and the enable
of the error line. The monitor's primitive that sets a bit in one
processor's mask, chosen by slot, is `0xc00000000010c984`; of its three
callers, `0xc00000000010cab0` passes bit 24 as a constant,
`0xc00000000010cea4` one of two constant masks chosen by slot
(`0x2aa555555` or `0x554aaaaaa`, neither with bit 24) and
`0xc0000000001101b0` a single shifted bit, and the path from call `0x18` with argument `0x99` to any of them is not traced.
Live on run `a5c61f` in mid-sweep, interface 0 mask A read `0x5558aaaaa` (bit
24 set) and interface 1 mask A `0x5548aaaaa` (clear), the same through the local
and the node-named windows. So the diagnostic needs the pending bit, readable
through `0x400` and `0x3f8` alike and cleared by writing it to `0x400`, and
the error line following pending bits under the mask, as the
processor-interface datasheet records and the model does. Which of the two
paths serviced each injected error on the model is not established: QEMU's
MIPS target does not log an external interrupt under `-d int`, so the record
of run `a5c61f`, with 5238 data bus errors and 10667 system calls across the
sweep, cannot show one. A hardware breakpoint on the handler's call at
`0xc000000000111060` during one injection would show whether the line path
runs.

Before the model raised the bit, the same diagnostic failed with the row
`nmem Failed Min: 0x00000243 0x00000000 0x0001` (runs
`prom-20260924T075835-5f3465` and `prom-20260924T080640-657806`). The
second run's QEMU log holds one refused uncorrectable read and its latched
error; the first ran without that log. At the end of both, processor 0 was
in the injection's flag-wait loop, inside its yield call (EPC `0x1001b38c`,
return address `0x10007ac8`), and processor 1 in the monitor's idle loop.
With the bit, the diagnostic sweeps all 127 uncorrectable syndromes, each an
uncached bus-error read in the model's log, and reaches `Life 0x273` (run
`3bc426`); a later failure there is separate.

## DMA

The unit is the target of DMA and block transfers; it is not a DMA master. Its only
address-side rule for the first model is the bank geometry above.

## Implementation limits

The first register increment does not implement refresh progression,
directory state beyond the watched lines (see
[directory state on processor requests](#directory-state-on-processor-requests)),
page migration, physical self-test algorithms,
queue-credit behavior, or error kinds other than the memory and directory
read errors recorded above. A cached uncorrectable memory read completes
without the processor's Cache Error exception, as the memory ECC section
records.
These remain required wherever Origin 300 diagnostics exercise them. The
functional self-test approximation above qualifies only configured carrier
presence; its only memory effect is the zero-pattern write phase's fill. Register storage does not qualify any omitted operation. The
fetch-and-op operations of the memory special space are modelled without
an operation cache, as recorded above.
Special-space protection requires a separately tested caller using the
current-cell access rule.

On the two-chassis Origin 300 the Hub-register check against current cell
uses the target Bedrock's own initiator region for every access, including
one from a processor on the other node. The hardware checks the requester's
region, which under region size 0 is the requesting node's number (the
two-node addressing design (citation: `docs/designs/2026-09-25-origin300-two-node-addressing.md`),
"The model change"); the model does not carry the requester's region to the
target yet. With current cell all ones, as the model's reset policy leaves
it, the two agree; a guest that clears the peer's bit would still be let in.

## Behavioral notes

**The Origin 200 LED path does not transfer.** OpenBSD's LED write on Origin 200 is
to an SN0 MD register with no counterpart at the same offset here; on IP35 the
public consumers never write an LED through the MD. The audit's LED row is settled by
this and by the Hub special LED window.

**The region-size attribution is a header self-contradiction.** The current-cell
description says region size comes from the network interface's local register
space; the local block header holds it in the revision register. The local-block
datasheet governs.

**Clearing is a consumer expectation, not a statement.** The header says "clear"
without a mechanism; the only consumer reads. Idle zero reads do not prove
read-to-clear. That requires a test which first creates a real error.

**No one-wire or MicroLAN register lives here.** The Bedrock's MicroLAN controller is
in the local block at `0x604030` (`hublb.h` lines 76 to 77, 523 to 549); the Origin
200's Hub kept it in the memory unit.

**The Itanium lock-word use of performance counter 0** contradicts the register's
nature; a model that counts nothing and retains bits satisfies it. The stock IP35
MIPS PROM uses it the same way: during "Copying PROM code to memory" it keeps a
software lock word here, whose bits a processor claims and releases around an
in-RAM per-processor table check, spinning on the word while a peer holds a bit
(the copy-phase stall notebook (citation: `docs/notebooks/origin300-irix-disk-boot-copy-stall-rootcause-20260917.md`)).
Performance counter 0 is plain storage, and the model no longer arbitrates
that spin here: an earlier bounded fairness hold on this register and its
matching hub-lock park at LB scratch 3 were dropped by QEMU
`hw/misc/sgi-bedrock: drop the second-rendezvous holds`,
once a boot campaign traced the copy-phase stall to the first barrier's
presence-publication race rather than to this lock word, which only turned
up orphaned afterwards -- see
the presence-publication join (citation: `docs/known-issues.md#the-presence-publication-join-approximates-same-edge-reset-at-the-first-barrier`).
The removal is not risk-free: the dropped hub-lock park was the only bound
on the PROM's own claim loop for the LB scratch 3 test-and-set this lock
word's read-modify-write brackets itself with
(`func_1fc066f8`/`func_1fc066dc` (citation: `docs/notebooks/origin300-irix-disk-boot-copy-stall-rootcause-20260917.md`)),
and the removal commit describes what it gives up as a park that "only ever
fired inside the PROM's own bounded poll-then-break lock loop." Host
contention severe enough to leave a bit's holder undescheduled past that
PROM-side bound could still let the PROM's own retry break a lock a live
peer holds; the 120-boot removal campaign measured the copy-phase stall this
lock word no longer causes, not that narrower window, and no test targets it.
The register is traced on read and write
(`trace_sgi_bedrock_md_perf0_read`/`_write`, `hw/misc/sgi-bedrock-md.c`) so
that rendezvous can still be followed by processor. A standalone
`sgi-bedrock-md` under a register-contract qtest has no owning node and sees
the same plain read/write contract as every other configuration.

## qtest checklist

- Refresh control bit 63 reads 0 after reset; a write of `0x8000_0000_0000_0c35` reads back with bits 63 and 11:0 set.
- Memory configuration retains a write to its defined bits below 34 (reserved bits 2, 10, 18 and 26 excluded) and reads it back after a local reset through the network interface, the model's reset equation.
- Directory configuration retains bits 1:0 across the same reset.
- Outgoing reply queue size reads `0x0018_1818_0c0c_0c00` (12 in each header field, 24 in each data field) and is unchanged by a write.
- Each of the four error-clear registers reads 0 twice in a row on a fresh machine, and the matching base error register reads 0 afterwards: a replay of the Itanium probe, not a test of read-to-clear, which needs an error path the model does not yet have.
- Current cell reads all ones after cold and soft reset under the explicit model policy. Writes of all ones and zero read back. This tests the chosen model, not the unknown hardware reset value.
- Junk bus timing retains a 48-bit write.
- LED 0 reads the byte last written to the Hub special LED window for slice 0 once that window is modelled (the model's mapping); before that, 0.
- Performance counter 0 retains a 20-bit pattern set and cleared bit by bit.
- On `-M origin300 -m 512`, configure both installed banks and bank-select 2
  so locational bank 2 occupies software bank 0. A 64-bit access at carrier address
  `0x101_c034_0000` reaches the directory word for memory line
  `0x01a0_0000`. A standard-format pattern confined to the low 32 bits reads
  back. A different full-width pattern written at `0x101_c034_0008` reads back
  without changing the directory word, and rewriting the directory word does
  not change the page-counter word. Distinct patterns at `0x101_c034_0008` and
  `0x101_c034_0018` remain independent. System reset restores all three words
  to zero, and migration preserves them. Narrow accesses and accesses not
  aligned to 8 bytes fail at both carrier positions. The last installed directory
  word at `0x101_c1ff_fff0` and its page-counter word at `0x101_c1ff_fff8`
  answer. The first carrier beyond that bank at `0x101_c200_0000` does not.
- In an isolated Bedrock fixture with 1 GiB of backing, aligned 64-bit patterns
  at directory offsets `0x0400_0000` and `0x07ff_fff0` read back independently,
  while `0x0800_0000` fails. The isolated flat 512 MiB fixture keeps
  `0x03ff_fff0` valid and `0x0400_0000` invalid. This proves that the directory
  boundary follows attached memory rather than a fixed 512 MiB limit or the
  whole aperture. It does not enable a one-GiB production Origin 300.
- Populate the 25 untranslated bank-0 carriers and the 49 distinct bank-1
  carriers from the retained one-GiB walking-address sequence with distinct
  values. All 74 read back by exact address. Premium ordinary words retain 64
  bits, standard ordinary words retain 32 bits, and `+8` page-counter words
  retain 64 bits. Insert one unrelated aligned untranslated carrier before the
  PROM sequence and show that it consumes capacity: the 75th distinct key is
  rejected. System reset clears the 74 entries and migration preserves them. A
  build with only 32 entries fails first at directory offset `0x0800_0200`.
- On `-M origin300 -m 512` with bank-select zero, an aligned 64-bit write of
  `0x599a_599a_599a_599a` to the absent software-bank 1 protection carrier
  `0x101_cf80_0408` completes without echoing the marker in the bank-selected
  lanes. The ordinary directory word at `0x101_c400_0000` remains invalid in
  the same test. The complete protection
  check repeats with distinct patterns over all 128 PROM samples and the low
  and high bounds of every software-bank position. Adjacent page and
  interleave slots remain independent. System reset clears them and migration
  preserves them. The first offset past the protection aperture,
  `0x4000_0408`, narrow accesses, and unaligned accesses fail.
- At ECC machine carriers `0x101_4000_0006`, `0x101_4400_0006`, and
  `0x101_4800_0006`, write distinct 16-bit values and read each value back
  without aliasing. A nonzero
  software-bank selector mapped to installed locational bank 2 or 3 must use
  its own backing. The same relative carrier in a disabled, non-admitted bank rejects.
  Write a halfword and read its two constituent bytes, then write both bytes and
  read the overlapping halfword: the low half is the even doubleword's byte at
  carrier 7 and the high half the odd doubleword's at carrier 6. Reject a four-byte
  access, an odd halfword and every untranslated ECC carrier except observed
  offsets `0x0400_0006` and `0x0800_0006`. System reset restores zero and
  migration preserves all three written values. A zero-returning ECC
  implementation fails the `0xa55a` readback at `0x101_4000_0006` when bank-select
  2 places installed bank 2 at software bank 0. A flat
  64 MiB bound fails the first admitted compatibility halfword at
  `0x101_4400_0006`.
- Execute big-endian R14000 `SH` through the Origin 300 CPU path for the four
  observed cases: memory-address residues `0x00`, `0x10`, `0x20`, and `0x30`,
  mapping to ECC carrier offsets 6, 4, 2, and 0. Use complete source GPR values
  `0x1111_2222_3333_4444`, `0x5555_6666_7777_8888`, and
  `0x9999_aaaa_bbbb_cccc`, and `0xdddd_eeee_1357_2468`. Require retained ECC
  values `0x4444`, `0x5555`, `0xaaaa`, and `0x1357` respectively, with
  adjacent entries unchanged. Repeat an
  ordinary-memory SH with source `0x5555_6666_7777_0000` and require zero,
  proving that the compatibility rule is confined to the Bedrock ECC path.
  A low-16-only device path fails the latter three ECC cases. Selecting the
  source slice from the carrier address produces `0x7777` instead of `0x5555`
  at offset 4 and `0xdddd` instead of `0x1357` at offset 0.
- In the isolated fixture with one configured bank, write a value to a
  memory doubleword and read it at fetch-and-op offset 0; then read offsets
  8, 16 and 24 in turn and require each to return the value before its
  operation while the memory doubleword shows the value after it (increment,
  decrement, clear), including a decrement of zero wrapping to all ones;
  write at 8 and 16 with nonzero data and require an increment and a
  decrement; write at 24 and 32 and require the and and or results in
  memory; require the next 64-byte block to be an independent variable and
  the block's other seven doublewords unchanged. With 32-bit accesses,
  store the two words separately and read each back; require a load at 12
  to return the low word and one at 8 the high word of the old value while
  the whole variable increments, a store at 20 to decrement the whole
  variable, a load at 16 to return the high word, an and at 28 and an or at
  32 to touch only their word, and a load at 28 to return the low word and
  clear the variable. Require reads at 32, 36 and 56, writes at 40 and 56,
  a 2-byte read and write, a 1-byte write, an 8-byte read at offset 4 and a
  4-byte read at offset 2 to leave memory unchanged and each to emit the
  exact diagnostic above, and a block in a disabled bank to emit the memory
  decoder's diagnostic. The parent exports the window as its region 9. On
  `-M origin300 -m 512` with bank-select 2 and both installed banks
  configured, physical `0x506000` and `0x38c000` answer through machine
  carrier `0x300_0000_0000`: a store at offset 0 lands in memory, and two
  reads at offset 8 of a zero variable return 0 then 1 and leave 2 in
  memory. A plain alias of memory fails the returned-value checks; an
  unmapped window fails every case.
- Directory ECC, in the isolated fixture with one configured bank and the
  premium flavor: a backdoor write of 1 with ECC in force reads back as
  `0x0000_001f_0000_0001`. With the ignore bit set, write
  `0x0000_001f_0000_0000` and require it to read back as written; clear the
  ignore bit, read the memory line, and require the backdoor to show the
  corrected word, the error register to hold correctable valid, syndrome
  `0x1f` and the word's address `0x1_c000_0000` plus its offset in place,
  a second read of the line to add nothing, and the clear register to
  return the record and then zero. Inject check bit 0 (`1 << 32`) and
  require the read to correct it to zero with syndrome `0x01`. Require a
  read with the ignore bit set to correct and latch nothing and the next
  read with it clear to do both. Inject two data bits and require the read
  to fail, uncorrectable valid with syndrome `0x30`, a second read to set
  the uncorrectable overrun, a correctable error on another line to add
  correctable valid and overrun without moving the address, the word to
  stay as stored, a data write to another doubleword of the line to
  rewrite the entry (the word with a fresh check field through the
  backdoor, the data readable, the next read of the line clean), and a
  rewrite with ECC in force to end the checking as well.
  Writes to the error registers change nothing. In the standard flavor a
  write of 1 reads back as `0x001f_0001` and an injected check bit 0
  (`0x1_0000`) corrects to zero with syndrome `0x01`. Cold reset after an
  injection leaves the line unchecked and the record zero. On
  `-M origin300 -m 512` with bank-select 2, inject at the word for physical
  `0x506000` through machine carrier `0x101_c00a_0c00` over data written
  beforehand, read the line, and require the corrected word, the
  local-window record and its clearing. A model that stores raw words
  fails the regenerated read-back; one that never checks fails the
  correction.
- The rewrite on a data write, in the isolated fixture: mark a line with
  two data bits and force the ECC byte of one of its doublewords, write
  that doubleword through the memory window, and require the word to
  read back with a regenerated check field, the data to land, its ECC
  byte to be the code's, and neither error register to latch; repeat with
  a fetch-and-op store at the line's first doubleword, and with a
  fetch-and-increment, which returns the old value and writes the new. A
  model that leaves the line checked fails the clean read that follows.
- The marked sets' size, in the isolated fixture: force the bytes of 1024
  consecutive doublewords, each with one data bit's syndrome, and require
  every byte to read back as written and the last doubleword's read to
  correct its bit and latch; with the ignore bit set write 256 lines'
  words as a single-bit non-codeword, clear it, and require every word to
  read back as written and the last line's read to correct and latch. A
  model bounded at 128 doublewords or 32 lines drops the later writes and
  fails the read-backs.
- Memory ECC checking, in the isolated fixture with one configured bank:
  write a doubleword and require its backdoor byte to read as the code's
  byte. Force the byte for data bit 5 through the backdoor and require a
  4-byte read of the low word to return the corrected word's low half, the
  error register to read read-correctable valid with syndrome `0xf1`
  (entry 5) and the doubleword's address in place, the next 8-byte read to
  return the corrected word with the register unchanged (no overrun), and
  the backdoor byte to read as the code's byte for the corrected word.
  Force a second doubleword while the first is recorded and require its
  correction to set the overrun bit and leave the address; require the
  clear register to return the record and then zero. Require the ignore
  bit to return the raw word, latch nothing and leave the forced byte, and
  clearing it to correct the next read. Force a check bit (entry 68) and
  require the data unchanged, the syndrome `0x10` and the byte restored.
  Force a byte, then overwrite the doubleword, and require no error, the
  code's byte and, for a one-byte store, the merged doubleword. Force two
  data bits and require the read to fail, read-uncorrectable valid with the
  even syndrome and address, a second read to set the uncorrectable overrun,
  a correctable error on another doubleword afterwards to set correctable
  valid and overrun without moving the address, and a write over the word to
  end the errors. Writes to either error register change nothing. A
  halfword backdoor write forces two doublewords. Under `installed-bank`
  properties, force a doubleword with bank-select 2, change to 0 and
  require the read through the relocated software bank to be corrected and
  the address field to name the relocated address. Cold reset after a
  forcing write leaves the data unchecked and the byte regenerated. The
  parent raises pending bank 1 bit 61 on both processor interfaces and the
  pending-modify register clears it on one. On `-M origin300 -m 512`, force
  physical `0x506000` through machine carrier `0x101_400a_0c07`, require the
  read to return the corrected word, the local-window error register to hold
  the syndrome and `0x506000`, bit 61 through the node-named windows of both
  interfaces, a zero store at the doubleword's scrub address and a byte
  read through the scrub carrier returning the code's byte, and the clear
  register to return the record after the word is rewritten. In the
  isolated fixture, force two doublewords of one block, read the first's
  byte and halfword through the scrub carrier, store a zero doubleword at
  the block's scrub address, and require both memory words corrected,
  both bytes regenerated, and the record to name the first doubleword with
  the second's correction as an overrun; require a 4-byte store, an
  unaligned 8-byte store and a store outside fitted memory to be refused
  with the named diagnostics. A model that never checks fails the first
  correction; one that checks without writing back fails the byte readback
  and the overrun checks; one that raises no interrupt fails the parent
  case; one without the scrub carrier fails its reads.
- Uncorrectable reads by cache attribute: on `-M origin300` with a
  synthesized PROM, force syndrome `0x03` on two doublewords and load one
  through xkphys cache algorithm 5 and the other through cache algorithm 2,
  address attribute 3. The cached load completes with the stored word and
  no exception; the uncached one takes a data bus error and leaves its
  register unchanged; neither word is scrubbed; the record names the first
  with syndrome `0x03`, and error-interrupt bit 24 is pending on both
  interfaces. In the isolated fixture a qtest read, which carries no
  processor cache attribute, completes with the stored word. A model that
  faults both reads fails the cached case; one that faults neither fails the
  uncached case.
- Self-test zero fill: on `-M origin300 -m 512`, mark software bank 3's
  doubleword `0xc0800008` (data `0x0f0f...`, carrier byte `0x81`), store a
  directory non-codeword for its line with directory ECC ignored, and write
  a sentinel in software bank 2. A selector-3 phase with data `0xc5` leaves
  the mark; one with `0x80` reads back carrier byte zero, a zero word, a
  clear memory and directory error record and no bit 24, and the sentinel
  survives. A model whose self-test writes no memory fails the carrier
  check; one that skips the directory entry rewrite fails the directory
  record check.
- Directory error interrupts: in the parent fixture, a correctable
  directory error raises bank 1 bit 61 and an uncorrectable one
  error-interrupt bit 24, each on both interfaces. A model that raises
  neither fails the first check.
