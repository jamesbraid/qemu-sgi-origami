# Origin 2000 firmware configuration independent review

References rendered as citation identities name material outside this document
set. Those targets are not included or cleared for redistribution. Review and
runtime statements retain their recorded dates and scope.

Reviewed source-data candidate `7a0508cb6f18b5d8441b8e0dca30f8e386d503fd`
and its public node-flash addition `3d668a9446ce35ec5c95101296196d4f78662b38`
on 2026-09-05. Scope: `docs/research/origin2000-firmware-configuration.md`.
The reviewer remains source-exposed and will not implement CPU reset wiring,
firmware configuration, flash, Router, routed fabric, or discovery paths.

## Clearance

Clear these values as the selected Origin 2000 configuration metadata:

| Property | Reviewed value |
| --- | --- |
| Processor clock | 195,000,000 Hz |
| Hub clock | 97,500,000 Hz |
| Secondary cache | 4 MiB per CPU |
| Processor configuration mode word | `0x1213a785` |
| Flash read controller field | 8 |
| Flash write controller field | 1 |
| Firmware machine type | 0 |

This is a release-bound configuration-data contract that a clean descriptor
owner may use. It is not a measurement of physical strap signals, a claim that
both CPUs have identical device-number inputs, or proof that an existing stock
image contains these values. The full mode word is not the whole CP0 Config
register. The public CPU manual says only its low 25 bits transfer to Config's
low 25 bits during reset. Other Config fields and per-CPU wiring need their own
existing contracts.

Clear one Hub-attached 1 MiB node flash, with sixteen 64 KiB sectors and the
published allocation of fourteen firmware sectors plus two environment/log
sectors. The published abbreviated part designation and the existing reviewed
Hub flash command contract support the AMD Am29F080 class. They do not establish
an ordering suffix, package, speed grade, or BaseIO flash selection. The public
local and remote special-space windows are 256 MiB apertures, not flash capacity.
The existing reset-view decode caveat remains unchanged.

## Independent reconciliation

Canonical Library reads of IRIX 6.5.7
`irix/kern/sys/SN/SN0/ip27config.h` covered lines 1-25, 150-249, 330-357,
and 431-459. No revision identifier occurred in the inspected ranges. The
configuration definitions and the selected 1 MiB/4 MiB entries were independently
read, and the mode arithmetic recomputed. This review reproduces no protected
identifiers, comments, structure layout, or code. Exposure includes the adjacent
configuration declarations visible in those bounded reads.

The 4 MiB word recomputes to `0x1213a785`, with low 25 bits `0x0013a785`.
The 1 MiB entry gives `0x1211a785`. Public R10000 User's Manual version 2.0,
Table 8-1, printed 164-166 / PDF 194-196, independently explains the fields.
Original pages 195 and 196 were rendered and inspected. The selected word uses
big endian memory, a system-clock divisor of two, a secondary-cache divisor of
one and one-half, 4 MiB cache size, and clock-tap code nine. The public table
supports interpreting the encoding, not selecting it for a board by itself.

SGI 108-0164-002 printed E-4 / PDF 108 supplies the matching published
195/97.5 MHz, 4 MiB, machine-type-zero configuration. The source configuration
entry supplies the encoded settings that the public procedure does not print.
Flash values eight and one are controller fields. No nanosecond conversion or
claim about physical access time is approved.

All four named local image/payload files were independently hashed and only the
selected configuration fields read using offsets from the existing reviewed
firmware contract. Their sizes, SHA-256 hashes, versions, clocks, mode, and flash
fields match the candidate table. Both releases contain 180/90 MHz, mode
`0x1211a785`, RTC 1,250,000 Hz, flash eight/one, and machine type zero.
The author's wider directory-count and failed Library-search observations were
not repeated. They remain explicitly bounded author observations.

SGI 108-0170-001 PDF 27 was read and its original page inspected for node-flash
geometry. PDF 88 was read for the difference between ordinary reconfiguration
and direct node-to-node copying, including matching clock/cache requirements.
Public Linux boot-window definitions were read at the cited ranges. The timing
field citation needs the small correction to `hubmd.h` lines 124-127 rather
than 127-134. The read field occupies bits 53:49 and the write field bits 48:44.
The author has been notified. This citation correction does not change values.

## Image and acceptance boundary

This review approves configuration facts. It does not authorize modifying a stock
image, changing firmware instructions, bypassing firmware checks, or synthesizing
discovery records. Selecting a hardware descriptor cannot silently reconcile a
mismatching image configuration. Any separately authorized configuration process
must preserve its original asset and record input/output hashes, exact changed
configuration fields, firmware version, and how CPU reset inputs agree with the
programmed image. The supported ordinary vendor configuration procedure remains
distinct from an unreviewed loader patch.

No firmware boot, flash operation, physical reset, or hardware probe ran. The
metadata checks and arithmetic are successful evidence checks, not first-machine
or diagnostic acceptance. No new timing, fault, BIST, or Router clearance follows.
