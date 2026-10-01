# Datasheet: the Origin 200's Timekeeper real-time clock

References rendered as citation identities name material outside this document
set. Those targets are not included or cleared for redistribution. Review and
runtime statements retain their recorded dates and scope.

**Status:** `reviewed (ds-hub with a codex second-opinion pass, 2026-08-11) — clears the
register contract and the access sequences for implementing this part, and the decision to
write our own device rather than extend an in-tree one, and the part's name, assigned at this
gate and recorded in the taxonomy. The device is built to this document and the machine boots
with it.`

**Reviewed with fixes (2026-09-24):** the sections "The part's calendar and leap-year rule" and
"The clock across power loss and host restarts", and the two qtest items that go with them. The
record is `docs/reviews/timekeeper-clock-persistence-review.md`. It examined revision `b24ac2605c`
and asked for four fixes, which this revision applies: the direction of the calendar gap, the
three clock rewrites after the February reset, the epoch paragraph's wording, and the source of
the part marking. The leap-year rule is adopted as inferred, strongly. Everything else keeps the
clearance above.

The review is `docs/reviews/timekeeper-gate.md`, verdict passes. It found one factual error,
fixed in place: the comparison against QEMU's `m48t59` gave the small model's clock block as
`0x07F0`–`0x07FF` when the source has only `0x07F8`–`0x07FF`, because that model's own guard
sends every address below `0x7F8` to ordinary storage before the clock switch is reached. The
correction sharpens the recommendation rather than changing it, since the closest size match
upstream also turns out to lack the alarm and watchdog block. **An earlier version of this line
said the review this needed was James's.** It is the delegated gate — an independent non-author
reviewer plus codex — which is how this project's other datasheets were gated; James is
reserved for milestone transitions and cross-cutting contract changes, and a sub-decode inside
the controller's existing window is neither.

A `codex` second-opinion pass ran first and its findings are
reconciled. It confirmed the register addresses, the epoch in all three drivers, the address
swizzle applying to memory-mapped `readb` as well as port I/O, and IRIX's part-identification
probe. It found two things wrong, both now fixed: this document said IRIX writes the
day-of-week register, which its write sequence in fact skips, and it described everything
below the clock block as ordinary storage, which overlooked the bank of eight at
`0x7FF0`–`0x7FF7` that OpenBSD's header names and does not identify. Three of its findings
were declined as artifacts of the file list it was given rather than gaps in this document —
the commit that changed Linux's byte addressing, IRIX's non-volatile layout, and the machine's
wiring are each established by a source outside the list it was handed. **Those three are
recorded verbatim, with the reason each was declined, in `docs/reviews/timekeeper-codex-pass.md`**,
so that the declines can be audited rather than taken on this paragraph's word. Two of its
*unsupported* marks were fair and led to real changes: what the 2018 Linux tree's structure
looks like is now read rather than assumed, and two sentences that asserted physical device
behaviour now say plainly that no source we hold establishes it.

**Fork file(s):** `hw/rtc/m48t35.c`, `include/hw/rtc/m48t35.h`, QOM type `m48t35`, mapped by
`hw/misc/sgi-ioc3.c` at the byte bus's first device select. The name was assigned at this
document's gate and recorded in `docs/component-taxonomy.md`, which had carried the part's
prose name with no identifier against it. It is `m48t35` rather than `mk48t35` because QEMU
spells this family with the ST/SGS-Thomson prefix throughout and names such files by part
number, so the identifier follows the tree it lives in while the prose name keeps the Mostek
origin the part is usually known by.

**Sources used:**

- SGI's own SN0 clock driver: IRIX 6.5.7, `irix/kern/ml/SN/klclock.c` and `klclock.h`. This is
  the designer-side source for this machine and it settles the register addresses.
- IRIX 6.5.7 `irix/kern/sys/SN/nvram.h`, for what IRIX keeps in the non-volatile bytes.
- OpenBSD 7.9, branch `origin/sgi-is-alive-at-7.9`: `sys/arch/sgi/dev/dsrtc.c` and the shared
  register header `sys/dev/ic/mk48txxreg.h`. This is the guest whose boot the model has to
  satisfy first.
- Mainline Linux: `drivers/rtc/rtc-m48t35.c`, `drivers/mfd/ioc3.c`, `arch/mips/include/asm/sn/ioc3.h`,
  `arch/mips/include/asm/mach-ip27/mangle-port.h` and `arch/mips/include/asm/io.h`, all from
  `refs/linux` at v7.2-rc7, which is the tree our Linux guest is built from. The 2018 tree in
  `refs/linux-mips` predates the byte-addressing fix and answers this question differently.
- QEMU's `hw/rtc/m48t59.c`, for judging whether an in-tree model can be reused.
- **The vendor datasheet**: STMicroelectronics, *M48T35, M48T35Y: 5 V, 256 Kbit (32 Kb x 8)
  TIMEKEEPER SRAM*, Doc ID 2611 Rev 10, June 2011, 28 pages. Library path
  `docs/library/cache/io-chip/st-m48t35-timekeeper-sram-datasheet.pdf`, SHA-256
  `ec4ecb512ec0d9a9621337308ea88af9ff714b638c081199e8037f7eb35ce624`, acquired on 2026-09-24
  from the Wayback capture of ST's own URL (see `docs/library/docs.tsv`). It covers both parts
  fitted to our machines. Photographs of the Origin 200 IP29 system board show a part marked
  `M48T35-70PCI` (machine-model audit (citation: `docs/library/machine-model-audit.md`)). Table 16 on page 25
  builds `M48T35-70PC1` from the M48T35, a 70 ns speed grade, the PCDIP28 package and the
  0–70 °C range, so the photographed `PCI` is most likely `PC1`. The Origin 2000 BaseIO
  photographs show an M48T35Y ([BaseIO datasheet](baseio.md)), the SNAPHAT SOIC variant. The
  two differ in supply and write-protect voltages, not in the clock or storage behaviour this
  document uses. This datasheet is the source for the clock's power-loss behaviour and
  calendar rules, which are described below. The register order and access sequences above came
  from the three drivers before we had the datasheet; its Table 5 agrees with them.

## Role

A battery-backed static RAM with a real-time clock in its top eight bytes — 32 KB of
byte-wide cells, of which everything below the last sixteen is ordinary non-volatile
storage, the last eight are the clock, and the eight between them are a bank whose purpose
no source we hold identifies and no driver we run touches. It hangs
off the base I/O controller's byte bus on device select 0, which puts it at controller offset
`0x80000` and gives it a flat 32 KB window there. The controller's own datasheet
(`docs/datasheets/ioc3.md`) owns the contract that says which clock this machine has and where
it sits: select 0, directly addressed, Timekeeper-class, byte-wide — as against the Octane's
DS1687, which needs a separate address port and data port on selects 1 and 2 and is a
different part with a different programming model.

A guest uses it for exactly one thing on our path: reading the wall-clock time once, early,
to set the system clock. OpenBSD attaches it as `dsrtc0` and Linux as an `rtc-m48t35`
platform device.

**The window is byte-wide and that has to be declared rather than inherited.** It is 32 KB
shaped like a plain block of memory, which is what invites mapping it without a thought about
access width, and no guest on our path reads it wide — so a boot cannot catch the mistake.

## Register map

Offsets are from the start of the window, which is controller offset `0x80000`. Every cell is
one byte and every access a guest makes is a byte access.

| Offset | Name | Width | Reset | R/W | Behavior (as guest drivers use it) |
|---|---|---|---|---|---|
| `0x0000`–`0x7FEF` | Non-volatile storage | 8 | battery-backed | RW | Ordinary storage. Holds whatever was last written to it, across a reset and, on real hardware, across power. |
| `0x7FF0`–`0x7FF7` | A bank of eight, purpose unknown | 8 | — | — | OpenBSD's shared header names these `X0` through `X7`, associates them with later MK48Txx models without identifying which ones, and leaves their purposes unresolved. **No driver on any of our paths touches them.** OpenBSD maps them, because it maps the last sixteen bytes as one subregion, and then reads and writes only the eight above. |
| `0x7FF8` | Control | 8 | — | RW | Bit 6 freezes the clock registers for a consistent read; bit 7 arms a write of them. Both are cleared by software when it has finished. The remaining bits are calibration and are not driven by any guest here. |
| `0x7FF9` | Seconds | 8 | — | RW | BCD, 0–59. |
| `0x7FFA` | Minutes | 8 | — | RW | BCD, 0–59. |
| `0x7FFB` | Hours | 8 | — | RW | BCD, 0–23. |
| `0x7FFC` | Day of week | 8 | — | RW | 1–7, and **zero until something sets it** — mainline Linux's driver records that the part leaves this register alone until it has been set to a non-zero value, so zero is what an unset part reports rather than a missing answer. OpenBSD writes it and does not read it; IRIX's write sequence skips it entirely; Linux reads it and discards it. Nothing on any path depends on its value, which is why it is storage here: the register is writable, and a model deriving it from the date would be dropping a write it advertises as accepted. Real hardware advances it daily and this model does not, which no driver we run can observe. |
| `0x7FFD` | Day of month | 8 | — | RW | BCD, 1–31. |
| `0x7FFE` | Month | 8 | — | RW | BCD, 1–12. |
| `0x7FFF` | Year | 8 | — | RW | BCD, 0–99, **counted from 1970 on this machine** — see the epoch discussion below. |

Reset values are left blank deliberately: a battery-backed part has no reset value, it has
whatever it was last set to. What our model answers on a fresh machine is a modelling choice
and is stated in the behavioral contract rather than smuggled into this table.

### The register order is settled, and all three drivers agree on the wire

**SGI's own driver puts the control register at `0x7FF8` and the six time fields immediately
above it**, in the order seconds, minutes, hours, day of week, day of month, month, year
(IRIX 6.5.7, `irix/kern/ml/SN/klclock.h`, the `RTC_SGS_*_ADDR` definitions). OpenBSD reads
them at exactly those offsets, through the generic Timekeeper layout in its own shared header.
That is the designer's source and this machine's driver agreeing, and it is what the model
must present.

**Mainline Linux declares a different order and issues the same addresses, and the reason is
worth stating because the declaration is what a reader meets first.** Its `rtc-m48t35` driver
declares its register structure twice: the ordinary layout is control, seconds, minutes,
hours, day, date, month, year — everyone else's — and under its IP27 configuration it declares
hours, minutes, seconds, control, year, month, date, day, each group of four reversed, which
puts its `control` field at structure offset `0x7FFB`. **That reversal is pre-compensation for
Linux's own accessor, not a claim about the hardware.** On IP27, `__swizzle_addr_b(port)` is
`port ^ 3` (`arch/mips/include/asm/mach-ip27/mangle-port.h`), and MIPS applies it to plain
memory-mapped `readb` and `writeb` as well as to port I/O — the `__BUILD_MEMORY_SINGLE` macro
in `arch/mips/include/asm/io.h` swizzles the address before dereferencing it. So Linux's
`readb` of a field it declared at `0x7FFB` puts `0x7FF8` on the bus, which is SGI's address and
OpenBSD's.

**The same pre-compensation appears in Linux's IOC3 UART structure**, which is what turns the
reading from plausible into checked: `struct ioc3_uartregs` declares line control, interrupt
identification, interrupt enable, receive buffer, scratch, modem status, line status, modem
control — a standard 16550's eight registers with each group of four reversed. Applying the
same `^ 3` recovers the ordinary 16550 order: receive buffer, interrupt enable, interrupt
identification, line control, modem control, line status, modem status, scratch, which is
the order QEMU's own 16550 model implements (`hw/char/serial.c`), the order the controller's
datasheet records for the two serial windows, and the order our serial region already answers
at — proven by OpenBSD attaching its console on it.

**The commit that made the change says so outright, and it changed both structures in the same
patch.** Mainline commit `10cf8300ecad`, "MIPS: SGI-IP27: fix readb/writeb addressing"
(9 January 2020): the machine's chosen convention is data invariance — `readl` and `writel` are
plain loads and stores, which is what the firmware already does — and that convention obliges
the kernel to mangle addresses for byte and halfword accesses. The halfword mangle was already
there; the byte one was missing. Its author states that adding it "causes different addresses
for accesses to the SuperIO and local bus of the IOC3 chip" and that this "is fixed by changing
byte order in ioc3 and m48t35_rtc structs." The swizzle and both structures move together in
one commit, which is why the wire addresses come out the same before and after it.

**Read the right tree, because the two in our corpus straddle that commit.** `refs/linux` is
current mainline and has the `^ 3` with the reordered structures. `refs/linux-mips` is a 2018
tree at 4.19-rc1, before the fix: identity swizzle, and its `drivers/rtc/rtc-m48t35.c`
declares one structure in the ordinary order with no IP27 variant at all. Quoting
either one alone is correct about that tree and says nothing about the other, and the pair is
what matters — **both combinations put the same plain offsets on the bus.** Our Linux guest is
built from mainline (the image in `assets/linux-ip27` reports 7.2.0-rc7), so it is the `^ 3`
side, but the conclusion does not depend on which.

**So there is no disagreement between the guests about this device, and none about any
byte-wide register in the controller's window.** All of them issue the plain offsets. A model
presenting the control register at `0x7FF8` is right for every one, and the bridge's
programmed-I/O byte swap does not enter into it — an earlier draft of this document said it
did, on the strength of one declared structure read without its accessor, and that was wrong.
What remains true is the consequence: a model built to the *declared* mainline layout would
hand every guest a clock whose fields are transposed, which does not fail — it reads as a
working clock telling a wrong time.

This also makes a prediction worth writing down, because the Linux/IP27 track can check it:
Linux's byte accesses to this window, and to the serial registers, should arrive at our model
at the plain offsets, unchanged. If they arrive XORed with 3, the swizzle is being applied
somewhere we did not account for and this section is wrong.

## Behavioral contract

**Reading, as all three drivers do it.** Write the freeze bit to the control register, read
the six fields, then write zero to the control register. The freeze is what makes the six
reads one consistent instant rather than six samples that might straddle a tick. IRIX's own
clock-read routine, OpenBSD's `ds1742_gettime()` in `dsrtc.c` and Linux's
`m48t35_read_time()` all follow this sequence — the two named here are from public drivers
and are how a reader checks the claim; IRIX's is described because its name is internal. What a part that ignored the freeze would
do to them is not established by anything we hold — each reads six bytes and moves on, and
none checks for a tick straddling the read — so the reason to implement the freeze is that
three drivers request it, not a failure any source demonstrates.

**Writing.** Write the write-arm bit to the control register, write the six fields, then
write zero. The part takes the written values as the new time when the bit clears.

A model that keeps its own notion of the time and freezes a snapshot on bit 6 satisfies both
sequences. Whether the real part's counters keep running underneath a freeze or stall is not
observable through any of these drivers — they freeze, read six bytes and release — so the
model should keep counting and snapshot, which is the reading that cannot lose time.

**The epoch is 1970 and getting it wrong produces a clock that looks right and is decades
out.** The year cell holds two BCD digits, and on this machine both kernels add 1970 to it:
IRIX subtracts `YRREF`, which its own comment states is 1970, before writing, and OpenBSD adds
`POSIX_BASE_YEAR` for an IP27 (it uses `POSIX_BASE_YEAR - 2` for an IP35, so the epoch is
machine-dependent and this machine's is the plain one). A model that presented the year as the
last two digits of the calendar year would therefore have OpenBSD report the mid-1990s, and
nothing in the boot would look wrong. **Present the year as the calendar year less 1970.**

IRIX's own comment explains the odd choice and is worth not re-deriving: 1970 is not a leap
year, so a two-digit year counted from it runs two years out of step with the part's own
leap-year tracking, and IRIX carries a compensation byte in the non-volatile storage rather
than change the epoch and shift everyone's clock. That is the reason the epoch is what it is.
It also binds the model: a model has to track leap years the way the part does, as described
in "The part's calendar and leap-year rule" below, or IRIX's compensation corrects a gap the
model never opened.

**IRIX identifies the part by writing to the non-volatile storage, so that storage has to be
real.** Before reading the time, IRIX probes which of two possible clocks is fitted: it writes
zero to the byte at window offset `0xB`, writes `0xFF` to the byte at offset `0x6`, writes
`0x80` back to offset `0xB`, and reads offset `0x6` back. Two cells, four accesses: on a Dallas
part `0xB` is its control register and `0x6` its day register, and on a Timekeeper both are
ordinary non-volatile storage, so the `0xFF` comes back unchanged and IRIX concludes
Timekeeper. **A
model whose window answered only the top eight bytes, or answered the low bytes as zero, would
be identified as a Dallas DS1386 and driven through the wrong register set entirely.** No
guest on our current path does this — it is IRIX — but it costs nothing to be right and it is
the kind of thing that is expensive to find later.

**What the non-volatile storage holds.** IRIX keeps its PROM environment there, with a
documented layout (`irix/kern/sys/SN/nvram.h`): a revision byte, the console selection and
path, the system partition, the operating-system loader, file and options, the page colour,
the console baud rate, the debug terminal, the diskless flag, the timezone, and the leap-year
compensation byte, behind a checksum. Neither OpenBSD nor Linux reads any of it, and our
firmware is synthesized rather than read from this part, so **nothing on our path depends on
the contents** — only on the cells existing and holding what is written to them.

**Measured against a real boot, byte for byte, and it agrees.** `tools/nvram-diff-probe`
locates a live ARCS environment variable's offset by setting it to a distinctive marker and
diffing the window before and after — not a search, a causal measurement — and confirmed most
of this layout independently of the source citation above. Full table, encodings, and every
measurement behind it: the NVRAM characterization (citation: `docs/library/nvram-inventory-characterization.md`).
Three things worth stating here because they bound what a model of this part should assume:

- **Not every field is a null-terminated string, and treating one as if it were produces wrong
  answers, not an error.** Several fields are fixed-width raw ASCII with no terminator at all
  — `dbaud` at five characters, several single-character flags — and one, `AutoLoad`, is a
  one-byte *code* (`'N'`/`'Y'`) that the firmware's own `printenv` expands back to `No`/`Yes`;
  the byte is not the value's first letter by coincidence, it names the value. `netaddr` is not
  text in any form: four raw big-endian bytes.
- **The checksum this document already names responds to content, not to time.** Rewriting a
  variable to its own current value — a genuine no-op — changed nothing anywhere in the window,
  including the byte that otherwise moves on every write. That rules out a write counter or
  timestamp and is consistent with a real content checksum. The exact algorithm — an 8-bit
  rotating XOR, seeded at 165 — is now established and independently reproduced against real
  data; see below. One byte in that same test run also said something not previously known: the
  firmware skips the NVRAM write entirely when the new value equals the old one, rather than
  writing an identical value back.
- **`cpufreq` is very likely computed, not stored.** Every `setenv` against it was refused —
  three separate real R10000 clock speeds, not just one implausible marker — and nothing in the
  window changed on any of the attempts. Consistent with a value the firmware reads from
  detected hardware at boot and reports rather than one it keeps.
- **Two of the environment variables, `gConsoleIn` and `gConsoleOut`, are not simple string
  slots.** Setting either rewrites a wide, scattered part of the window — roughly `0x34c0` to
  `0x35bc`. First looked like a device table; it isn't one — see below for the actual
  mechanism.

**The checksum algorithm: solved.** Both firmware releases held in this repository are
self-describing containers whose CPU-board firmware decompresses a second, embedded I/O-board
firmware segment into RAM at boot, using a small public-domain DEFLATE variant — the segment
decompresses cleanly offline, no boot required. That segment holds the working checksum
routine, present and byte-for-byte identical across PROM 6.11 and PROM 6.156 (the 6.5.30
release the bug report that opens this datasheet's history was against). The CPU-board's own
copy of the routine — an unfinished, permanently-zero-returning placeholder — is real, and
dead, in both releases; it was never the one running.

The algorithm: an 8-bit rotating XOR, seed byte 165, walking every byte from offset `0xe`
through offset `0x7fd1`, not including `0x7fd1` itself — the boundary the header itself computes
between the persistent record and the free space beyond it. It XOR-folds each byte into a
running value except at two single-byte positions skipped by name: the checksum's own storage
byte (`0x4bc`) and a byte, `0x46f`, that records why the machine last restarted, a value a
content checksum has good reason to exclude. Every position, skipped or not, also advances the
checksum through a one-bit left rotation, applied after that position's XOR fold. The fourteen
skipped leading bytes are exactly where a different, incompatible real-time-clock chip this
machine doesn't use would keep its own live registers — skipped so the checksum isn't
invalidated by a register this machine's actual clock chip never occupies.

This project's own reproduction of the algorithm against fifteen real (content, checksum-byte)
pairs matches all fifteen — the corrected starting offset (`0xe`, not `0x0`) is what makes the
difference; the earlier, wrong offset still fails on the identical data. A source-reader
separately confirmed the same algorithm against a freshly captured checksum from a live PROM
6.5.30 boot.

### The part's calendar and leap-year rule

The vendor datasheet lists year, month, date, day, hours, minutes and seconds as BCD. The year
cell runs `00`–`99`, and Table 5 on page 13 gives no century digits. The part corrects
automatically for 28-, 29-, 30- and 31-day months, with the leap-year correction described as
"valid until 2100" (section 2, page 8). **Established:** the part keeps its own calendar,
including February's length, from what is in its registers.

**Inferred, strongly: the part treats a year-cell value that is a multiple of four as a leap
year.** The datasheet does not state the rule in those words. Three things support it:

- The part holds only two year digits, and "valid until 2100" is exactly the limit of a
  divisible-by-four rule applied to the last two digits of the calendar year: 2000 is a leap
  year and 2100 is not.
- IRIX's own account of the epoch, quoted above, says a year counted from 1970 "runs two years
  out of step with the part's own leap-year tracking". That offset is what a divisible-by-four
  rule over the cell value produces.
- IRIX's leap-cycle marker, described in the next section, is `2` from March of a year whose cell
  value leaves remainder `2` when divided by four, until the end of February of a year whose cell
  value leaves remainder `0`. It is `1` otherwise. During that interval the part's calendar,
  with its leap day in remainder-`0` years, is one day ahead of the true calendar, whose leap
  day falls in remainder-`2` years. At true 1 March 2024 (cell 54) the part, with no
  29 February that year, already shows 2 March. It falls back into step at its own 29 February
  in cell 56. The marker therefore tracks the gap between the true calendar and a
  divisible-by-four rule over the cell value.

A model cannot use the real calendar instead, whatever the exact rule is. The part has no epoch,
so it cannot know that cell 54 is 2024. Two things would settle the rule. One is ST's application
note AN923, *TIMEKEEPER Rolling Into the 21st Century*, which the datasheet cites for century
rollover and the Library does not hold. The other is a measurement on any part of this family:
set 28 February 23:59:59 in cell years `0x56` and `0x54` and read the date one second later.

**What that means on this machine.** The year cell holds the calendar year less 1970. A cell value
that is a multiple of four is a calendar year of the form 1970 + 4k: 2022, 2026, 2030. None of
those are leap years. The true leap years 2024 and 2028 are cell values 54 and 58, where the part
has a 28-day February. So the part gets February wrong every other year. That is the cost of the
1970 epoch described above, and the reason IRIX keeps the marker. **A model must follow the
part's rule, not the real calendar:** February has 29 days when the year cell's value is a
multiple of four, and 28 otherwise. The other months keep their usual lengths. A model that runs
the real calendar tells IRIX a different story from the part whenever the clock runs past the end
of February without an IRIX clock write in between. IRIX then applies its one-day correction to a
gap the model never opened.

**Observed on the current model, which runs the real calendar.** Run `irix-20260924T184821-8c4941`
(QEMU sgi-origami `cb6a81a0a1`, Origami `b62d7bdd31`, `-M origin2000,baseio-preset=io6g`) set
the clock to 2026-02-28 23:58 UTC with IRIX's `date`. The trace shows IRIX writing marker `2` and
cell values 23:58:00, 28 February, cell year `0x56`. A `system_reset` followed after the clock had
passed midnight. The next boot printed `WARNING: clock lost 2 days` and `CHECK AND RESET THE DATE!`,
and `date -u` read Mon Mar 2 00:03 GMT 2026, one day late. That boot also wrote the clock three
times, each write a day later than the one before:

1. 2 March 00:00:57;
2. 3 March 00:00:59;
3. 4 March 00:01:51, with `0x3883` rewritten to `1`.

The one-day error is in the time IRIX reports. By the end of the boot the part's own cells were
three days past the real date. By the rule above, a real part would have counted 29 February in
cell year 56, and the marker exists for IRIX to turn that back into 1 March. Neither how IRIX
decodes that date nor the real-hardware result has been traced or measured. What is established
is that the model's real-calendar count leads IRIX to report a date one day late and to push the
clock cells further forward with each rewrite.

Other guests see the part's rule directly. OpenBSD and Linux add 1970 to the cell and convert,
without IRIX's marker. On real hardware, if the rule holds, they would read an impossible
29 February in 2026 and 2030, and would miss 29 February in 2028. How either guest reacts to
29 February 2026 is untested. A faithful model reproduces the part's dates. A guest that rejects
them is seeing the part's behaviour, not a model defect.

**Century.** The day-of-week cell at `0x7FFC` also holds a century-enable bit (bit 5) and a
century bit (bit 4). With century-enable set, the century bit toggles when the year cell rolls
from `99` to `00`. With it clear, the bit does not move (Table 5 note, page 13; section 3.5,
page 14; the section 3.5 text names the register `1FFCh`, a leftover from a smaller sibling part,
while Table 5 gives `7FFCh`). Writing the century bit needs the write bit set. Writing
century-enable does not. No guest on our path sets century-enable.
The model keeps `0x7FFC` as storage and does not toggle the century bit. On this machine the
year cell wraps from `99` to `00` at the start of 2070.

**The frequency-test bit and stop bit.** Frequency test is bit 6 of `0x7FFC`, and it resets
automatically on power-down (page 14). The model does not implement it. Stop is bit 7 of
`0x7FF9` (Table 5, page 13). Writing `1` stops the oscillator, writing `0` starts it within one
second, and parts leave the factory with it set so the battery lasts on the shelf (section 3.3,
page 12).

What follows for the model, and how settled each part is:

- **Established:** a stopped oscillator does not count (section 3.3). The clock cells are
  read/write memory beside the counters (section 2, page 8), and the battery powers the clock and
  preserves data below the switchover voltage (section 2.3, page 11).
- **Inferred, strongly:** the bit reads back and survives power loss. Table 5 lists it as a bit of
  the seconds register, and shipping a part stopped only saves the battery if the bit holds with
  no supply. The frequency-test procedure also speaks of D7 of the seconds register being `0`
  with the oscillator running (section 3.4, page 14). The datasheet never says "reads back"
  outright. Linux's `rtc-m48t59` driver masks bit 7 off when it reads seconds, which is a driver
  expectation, not proof.
- **Established, for a write through the write bit:** clearing the write bit transfers all time
  registers `7FF9h`–`7FFFh`, which include the stop bit, to the counters (section 3.2, page 12).
  The Table 5 note names century-enable as the one clock bit the write bit is not needed for.
- **Unknown:** what a direct write of the seconds register does with the write bit
  (`0x7FF8` bit 7) clear, stop bit included. Section 3.3 does not say. The model drops that write
  and logs it as a guest error, so the case stays visible.

The model therefore holds a stopped clock at its instant, returns bit 7 set in every seconds read
while stopped, keeps the bit in the saved seconds cell, and restores a stopped image with no time
added and still stopped. Like the rest of the seconds register, the bit is taken only through a
write staged with the write bit and applied when that bit clears, as section 3.2 describes. A
completed write that never touched the seconds register leaves the bit as it was. No guest on our
path writes the bit: IRIX's clock write stores seconds with bit 7 clear, which starts a stopped
clock.

### The clock across power loss and host restarts

**Established by the vendor datasheet.** The clock cells are not the counters. They are
dual-ported memory that the clock circuit refreshes once a second (section 2, page 8). When the
supply drops below the switchover voltage, the internal battery "preserves data and powers the
clock", for at least seven years accumulated (section 2.3, page 11). So the time keeps advancing
while the machine is off. When power returns, the cells show the current count, because the
counters never stopped. Only the stop bit halts them. Setting the read bit halts updates to the
cells, not the counters. Clearing the write bit loads the cells into the counters (sections 3.1
and 3.2, page 12).

**Not stated by the datasheet:** the read and write bits' values after power returns. The model
clears both on reset, so a guest that reset part-way through a frozen read or a staged write does
not inherit a frozen clock. That is a model policy. It is also what a part would show if the
bits do not survive power loss, but the datasheet does not say either way.

**Contract for a model with a backing image.** Storage bytes already persist through the
`drive` property, as the paragraph on persisting across a QEMU process, later in this contract,
states. The clock has to persist with them, or a restored image pairs IRIX's leap-cycle marker
with a clock at a different date. Runs `irix-20260924T181131-e50685` and
`irix-20260924T181841-885873` (QEMU `cb6a81a0a1`, io6g, one backing image) showed it: `0x3883 = 2`,
written for January 2030, came back after a full QEMU restart against a clock reset to September
2026, and IRIX printed `clock lost 2 days` and read one day late. The model must:

1. keep the saved calendar in the image's own clock cells, `0x7FF9`–`0x7FFF`, in the part's
   format: BCD, the year less 1970. The day-of-week cell at `0x7FFC` is already storage and
   keeps what the guest wrote. **Model policy:** write these cells whenever a guest clock write
   completes, whenever the machine stops running (a pause, a QMP quit or a terminating
   signal), and when the device is torn down. QEMU does not tear devices down at exit, so the
   stop is what normally saves the clock. A real part's cells follow its counters every
   second, and these points are the emulator's stand-in. The elapsed-time reference that
   item 2 needs lives in a companion drive of at least 16 bytes, named after the storage drive
   with a `-clock` suffix unless configured otherwise; a shorter drive cannot hold it, so the
   elapsed time is lost. An image read on its own then shows
   the time it was saved at, as a real part pulled from a powered-off board would show a time. Rewriting these cells must not touch any storage byte, including
   `0x3883` and the checksum at `0x4BC`.
2. on realize with a backing image, restore the clock to the saved calendar plus the host time
   that passed while the process was not running, as the part's battery-driven counter would.
   The elapsed time needs a reference outside the part: when the image was saved, in the host
   clock QEMU's `-rtc` option selects. **Model policy:** keep that reference outside the
   32 KiB image, for example in a small companion record beside it or in a property the
   launcher supplies. The image stays exactly the part's 32 KiB and nothing the guest can read
   changes. If the reference is missing, for instance for an image made before this contract,
   restore the saved calendar with no elapsed time and log it at `LOG_UNIMP`. Don't fall back
   to host time. A saved calendar with stale elapsed time keeps the marker and clock consistent,
   while host time can break that pairing, as the runs above show.
3. **model policy:** treat an image whose clock cells are all zero, or not valid BCD, as never
   saved. A real part has no such state. Those cells are what every image made before this
   contract holds. Start its clock at host time,
   as the model does today, and leave the storage bytes alone.
4. keep the stop bit out of the restored calendar. If a saved seconds cell has bit 7 set, the
   part's oscillator was stopped. Restore that calendar with no elapsed time and leave the clock
   stopped, so it neither counts while the machine runs nor drops the bit at the next save.
5. restore the calendar with the part's leap-year rule from the previous section. Adding
   elapsed time across the end of February has to count February's length the way the part
   would.

An emulator restart without a backing image keeps today's behaviour: the fixture described in
"Leap-cycle fixture for a new machine" and the host's time. A migration or snapshot load keeps
the rule already stated in that section: restored state wins, and no realize-time restore runs
after it.

### IRIX leap-cycle record

IRIX keeps the leap-cycle state from its last clock write in the ordinary
non-volatile byte at `0x3883`. The byte is binary. Values `1` and `2` are the
only accepted encodings. Zero and every other value mean that the previous
state is unknown and make the kernel warn. IRIX 6.5.30 does not repair the
record while handling that warning.

The value depends on the decoded two-digit clock year `y` and month `m`. It is
`2` from March through December when `y` modulo four is `2`, throughout years
whose remainder is `3`, and in January or February when the remainder is `0`.
It is `1` for every other date. The rule follows from this machine's 1970 epoch
and cannot be replaced by a fixed initial byte. September 2026 encodes year
`56` and requires `1`. April 2024 encodes year `54` and requires `2`.

The record participates in the global NVRAM checksum. When the retained clock
write path changes the leap-cycle state, it first writes `0x3883`, then
recomputes and writes the checksum at `0x4bc`, and only then stages the clock
cells. This is not the path IRIX 6.5.30 takes after an invalid marker read. For
an otherwise-zero 32 KiB image, the existing checksum algorithm produces
`0x6d` with a marker of `1` and `0xad` with a marker of `2`. These values are
fixtures for that synthetic image. A populated image requires a checksum over
its own contents.

The marker and checksum are ordinary byte-wide battery-backed storage. A
machine reset must preserve both. File-backed storage must preserve them when
the emulator process closes and reopens. A clock-cell rewrite at `0x7ff9`
through `0x7fff` may leave both bytes unchanged. A zero marker therefore warns
again on every boot until a path that updates the persistent record runs.

Retained IRIX 6.5.5 feature and maintenance sources and IRIX 6.5.7 sources
establish the accepted values, computation, clock-write order, checksum
coupling, and offsets. The exact IRIX 6.5.30 IP27 kernel object confirms those
accepted values and separates the warning path from the persistent clock-write
path. Run `irix-20260912T122101-1145f1` confirms two invalid-byte reads and two
clock-cell rewrites with no NVRAM write during the traced intervals. The bounded
source review (citation: `docs/reviews/origin2000-timekeeper-cycle-source-review-20260912.md`)
and its independent contract review (citation: `docs/reviews/origin2000-timekeeper-cycle-contract-review-20260912.md`)
record the evidence, release limit, and clean-room exposure.

### Two initial-state checks

The BASEIO PROM and IRIX validate different records in the same Timekeeper
storage. The PROM validates its ARCS environment. Its checksum covers offsets
`0x0e` through `0x7fd0`, while layout revision `4` at `0x0e` is a separate
validity condition. IRIX validates the leap-cycle byte at `0x3883` while
reading the clock. The two checks share the checksum byte at `0x4bc`, but a
valid checksum and leap-cycle byte do not make the environment valid when the
layout revision is still zero.

This distinction explains run `irix-20260912T132840-04054e`. Its first BASEIO
PROM 6.156 pass reports `NVRAM checksum is incorrect: reinitializing.` after
the model's initial leap-cycle fixture has removed the later IRIX date
warnings. That message is expected for a new unbacked machine. The fixture
leaves the environment revision zero so the PROM can install its own defaults.

When either the checksum or revision check fails, the PROM writes its compiled
defaults into the fixed environment fields, stores revision `4`, and updates
the checksum after each field write and again after storing the revision. The
defaults include the console mode and path, boot device and loader, serial
rates, timezone, root device, autoboot policy, SCSI host ID, and several
diagnostic flags. Some values are converted before storage, and some empty
fields remain zero. The complete resulting image is not a universal factory
constant: the PROM also writes machine inventory and other detected values,
and two 128-byte console fields copy adjacent firmware data past the end of
their short default string. Emulation must let the PROM construct this state
rather than install a captured vendor image.

The PROM-created record is valid state. A machine reset must preserve it so
the next BASEIO PROM pass accepts the environment without repeating the
warning. File-backed storage must preserve it across process exit and reopen.
Migration and snapshot restore must replace the destination's complete 32 KiB
image before guest access. Neither reset nor a post-load callback may apply a
fresh fixture over the retained bytes. Starting another unbacked process
creates another new machine image, so one warning in each such process remains
expected.

The byte-level checksum is defined in the preceding section. Independent
self-check vectors for an otherwise-zero 32 KiB image are:

| Nonzero input bytes | Expected checksum at `0x4bc` | What it checks |
|---|---:|---|
| none | `0x2d` | seed, range, and rotation count |
| `0x0e = 4` | `0x0d` | environment revision participates |
| `0x3883 = 1` | `0x6d` | current-cycle fixture participates |
| `0x0e = 4`, `0x3883 = 1` | `0x4d` | combined valid-revision fixture |
| `0x0f = 1` | `0x29` | an ordinary covered byte participates |
| `0x7fd0 = 1` | `0x2f` | last covered byte participates |
| `0x46f = 0x5a` | `0x2d` | restart-reason byte is excluded |
| `0x4bc = 0x5a` | `0x2d` | stored checksum byte is excluded |
| `0x7fd1 = 1` | `0x2d` | exclusive upper bound |

These synthetic vectors test the algorithm only. The PROM-default acceptance
test starts with the date-derived leap-cycle fixture and revision zero, expects
one initialization warning, reaches the maintenance menu, performs an in-band
reset, and then requires no environment-checksum warning. It also compares
the complete Timekeeper image across reset apart from advancing clock cells.
The retained PROM 6.11 three-pass capture establishes reset preservation. The
PROM 6.156 first-boot capture establishes one initialization warning on that
release, but does not test its second pass. The bounded
source review (citation: `docs/reviews/origin2000-baseio-nvram-checksum-source-review-20260912.md`)
records the source identities, release limit, and exposure.

### Leap-cycle fixture for a new machine

The smallest initialization policy is to provision only a newly created
machine image that has no prior NVRAM contents. Start with 32 KiB of zeroes,
compute the marker from the machine's initial clock month and two-digit year,
store it at `0x3883`, compute the checksum over that image, and store the result
at `0x4bc`. For September 2026 the exact fresh-image pair is `1` and `0x6d`.
For a date requiring marker `2`, the exact fresh-image pair is `2` and `0xad`.

Prior contents take precedence over blank-looking bytes. If a caller supplies
an NVRAM backing image, load all 32 KiB exactly as supplied, including an
all-zero image, and do not provision it. This rule avoids treating an existing
image as disposable merely because every byte is zero. If the storage API
cannot distinguish a newly created image from an existing all-zero image, the
creator must write the initial fixture before attaching it. Device reset and
reopening file-backed storage preserve the existing image.

An unbacked device may install the fresh fixture when it realizes, including
when realization precedes an incoming migration or snapshot load. Incoming
migration or snapshot state must replace all 32 KiB before any guest CPU can
observe or access the device. The restored bytes win byte-for-byte. Existing
The timekeeper VMState restores its derived clock state, while the parent
IOC3 post-load hook reconstructs its interrupt output. Neither operation may
alter the restored byte image, and no factory initialization runs after load.

**The 0x34c0 table isn't a table.** `gConsoleIn` and `gConsoleOut` are two adjacent 128-byte
fields, matching the boundaries already measured (`0x34c0`, `0x3540`, `0x35c1`) — that part
stood from the first pass. What scatters the write across the window is simpler than a device
table: the generic fixed-length copy backing every `setenv` writes the full 128 bytes regardless
of the source string's own length, with no regard for that string's own terminator. On the
untouched default, the source is a seven-character compiled-in constant; copying 128 bytes from
it runs past its terminator into whatever the compiler placed next — other short default
strings, then two of the firmware's own printed messages, confirmed byte-for-byte against the
decompressed firmware image. Nothing in the source declares a table here; the earlier
"per-device record keyed by a hardware identifier" reading is retired. What isn't confirmed: the
same mechanism for an actually `setenv`'d value, where the specific "repeated six-byte value at
a consistent sub-offset" this project measured stays unexplained by direct evidence — plausible
by extension (the copy would drag in whatever sits near the command interpreter's own input
buffer instead), not proven.

Cleanroom position: source-exposed across two commissioned passes, from a different hand than
the rest of this document. Full findings, including what remains open, are in
the NVRAM characterization (citation: `docs/library/nvram-inventory-characterization.md`).

**Persisting across a QEMU process is an emulation convenience matching a contract this
document already states, not new hardware behavior.** The non-volatile storage row above
already says "on real hardware, across power" — a real part's content survives the host
machine being off. QEMU's own process boundary is our equivalent of that: unset, a fresh
process starts with zeroed cells exactly as it always has (the factory-first NVRAM-checksum
message this behavior produces on both firmware releases we hold is expected and harmless, see
the NVRAM characterization (citation: `docs/library/nvram-inventory-characterization.md`)); given an
optional `drive` property naming a 32 KB raw file, `hw/rtc/m48t35.c` loads it once on realize
and writes every cell back through as the guest writes it, so content outlives the process the
way it would outlive the real machine being switched off.

## Interrupts

None to model. No driver on any of our paths arms an alarm or registers a handler against
this part, and the periodic interrupt the system runs on comes from the Hub's own real-time
counter, which the Hub datasheets own. **Whether the board physically routes a line from this
part anywhere is not something the sources here establish** — it is a wiring question for the
controller's datasheet and the machine contract, and nothing in the model turns on it while no
guest asks for an interrupt.

## DMA

None. Every access is a byte-wide programmed-I/O access from the processor through the I/O
bridge and the controller's byte bus.

## Reusing QEMU's `m48t59`, or writing our own

The gate has to decide this, and the decision carries the naming with it, so here is the
comparison rather than the question. **The recommendation is our own device**, and the reasons
are specific rather than a preference for new code.

`hw/rtc/m48t59.c` knows three parts: model 2 at `0x800`, model 8 at `0x2000` and model 59 at
`0x2000`. Ours is 32 KB. The clock block's address is not derived from the size — it is written
out as literal cases, `0x1FF0`–`0x1FFF` for the two larger models. The small model has no
counterpart to that block's first eight bytes at all — the flags, alarm and watchdog registers
that live at `0x1FF0`–`0x1FF7` — because its own guard sends every address below `0x7F8` straight
to ordinary storage; its clock cases are `0x07F8`–`0x07FF` only, the control-through-year
registers, paired one-for-one with the larger models' equivalents. So fitting our part still
means adding a fourth model *and* a third set of address cases through the read and write
switches, just eight of them rather than sixteen.

**That is a change to a shared upstream file, and this fork rebases onto master.** A new file
of ours conflicts with nothing on a rebase; an edit threaded through the switch statements of
an in-tree device conflicts with every upstream change to it, forever, for as long as the fork
lives. That is the decisive argument and it is a property of our situation rather than of the
code.

Three smaller things point the same way:

- **The machinery we would inherit is machinery this part does not have.** The eight bytes
  below the clock are, in QEMU's model, an alarm block and a watchdog with an interrupt output
  and a timer. Our part has no interrupt and nothing on our path arms an alarm.
- **The behaviour we need is the one it does not implement.** Its clock reads come from
  `qemu_get_timedate` at the moment of each read, so the freeze bit is stored and does
  nothing; six field reads can straddle a tick. Its control register write is
  `(val & ~0xA0) | 0x90`, which does not keep the write-arm bit our drivers set.
- **It is a sysbus device with its own region**, so embedding it inside the controller's
  byte-bus window is possible but is not what it was shaped for.

**What to take from it rather than fork:** `qemu_get_timedate` with a stored offset is the
right way to answer the time, and its handling of the seconds register's stop bit — recording
when the oscillator was halted and folding the elapsed time back into the offset when it
restarts — is the shape the model copies for that bit.

**One identification worth recording, which is a reading rather than a fact.** QEMU's model
puts the M48T59's alarm and watchdog registers in the eight bytes immediately below the clock
block, and OpenBSD's header describes the eight bytes immediately below *our* clock block as a
bank present "only on recenter models" whose purpose its author did not know. The positions
match, so those are plausibly the same registers, seen from two sides. Nothing on our path
touches them either way, so this changes no decision — it is here so that whoever meets the
mystery bank next does not have to notice the coincidence again.

## Out of scope

- **The battery and oscillator-fail reporting**, and the write-protect behaviour a real part
  applies when its battery is flat. Nothing reads them; a model that never reports a flat
  battery is reporting the truth about an emulated part.
- **The frequency-test and calibration bits** in the control and day-of-week registers. No
  driver on any of our paths writes them.
- **The Dallas DS1687 and DS1386 alternates on byte-bus selects 1 and 2.** Those are other
  machines' clocks, indirectly addressed through an address port and a data port, and they are
  a different device. The Origin 200 wires select 0 only.

Everything out of scope gets a catch-all that logs unimplemented accesses rather than failing
silently.

## Open questions for review

1. **Closed before review, recorded because a draft of this document carried it as open:**
   whether Linux and OpenBSD disagree about this part's register order. They do not. Linux's
   IP27 structure is pre-compensation for its own `readb` address swizzle, both guests put the
   same offsets on the bus, and the bridge's programmed-I/O byte swap is not involved. The
   register-order section carries the evidence and the prediction that would falsify it.
2. **What the model answers on a fresh machine.** The obvious choice is the host's wall clock,
   converted to this part's epoch and BCD, which is what QEMU's other real-time clocks do and
   what makes a guest's date sensible without configuration. The alternative is a fixed time,
   for reproducibility. Whether that matters here is not established and should be checked
   rather than assumed: no line in the OpenBSD boot transcripts we compare against carries a
   date today, and the question is whether one appears once the clock answers — a guest that
   finds the time implausible says so on the console.
3. **Whether to reuse QEMU's `hw/rtc/m48t59.c`.** Analysed above from the file rather than
   from its name, with a recommendation to write our own and a list of what to borrow from it.
   The gate decides, and **the part's QOM type, `-device` name and filename follow from that
   decision** — which is why this document does not name them.

## qtest checklist

- **The window is byte-wide.** Byte writes at consecutive offsets land in distinct cells, and
  the clock block does not alias the storage below it.
- **The control register reads back what was written**, because a driver both sets it and
  reads it — assert both directions, not just the write.
- **A frozen read is consistent.** With the freeze bit set, the six time fields do not change
  between reads; clearing it lets them advance again.
- **The non-volatile storage holds a write**, at both offsets IRIX's part identification
  uses — `0x6` and `0xB` — and at the bottom and top of the storage region, so a model that
  answered only the clock block fails here rather than at an IRIX boot nobody has run yet.
- **The leap-cycle record and checksum retain byte writes.** On an otherwise-zero test image,
  write and read back marker/checksum pairs `1`/`0x6d` and `2`/`0xad` through the guest's CPU
  route. Require both bytes to survive machine reset and, for file-backed storage, closing and
  reopening the machine. Rewriting the clock cells must leave both bytes unchanged.
- **Fresh-image provisioning is lifecycle-gated.** With no supplied backing contents and a
  fixed initial clock, require the matching marker and checksum fixture before the first CPU
  access. An explicitly supplied all-zero 32 KiB image must remain all zero. An existing image
  containing arbitrary marker, checksum, and sentinel bytes must load byte-for-byte unchanged.
  Reset and close/reopen must not provision or rewrite either case.
- **Restored state wins before guest access.** Start a fresh unbacked instance, then migrate
  or load a snapshot containing a deliberately non-factory all-zero image and a second image
  with distinct marker, checksum, and sentinel bytes. Before the first guest CPU read, require
  the complete restored image in each case. Reset after restore must not change it. A later
  post-load callback must not reinstall the factory fixture. Verify the timekeeper's restored
  clock state and the parent IOC3's reconstructed interrupt output separately from the byte image.
- **The epoch.** With a known time set through the write sequence, the year cell holds the
  calendar year less 1970 in BCD. This is the assertion that catches a clock that is decades
  out while looking correct, and no boot on our path can catch it — both guests print a date
  that looks plausible either way.
- **February follows the part's rule.** This item asserts the inferred rule in "The part's
  calendar and leap-year rule". Revisit it if AN923 or a measurement says otherwise. Set
  28 February 23:59:59 with cell year `0x56` and let it tick: the next second reads 29 February. With cell year `0x54` the next second reads
  1 March. Cell years `0x60` and `0x58` repeat the pair.
- **The clock outlives the process when an image backs it.** With a backing image, set a known
  time, tear the device down, and verify the image's `0x7FF9`–`0x7FFF` hold that calendar in
  BCD with the year less 1970, and that no storage byte changed. Realize again with an elapsed
  host time of at least two seconds, under a controllable `-rtc` clock, and require the time read
  back to equal the saved time plus the elapsed time, within one second. Repeat across the end
  of February in cell year `0x56` to check the restore applies the leap rule. Repeat with no
  elapsed-time reference and require the saved calendar exactly. Repeat with all-zero clock
  cells and require the storage bytes unchanged and the clock at host time.
- **A stopped clock stays stopped.** Restore an image whose seconds cell has bit 7 set, run
  several seconds, stop, and require the image's clock cells unchanged with bit 7 still set; a
  restart after more host time must read the same instant, bit set. Stop the clock by a guest
  write with bit 7 set, then require that a write of another field keeps it stopped and a write
  with bit 7 clear starts it. Migrate a stopped clock and require the destination to read it
  stopped.
- **The clock block sits at the top of the window**, at `0x7FF8` through `0x7FFF`, with the
  control register first. Assert the address, not just the behaviour. Three drivers put the
  control register at `0x7FF8` on the bus and one of them declares it at `0x7FFB` in its own
  source, so this is the offset most likely to be transcribed wrong by whoever reads that
  source first.
