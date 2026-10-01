# Origin 2000 CPU reset-mode verification

References rendered as citation identities name material outside this document
set. Those targets are not included or cleared for redistribution. Review and
runtime statements retain their recorded dates and scope.

## Status and scope

Independently source-reviewed on 2026-09-09. The
publication record and review disposition (private record `jamesb/origami#917`)
bind the cleared source revision and content hashes. This supplies an original,
clean-room account of the stock firmware's early mode-word check. It proposes no
QEMU change and does not qualify an Origin 2000 boot.

The selected emulated processor composition is an R10000 at 180 MHz with a
90 MHz Hub and 1 MiB secondary cache. The corresponding mode word is
`0x1211a785`.

The check compares a 32-bit word in node-local boot flash with CP0 Config on
each processor. Failure reports LED value `0xb4`. The check occurs before
normal I2C initialization, module discovery, and console setup. Its data
source is the boot flash, not the system controller or timekeeper NVRAM. [S1, S2]

## Established hardware and firmware facts

The R10000 loads its reset mode from the system data bus during power-on or
cold reset. The mode's low 25 bits become CP0 Config bits 24:0. A soft reset
preserves the mode bits. The primary-cache size fields are hardwired. These
are processor facts from the published manual, independent of firmware or
QEMU. [P1, printed pages 160–165 and 256]

The firmware check is established separately in two releases:

- IRIX 6.5.7's retained firmware startup source, revision 1.115. That tree
  builds PROM 6.49, so it is not source for PROM 6.11. [S1, S3]
- The retained PROM 6.11 image, built 29 April 1998, SHA-256
  `14e0d0a77b92154e3b33573f30608f241ded249db6060fba9f97bfd00b2145b2`.
  Bounded static instruction decoding confirms the same accesses and
  comparisons. It does not establish which branch a particular run took. [S2]

The [boot-flash address contract](address-map.md#the-firmware-configuration-block)
places the processor mode word at flash offset `0x64`. The local uncached
address used by this check is `0x9000000010000064`, corresponding to
node-local physical address `0x10000064`. The reset-window alias is
`0x1fc00064`. The processor's current node selects the local flash. [C1, S2]

### Accesses and order

This is the cold-start path. NMI and soft-reset entry paths are outside this
account. Hub addresses in this table are node-local physical offsets. Their
uncached register mapping uses prefix `0x9200000000000000`. The flash read
uses prefix `0x9000000000000000`. [C1, S2]

| Order | Operation | Width and address | Meaning |
|---|---|---|---|
| 1 | Read processor slice | 64-bit Hub read at `0x01000020` | Select this processor's progress register. |
| 2 | Write progress value `0x5f` | 64-bit Hub write at `0x01000108 + 8 × slice` | Record entry to the check. These locations are the two real-time compare registers, temporarily used by firmware for progress. |
| 3 | Read flash mode word | 32-bit big-endian read at `0x10000064` | Obtain the node's expected processor mode. |
| 4 | Write CP0 Config | 32-bit coprocessor transfer, register 16, selector 0 | Attempt to apply the word, including the writable cache-algorithm field. This is not a Hub MMIO write. |
| 5 | Read CP0 Config | 32-bit coprocessor transfer, register 16, selector 0 | Compare bits 15:0 with the flash word. A difference enters the failure path immediately. |
| 6 | Read the same flash word again | 32-bit big-endian read at `0x10000064` | Re-obtain the expected bits for the second comparison. |
| 7 | Read CP0 Config again | 32-bit coprocessor transfer, register 16, selector 0 | Compare bits 24:16. Equality continues to normal I2C initialization. A difference enters the failure path. |

Both comparisons must succeed. Equivalently, the returned Config value and
the flash mode word must agree under mask `0x01ffffff`. Bits 31:25 do not
participate. The complete mode word is not copied into an ordinary writable
32-bit register: reset configuration and hardwired processor fields retain
their hardware meanings. This candidate does not redefine the CPU's Config
write mask. [P1, S1, S2]

The failure path re-reads the 64-bit processor slice and writes `0xb4` to its
64-bit progress location. It then reads the 32-bit machine-type field at
physical `0x10000090` to select the LED wiring, reads the slice again, and
writes `0xb4` with a 64-bit store. For machine type zero, the LED destination
is `0x01220050 + 8 × slice`. For nonzero machine type, this binary selects
`0x01220080 + 32 × slice`. These are the binary's observed address choices,
not a new general contract for every nonzero machine type. It then calls
the firmware's processor-stop path. [S2]

The hardware LED field is eight bits wide. The firmware's 64-bit store is
the CPU transaction width through the Hub adapter. Those are distinct
properties. The existing [Hub indicator contract](hub-memory-directory.md#identity-indicators-and-the-slot-identityled-registers)
owns the field's behavior. [C2, S2]

On successful comparison, the first normal I2C operation is a 64-bit write
to physical `0x01220008`. In this cold-start path it is downstream of both
comparisons. Changing I2C, module arbitration, or NVRAM cannot make unequal
comparison operands equal. This conclusion is limited to the mode-word
check, not later firmware progress. [S1, S2]

### Cache-size interpretation

CP0 Config bits 18:16 encode the secondary-cache size. The published manual
assigns value one to 1 MB and value three to 4 MB. [P1, printed page 165,
visually checked against PDF page 195]

SGI's hardware booklet identifies the corresponding IP27 node-board
assemblies as a 1 MB HIMM with a 180 MHz processor and a 4 MB HIMM with a
195 MHz processor. The midplane instructions distinguish those two cache
sizes even where the assemblies share the same oscillator-jumper position.
This is direct public evidence for the 180 MHz and 1 MB hardware pairing. It
does not document a 195 MHz and 1 MB Origin 2000 assembly. [P2, printed page
93]

The retained stock PROM 6.156 configuration record contains mode word
`0x1211a785`, whose secondary-cache-size field is one. The same record names
a 180 MHz processor, a 90 MHz Hub, and machine type zero. The older retained
PROM 6.11 record has the same selected values. These are measurements of the
stock images, not evidence that every Origin 2000 had this configuration.
[R1]

The first emulated composition is therefore an R10000 at 180 MHz, a Hub at
90 MHz, and 1 MB of secondary cache, with the stock PROM 6.156 mode word
left unchanged at `0x1211a785`. The processor and cache pairing comes from
the published hardware booklet. The Hub clock and exact mode word come from
the stock image's configuration metadata. Their agreement selects a real
documented board composition that matches the retained image. Neither source
alone establishes the whole composition.

A 195 MHz processor with 1 MB of secondary cache is not selected because the
published Origin 2000 hardware booklet instead pairs 195 MHz with 4 MB. The
4 MB size encoding would change bit 17 and produce `0x1213a785`, which does
not match the unchanged stock image. Selecting a 195 MHz and 4 MB composition
would require an explicit, separately reviewed firmware-configuration change.

An earlier runtime capture recorded flash word `0x1211a785` and CP0 Config
`0x6c13a785`. Their low 16 bits match. Under mask `0x01ffffff`, their XOR is
`0x00020000`: the size fields are one and three respectively. Therefore
those operands necessarily fail the second comparison in this image.
The result identifies disagreement between the CPU's reset configuration
and its local flash configuration. It does not establish which side should
be changed for the selected board. [O1, P1, S2]

## Smallest discriminating check

Use the supported Origami guest/debug interface with the exact QEMU binary
and unchanged payload. Record both hashes and the full machine options.
For each CPU-global ID, record its node, slice, local flash word, CP0
Config before the attempted write, and both comparison operands after it.
Only numeric register data and outcomes need enter the handoff.

For the exact PROM 6.11 hash in this document, useful instruction addresses
are `0xffffffffbfc00ca8` for the low-16-bit comparison and
`0xffffffffbfc00ce0` for the nine-bit comparison. At either address, `k0`
and `k1` contain the masked operands. A breakpoint at
`0xffffffffbfc00ce8` identifies failure entry. Successful equality at the
second comparison reaches `0xffffffffbfc00da8`. That last address is also
the return address of the failure-path call, so a stopped PC there alone
does not prove success. These addresses are valid only for the cited image.
[S2]

The discriminators are:

- Different words through local and reset-window flash views identify a
  mapping or flash-content problem.
- Equal flash views and unequal Config bits identify a processor/board
  configuration disagreement. The XOR and the public manual name the field.
- Equal operands followed by the failure entry identify a CPU execution or
  observation problem, rather than a mode-word mismatch.
- Success on some CPUs requires recording node and slice before drawing
  conclusions about per-node reset state or local addressing.

A focused mapping qtest must install a distinct test-owned 32-bit fixture
word at flash offset `0x64` on every node. Through every CPU-global ID,
read both the local-special view at `0x10000064` and the reset-window view
at `0x1fc00064`. Each must return the fixture word assigned to that CPU's
own node. Repeat every CPU/view read after system reset. Distinct words
are required: identical contents would hide an incorrect node selection.

These fixture words test address selection only. They are not processor
mode expectations or replacements for the stock payload. A separate CPU
ROM test can execute the Config write/read check and report the differing
bits through test-owned RAM. Its expectations must come from the selected
board's reviewed reset configuration, not the current implementation or
the mapping fixtures. The stock-PROM capture uses the unchanged, hashed
payload. Do not weaken Config semantics or alter the firmware to manufacture
equality.

## Remaining questions

- Capture the operands on the exact current binary and mounted payload.
- After equality is restored through the correct configuration, measure
  subsequent firmware progress independently. This check says nothing
  about Router discovery, the maintenance menu, or IRIX acceptance.

## Sources and clean-room boundary

This datasheet contains independently worded behavior derived from restricted
source and bounded firmware decoding. It contains no vendor code, comments,
instruction listing, private symbol names, or firmware bytes. A clean
implementer may use it only after independent source review. The source
researcher must not implement or supply a clean implementation review for
the CPU reset, Hub boot/progress, flash-configuration, or I2C paths read.

The complete exposure inventory is recorded in
the provenance entry (citation: `docs/provenance.md#origin-2000-cpu-reset-mode-investigation`).

- **P1:** *MIPS R10000 Microprocessor User's Manual*, version 2.0,
  10 October 1996. Retained Library path
  `origami-docs:library/cache/mips/r10000-users-manual-v2.0.pdf`, SHA-256
  `6023b060b52e136cc3bda8a4ed348b3eb9a99d7947f2390eedf775d431213006`.
  Printed pages 160–165 and 256 are PDF pages 190–195 and 286.
- **P2:** *Hardware Quick-reference Booklet (Origin and Onyx2 Series)*,
  HMQ-380-C, retained Library path
  `origami-docs:library/cache/sgi-hw/origin-onyx2-hardware-quick-reference-booklet.pdf`,
  SHA-256
  `75f82fa4123bec59e81b2e4f1330ee03e2a714e7d5b6e9216c919648a704fb04`.
  Printed page 93 identifies the 1 MB/180 MHz and 4 MB/195 MHz IP27
  node-board assemblies.
- **S1, restricted:** IRIX 6.5.7,
  `stand/arcs/IP27prom/entry.s`, revision 1.115, lines 1–320,
  SHA-256 `9bdc40d3519c26ce6ae5c2dc6234ec2b66f48ded65af92037785426180be9925`.
  Supporting reads: `stand/arcs/IP27prom/ip27prom.h`, lines 270–301,
  SHA-256 `96ea5a986f1324add7e8237faa51774e006c15e3a9465e3d2d77169e660dd686`,
  and `irix/kern/sys/SN/SN0/ip27config.h`, lines 1–115,
  SHA-256 `a679beefa2aba4da5fa1dbc1cf09e4e58c7c4d487c9e53286e13fabe68959cb8`.
  The latter two files have no revision marker in the examined text.
- **S2, restricted:** `assets/firmware/6.5-base/ip27prom.payload.bin`,
  PROM 6.11, SHA-256 stated with the release identity. Static decoding was
  bounded to image offsets `0xc00`–`0xe3f`, with direct configuration-data
  reads and branch-target arithmetic. No guest run was launched by this
  investigation.
- **S3, restricted:** IRIX 6.5.7, `stand/arcs/include/release.mk`,
  lines 7–8, SHA-256
  `d90312682dffc69c55bdf766855e6d927636d07f4404160323aac82862d75de5`.
  This additional bounded read establishes firmware version 6, revision 49.
  The retained file was read after the Library source-root failure.
- **C1:** [Firmware configuration block](address-map.md#the-firmware-configuration-block)
  and [firmware structures](firmware-structures.md), at Origami
  `2aa0ce931c8a61d2aeb85450a0000d69f673c3bb`.
- **C2:** [Hub memory/directory contract](hub-memory-directory.md), at the
  same Origami revision, indicator registers and firmware-progress codes.
- **O1:** Existing Origin 2000 runtime mode-word measurement in
  `docs/notebooks/origin2000-8p.md` at Origami `d8a096b232`,
  run `prom-20260906T003553-4d1d6a`. Its measured values are restated here
  so implementers need not read a research notebook.
- **R1:** Origin 2000 stock firmware configuration evidence (citation: `docs/research/origin2000-firmware-configuration.md`),
  including the independently reviewed measurements of the retained PROM
  6.11 and 6.156 configuration records and image hashes.
Library retrieval preceded local reference reads. Artifact search for
`mode bits` timed out. Text search found the IRIX 6.5.7 startup file, but
both exact-path resolution and bounded reading reported its root unavailable.
Exact resolution of the retained manual returned missing. The retained copies
in the stable main checkout were then read. No external source was fetched.
Broad text-search snippets also exposed IRIX 6.5.5/6.5.7 ACL material and
firmware diagnostic/header references. Those unrelated snippets supply no
claims in this datasheet. No absence claim is made about other releases.
