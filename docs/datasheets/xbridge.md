# Datasheet: XBridge (delta from the Bridge)

References rendered as citation identities name material outside this document
set. Those targets are not included or cleared for redistribution. Review and
runtime statements retain their recorded dates and scope.

**Status:** reviewed in stages. The initial review (citation: `docs/reviews/xbridge-datasheet-review.md`)
cleared identity, register deltas, crossbar reset, access widths and bounded DMA
and interrupt behavior. Later reviews narrow that clearance: masked force
delivery, multiple-interrupt recurrence, ATE unused-bit readback and parts of
page-DMA fault handling remain unresolved. The
handoff review (citation: `docs/reviews/xbridge-handoff-readiness-review.md`) identifies
these limits. The
initial device-contract decision (citation: `docs/decisions/D-0022-origin300-initial-bedrock-and-xbridge-contracts.md`)
approves the reviewed own-error, raw PCI INTx and page-mapped DMA subsets.
It does not clear the complete device or diagnostic acceptance.
Earlier amendments attributed stock-PROM IO8 identity and a later BASEIO
null-pointer exception to a missing widget-15 one-wire answer. The corrected
stock-PROM call path and reset-to-selector watch do not support the claimed
`0xb0` register access, and the null-pointer run did not isolate its cause.
QEMU's widget-15 DS2505-shaped answer remains an explicitly synthetic
placeholder, not a cleared IP45 board population. The rev-A specification
calls Bridge-half `0xb0` unused in XBridge mode and assigns its one-wire pin
to widget 0; no matching revision-2 IO8 drawing or capture resolves that
route. The emulated empty option slot at widget 14 remains inert.

The swap-table comparison (citation: `docs/reviews/bridge-xbridge-swap-tables-comparison.md`)
found the byte-placement rules identical across the two specifications.
The widget-routing review reconciles fixed destination routing, source-ID
writes and mode-specific one-wire ownership against the original scan.
An existing partial implementation must be transferred explicitly to its next
author. Do not start a competing implementation from this sheet.
**Fork file(s):** `hw/pci-host/sgi-xbridge.c` (the head and the composed chip),
`include/hw/pci-host/sgi-xbridge.h`, `hw/misc/sgi-xbridge-xbow.c`,
`include/hw/misc/sgi-xbridge-xbow.h`, and the tests `tests/qtest/sn1-xbridge-test.c`,
`sn1-xbridge-xbow-test.c` and `sn1-xbridge-chip-test.c`; the fork's
`docs/specs/sgi-xbridge.rst` describes what the models do and defer.
**Evidence cutoff:** 2026-09-12, Origami trunk `b7b0271831`. The earlier
register-contract revision used trunk `b96bfdb68a`.

**Sources used:** the SGI XBridge ASIC Specification rev 0.5 for rev A silicon
(1998), held as `docs/library/cache/asic/sgi-xbridge-asic-spec-rev0.5.pdf` and read
through its character-recognised text; cited by printed page, with the OCR line so a
reviewer can re-check the scan. The SGI Bridge ASIC Specification rev 4.0 where the
baseline value matters. [The Bridge datasheet](bridge.md) and [the Crossbow
datasheet](xbow.md) as the baselines this delta is measured against, and the
[Bedrock I/O-interface datasheet](bedrock-io-interface.md) for the address form the
host presents. OpenBSD at
`2df91d80c1`: `sys/arch/sgi/xbow/xbridgereg.h`, `xbridge.c`, `xbow.h`, `xbow.c`,
`xbowdevs`, `widget.h`, `hub.h`, `sys/arch/sgi/sgi/sginode.c`, `ip27_machdep.c`. Linux
mainline `arch/mips/include/asm/pci/bridge.h`, `arch/mips/pci/pci-xtalk-bridge.c`,
`arch/mips/sgi-ip27/ip27-xtalk.c`. The Linux SN1 port at `v2.5.40`:
`include/asm-ia64/sn/pci/bridge.h`, `sn/xtalk/xbow.h`, `arch/ia64/sn/io/sn1/pcibr.c`,
`arch/ia64/sn/io/xbow.c` (SGI-written GPL code, a register-definition source and an
Itanium consumer); for the translation table also the same header on the Linux 2.4
MIPS tree's `linux-2.4` branch. The out-of-tree MIPS port `refs/linux-ip35`:
`arch/mips/sgi-ip35/ip35-xtalk.c`, `arch/mips/pci/pci-sgipic.c`,
`arch/mips/include/asm/mach-ip35/dma-coherence.h`. The Origin 3000 System
Architecture 108-0240-002 and the Origin 300 User's Guide 007-4365-002 for topology.
The reading is preserved in the SN1 research notebook. The stock-PROM amendment
uses payload SHA-256
`554924380189a03a512156af2faa38344902f251b49fd5c80c253c23ecaff27b`, the
complete annotated disassembly with SHA-256
`dcf0e9a9901b0407828173f50933b24bab8260a48493a4073cac9fd69eb1d4f5`, and run
`prom-20260912T075429-15ae87` as bounded under the new behavioral note.

**Facts derived from `refs/irix-source`: none.** The stock-PROM access added on
2026-09-12 was read from the restricted firmware disassembly. Its reader is
source-exposed for XBridge and Bridge identity, IO-8, L1, Bedrock, the processor
and the Origin 300 machine, and may neither implement nor review those components.
No QEMU source was inspected for the amendment. The Bridge datasheet this delta refers to
carries two protected interface facts (a PROM zero store to Bridge byte address
`0x19c`, restated here only by its public consequence, and the Origin 200 PROM's
sanity provoke at `0x7c`, quoted here with attribution).

## Role

The XBridge is the SN1 crosstalk-to-PCI part: a Crossbow crossbar section fused with
two Bridge sections, one PCI bus each, in a single chip. A mode pin straps it either
as a crossbar with the two bridges on internal ports 0xe and 0xf and six external
crosstalk ports 8 to 0xd (XBridge mode), or as two independent bridges on external
links with the crossbar disabled (Super Bridge mode). The Origin 300's IP45 board
carries one XBridge between the Bedrock and its PCI buses and exposes an XIO
connector, which only the crossbar section can serve. The board runs XBridge mode,
first inferred from that and then observed: three real Origin 300s' inventories
report widget 0 as a crossbar (`ASIC XBOW Rev 2, on CBrick`) with `ASIC BRIDGE Rev
2` at widgets 14 and 15, the IO-8's devices behind widget 15 and the option slots
behind widget 14, and a verbose PROM trace from one prints `HUB at 0x0 attached as
widget 0xa` and `Check_master: link 10 is master` (the Origin 300 captures in
`docs/library/boot-logs.md`). So the emulated machine presents the crossbar section
at widget 0, the PCI bridges at widgets 0xe and 0xf, and the Bedrock at port 0xa.

**A request to the host's own port number is an open question the model answers
with widget 0.** The specification's only word on it is its arbitration note (printed
page 68): "Please note that since a link cannot send packets to itself, (loopback) the
value for link A in the Link (A) Arbitration register has no meaning. As an artifact of
the Crossbow Subsection of XBridge implementation, this location will be used for widget 0
and should be programmed to 0x1F." That reuses an arbitration slot for widget 0's traffic;
it does not say what the chip does with a request whose destination is the port it
arrived on. SGI's sMDK needs an answer
there: its I/O discovery and its IO-8 board test walk Bedrock's ports, and with
nothing at port 0xa the discovery printed "could not find route to dest 0" and
"Global barrier failed" (a trace of sMDK's I/O discovery); that a real machine
answers is inferred from the sMDK being SGI's own diagnostic for it. The stock IP35
PROM 6.210 makes no access there on its way to the menu (a run with the answer
removed and unimplemented-access logging on logged none), and IRIX 6.5.30's
diagnostic row gives the same results either way: every check passes but reboot,
which fails with the answer present too (jamesb/qemu#875). So the model answers a request at port 0xa with widget 0's
registers, an approximation for the sMDK's sake. The Crossbow model, on the other
hand, answers nothing at a host's own port, and no guest there needs otherwise. A
read at the host's own port number on a real Origin 300 would settle what the
XBridge does.

Everything the Bridge datasheet says about a bridge's programming model carries
over except where this document says otherwise. The rule for a clean implementer is:
build the bridge halves from the Bridge datasheet, apply the deltas below, and model
the crossbar half from the Crossbow datasheet with the additions below.

## Register map

### Identity

| Face | Part (bits 27:12) | Manufacturer (bits 11:1) | Source |
|---|---|---|---|
| XBridge, bridge half (widgets 0xe and 0xf in XBridge mode) | `0xd002` | `0x024` | specification printed page 132 (OCR 6772 to 6774); OpenBSD `xbowdevs` vendor SGI3 `0x0024`, product `0xd002`; Linux `bridge.h` lines 396 and 400; SN1 `sn/pci/bridge.h` lines 918 and 922; `ip35-xtalk.c` line 130 |
| XBridge, crossbar half (widget 0 in XBridge mode) | `0xd000` | `0x000` | specification printed page 71 (OCR 3563 to 3565); OpenBSD `xbowdevs` product `0xd000` under vendor 0 ("XXBow"); `ip27-xtalk.c` line 23; SN1 `sn/xtalk/xbow.h` lines 384 and 386 |
| Bridge, for comparison | `0xc002` | `0x036` | Bridge specification OCR 1612 to 1613; `xbowdevs` |
| PIC, for recognition only | bridge heads `0xd102` and `0xd112` under `0x024`; crossbar face `0xd100` under `0x000` | as stated per face | OpenBSD `xbowdevs` lines 34 (vendor SGI, `0x0000`) and 51 to 56 (SGI3); `ip35-xtalk.c` lines 22 and 25 |
| Bedrock, the host at an external port | `0xc110` | `0x036` | OpenBSD `xbowdevs` line 80; Linux `xwidget.h` lines 96 and 109; the Bedrock I/O-interface datasheet owns it |

The identification register layout is unchanged: revision 31:28, part 27:12,
manufacturer 11:1, bit 0 reads one (printed pages 71 and 132). Revision numbering
starts at 1 and the bridge half's marking is alphabetic, 1 = A (printed page 132,
OCR 6769 to 6770). OpenBSD keys its bridge attach on the vendor and product pair, so
a model must present manufacturer `0x024` with `0xd002` or OpenBSD attaches no bridge
(`xbridge.c` lines 586 to 611); its crossbar attach takes the crossbar path for any
widget 0 that is not the `0x036`/`0xc002` Bridge and uses the pair only to print a
name (`xbow.c` lines 293 to 334). Linux mainline and the MIPS port match on the part
number alone and never read the manufacturer (`ip27-xtalk.c` lines 137 to 141 and
218 to 226; `ip35-xtalk.c` lines 88 to 97 and 170 to 179). A Linux SN1 source comment
records reading `0x2d002049` at widget 0 of an "o200/ibrick" under a simulation
conditional, where the crossbar's identity was expected, and substituting `0xd000`:
revision 2, part `0xd002`, manufacturer `0x024`, bit 0 set (`io/xbow.c` lines 316 to
321). Real Origin 300s now supply the direct observation: IRIX's inventory names
widget 0 `XBOW Rev 2` and both bridges `BRIDGE Rev 2`, and OpenBSD on a Fuel, the
sibling board with the same chip, attaches `xbow0: XXBow revision 2` and two
`XBridge revision 2` bridges at widgets 14 and 15, so revision 2 is what this
generation of the chip reports on both halves.

One consumer checks an XBridge revision and does nothing with it: SN1 tests for
revision A and sets a flag that a counted search shows nothing reads (`pcibr.c` lines
3527 to 3528), and names revisions A and B only for printing (lines 3642 to 3643).
OpenBSD's three Bridge revision workarounds (no direct I/O window below revision 4,
the PCI retry-count clear below revision 4, extra masked error bits below revisions 2
and 3) are all guarded by the not-XBridge flag and do not apply (`xbridge.c` lines 636
to 637, 2110 to 2114, 2225 to 2233).

OpenBSD's probe agrees with the inventories: on two real Origin 300s it attaches `xbow0
at mainbus0 nasid 0: XXBow revision 2`, its name for part `0xd000` (`xbowdevs` line 33),
and `XBridge revision 2` at widgets 14 and 15 (the openbsd-sgi list captures in
`docs/library/boot-logs.md`). The same boot reports the bus behind widget 15 as a 33 MHz
PCI bus and the bus behind widget 14 as a 66 MHz PCI bus, the bridge's bus-speed state as
OpenBSD reads it.

### Operating modes

| Item | Value | Source |
|---|---|---|
| mode select | an input pin: 1 = Super Bridge mode, 0 = XBridge mode | printed page 41, OCR 2112 to 2113 |
| XBridge mode | the chip is a Crossbow with two bridges on internal crossbar ports 0xe and 0xf; nine crossbar ports in all: six external (8, 9, A, B, C, D), the two bridge ports and widget 0 | printed pages 17, 20 and 44 (OCR 823 to 840, 1012 to 1014, 2269 to 2273) |
| Super Bridge mode | the internal crossbar is disabled and inaccessible; four crosstalk ports unused; each bridge on its own external link (the plan named ports D and 8); both bridges power up as widgets 0xe and 0xf and can be renumbered through the control register's widget-ID field; the bridge that powered up as 0xf owns the one-wire pin | printed page 45, OCR 2296 to 2310 |
| flash | in XBridge mode the external flash is reached through the bridge on port 0xf | printed page 44, OCR 2278 to 2279 |
| one-wire pin | XBridge mode: driven from widget 0's NIC register and the bridge halves' NIC registers do nothing; Super Bridge mode: the port-F bridge's NIC register | OCR 1270 to 1271, 2277 to 2278, 2291 to 2292, 7309 to 7311 |
| LLP configuration | the bridge half's LLP configuration register has no effect in XBridge mode | OCR 2277 to 2278, 7021 to 7022 |
| a widget's own port number | new: widget 0's status register bits 9:6 carry the port the read arrived from, so a host reads it to learn its port | printed pages 26 to 27 and 75 (OCR 1342 to 1346, 3797 to 3799); SN1 `sn/xtalk/xbow.h` lines 363 and 371 |
| host port choice | no register names a host; dual-host support is per-port warm-reset fences, a lock register and widget-0 timeouts; the host widget number written into bridges comes from the consumer (Linux from the configuration graph's crossbar record, OpenBSD from its hub-master-link field defaulting to 0xa, the MIPS port from the Bedrock's own widget-number register) | printed pages 57 to 58 (OCR 2845 to 2890); `ip27-xtalk.c` lines 175 to 191; `sginode.c` lines 150 to 160 and 338; `ip35-xtalk.c` line 165 |
| crossbar credits for a bridge port | program three buffer locations; the link control credit field resets to 2 | printed page 210 (OCR 10556 to 10559); OCR 3235 to 3236; Linux `bridge.h` line 694 |

### Bridge half: registers that change from the Bridge

Offsets are the eight-byte-aligned slot the Bridge datasheet uses. "X" means the
specification marks the field undefined after reset.

| Offset | Register | XBridge definition | Bridge baseline | Consumers |
|---|---|---|---|---|
| `0x08` | status | 31:24 receive retry count, 23:16 transmit retry count; **15:12 receive credits, 11:8 transmit credits, 7:0 eight general-purpose input pins** (printed page 133, OCR 6799 to 6805) | flash-select bit 6, PCI-not-GIO bit 5, pending-read count 4:0 | SN1 refuses a bridge whose status bit 5 reads 0 as "GIO" (`pcibr.c` lines 3523 to 3524); on an XBridge that bit is an input pin, so a model must keep it set |
| `0x20` | control | 27:24 PCI reset pins, reset `0xf`; 23 I/O swap 0; 22 memory swap 0; 21 page size 0; 16 force bad packet 0; 15:12 crossbar credit 3; 11 and 10 clear retry counters (read 0); 9 system endianness 1 = big; **5:4 PCI speed, read-only from an input**; 3:0 widget ID `0xf` (OCR 6858 to 6912) | flash write enable, three clock enables, SSRAM fields and the max-outstanding field | OpenBSD decodes bits 5:4 as 33, 66, 100 or 133 MHz only under the XBridge flag (`xbridgereg.h` lines 45 to 46; `xbridge.c` lines 2022 to 2028); OpenBSD and Linux clear both swap bits, the MIPS port sets them |
| `0x30` | interrupt destination upper | 19:16 target widget, 15:0 crosstalk address bits 47:32, both X; **no vector field** in the bridge half (OCR 6949 to 6951) | the Bridge datasheet's line for this register claims a vector field at 31:24; the Bridge specification rev 4.0, Table 13 on printed page 23, has bits 31:20 reserved, the same layout as here | closed: the vector field belongs to the generic widget header (Linux `xwidget.h` lines 60 to 64) and to the crossbar's own register, never to a Bridge; a finding for the Bridge datasheet's owner. Every consumer writes only target and address |
| `0x80` | direct map | 23:20 target widget 0; **19:18 reserved**; 17 add 512 MB, **reset 1**; 16:0 offset 0 (OCR 7238 to 7244) | bit 18 selected the 64-bit remote-map field | bit 18 removed; the remote-map field is always zero (OCR 1279, 9270, 9333); OpenBSD, Linux and the MIPS port write widget and offset and so clear bit 17; SN1 sets bit 17 only for a DMA base at or above 512 MB |
| `0x90` | **map fault address** (read/write) | listed in the address map (printed page 124, OCR 6333), which is the specification's only mention; no field table anywhere | SSRAM parity error, read-only | replaced (OCR 1307 to 1308); layout open; SN1 names it (`sn/pci/bridge.h` lines 133 to 134) |
| `0xb0` | NIC | layout unchanged: 19:10 pulse, 9:2 offset, 1 data valid 0, 0 data X (OCR 7316 to 7324) | same | the rev-A specification makes this inert in XBridge mode and puts the pin at widget 0 offset `0x70` (printed pages 44 and 143). The stock-PROM call once attributed to this register is generic board-information discovery; a 32-bit register watch on QEMU saw no access through its identity selection. Linux's probe establishes software expectation, not a rev-2 alias or physical responder |
| `0xd0` | PCI error upper | **27:24 target crosstalk ID; 21 dual-address-cycle flag**; 20 device was master; 19 virtual request; 18:16 device; 15:0 address 47:32; all X (OCR 7419 to 7446) | no fields recorded | two fields added |
| `0x100` | interrupt status | **31 reserved; 30 page-map fault; 16 reserved; 8 reserved**; 29:17 and 15:9 as Bridge; 7:0 pin levels (OCR 7523 to 7613) | 31 multiple error, 30 PMU size fault, 16 SSRAM parity, 8 GIO byte-enable error | OpenBSD marks 31, 16 and 8 Bridge-only (`xbridgereg.h` lines 134 to 137, 164 to 165, 180 to 181) |
| `0x108` | interrupt enable | same map; bits 16 and 8 reserved; reset 0 (OCR 7633 to 7701) | | Linux and the MIPS port write `0x7ffffe00`; OpenBSD its mask without 31, 16, 8 |
| `0x110` | reset interrupt status | bit 6 clears **the error-interrupt view**; bit 1 clears **the page-map fault**; 5, 4, 3, 2, 0 clear the CRP, response-buffer, request-dispatch, LLP and PCI groups (OCR 7716 to 7761) | bit 6 cleared the multiple-error bit, bit 1 SSRAM parity and PMU fault | OpenBSD and Linux write `0x7f` at start of day; SN1 writes bit 6 to clear the view (`pcibr.c` lines 1426 to 1433) |
| `0x170` | **error interrupt view** (read-only) | bits 30:9 mirror the status register's error bits regardless of enable; reset 0; cleared by the same group strobes (OCR 6394, 7886 to 8005) | undefined slot | added (OCR 1262 to 1268); SN1 reads it after a configuration probe |
| `0x178` | **multiple interrupt** (read-only) | same bit map as status including 7:0; a bit sets when its condition recurs before being cleared; reset 0 (OCR 6396, 8007 to 8129) | undefined | added; no consumer reads it |
| `0x180` to `0x1b8` | **force-always interrupt 0 to 7** (write-only) | a write of any data sends a set packet for pin n as if the pin had asserted, even if already asserted, with the same buffer flushes as a real pin (OCR 6412 to 6426, 8132 to 8135) | undefined; the Bridge datasheet records a PROM zero store to byte `0x19c`, the data half of the `0x198` slot | added (OCR 1321 to 1323). **`0x198` is force-always interrupt 3** (Table 37, printed page 126; OpenBSD `xbridgereg.h` line 99; SN1 `bridge.h` line 697), the address identifies pin 3 force-always, but the Origin 200 byte store does not establish an accepted XBridge access width; delivery with the pin masked is unresolved |
| `0x1c0` to `0x1f8` | **force-with-pin interrupt 0 to 7** (write-only) | a write sends a set packet only if pin n is asserted, otherwise it is ignored (OCR 6428 to 6441, 8139 to 8146) | undefined | added; OpenBSD writes pin n's register after every serviced interrupt on an XBridge (`xbridge.c` lines 1563 to 1565); SN1 does the same (`pcibr.c` lines 6448 to 6449) |
| `0x200` to `0x238` | device 0 to 7 | 28 error lock 1; 27 page-cross check 1; 26 force parity 0; 25 virtual 0; **24 reserved**; 23 direct write-gather 0; 22 device size 0; 21 real time 0; **20 reserved**; 19 direct swap 0; 18 prefetch 0; 17 precise 0; 16 coherent 0; 15 barrier 0; 14 GBR 0; 13 PIO swap 0; 12 memory-not-I/O **1**; 11:0 window base, no reset in the specification: its Device (x) table points the field's reset at chapter 3 (OCR 8317 to 8319), which describes the architecture and gives no window value, and Table 83 gives widths only (OCR 9016 to 9044); firmware-sourced, device *n* comes up with base 0, 2, 4, 5, 6, 7, 8, 9, the layout of two 2 MB windows then six of 1 MB: against a model that reset every base to zero, the stock IP35 PROM's (version 6.210) `bridge_sanity` diagnostic reported on each head, in order, seven expected values `0x1800_1002`, `0x1800_1004`, `0x1800_1005`, `0x1800_1006`, `0x1800_1007`, `0x1800_1008` and `0x1800_1009` against `0x1800_1000` received (run `prom-20260910T164049-de445a`, captured in the boot archive (citation: `docs/boot-logs/firmware.boot.2026-09-10T16:41:23+00:00`)); that they are devices 1 to 7 in order, with device 0 at base 0 silent because it matched, is inferred from the report order and the Bridge's layout. The Bridge model resets its device registers to the same eight, from the Origin 200 PROM's diagnostic; the Bridge datasheet itself records no value | bit 24 page-mapped write gather, bit 20 page-mapped swap | bits 24 and 20 removed: page-mapped write gather is always off (OCR 9483 to 9484) and the page-mapped swap moved into the translation entry (OCR 1278 to 1281); Both consumers still write the removed bits: OpenBSD clears both write-gather bits on every device and sets bit 20 on every device that is not an IOC3, IOC4 or RAD1 (`xbridge.c` lines 2460 to 2473); SN1 removes only bit 20 from its XBridge conflict mask (`bridge.h` line 1208), sets both swap bits in its endianness path with no XBridge conditional (`pcibr.c` lines 7300 to 7303), and reinterprets a stale bit 20 as address bit 29 (lines 5166 to 5168). A model accepts and ignores writes to 24 and 20 |
| `0x300` to `0x3f8` | **buffer 0 to 15 address match upper and lower** (read-only) | upper: 31 filled, 30 armed, 29 flush, 28 crosstalk error, 27 packet error, 26 timeout, 25 prefetch, 24 precise, 23:16 byte enables, 15:0 address 47:32; lower 31:0; all X (OCR 6503 to 6583, 8532 to 8579) | undefined | added (OCR 1284 to 1304) |
| `0x400` to `0x5f8` | **performance counters, even buffers** (read/write) | seven 16-bit counters per even buffer plus a clear-all slot, eight slots in all; a word write clears a counter; reset X (Table 37, printed pages 129 to 131; sections 6.9.1 to 6.9.8, printed pages 169 to 171) | undefined | added; SN1 declares them |
| `0x1_0000` | translation table RAM | revision A, per the specification: `0x1_0000` to `0x1_0fff` in the widget map, 4 KB, 512 eight-byte entries (OCR 9067); the prose says 512 entries and 8 MB mapped with 16 KB pages, consistently through the change list, the architecture chapter and the mapping chapter (printed pages 25, 44, 181, 187). The widget map has no read range; the software-notes appendix (Table 134, printed page 252) gives the Bridge's read view, upper word at `0x1_0004` and lower at `0x1_1004`, in an appendix carried over unchanged from the Bridge. Revision B, which the Origin 300 carries, differs: see [Translation RAM by chip revision](#translation-ram-by-chip-revision) | 128 entries, read view at `0x1_1000` | size and read view differ by revision; **the model follows revision B as the SN1 header describes it: 1024 entries written from `0x1_0000` to `0x1_1fff`, upper words read at each entry's `+4`, lower words read from `0x1_2000` to `0x1_3fff`**, with page translation still limited to the first 512 |
| `0x8_0000` to `0x1f_ffff` | reserved | (OCR 9089) | external SSRAM, 512 KB | removed (OCR 1230); SN1 skips its SSRAM probe on XBridge |
| `0xc0_0000` to `0xff_ffff` | 4 MB section | translated to PCI memory `0x0` to `0x3f_ffff` or to the external flash port depending on which bridge (OCR 8917 to 8919, 9106); the flash is behind the port-0xf bridge | external flash | redefined |

Registers whose definition, fields and reset values are unchanged from the Bridge:
request timeout `0x28` (reload `0xfffff`, 960 ns tick); interrupt destination lower
`0x38`; error command word `0x40`; LLP configuration `0x48` (`0x3ff`, `0x06`,
`0x010`, inert in XBridge mode); target flush `0x50`; aux error and response-buffer
error `0x58` to `0x68`; arbitration `0xa0` (tick 2, high-ring `001`); PCI timeout
`0xc0` (retry hold 1; the Bridge's GIO-timeout bit 12 is reserved, since GIO mode is
gone, Table 55 printed page 144); type 1 configuration `0xc8`; PCI error lower `0xd8`; interrupt
mode `0x118`; interrupt device `0x120`; host error field `0x128`; the eight host
address registers `0x130` to `0x168`; write request buffer flush `0x240` to `0x278`;
even and odd read response buffers `0x280` and `0x288`; RRB status and clear `0x290`
and `0x298`. Test pin control `0x70` has fields in the XBridge specification (23:16
test data out, 15:0 eight two-bit selects, reset 0; OCR 7141 to 7211) that the Bridge
datasheet does not record; not compared against the Bridge specification.

### Crossbar half (widget 0): registers against the Crossbow

| Offset | Register | XBridge | Crossbow baseline | Delta |
|---|---|---|---|---|
| `0x00` | identification | part `0xd000`, manufacturer 0, revision from 1 (OCR 3562 to 3567) | part 0, manufacturer 0 | part number |
| `0x08` | widget 0 status | 31:24 link F to 8 interrupt requests; 23 widget 0 interrupt; **9:6 source port**; 5 register access error; **4 receive timeout; 3 arbiter timeout**; 2 crosstalk error; **1 destination timeout**; 0 multiple error; all reset 0 (OCR 3768 to 3839) | error and status summary | bits 9:6, 4, 3, 1 added; SN1 marks exactly these Xbridge-only (`sn/xtalk/xbow.h` lines 363 to 368) |
| `0x20` | widget 0 control | **7 enable link timeout counters, reset 1; 6 enable widget 0 timeout counters, reset 0**; 5 interrupt on access error; **4 interrupt on receive timeout; 3 on arbiter timeout**; 2 on crosstalk error; **1 on destination timeout**; all interrupt enables reset 0 (OCR 3850 to 3894) | control, reset 0 | bits 7, 6, 4, 3, 1 added; **reset value `0x80`, not 0** |
| `0x28` | packet timeout | 19:0 reset `0xfffff`, 1.28 µs tick; the first interval after reset is 2.56 µs (OCR 3724 to 3740) | same value | none |
| `0x30` | interrupt destination upper | 31:24 vector, reset 0; 19:16 target, **reset 8, bit 19 always 1**; 15:0 address 47:32, reset 0 (OCR 3673 to 3678) | reset 0 | reset differs: `0x0008_0000`; open whether the Crossbow datasheet is wrong or the chips differ |
| `0x60`, `0x68` | performance counters A and B | **22:20 link select**, 000 = link 8, reset link 8; 19:0 count; only 22:20 writable (OCR 3930 to 3963) | 23:20 select, reset `0x0080_0000` | field narrowed; resets to 0 in the three-bit encoding; open which datasheet misreads |
| `0x70` | NIC | 19:10 pulse 0; 9:2 offset 0; 1 data valid **1**; 0 data 0 (OCR 3988 to 3997) | same | none |
| `0x78` | **widget 0 reset fence** | 7:0 accept warm reset from widget F to 8, each reset 1 (OCR 4103 to 4118) | reserved | added |
| `0x80` to `0xb8` | **link 8 to F reset fences** | same layout, reset all ones; every bit read/write, the link's own bit included (OCR 1362 to 1392, 2955 to 2979, 4129 to 4161) | reserved | added |
| `0xc0` | **lock** | 7:0 lock value, reset 0; a write succeeds only while the value is zero (OCR 4167 to 4200) | reserved | added |
| `0xc8` | **lock clear** | a write clears the lock; a read returns the lock value (OCR 2994, 4177 to 4179) | reserved | added |
| `0x100` to `0x2f8` | link 8 to F: flush, control, status, arbitration upper and lower, status clear, reset (write-only), aux status, `0x40` per link (OCR 3003 to 3154) | same | none in layout |
| link control | 31 link-alive interrupt; 29:28 performance mode; 27:25 buffer level; 24 eight-bit mode (survives link reset); 23 force bad packet; 22:18 credit, reset 2; 17, 16 and 8:0 interrupt enables (OCR 3189 to 3287) | same | none |
| link status | 31 alive; 18 multiple; 17 illegal destination; 16 over-allocation; 15:8 bandwidth-error sources; 7:3 LLP counters and errors; 2 reserved; **1 max request timeout**; 0 source timeout (OCR 3320 to 3384) | same | SN1 notes the older names for bits 1 and 2 |
| link aux status | 31:24 receive retries; 23:16 transmit retries; 15:8 timed-out sources; 6 failure mode; 5 widget present; 4 eight-bit mode (OCR 3397 to 3431) | same | none |

Unchanged from the Crossbow: error upper and lower `0x10`, `0x18` (reset 0); error
command word `0x40` (a write clears it and both error address registers); LLP control
`0x48`; widget 0 status read-and-clear `0x50`; arbitration reload `0x58` (reset 2).
Arbitration for widget 0 reuses the unused self-field of link A's arbitration register
and the specification asks software to program it to `0x1f` (OCR 3447 to 3450).

## Behavioral contract

**PCI configuration access.** Type 0 through eight 4 KB apertures at `0x2_0000` plus
device times `0x1000`, function times `0x100` inside; type 1 through `0x2_8000` with
bus and device from register `0xc8`; interrupt acknowledge through an eight-byte
aperture, `0x3_0000` to `0x3_0007` (Table 84, printed page 179, OCR 9069 to 9087).
Unchanged from the Bridge, and OpenBSD, Linux and the MIPS port use the
same arithmetic on both parts; only the PIC offsets device numbers by one.

**The model splits a doubleword configuration access into two words, on the
evidence of one diagnostic.**
The specification's register map lists each device's configuration space as
"read/write Word" (the PCI Configuration Spaces rows, OCR 12577 to 12607), a
32-bit quantity in its own definition (OCR 8812 to 8815: SGI's word is 32 bits
and its double word 64), and says nothing about a 64-bit access to it. The
Origin 300's own field diagnostic issues one: io8bt (`sec0_0.c` line 119,
"Verifying the different size config space reads for the IOC3 in slot 4")
reads the IOC3's configuration header at offset 0 with a 64-bit load and
compares it with the two 32-bit words, expecting the identity word in the upper
half and the command and status word in the lower. Against a model that
answered the doubleword with zero it reported "Data miscompare on IOC3, exp
data was read in 64 bit mode, rec in 32 bit mode", received
`0x000310a902800006` (the two words) and expected `0x0000000000000000`, run
`prom-20260923T071849-dcc0fb`; the monitor's own `rp` at
`0x920000000f024000` read zero where `rp32` read the two words. So the head
performs an 8-byte configuration access as two 4-byte accesses, lower address
first into the upper half, the big-endian order of the Crosstalk doubleword.
That is inferred from what SGI's diagnostic expects, not measured on the chip:
the only observation is the model's own failure, and the expected value is the
diagnostic's. The same split for a doubleword write is a further inference,
since neither the specification nor any consumer issues one. Whether the chip issues two PCI configuration cycles or one 64-bit
cycle is not stated; the visible result is what the diagnostic checks.
`tests/qtest/sn1-xbridge-test.c`'s `config-doubleword` case pins the model's
behaviour; a doubleword configuration read captured on an Origin 300 would
settle the contract. *(read split inferred from the diagnostic; write split
inferred by analogy; not established)*

The specification does not state what an empty slot returns; the master-timeout error is
the mechanism (OCR 10013 to 10017). OpenBSD probes with a fault-guarded 32-bit read on
every chip (`xbridge.c` lines 1044 to 1045). SN1 probes the same way and then reads
the error-interrupt view for the master-timeout bit, because on an XBridge that bit
does not appear in the interrupt status register during the probe, and clears the
view with reset bit 6 (`pcibr.c` lines 1426 to 1449). A model should reproduce that:
a configuration read of an empty device raises the master-timeout condition into the
view register whether or not it reaches the status register.

**Interrupt acknowledge.** The aperture is the eight bytes the Bridge datasheet sizes
it at, and the Bridge specification's own map agrees (rev 4.0, printed page 55, OCR
3517). `0x3_0008` to `0x7_ffff` appears in no map entry, so it is an undefined address
and an invalid-address error; inferred by combining the map with the undefined-address
rule (printed page 123, OCR 6265 to 6266). No undocumented aperture hides there: the
special-cycle generation the mapping chapter lists for the first 2 MB (printed page
176, OCR 8915 to 8916) has no map entry either, and the PCI command table marks the
Special Cycle command not used and Interrupt Acknowledge as generated only by the
bridge (Table 93, printed page 194, OCR 9797 to 9798). The specification's PCI
chapter says the cycle exists for a system with an interrupt controller, which the
bridge's own pin
reporting makes unnecessary; that a host write to the register generates the
interrupt-acknowledge command on the PCI bus; and that a read returns the eight-bit
value the controller supplies (printed page 201, OCR 10188 to 10194). Its software-notes
appendix lists the slot as a word read (Table 134, printed page 253, OCR 12611). Which
access starts the cycle is therefore unsettled within the document. Linux mainline
names the slot (`bridge.h` lines 228 to 234 and 372); OpenBSD's header does not, and
neither driver uses it (counted: no reference in `pci-xtalk-bridge.c` or
`xbridge.c`). What a cycle with no responder returns is not stated for this cycle;
the master-abort rule for any PCI cycle the bridge originates (printed page 198, OCR
10014 to 10017) is the mechanism the empty-slot configuration read already relies on.
A word read of the slot on an Origin 300 would settle both the access direction and
the returned value.

**Device windows.** Two 2 MB windows for devices 0 and 1 at `0x20_0000` and
`0x40_0000`, six 1 MB windows for devices 2 to 7 from `0x60_0000`, then the 4 MB
section at `0xc0_0000` (Table 84, OCR 9091 to 9106), stated by the specification and
not only inferred from OpenBSD. In memory space the top two bits of the 12-bit device
base are ignored, so devices 0 and 1 use base bits 9:1 for PCI 29:21 and devices 2 to
7 bits 9:0 for PCI 29:20; in I/O space all twelve bits map (Table 83, OCR 9016 to
9041). The PIC repeats the layout for its second bus at `0x80_0000`; the XBridge does
not.

**PCI memory and I/O space above the widget face.** The specification gives each
widget one 48-bit address space, of which a bridge half uses the lower 8 GB, and
describes no second, narrower address form: every mapping is stated on that one
address, so decoding depends on the value alone, however many bits the host's window
presents (printed page 173, OCR 8763 to 8770; page 175, OCR 8866 to 8869). Above
the 16 MB face, `0x0000_4000_0000` to `0x0000_7fff_ffff` reaches PCI memory `0x0`
to `0x3fff_ffff` as a 32-bit device target; `0x0000_8000_0000` to
`0x0000_bfff_ffff` reaches the same 1 GB as a 64-bit device target;
`0x0001_0000_0000` to `0x0001_ffff_ffff` reaches the whole 4 GB of PCI I/O space;
`0x100_0000` to `0x3fff_ffff`, `0xc000_0000` to `0xffff_ffff` and everything from
`0x2_0000_0000` up are reserved (printed page 176, OCR 8925 to 8932; Figure 18,
printed page 177, read from the original scan because its OCR is scrambled). The
PCI address is the offset within the range: the specification calls both memory
ranges an alias of the lower 1 GB of PCI memory (page 176, OCR 8928 to 8929) and
Figure 18 draws the I/O band onto the whole 4 GB I/O space, so crosstalk bits 29:0
become PCI memory bits 29:0 with 31:30 zero, crosstalk bits 31:0 become the PCI I/O
address, and the range-selecting bits above them are consumed. Both public drivers
agree: OpenBSD sets each bus space's base to the window base minus the range base
(`xbridge.c` lines 2771, 2777, 2827, 2833) and Linux mainline bounds programmed I/O
at PCI `0x0` to `0x3fff_ffff` for memory and `0x0` to `0xffff_ffff` for I/O
(`bridge.h` lines 730 to 733). Unchanged from the Bridge: the Bridge specification
rev 4.0 has the same paragraph (printed page 52, OCR 3341 to 3348), which the Bridge
datasheet's widget-space table stops short of; a finding for that datasheet's owner.
Linux mainline declares the three ranges (`bridge.h` lines 722 to 741) but keeps its
PCI resources inside the face (`ip27-xtalk.c` lines 89 to 99); OpenBSD maps its PCI
memory and I/O bus spaces through the 32-bit alias and the I/O range on every
XBridge, withholding the I/O range only from the PIC, and notes that the hardware
honours only the low 30 bits of a memory address (`xbridgereg.h` lines 68 to 72;
`xbridge.c` lines 632 to 634, 2729 to 2744, 2790 to 2804). OpenBSD never uses the
64-bit alias (counted: no reference to its base in `xbridgereg.h` or `xbridge.c`),
and its second-bus memory base `0xc000_0000` (`xbridgereg.h` line 70; `xbridge.c`
line 2793) is a PIC address that lies inside the XBridge's reserved gap between the
64-bit alias and the I/O range.

Byte placement in these ranges is control bit 22 for the two memory ranges and bit 23
for the I/O range, each enabling the swapper for accesses above widget space to that
PCI space, where the device registers' swap bits do not reach (printed page 134, OCR
6863 to 6871; page 178, OCR 9048 to 9051). Inferred, not stated: the alias, not a
device register, selects the PCI-32 or PCI-64 placement of a narrow access, because
the specification names the two aliases for 32-bit and 64-bit device targets and
defines the device-size bit for the device windows (printed page 161, OCR 8215 to
8217); the I/O range has no 64-bit form. With both bits clear, which Linux mainline
and OpenBSD both ensure ("disable swapping for big windows", `pci-xtalk-bridge.c`
lines 677 to 681; `xbridge.c` lines 2087 to 2094), the no-swap placement of the
carry-over tables applies. A captured programmed-I/O access through each alias to a
device that reports its byte enables would settle the width inference.

A request whose address falls in no decoded range is an invalid-address error,
inferred by combining the reservation of the unused areas with the rule that any
access to an undefined address is one (printed page 123, OCR 6265 to 6266): interrupt
status bit 24 sets and the widget error-address and error command word registers
capture the request (Table 96, printed page 222, OCR 11082 to 11087), as the approved
invalid-address subset defines. The specification's error table does not name the
gaps between the aliases, and an address with bit 47 set falls under the same rule.

**Host address form.** The specification describes the widget's side only. Through a
Bedrock big window the public consumer presents a widget-relative address: OpenBSD
programs a translation entry with the memory-space form (I/O-space bit clear), the
target widget, and the widget address shifted right by 29 bits in M mode or 28 in N
mode, so the widget sees the entry's offset field followed by the processor's offset
within the window, 34 or 33 bits with bit 47 clear (`ip27_machdep.c` lines 69, 237,
493 to 496, 523 to 524, 568 to 569; `hub.h` lines 195 to 201; the [Bedrock
I/O-interface datasheet's translation-entry row](bedrock-io-interface.md)). The
XBridge decodes the lower 8 GB of that space; an M-mode entry with offset 16 or above
presents a reserved address. The fork decodes the ranges from jamesb/qemu#405 on; the
qtest checklist carries the assertions its tests meet, the inferred parts labeled.

**Translation table.** Entry format: 47:12 crosstalk address; 11:8 target widget;
**5 swap**; 4 barrier; 3 prefetch; 2 precise; 1 coherent; 0 valid; the Bridge's
remote-map field at 63:48 is gone (Table 92, OCR 9561 to 9603). Written as 64-bit
double words only, read back as 32-bit words only (OCR 9546 to 9548). Page size 4 KB
or 16 KB by control bit 21; index from PCI address 29:12 or 29:14; bits 31:30 select
local, page-mapped or direct (OCR 9551 to 9556). The 512 entries cover 2 MB in
4 KB mode (`0x40000000` through `0x401fffff`) and 8 MB in 16 KB mode
(`0x40000000` through `0x407fffff`). The latter is the maximum region in Table 82.
An address at or above `0x40800000` with bits 31:30 = 01 remains unresolved,
because the flow chart still routes it to the page mapper. Write gather through the page
map is always disabled (OCR 9483 to 9484). No public MIPS consumer programs the
table: OpenBSD's driver never touches it (`xbridge.c` lines 199 to 202) and confines
DMA to the 31-bit direct window.

**Direct-mapped DMA.** Local space `0x0` to `0x3fff_ffff` gets no response except
`0x3fff_0000` to `0x3fff_ffff`, which flushes the device's buffers (OCR 8824 to
8826). The 32-bit window `0x8000_0000` to `0xffff_ffff` forms the crosstalk address
from register `0x80`: target widget 23:20, offset 16:0 into address bits 47:31, bit
17 adding 512 MB; attributes from the device register (OCR 9264 to 9310). The 64-bit
window at or above `0x1000_0000_0000_0000` maps straight to a 48-bit crosstalk
address, and the bridge does not respond to 64-bit addresses from `0x1_0000_0000` to
`0x0fff_ffff_ffff_ffff` (Table 82, printed page 174), with attribute bits **63:60 target, 59 prefetch, 58 precise, 57 virtual, 56
barrier, 55 swap, 54:48 unused** (Table 87, OCR 9316 to 9324); only GBR and coherent
come from the device register. A 64-bit address with target 0 is not supported (OCR
9329 to 9331). A read is precise, prefetch or neither, never both, and neither bit
belongs on a 64-bit write (OCR 9238 to 9245). The MIPS port composes its 64-bit DMA
addresses with the swap and precise bits and records that the Origin 300's SCSI
controller needed the swap bit (`pci-sgipic.c` line 317; commit `f7ba467de043`), a
developer's report rather than a captured boot.

**Byte swapping.** Programmed-I/O through a device window: device register bit 13.
Outside the windows: control register bits 23 (I/O) and 22 (memory); OpenBSD and
Linux clear both, the MIPS port sets both. 32-bit direct DMA: device register bit 19.
Page-mapped DMA: entry bit 5 per the specification, but SN1 never sets it and sets PCI
address bit 29 of the mapped address instead, treating a stale device-register bit 20
as a request for bit 29 (`sn/pci/bridge.h` lines 1398 to 1402; `pcibr.c` lines 5156
to 5171). OpenBSD's header defines the same address-bit constant. Which rule
the hardware honours in that consumer's configuration remains unresolved. The
original specification places PCI bit 29 in the candidate index and assigns
swapping to ATE bit 5 alone. The model retains that rule for other revisions;
its revision-2 compatibility behavior is recorded below.
64-bit direct DMA: address bit 55.

**Origin 300 USB compatibility question.** A focused Origin 300 run (citation: `docs/notebooks/origin300-usb-dma-20260929.md`)
with stock PROM 6.210 and USB input observed the revision-B IO-8 XBridge set
16 KB pages and program valid ATE 1 with target `0xa` and ATE swap bit 5 clear.
The model then rejected DMA at PCI `0x6000_581c` and `0x6000_5820`.
Removing address bit 29 selects ATE 1, with offsets `0x181c` and `0x1820`.
The revision-A specification instead selects candidate index 32769 and cannot
map those accesses with its internal table. This is observed guest programming
on QEMU, not a measurement of revision-B silicon. Real Origin 300 inventories
and OpenBSD boots identify both fitted XBridge heads as revision 2. Linux
`v2.5.40` selects PCI bit 29 for XBridge mapped-DMA swapping by part identity,
without a revision condition, and leaves ATE swap bit 5 clear. The specification
explicitly covers revision A as designed; whether its page decoder shipped
unchanged is unknown.

For the modeled revision-2 XBridge, treat PCI bit 29 as a swap request and
omit it from the ATE index. This is an **inferred compatibility behavior** from
public software and the IRIX guest's programming, not an established revision-B
silicon measurement. The model also accepts ATE bit 5 as a swap request from
the revision-A specification; no observed revision-2 consumer tests both
requests together, so their combined behavior is an approximation. Every
currently instantiated XBridge head reports revision 2 and follows this rule;
there is no Origin 300-only selection property. Test bit 29 and ATE bit 5
independently at both page sizes, with translated bytes, faults, and addresses
outside the active table. A revision-B specification or matching physical DMA
measurement would settle the hardware rule. A revision-A measurement would
settle whether the published revision-A design reached silicon.

### Translation RAM by chip revision

**The Origin 300 carries revision B.** Real Origin 300s report both bridge heads as
revision 2: the PROM's own inventory lists `ASIC BRIDGE Rev 2` on widgets 14 and 15, and
OpenBSD attaches `XBridge revision 2` (see [Identity](#identity)). The specification
numbers revisions from 1 = A (Table 38, OCR 6769 to 6770), and the SN1 header names 1
`XBRIDGE_REV_A` and 2 `XBRIDGE_REV_B` (`sn/pci/bridge.h` at `v2.5.40` lines 929 to 930).
*(established)*

**The specification describes revision A as designed.** Its title page says rev 0.5
"for RevA Si", dated June 1998, before or during that silicon; it gives 512 entries in
4 KB at `0x1_0000` and only the Bridge's carried-over read view (the row above). It
says nothing of revision B. *(established for revision A's specified design, not
measured on silicon)*

**The SN1 header describes a larger table with a new read view, and names no revision
for it.** At `v2.5.40` it defines `XBRIDGE_INTERNAL_ATES` as 1024 and an 8 KB RAM (lines
33 to 37), says "The Xbridge has 1024 internal ATE's", lays the RAM out from `0x1_0000`
to `0x1_1fff` as doubleword writes whose upper words read back at each entry's `+4`, and
adds a lower-word view from `0x1_2000` to `0x1_3fff`, with the comment "the xbridge read
path for internal ates starts at 0x12000. I don't believe we ever try to read the ates."
(lines 264 to 290). The Linux 2.4 MIPS tree's copy of the same header has the same
layout and adds that the RAM is written as doublewords and, "due to internal design
issues", read back as single words (`include/asm-ia64/sn/pci/bridge.h` on its
`linux-2.4` branch, lines 61 to 65 and 497 to 521). OpenBSD's header also says 1024
(`xbridgereg.h` line 281), probably from the same source. The header's accessor
`b_int_ate_ram_lo` reads 512 entries up the write array, `0x1_1004`, which is the
Bridge's own lower-word view; the header serves both chips, so that says nothing about
the XBridge. The SN1 driver sizes its table from the 1024 define whatever the revision,
while a comment in it says 512. The header is SGI-written and public: evidence of the
chip SGI's software addressed, not a measurement, and by its own comment the driver
never read the entries back.

**SGI's `xbg` reads the new view on this machine, and is the only consumer that does.**
Its section 1 (`xbgRoa.c`) writes 512 entries as doublewords at `0x1_0000 + 8i`, each
`(~i << 32 | i) & 0x0000ffffffffff1f` — the upper word `~i & 0xffff`, the lower word `i`
with bits 7 to 5 cleared — then reads the upper words at `0x1_0004 + 8i` and the lower
words at `0x1_2004 + 8i`, comparing each with the value it wrote (loops bounded by `0x200`
at `0x10016c68` and `0x10016cf4`, the reads at `0x10016d2c` and `0x10016e20` in the
Origin 300 package's `xbg`). Against a model that treated `0x1_2000` as undefined it
failed section 1, sub-section 0, condition 1: received 0, expected 1, at
`0x920000000e01200c`, with the invalid-access log naming `0x1_2004` and `0x1_200c` (run
`prom-20260924T212506-ab8708`, and runs `prom-20260924T174227-a5fb3f` at four
processors and `prom-20260924T182131-28d027` at two). It does not touch entries 512 to
1023, so it says nothing about the table's size.

**How the two descriptions fit, and the alternative.** The reading taken here is that
the specification is revision A's design and the header, which `xbg` confirms on a
revision B machine for the view, is what the Origin 300's revision B does. The other
reading is that the header describes every XBridge, revision A included, and the
specification's 512 entries and `0x1_1004` view were superseded before revision A
shipped: the header carries no revision qualifier and SN1 software never sizes by
revision. The model is the same under either reading, since it presents revision 2. A
later revision of the specification, or a register dump from a revision A part, would
settle which. *(the revision split inferred; the alternative open)*

**What the model does.** It follows the header's layout: 1024 entries of storage
written as doublewords anywhere from `0x1_0000` to `0x1_1fff`, upper words readable at
`+4` of each entry, lower words readable from `0x1_2000` to `0x1_3fff`. *(lower-word view
at `0x1_2000` inferred from the header and the diagnostic, which agree; 1024 entries
inferred from the header)* Page translation keeps the reviewed 512-entry rule below —
indices above 511 fault — so entries 512 to 1023 are stored and read back but map
nothing. That is an approximation: whether revision B's page mapper reaches 1024
entries, and so maps 16 MB in 16 KB mode, is unknown, and a guest programming an index
above 511 and running DMA through it would tell. A register dump of the RAM on an Origin
300, writing distinct values to entries 0 and 512 and reading both views, would settle
the size. As before, only a doubleword write at an entry's own address and a word read
at `+4` are served; any other access in the RAM or the view, a write into the view
included, is an invalid-address error, the register window's rule.

### Reviewed page translation behavior

The page-mapped DMA review (citation: `docs/reviews/xbridge-page-mapped-dma-review.md`)
visually checks the original specification and clears these rules:

- Form the full candidate index from PCI bits 29:12 for 4 KB pages or 29:14
  for 16 KB pages. Reject indices above 511 rather than wrapping them to nine bits.
- A 4 KB translation uses ATE address bits 47:12 and PCI offset bits 11:0.
  A 16 KB translation uses ATE bits 47:14 and PCI bits 13:0. ATE bits 13:12
  do not affect the latter translation.
- ATE target bits 11:8 permit target zero. The target-zero restriction on
  direct 64-bit DMA does not apply here.
- ATE bit 5 controls swapping. Bits 4:1 name the barrier, prefetch, precise,
  and coherent attributes. Storage of those attributes does not qualify their
  packet semantics while the transport lacks that metadata.
- ATE bits 63:48 and 7:6 are ignored for translation. Their storage and
  readback behavior is unresolved and excluded from hardware qualification.

As an explicitly reviewed model deduction, an invalid entry or a candidate index
above 511 within the documented maximum PMU region sets status bit 30 and emits
no translated transaction. This combines the table-size, valid-bit, and invalid
page-access descriptions; the source does not enumerate both triggers verbatim.
Reset-interrupt-status bit 1 clears and rearms the page-fault group. Error-packet
delivery uses the separately reviewed enable and destination contract.

Register `0x90` has no published field layout, capture trigger, reset value, or
overwrite rule in the retained specification. Its contents cannot be qualified
as a faulting address or index. Addresses `0x40800000` through `0x7fffffff`
also remain outside clearance. Neither range may become a translated alias.

Tests must exercise both page sizes, full address reconstruction, target zero,
different targets and swapping on adjacent pages, descriptor rewrites, invalid
entries, and the within-region out-of-table case. Failed translations must leave
untouched destinations unchanged; multi-page operations are not assumed atomic.

## Interrupts

Eight active-low open-drain pins per PCI bus, assignment not predefined (OCR 1205 to
1209, 10447 to 10450). Status bit n follows the inverse of the pin even while masked,
needs no host clear and ignores one (OCR 10813 to 10814, 10862 to 10867): the
property the Bridge datasheet calls its most important requirement carries over. A
set packet is a double-word write to the destination named by the destination
registers with the interrupt number in 7:0 and bit 8 set; a clear packet is sent on
the pin's rising edge only when **both** the pin's enable bit and its mode bit are
set, with bit 8 clear (OCR 10808 to 10818, 7774 to 7777); OpenBSD, Linux and the MIPS
port set the mode bit. The SN1 consumer clears it, as described in its
force-interrupt sequence. Per-pin host address register n overlays crosstalk address
bits 47:38 from its bits 17:8 and supplies the host bit number in 7:0 (OCR 7863 to
7870); all four consumers write the same layout to Bridge and XBridge. The target
widget is destination-upper bits 19:16 and the node rides in the crosstalk address of
the Bedrock's interrupt register, node at bit 39 on IP35 (OpenBSD `ip27_machdep.c`
lines 350 to 353; SN1 insists a bridge's target never changes once set, `pcibr.c`
lines 7007 to 7031). Error interrupts are set packets only, grouped by logging
register, first error logged (OCR 10847 to 10856).

For the approved invalid-address subset, the first invalid register access sets
request-dispatch error bit 24 in interrupt status and the error view and captures
the access address in the widget error-address registers. If bit 24 is enabled,
XBridge sends one set packet to the target and address programmed in the error
destination registers. The payload is bit 8 set plus the low eight bits of the
host-error field. Further invalid accesses while the condition is set neither
replace the captured address nor send another packet. Reset-group bit 3 clears
status and view bit 24 and rearms the group. This does not define other error
groups or the map-fault-address register.

The captured address is the request packet's 48-bit address field: the upper register
holds bits 47:32 and the lower bits 31:0 of the address of the first error (Tables 40
and 41, printed pages 133 to 134, OCR 6812 to 6823 and 6838 to 6846), and the head's
error table says an invalid request address stores the 48 bits of the crosstalk
address there and the command word in the error command word register (Table 96,
printed page 222, OCR 11082 to 11087). A crosstalk address is relative to the widget:
the interconnect gives each widget a 48-bit space of its own (printed page 175, OCR
8866 to 8869), the register and widget-space maps are written in that form (page 123,
OCR 6277; page 179, OCR 9064), and the destination widget number travels in the
command word, whose destination-ID field at bits 31:28 the error command word keeps
(Table 46, printed page 137, OCR 6984 to 6995). So the captured value carries no
widget number; for a request inside the 16 MB face it equals the face offset with bits
47:24 zero, and for a request in the ranges above the face it is the full value, up to
34 bits from a Bedrock big window. That the internal crossbar delivers the address as
sent is inferred from the [Crossbow datasheet's statement that the crossbar performs
no address translation](xbow.md#dma) and from this specification's description of
forwarding whole packets from the input buffer through the crossbar (printed page
100, OCR 5097 to 5125); the specification does not repeat the Crossbow's statement.
The Bridge datasheet's "48-bit request address" and this document's "the address"
name the same value; the model's latch of the face offset matches it only inside the
face.

Each PCI bridge half exposes 32 raw legacy-interrupt sources for its own PCI bus:
pins A through D for slots 0 through 7, numbered `slot * 4 + pin`, with A as
pin zero through D as pin three. Each half preserves its levels across migration,
re-drives them after load, and clears them on reset. These outputs are an
integration boundary. They do not select one of that half's eight XBridge
interrupt pins or state the IP45 board's physical wiring.

The four added registers matter to the consumers: OpenBSD and SN1 write force-with-pin
interrupt n after every serviced interrupt on an XBridge instead of re-reading status
and poking the Bedrock (`xbridge.c` lines 1563 to 1569; `pcibr.c` lines 6420 to
6462), and SN1 reads the error-interrupt view after configuration probes. The
specification lists as will-not-fix a prefetch-stream re-request when an interrupt
flushes an active stream (OCR 1327 to 1332); the two-pin race OpenBSD describes for
the Bridge is not named in it. Open whether the race exists on an XBridge. SN1 is the
fourth consumer and writes the interrupt mode register to zero, saying it does not
want clear packets (`pcibr.c` line 3931), so the no-clear-packet path is exercised by
a real consumer too. With bits 31, 16 and 8 reserved and reserved bits reading zero
(printed page 123), an all-ones write to the enable register reads back
`0x7ffe_feff`, where the Bridge datasheet has `0x7fff_ffff`.

## Reset

Crossbar half: power-on reset initialises every register and every link; a link
reset resets one port (from the far end or by a write to that port's reset register, a
two-clock one-shot that also drives the remote reset); a warm reset received on any
port is treated as a power-on reset of the core and propagated to every port whose reset
fence admits the source, the source's own link included: a fence's own-port bit resets
set (printed page 27), and "for normal operation a link 8 should always be enabled to
accept a warm reset from itself" (page 81); the fences reset to all ones so an unprogrammed
chip propagates warm resets as older Crossbows did; a port ignores further reset patterns
for 1024 cycles (OCR 6161 to 6249, 3534 to 3541, 2860 to 2863). Which pattern the link
reset one-shot sends is stated two ways: its register's page says it makes the LLP "assert
the remote link reset signal" (page 70), the reset chapter that it asserts SendWarmReset
(page 122), the same conflict the Crossbow datasheet leaves open for its own chip. The model
follows each of these: a link's reset one-shot pulses that link's widget and nothing else,
through the one line the model has, which the bridge heads treat alike for either pattern
(inferred); propagation includes the source's link when its fence admits it; and a warm reset
inside 1024 cycles of the 100 MHz core clock ("single 100 Mhz clock known as the core_clock",
page 86) after the last reset pattern is ignored. Three parts of that window are inferred:
the page gives it to each port's LLP, where the model keeps one for the chip; it does not
name the clock the cycles count; and a pattern widget 0's fence rejects still opens it,
since the LLP detected it. A machine reset ends the window. What ends a link's
initialization after a reset is not stated, so a present link reports alive throughout, the
policy the Crossbow model applies to the links a machine selects. Bridge half: PCI
reset pins reset deasserted (`0xf`); the widget-ID field reads `0xf` in both halves at
reset and is reassigned later (OCR 6912). In XBridge mode the crossbar delivers a packet to internal port
`0xe` or `0xf` by its destination field, and nothing in the specification says the
bridge half compares an incoming destination against its widget-ID field; the only
role the specification gives a widget's own number is the source ID it places in the
packets it originates, which is why widget 0's status register grew a source-port
field. So the open question is whether firmware writes `0xe` and `0xf` into the two
control registers before the halves originate packets, and the settling observation is
a PROM trace of writes to those two control registers. The Bridge datasheet's rule that
the field must read back as the widget the guest expects carries over as the reason
the value must end up right. Fields marked X are undefined after reset (OCR 6266 to
6267).

**Fence scope.** Each fence gates one destination. Widget 0's fence names the source
ports from which widget 0 accepts a warm reset, and link n's fence names the source
ports from which link n accepts one, itself included (printed page 81, OCR 4097 to
4100 and 4124 to 4133). Established: all eight bits of every fence, the link's own bit
included, are read/write with reset value 1, and a written value reads back as
written. The fence tables list each bit as "Enable = 1, Disable = 0, Default = 1"
with no read-only marking (printed pages 81 and 82, OCR 4103 to 4118 and 4143 to
4161); the change list's table for the port 8 fence gives every bit, bit 0 from port
8 itself included, access R/W and reset 1 (printed page 27, OCR 1362 to 1392), and
its register-map table lists all nine fences (printed page 28, OCR 1398 to 1424);
and the widget 0 address map marks all nine fences read/write (printed page 59, OCR
2955 to 2979). That a link should admit resets
from itself is a note to software, "for normal operation a link 8 should always be
enabled to accept a warm reset from itself" (printed page 81, OCR 4129 to 4133), not
a bit the chip holds. The earlier reading of that note as a forced bit came from the
note alone, with no capture or measurement behind it; SGI's sMDK `xbg` register test
writes a fence value with the link's own bit clear and expects it back (run
`prom-20260924T172408-ca9240`), which is software expectation consistent with the
specification rather than independent hardware evidence. The dual-host
description is per link as well: its example programs link F's fence so that only
resets originating at port B propagate to link F, to keep one host from resetting
the other host or the other host's widgets (printed page 57, OCR 2850 to 2860).
A warm reset reaches a link when that link transmits the warm-reset pattern to its
widget. Established: a link whose fence rejects the source is not reached, so it
transmits no pattern and its widget is not reset, whatever widget 0's fence says
(printed page 81, OCR 4124 to 4127; page 57, OCR 2856 to 2860). Inferred,
for the one combination the specification does not state: the fences are
independent gates on one received reset, so a warm reset that widget 0's fence
rejects still reaches every link whose fence admits its source port. The reset
chapter describes only the unfenced default in which the core resets and every link
forwards (printed page 122, OCR 6238 to 6245); it says an accepted warm reset resets
all core logic without saying whether a link controller's own registers are core
logic, while a link reset affects no other link or link controller (page 122, OCR
6227 to 6228, 6242 to 6244), so whether a fenced-out link's control and status
registers survive the core reset is unknown. The model at the evidence cutoff
forwards nothing when widget 0's fence rejects the source, labeled as model behavior
in the implementation review (citation: `docs/reviews/xbridge-implementation-review.md`); a warm
reset captured on a two-host XBridge, or the register-transfer source, would settle
the combined case and the register question, and neither is held. A second open
conflict concerns the fences themselves: the change list says the nine fence
registers "are reset only by power on reset" (printed page 27, OCR 1357 to 1360),
while the reset chapter treats an accepted warm reset as a power-on reset of the
core (printed page 122, OCR 6238 to 6245). The model returns the fences to all ones
on an accepted warm reset; whether they instead keep their programmed values is
unknown, and a warm reset on hardware followed by a fence read would settle it.

Reset values the specification gives, bridge half: control `0x0f00_320f` with bits
5:4 from the speed pins (PCI resets `0xf`, credit 3, big-endian 1, widget `0xf`); request
timeout `0xfffff`; LLP configuration `0x3ff` retries, `0x06` null timeout, `0x010`
burst; direct map bit 17 set; arbitration tick 2 and high-ring `001`; NIC data valid
0; PCI timeout retry hold 1; interrupt enable 0; interrupt mode 0; interrupt device 0;
host address registers 0; each device register error-lock 1, page-cross 1,
memory-not-I/O 1, rest 0; read response buffers 0; error-interrupt view and multiple
interrupt 0. Not the specification's but firmware-sourced: the device registers'
window bases come up 0, 2, 4, 5, 6, 7, 8 and 9 for devices 0 to 7, which the stock
PROM's sanity diagnostic asserts and the specification leaves unset. Crossbar half: widget 0 control `0x80`; packet timeout `0xfffff`;
interrupt destination upper `0x0008_0000`; arbitration reload 2; performance counter
selects link 8; NIC data valid 1; every reset fence all ones; lock 0; link control
credit 2. The complete crossbar-half table, from the review's reading of the
specification (a field defaults to zero unless its table says otherwise, and
reserved fields read zero, printed page 63):

| Offset | Register | Reset | Printed page |
|---|---|---|---|
| `0x00` | identification | fixed: revision at least 1, part `0xd000`, manufacturer 0, bit 0 one | 71 |
| `0x08` | widget 0 status | 0; bits 9:6 report the requesting port and are not state | 75 to 76 |
| `0x10`, `0x18` | error upper and lower address | 0 | 72 to 73 |
| `0x20` | widget 0 control | `0x0000_0080` | 76 to 77 |
| `0x28` | packet timeout | `0x000f_ffff`; first interval after reset 2.56 µs | 74 |
| `0x30` | interrupt destination upper | `0x0008_0000` | 73 |
| `0x38` | interrupt destination lower | 0 | 73 |
| `0x40` | error command word | 0 | 71 to 72 |
| `0x48` | LLP control | `0x03ff_1810` (retry `0x3ff` at 25:16, null timeout `0x06` at 15:10, burst `0x010` at 9:0) | 77 |
| `0x50` | widget 0 status, read-and-clear | alias of `0x08`, no state of its own | 59 |
| `0x58` | arbitration reload | `0x0000_0002` | 74 |
| `0x60`, `0x68` | performance counters A and B | 0 (select `000` is link 8) | 78 |
| `0x70` | NIC | `0x0000_0002` | 79 |
| `0x78` | widget 0 reset fence | `0x0000_00ff` | 81 |
| `0x80` to `0xb8` | link 8 to F reset fences | `0x0000_00ff` each | 81 to 82 |
| `0xc0` | lock | 0 | 82 to 83 |
| `0xc8` | lock clear | no state; a read returns the lock value | 60 and 82 |
| `0xd0` to `0xf8` | reserved | access sets the register-access-error bit | 60 |
| link `+0x00` | input buffer flush | no state; reads 0, or 1 while another flush runs | 63 |
| link `+0x08` | link control | `0x0008_0000` (credit 2 at 22:18) | 63 to 65 |
| link `+0x10` | link status | 0 for every error bit; bit 31 link-alive is link state | 65 to 67 |
| link `+0x18`, `+0x20` | arbitration upper and lower | 0 | 69 to 70 |
| link `+0x28` | link status, read-and-clear | alias of `+0x10` | 60 to 63 |
| link `+0x30` | link reset | write-only one-shot, no state | 70 |
| link `+0x38` | link auxiliary status | 0 for the retry counters, timed-out sources and failure mode; widget present (5) and eight-bit mode (4) are link state | 67 to 68 |
| `0x300` to `0xff_ffff` | reserved | access sets the register-access-error bit | 63 |

## Access widths

Bridge-half registers hold 32 bits or fewer in bits 31:0 of a double word and accept
only a double-word packet with four data enables, a 32-bit load or store; anything
else is an address error (OCR 6280 to 6284). Crossbar-half registers likewise; a
reserved address, another packet size, a write to a read-only or read-and-clear
register, or a read of a write-only register, which in this half means the eight
link reset registers, sets the register-access-error bit (printed page 58, OCR 2895
to 2902; page 75, OCR 3801 to 3805). The specification names the write-only read
only in that bit's definition. That such a read is also logged, command word and
48-bit address in the crossbar error registers, and interrupts if widget 0 control
bit 5 is set, is inferred from the shared bit and from the action the specification
gives every other register-access error (printed pages 51 to 52, OCR 2554 to 2573;
Table 9, page 54, OCR 2675 to 2711), none of which names this case. Whether widget 0
answers such a read, and with what data, the specification does not say; the
model's zero is its choice, and a register read on an Origin 300 would settle it. The
translation RAM takes 64-bit writes and 32-bit reads. OpenBSD reads Bridge and
XBridge registers with a 32-bit access at offset plus 4 (`xbow.h` lines 109 to 113).
Any access to an undefined address is an invalid-address error, interrupt status bit
24 (OCR 6265 to 6266, 7544 to 7545): the same latch the Origin 200 model already
implements. For the fork: `impl.max` of 4 on the register window, as the Bridge.

## Migration state

Bridge half, per bridge: control, request timeout, both interrupt destinations, LLP
configuration, test pin control, direct map, map fault address, arbitration, NIC, PCI
timeout, type 1 configuration, interrupt enable, mode, device and host error field,
eight host address registers, eight device registers, both read response buffer
allocations, the performance counters, and the translation RAM. Latched read-only
state: interrupt status, error-interrupt view, multiple interrupt, error address and
command, aux error and response-buffer error, PCI error upper and lower, RRB status,
sixteen address-match pairs, the status register's counters. Crossbar half: widget 0
control, packet timeout, interrupt destinations, error command and addresses, LLP
control, arbitration reload, the counter selects, NIC, nine fences, the lock, and per
link the control, arbitration and latched status registers (Table 37, OCR 6296 to
6729; Table 10, OCR 2911 to 3161).

## Implementation limits

Super Bridge mode and the PIC are outside the selected Origin 300 configuration.
External crosstalk links initially report no widget on the empty XIO connector.

### The XIO connector is an external cable port

The Origin 300 manual (007-4365-002, printed page 8) describes the rear XIO connector:
"This Crosstown2 connector connects an SGI Origin 300 server to an InfiniteReality
graphics pipeline. This connection is made with a NUMAlink 3 cable at 800 MB/s in each
direction." It is an external cable port, not an XIO slot: an expansion enclosure's cable
ends there, and no XIO board attaches to it directly. The project owner states that it
also takes P-brick (PCI) and X-brick (XIO) expansion enclosures, beyond the InfiniteReality
graphics pipeline the manual names. A Library search of the Origin 300 manuals found no
source for that, so it is graded unknown; for the P-brick the same manual page contradicts
it, saying a peer-attached PCI expansion module connects through the LINK (NUMAlink)
connector. None of these enclosures is modelled.

That the XBridge's crossbar section serves the connector is inferred: the Bedrock's own XIO
port is already the XBridge's host port, 0xa, so the connector can reach the Bedrock only
through the crossbar. Which of the six external ports 8 to 0xd it is wired to, no source
read so far says. The model selects 0xb, stated at `SGI_XBRIDGE_XIO_PORT_DEFAULT` in the
QEMU header (approximated). What would settle it: `hinv` or the hardware graph of an Onyx
300 with a graphics brick attached, or, on real hardware, the XBridge's per-link
widget-present status for links 8 to 0xd with that brick connected.

The model decodes the port with nothing behind it: its link reports no widget and not
alive, and a read of the position returns zero, which is the model's choice rather than a
recorded answer. The machine offers no XIO slot there and refuses ordinary
`-device` XIO boards. One narrow synthetic exception permits an explicit
`-device sgi-odyssey` for register tests; the
`origin300-v12-direct` composition (citation: `docs/machines/origin300-v12-direct.md`)
selects the same direct widget in the compiled catalogue. Both use port 0xb
without an enclosure XBridge or L1. This does not establish that a V12 board
can connect directly to an Origin 300, and the physical
`origin300-vbrick` composition (citation: `docs/machines/origin300-vbrick.md`) remains
reference-only. QEMU's `sn1-odyssey-test` checks widget identity, link
state, reset and duplicate-attachment behavior on the model; it does not settle
the physical cable route. The chip interface that attaches a widget to the port
(`sgi_xbridge_attach_xio()`) routes it as a crossbar port routes, to widget 0,
the host's port and both heads, with the
one exclusion the specification gives, "a link cannot send packets to itself" (rev 0.5,
printed page 68). A packet for its own port, or for an external port with nothing behind
it, fails and is logged as a guest error. The port's link reset and propagated warm resets
leave on its warm-reset line for whatever is linked there. The qtests
`sn1-xbridge-chip-test` and `sn1-address-map` check these.

The approved PCI-head increment includes the reviewed page-mapped DMA subset.
Performance counters, address-match behavior, and the flash port remain deferred.
This sequencing does not remove diagnostic requirements. The retained Origin 300
field-diagnostics guide names page-mapped DMA in `xbg`; passing its register and
translation qtests does not qualify that diagnostic.
Storage-only registers or zero readback do not qualify the operations they
describe. Reconcile the other deferred behavior with the diagnostic inventory
before claiming device acceptance.

### Multiple-interrupt detection remains unresolved

The independent review of 2026-09-05 distinguishes the visible multiple bit from
state recording an earlier occurrence. Offset `0x178` is read-only and resets
to zero. A set bit reports a repeated condition before that condition was cleared,
but the specification does not identify the complete detector or its service
boundary. Pin status itself follows the physical wire. This does not establish
whether deassertion, a clear packet or a force operation changes the detector's
prior-occurrence state.

Do not add a history latch that persists until reset merely because the visible
multiple register has no documented software clear. In particular, two forced
set packets are not sufficient evidence for a multiple-bit transition. The
named `0x110` reset groups do not establish a clear for this register. Device
reset clears it. Cleared tests cover reset-zero and ignored writes; actual
recurrence generation remains an unimplemented requirement until its transitions
are specified. Normal pin and force packet delivery can be implemented separately.

### Masked force-interrupt delivery remains unresolved

A force-always write bypasses physical pin state. The published specification
does not state whether it also bypasses the matching interrupt-enable bit.
Tests requiring delivery must enable the pin first. Neither delivery nor
suppression of a masked force write is cleared as hardware behavior. The
force-enable review (citation: `docs/reviews/xbridge-force-enable-review.md`) records the
original specification and consumer evidence, which does not exercise that case.

## Behavioral notes

**The translation-table size is a source conflict, not a choice.** The specification
says 512 entries in prose and in its widget map, consistently; both public driver
headers say 1024, and the SN1 driver's own comment says 512 while its code zeroes
1024. The specification has no revision history to consult (a counted search finds
one occurrence of 1024, the reset ignore window). The model serves 512 entries and
ignores writes to the next 4 KB; settle by observing a stock PROM's writes to the
table.

**One-wire ownership depends on operating mode, and on whether a head has a
board.** In the selected XBridge mode, widget 0 offset `0x70` (the crossbar's
own NIC register, in the Xbow half) owns the one-wire connection to the
system board's own identity part; that register is not yet modelled
(`hw/misc/sgi-xbridge-xbow.c` logs "one-wire pulse not modeled" and takes no
other action). The rev-A specification separately calls each bridge half's own
NIC register at offset `0xb0` unused in XBridge mode, wired only to widget 0's
pin instead. QEMU currently supplies a placeholder responder at widget 15,
but the stock-PROM call formerly attributed to `0xb0` is a generic
board-information path, and a run-scoped register watch saw no NIC-register
access through its BaseIO identity selection. The earlier BASEIO crash did not
isolate a missing one-wire response as its cause. Neither run establishes an
alternate rev-2 pin route or real IO8 one-wire population. Widget 14, without
a board, remains inert in the model. In Super Bridge mode the bridge
associated with port F owns the pin; that mode is outside this Origin 300
configuration. Mainline Linux's bridge-half probe is a consumer mismatch, not
evidence for an alias, and the earlier Linux-attachment workaround stays
withdrawn on that basis.

### Stock-PROM and BASEIO PROM IO-8 identity access

The stock IP35 PROM call at `0xc00000001fc43370` invokes generic
board-information discovery, not an established Bridge one-wire reader. Its
`Laser` text comes from a board-information path that sends L1 EEPROM-area
request `0x0006` on the inferred IP45 branch; `0xc00000001fc433b0` can save
the text converted to a numeric identity candidate. This is not the ASCII
Ethernet field used by loaded BASEIO's production `ef` path. The old claim
that this call reads widget-15 Bridge NIC register `0xb0` was inferred from
the XBridge contract, not decoded from its callee. In a fresh QEMU
`816f74ec2abbe864605d1072a7f4c33d9ee4010a` run, supported 32-bit
read/write watchpoints at widget-15 `0x9200_0000_0f00_00b0` and widget-0
`0x9200_0000_0000_0070` recorded zero accesses from reset to the BaseIO
identity selector at `0xc00000001fc6a544`. A second bounded watch recorded
no access from its attachment after that selector to the maintenance menu;
it timed out at the menu, so it does not cover the gap between attachments.
These are emulator-run negatives at the watched widths, not measurements of
XBridge silicon. A third paused-reset run
`irix-20260915T053802-8a5e66` reached the selector's "using BaseIO nic"
print branch at `0xc00000001fc6a57c`. Its preceding board-information helper
returned `1`, but the debugger dumped zero at the selector's saved candidate
slot. The branch requires a nonzero loaded value, so register and memory
observations conflict; the banner alone cannot establish a read from either
one-wire register. The manufacturing source of the IO8 `Laser` field remains
unidentified.

The loaded BASEIO PROM -- the gzip segment inside the same stock PROM payload
at file offset `0x8bbb0`, decompressing to the 1,468,990-byte image the
project's `docs/notebooks/ioc3-prom-netboot-driver.md` also disassembles, SHA-256
`f903b3ce33b86c286f168ba43395f36a0073c24b1d4e703adce240f683b8c2d7` -- attaches
the IO8 head as a bridge object. The loaded image's I/O setup path obtains a
global device-description object and follows pointers at offsets `+0x10` and
`+0x38` without checking whether the latter is null. A separate installation
path handles a missing `+0x38` pointer by defaulting the DMA target to node 0.
Before this amendment's fix, a fast single-CPU
boot on `sgi-origami-o300-bringup` at `915278a59a` reproduced this exactly at
"Loading BASEIO prom .......................": console `*** TLB Refill
Exception on node 0` / `*** EPC: 0xc000000013072bd8`, and QEMU's own trace
`cause 2 ... A 0000000000000028` -- an exact match to this document's cited
EPC and BadVAddr. A breakpoint before the pointer load at
`0xc000000013072bd4` dumped the live connection object at
`$a2 = 0x96000000000328c8`: offset `+0x38` read `0x0000000000000000` (the
NULL later dereferenced), and the very next field, offset `+0x40`, read
`0x920000000f900000` -- a well-formed widget-15 crosstalk address. The
connection object therefore already knows it concerns widget 15; only the
software bridge-object pointer at `+0x38` was unset. This identifies the
null dereference. It does not identify the missing producer of that pointer,
or establish that the stock PROM read `0xb0`.

That model run establishes the pointer failure, not one-wire causation or
what bytes a real IP45 IO8 presents. No identified configuration-space or
one-wire capture of an Origin 300 IO8 exists in this project (the gap this
datasheet's "Origin 300 IO-8 population supplement" records for other IO8
fields). The generated L1 BIA/CIA are synthetic inputs, not physical dumps.
The implementation
(`hw/pci-host/sgi-xbridge.c`, `hw/mips/sn1.c`) therefore answers with an
explicit placeholder record rather than an invented SGI part number: a
DS2505 board-identity part (family `0x0b`, the same family this project's
Bridge model and OpenBSD's board-part reader already use), carrying the text
"NOT-OBSERVED" in the twelve-character part field and serial `0x060504030201`
(the same ascending-byte placeholder pattern `hw/pci-host/sgi-bridge.c`
already uses for its own unobserved default). Family `0x0b`'s applicability to
this specific board, and the exact identity bytes, are inferred from the one
board-identity NIC family this project has confirmed elsewhere, not observed
for IP45; they are model expectations, not a hardware reading, and remain open
to independent review and replacement by a real IP45 capture. A head with no
board (`board-part` unset, the default for widget 14) gets no bus and no
responder, leaving `0xb0` exactly as inert as before.

The io8 record has not yet moved onto the fixed-offset layout that the
[Bridge datasheet](bridge.md#the-page-is-a-fixed-offset-binary-record-and-the-fieldvalue-string-is-formatted-from-it)
gives and that QEMU now uses for the IP29, plain BaseIO and BaseIO-G
records. jamesb/origami#1613 tracks grounding its fields and moving it.

The qtest `qtest-mips64/sn1-xbridge-onewire-test` exercises the implemented
register contract directly: a head given a `board-part` answers a one-wire
reset, ROM search and page read with the placeholder identity and a correct
CRC (`test_placeholder_identity_reads_back`), while a head given none leaves
`0xb0` unresponsive (`test_no_board_stays_inert`). It does not, and cannot,
assert what a real IP45 IO-8 returns.

### IO8 one-wire identity is separate from the Ethernet address

The loaded BASEIO diagnostic caller at `0xc0000000130bfbac` passes IOC3
MicroLAN at its mapped base plus `0x30` and IOC3 GPIO at base plus `0x34`
to a one-wire Ethernet-address reader. This is separate from production `ef`
initialization. Its accepted custom DS2501 `0x91` branch and Read Memory
`0xf0 00 00` CRC checks are [IOC3 diagnostic expectations](ioc3.md#the-ethernet-address-nics-read-protocol).
They do not establish an IO8 part at widget 15. On diagnostic failure it
returns status `0x7f`, without consulting L1 or `OSLoadOptions`.

Production `ef` obtains its station address from the IO8 board-information
area via L1 request `0x0006`, component 1, area 2. The original generated
record misplaced the MAC field and BASEIO parsed adjacent non-hex bytes as
`e1:8b:00:3a:d0:d0`. Replacing that input with a published-layout record
carrying the same configured address reached `sash:` with no environment
override on QEMU `816f74ec2abbe864605d1072a7f4c33d9ee4010a`. The
[L1 datasheet](l1-controller.md#origin-300-io8-ethernet-identity) owns the BIA
field layout and the limits of those emulator runs. No additional station-
address packet at XBridge widget 15 is needed for this ef path.

QEMU's DS2505-shaped widget-15 responder remains an explicitly synthetic
board-identity placeholder, not a photographed or read-out IP45 IO8 part.
The rev-A XBridge specification says the bridge-half NIC `0xb0` does not
operate a one-wire pin in XBridge mode and assigns that pin to widget 0
`0x70`. The two public Origin 300 boot captures report XBridge revision 2,
whose pin routing is not settled by the rev-A document. The corrected stock
PROM disassembly and run-scoped register watches no longer support the old
claim that `0xc00000001fc43370` reads `0xb0`. An identified revision-2
board drawing or register/ROM capture is needed before assigning a real
family, page, or route to widget 15. Keep widget 14's absent-board NIC
unresponsive; do not add a second responder there or at widget 15 solely
to satisfy netboot.

**What an IO8 record would hold, and why the placeholder keeps its text
layout.** The Bridge datasheet's
[record layout](bridge.md#the-record-layout-and-every-constraint-on-building-it)
gives the fixed offsets IRIX reads on Bridge-based boards. Owners' inventory
and L1 `serial all` output name this board `IO8`, part `030-1673-003`, at
revisions E, F and G (the revision evidence (citation: `docs/research/origin300-io8-revision-evidence.md`)
cites each capture). If a real IO8 identity part is ever placed on an
established route, its record in that layout, with a placeholder barcode and
revision F (the revision reported beside XBridge revision 2 in one capture),
would be:

```text
IO8
01 4f 49 4f 38 30 30 30 30 30 31 20 20 20 20 20 20 20 20 20 20 30 33 30 2d 31 36 37 33 2d fe d9
20 20 20 30 30 33 46 20 20 20 ff ff ff ff ff ff 49 4f 38 20 20 20 20 20 20 20 20 20 20 20 2c 18
```

The check values are computed as for the Bridge records, and each page leaves
the residue `0xb001`. The model does not store this image. Its widget-15
responder keeps the older text layout, now carrying the io8 preset's
`030-1673-003` rather than `NOT-OBSERVED` (the qtest's own head still uses
`NOT-OBSERVED`). No guest is known to read the responder: the stock-PROM
watches above saw no access, and the Origin 300 PROM builds its board
identity records from the L1's EEPROM reads
([L1 datasheet](l1-controller.md#origin-300-io8-ethernet-identity)). Whether a real IO8
has an identity part on this route at all is the open question above. A
fixed-offset record there would present a placeholder as an identified board
part and would change no guest-visible behavior. It waits for the same
revision-2 evidence.

Origin 200's station-address part is on IOC3 MicroLAN, separate from its
Bridge identity connection. SGI's published Origin 2000 BaseIO diagram also
draws separate IOC3 and Bridge NICs but gives neither family code nor page
payload. Those products establish that identity roles can be separate, not
that Origin 300 has the same fitted parts or wiring. QEMU's one-wire qtest
can verify its placeholder exchange; it cannot certify physical IO8
population.

The Origin 200 PROM's Bridge byte address `0x19c` identifies the data half
of XBridge force-always interrupt 3 (Table 37, printed page 126). That register
is write-only and data-independent. With pin 3 enabled, a valid 32-bit write
sends a set packet and performs the buffer flushes (printed page 159).
The Origin 200 byte store does not establish that XBridge accepts that width.
Delivery while masked and the stock IP35 PROM's accesses remain unobserved.

**Two Crossbow datasheet values are contradicted by the XBridge specification** (the
interrupt destination's reset target, `0x0008_0000` here against 0 there, and the
performance counter select width, three bits at 22:20 here against four at 23:20
there). On the second, both documents agree the counter watches link 8 at reset and
differ only on encoding, so it may be a genuine change between chips; on the first
the values differ in substance. Both go to the Crossbow datasheet's owner with the
XBridge pages as the comparison.

**The specification contradicts itself on where multiple errors are reported.** The
register chapter marks interrupt status bit 31 reserved and gives a per-bit multiple
interrupt register at `0x178` (printed pages 148 and 157 to 159); the interrupt
chapter's prose still says a second error in a group sets a shared multi-error bit in
the interrupt status register (printed page 216). The specification's own reading rule
makes the register chapter win (printed page 30); the model follows the register
chapter.

**Withdrawn Bridge contracts.** GIO mode does not exist on this part (printed page 24),
so the PCI timeout register's GIO-timeout bit is reserved and the status register's
flash-select and PCI-not-GIO bits are input pins. The Bridge datasheet's requirement
that the external-SSRAM window complete accesses for the IRIX size probe is withdrawn:
the range is reserved (printed page 179), an undefined address is an invalid-address
error (printed page 123), and SN1 skips the probe on an XBridge.

**Carry-over from the Bridge datasheet.** Every Bridge clause an implementer acts on
carries over unless a delta table above overrides it; the review's record lists each
clause with its verdict and specification page. The clauses the review found this
document silent on, and the model's position on each: the translation RAM's read
view is served at `0x1_1000` (the choice above); the whole-face rule that a Bridge
answers at every non-zero widget is overridden by construction, because the crossbar
routes by destination port; the control register's widget-ID readback is reframed
under reset above; the board-identity contents behind the one-wire (family `0x0b`,
the two CRC blocks, the part-number string) are unobserved for the IP45 and Linux's
fixup table is keyed on Origin 200 and Origin 2000 boards; whether a device window
serves an eight-byte doubleword and whether a type 0 read returns Command and Status
unchanged have no XBridge evidence either way and inherit the Bridge datasheet's
statements as the model's assumption; the byte placement of narrow accesses is
confirmed by the XBridge's own swap tables (offset exclusive-or four minus width
with the swap bit clear, a plain byte stream with it set, Tables 109 to 111 on
printed pages 232 to 233 and 101 to 103 on 228 to 229), identical to the Bridge's,
and the same tables give the 64-bit device's narrow-access placement as offset
exclusive-or eight minus width, a clause neither datasheet had stated; the
patent re-examination mechanism the Bridge datasheet does not rely on has its
hardware form in the force registers here.

**A DMA fault report on IP35-class systems, undiagnosed.** In 2010 the port's author
attributed QLogic command timeouts during installation on an Origin 3000 and an Origin
350, both with PCI option cards, to an xbridge(4) DMA bug he could not find and had "only
seen ... on IP35 class systems" (the openbsd-sgi list thread of 2010-06-15 in
`docs/library/boot-logs.md`). No public diagnosis followed. A guest that stalls the same
way on the model is not evidence about the model until the same run works on hardware.

## Proposed two-chip XIO route for a V-brick model

**QEMU routing proposal, not a cleared Origin 300 wiring contract.** The
Origin 300 User's Guide (007-4365-002, printed page 8) identifies the rear
XIO connector. The Onyx 3000 graphics guide (007-4264-002, printed pages
52, 54–56) places a host-interface board between the V-brick's external
ports and its XBridge, places V12 boards behind that chip, and describes
its C-brick connections. Neither publication identifies an Origin 300 to
V-brick cable, the receiving XBridge port, nested widget numbers, or the
V12's reachable address. The node model's connector at port `0xb` is an
approximation. These facts support an isolated routing experiment, not a
physical V-brick configuration.

For that experiment, join the node XBridge's external port `0xb` to a
second XBridge's chosen host port. Treat the second chip as a switched peer
with its own crossbar face at the node-facing position. This is a proposed
QEMU address projection: a read of the node's widget-`0xb` identity window
should reach the second chip's widget-0 identification register. It does
not assign the V12 board a port or make the second chip's bridge heads
reachable. Keep those addresses and any reset, DMA and interrupt forwarding
outside this proposal until a matching route is established and reviewed.

Before the routing experiment, QEMU at
`b368e8ed92de8aefbbd8f780ed88ca3fd3110962` could not complete that
link. `sgi_xio_link_connect()` skips an end whose peer
offers neither a processor nor packet face (`hw/mips/sgi/xio-link.c`, lines
66–95). The XBridge exposes views for widget 0, heads E/F and its external
port, but none for its host port (`hw/pci-host/sgi-xbridge.c`, lines
1185–1197 and 1618–1638). Its host-port connector consumes the peer's
packet face before realization (`sgi-xbridge.c`, lines 1667–1701). Those
interfaces leave the node's external port without a receiving face from
the second chip; the earlier two-chip probe below observed the incomplete
link. The existing single-chip qtest covers a leaf widget on an external
port (`tests/qtest/sn1-xbridge-chip-test.c`, lines 439–459), not a second
XBridge.

A register-contract qtest constructs two chips and the link without a V12
board or guest firmware. It requires a complete link, present/alive status
on the node's port `0xb`, and the second chip's crossbar identity through
that port. The two chips have the same default identity, so the test also
writes the remote chip's arbitration reload register to `0x1f`. It reads
`0x1f` through the node's port while the node chip retains its reset value
`2`. With no second chip, the node's port reads empty and reports absent.
The independent QEMU code review (citation: `docs/reviews/2026-09-29-xbridge-two-chip-qemu-review.md`)
records why this distinct state is needed.

QEMU `018b14440324bcc67e2fbb5702462c1365cf6aad` passes that focused
two-chip qtest. Its host-port processor face exposes only the second chip's
widget-0 view, a `1 << 44` byte window. A board on the second chip's
external port occupies a different window at `port << 44`. The packet face
routes destination-tagged traffic, but it gives a processor no nested
address selection. This QEMU route alone cannot provide processor PIO to a
V12 behind the second chip. The V-brick experiment (citation: `docs/datasheets/vbrick.md#experimental-origin-300-attachment`)
adds a machine-specific 8 MiB projection inside the node's widget-`0xb`
aperture. Its lower half maps to the V12 and its upper half exposes the remote
XBridge. This is an approximation, not an XBridge hardware rule. A qtest
must distinguish the remote XBridge from the V12 through those windows; a
direct attachment must fail that distinction. No published source establishes
the nested address, return DMA, interrupts, reset behavior or physical cable.
This model limit does not establish whether the physical cable works.

Independent evidence review must clear the address projection before this
becomes a device requirement. A published matching wiring diagram or a
captured widget scan with V12 register access through an Origin 300 rear
connector would be needed to claim physical Origin 300 compatibility.

## Open questions

- Can an Origin 300's XBridge cable port reach a V-brick's own XBridge and
  its V12 pipe boards? The Origin 300 manual (007-4365-002, printed page 8)
  names the connector, and the Onyx 3000 graphics guide (007-4264-002,
  printed pages 52 and 54) shows the V-brick's internal XBridge. The same
  guide labels its rear connectors XIO 10 and XIO 11 and describes one
  connection to a C-brick. A second C-brick can connect to a one-pipe V-brick
  if that C-brick is not already supporting a second V12 board (printed
  pages 55–56). It does not establish an Origin 300 cable route, V12
  reachability through both XBridges, or widget numbering. The Origin 300
  XBridge's external cable port assignment to `0xb` remains approximated.
  In a QEMU-only test based on maintained revision `89dc3934ede6`, two
  XBridge objects realize, but a link from the first chip's port `0xb` to
  the second chip's host port remains incomplete. The second chip's host
  port exposes neither face the first chip needs. The probe uses a dummy
  host packet target and no V12 board; it tests QEMU's link interface, not
  graphics reachability or the hardware cable. A published wiring diagram,
  matching hardware graph, or captured widget scan and V12 access through
  this exact cable route would settle the hardware question.
- Which host port (A or B) a boot I-brick uses on an Origin 3000, and what
  XBridge revision line it reports. No posted capture or manual states which
  port a boot I-brick occupies (108-0240-002 names both ports without a
  boot-path rule); a reference composition wires the I-brick to host port A
  as a documented default, not a captured fact. Settled by a verbose Origin
  3000 PROM boot or a published I-brick installation procedure naming the
  boot host port and XBridge revision.

## qtest checklist

- Widget 0 identification reads part `0xd000`, manufacturer 0, revision at least 1,
  bit 0 set; widgets 0xe and 0xf read `0xd002`, manufacturer `0x024`.
  The integrated topology has present links A (Bedrock), E and F. Other external
  ports report absent in link aux status bit 5. Widget 0 is the crossbar
  register destination, not an additional physical link.
- Widget 0 status bits 9:6 return the port number the reading widget entered on.
- Widget 0 control resets to `0x80`; the reset fences reset to all ones; the lock
  accepts one non-zero write and refuses the next until cleared through `0xc8`.
- Bridge control resets with PCI resets `0xf`, credit 3, big-endian set and widget
  `0xf`; writes to the widget-ID field change readback and the source identity of
  originated packets. Destination routing to bridge halves remains fixed at
  widgets `0xe` and `0xf` in XBridge mode.
- Bridge status bit 5 reads 1.
- Direct map resets with bit 17 set; a write of widget and offset clears it; bit 18
  is not retained.
- Device register n resets to error-lock, page-cross and memory-not-I/O set; bits 24
  and 20 are not retained.
- A configuration read of an empty device sets the master-timeout bit in the
  error-interrupt view at `0x170`; a write of bit 6 to `0x110` clears it.
- Interrupt status bit n follows the inverse of pin n while masked; a write to `0x100`
  changes nothing. With pin n enabled, a write to force-with-pin n sends a set packet
  only while pin n is asserted, and force-always n sends one regardless of physical
  pin state. With pin 3 enabled, a 32-bit zero store to `0x19c` sends its set packet.
  A set packet carries the
  number in 7:0 with bit 8 set, and a clear packet is sent on the rising edge only when
  enable and mode are both set; with mode 0 no clear packet is sent.
- An all-ones write to the interrupt enable register reads back `0x7ffe_feff`.
- The rev-A physical specification predicts that widget 0 offset `0x70`
  operates the one-wire connection and bridge-half NIC offset `0xb0` does
  not operate its pin. This is a **hardware-evidence discriminator**, not a
  QEMU acceptance requirement. The model's widget-15 placeholder exchange
  is tested but not established as an IP45 firmware requirement or physical
  route. A register watch at both offsets saw no access through the stock
  BaseIO identity selector on one QEMU run. Matching-revision IO8 evidence
  and independent review are required before replacing this physical unknown
  with a hardware claim.
- The multiple-interrupt register at `0x178` is read-only and resets to zero.
  Its occurrence detector and service boundary remain unresolved; reset/read-only
  checks do not qualify recurrence detection.
- A 32-bit read at register offset plus 4 returns the register; a 64-bit access to a
  register raises status and view bit 24. With that error enabled, the first
  access captures its address and sends the programmed vector to the programmed
  destination. A second access does not replace or redeliver it. Reset-group bit
  3 clears and rearms the condition.
- On each bridge half, drive unequal PCI legacy-interrupt pins on at least two
  slots and verify the distinct raw outputs numbered `slot * 4 + pin`, with A as
  pin zero through D as pin three. Migration preserves asserted levels and reset
  clears them. This test assigns no physical XBridge pin.
- A translation entry written as a 64-bit double word reads back as two 32-bit words
  through the modeled read view at `0x1_1000`; entry bit 5 is retained.
  Bits 63:48 and 7:6 must not affect translation. Their storage and readback
  remain unresolved. A write between `0x1_1000` and `0x1_1fff` changes nothing
  under the retained read-view compatibility policy.
- In both page modes, translate payload bytes through the first and last active
  entries, reconstruct the complete destination, accept target zero, and select
  swapping with ATE bit 5 alone. An invalid entry and the first out-of-table
  candidate within the documented aperture set status bit 30 without touching a
  destination. Reset-group bit 1 clears and rearms the condition. Addresses above
  `0x407fffff` and map-fault-address contents stay outside the test contract.
- A 32-bit DMA to `0x3fff_0000` completes as a flush, and one to `0x0` to
  `0x3ffe_ffff` gets no response; a 64-bit DMA with target 0, or to any address from
  `0x1_0000_0000` to `0x0fff_ffff_ffff_ffff`, gets no response.
- Above the face, a 32-bit read at `0x0000_4000_0000` plus a function's memory
  base-address offset, one at `0x0000_8000_0000` plus the same offset, and one at
  `0x0001_0000_0000` plus its I/O base-address offset each return that function's
  register through the named PCI space with no bit-24 error; only crosstalk bits
  29:0 (memory) or 31:0 (I/O) reach PCI. With control bits 22 and 23 clear, a byte
  store through the 32-bit alias or the I/O range lands at offset exclusive-or three
  whatever the device register's swap bit says; setting bit 22 moves the placement
  in the memory ranges only, bit 23 in the I/O range only. A byte store through the
  64-bit alias landing at offset exclusive-or seven follows the inferred rule that
  the alias selects the width, so a test asserts it as the model's reading, not as
  hardware.
- A request at `0x100_0000`, `0xc000_0000`, `0x2_0000_0000` or an address with bit
  47 set raises status and view bit 24, captures that address in the error-address
  registers, and reaches no PCI space. The step from reserved to invalid address is
  inferred, so a test asserts this as the model's reading, not as hardware.
- A 32-bit access to `0x3_0000` raises no bit-24 error; one to `0x3_0008` raises it
  like any undefined address, an inferred expectation a test asserts as the model's
  reading, not as hardware. What the acknowledge cycle returns without a responder
  stays outside the test contract.
- With widget 0's fence admitting port A and link E's fence rejecting it, a warm
  reset arriving from port A resets the crossbar core and does not reach link E;
  with link F's fence admitting it, the same reset reaches link F. The reverse
  combination, widget 0 rejecting and a link admitting, is a labeled model
  behavior, not a hardware assertion.
- A 32-bit read of a link reset register sets widget 0 status bit 5. That it also
  logs the request in the crossbar error registers and, with control bit 5 set, sends
  the crossbar's error interrupt is inferred from the shared bit, so a test asserts
  those two as the model's reading, not as hardware; the data it returns is not
  asserted.
- An invalid access at a widget address wider than 24 bits captures the full 48-bit
  value, bits 47:32 in the upper register and 31:0 in the lower, with no widget
  number in either; the error command word's bits 31:28 hold the head's own port
  number.
- After reset the eight device registers read `0x1800_1000`, `0x1800_1002` and
  `0x1800_1004` to `0x1800_1009` in device order: firmware-sourced, asserted because
  the stock PROM's sanity diagnostic asserts it, not from the specification; the
  device order behind the seven reported values is inferred, so the diagnostic
  passing on the model is the observation that settles it.
- A 32-bit access to a slot below `0x300` that the register map does not define
  (`0x78`, `0x88`, `0x98`, `0xa8`, `0xb8`, `0xe0` to `0xf8`, `0x2a0` to `0x2f8`) is an
  invalid-address error like any undefined address: bit 24, the address captured,
  the error packet if enabled. The Bridge datasheet records the Origin 200 PROM's
  provoke as a 32-bit store to `0x7c`; the IP35 diagnostic's own slot is unobserved,
  its console showing only the bit 24 and pending bit it expects. With the
  destination registers naming the Bedrock's interrupt-pending modify register and
  the host-error field naming a bit, that bit sets in the processor interface's
  pending register.
