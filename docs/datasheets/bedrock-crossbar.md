# Datasheet: Bedrock internal crossbar (XB)

References rendered as citation identities name material outside this document
set. Those targets are not included or cleared for redistribution. Review and
runtime statements retain their recorded dates and scope.

**Status:** reviewed 2026-09-03 by an independent second reader
(`docs/reviews/bedrock-io-md-xb-datasheet-review.md`): all 36 registers cleared position for position
against the SN1 header, with the review's three findings applied in this revision.
The contract correction review (citation: `docs/reviews/bedrock-crossbar-contract-correction-review.md`)
narrows that clearance to the documented register fields and PI0 output
software-injection path. Structureless-register masks, clear mechanisms and
traffic behavior remain unresolved.
The
initial device-contract decision (citation: `docs/decisions/D-0022-origin300-initial-bedrock-and-xbridge-contracts.md`)
approves the deterministic compatibility policy stated here for implementation.
Nothing here is implemented or tested.
**Fork file(s):** proposed `hw/misc/sgi-bedrock-xb.c` and
`tests/qtest/sn1-bedrock-xb-test.c`, composed by the Bedrock parent.
**Evidence cutoff:** 2026-09-03, Origami trunk `506836b9dd`.

**Sources used:** the SGI-generated Linux SN1 register headers at tag `v2.5.40`
(commit `7570df54ef8c`): `include/asm-ia64/sn/sn1/hubxb.h` (36 registers),
`hubxb_next.h`, `sn1/intr.h`; the Itanium consumers `arch/ia64/sn/io/sn1/huberror.c`
and `arch/ia64/sn/kernel/sn1/error.c`. OpenBSD at `2df91d80c1`: `sys/arch/sgi/xbow/hub.h`
(the base constant only). The out-of-tree MIPS port carries no crossbar header and
touches no crossbar register. The Origin 3000 System Architecture 108-0240-002
printed page 2-11 for the unit's role. Reading preserved in the SN1 research notebook.

**Facts that are IRIX-derived: none.** **Reset values the header states:** none in
the register header. The companion header carries two software constants for the
parameters register: a reset default of `0x0008_0000_8000_0021` and an "active"
default of `0x0008_0000_ffff_f021` (`hubxb_next.h` lines 29 to 30). No consumer uses
either. Everything else is `unknown`.

This is the crossbar inside the Bedrock. The crossbar that sits at crosstalk widget
0 outside it is the XBridge's crossbar half, in `docs/datasheets/xbridge.md`.

## Role

Per the Origin 3000 architecture book the crossbar is an 8-input, 6-output switch
connecting the Bedrock's units to one another, with arbitration and some error
handling. The inspected Itanium consumer writes 0 to all fifteen error-clear
registers at probe (`huberror.c` lines 95 to 100). The inspected OpenBSD consumer
defines the base and the out-of-tree port has no matching definitions. This
consumer coverage does not establish what the stock PROM or SGI diagnostics
access. Register storage and software error injection are initial implementation
work; traffic-generated errors, arbitration and diagnostic behavior remain
required qualification work.

## Register map

Offsets are hub-relative; the unit starts at `0x700000`. Every register is 64 bits.
Line numbers are `hubxb.h` unless named.

| Offset | Name | Fields | Reset | Access | Consumer | Behavior |
|---|---|---|---|---|---|---|
| `0x700000` | parameters | 0 bypass grants enabled; 11:4 age wrap; 31:12 deadlock timeout wrap; 51:32 tail timeout wrap | the tree's reset constant decodes to bypass 1, age wrap 2, deadlock wrap `0x80000`, tail wrap `0x80000`; the "active" constant raises the deadlock wrap to `0xfffff` | R/W | none | Parameters controlling the crossbar's operation (lines 279 to 280). Whether the PROM writes the active value is open. |
| `0x700008` | slow grant | 4 bits per unit | unknown | R/W | none | Per-unit wavefront-grant period: `0xf` no grants, `0xe` slowest; backpressure only, does not affect bypass grants (lines 322 to 328). |
| `0x700010` | spew control | debug | unknown | R/W | none | |
| `0x700018` | I/O queue arbitration trigger | trigger level | unknown | R/W | none | |
| `0x700020` | PI0 output queue errors | 1:0 invalid crossbar select; 5:4 overflow; 9:8 underflow; 13:12 tail timeout (lines 496 to 509) | unknown | R/W; software-test writes cause an interrupt (lines 469 to 470) | none | Per-queue error record. |
| `0x700028` | PI0 input queue errors | 1:0 overflow; 5:4 deadlock timeout (lines 540 to 548) | unknown | software error writes; storage readback is model policy | none | |
| `0x700040` | MD-to-PI0 output queue errors | 5:4 overflow; 9:8 underflow; 13:12 tail timeout (lines 583 to 594) | unknown | software error writes; storage readback is model policy | none | |
| `0x700058` | MD input queue errors | 7:4 deadlock timeout (lines 622 to 629) | unknown | software error writes; storage readback is model policy | none | Further errors of this queue are logged in the memory unit's input error registers (line 114). |
| `0x700060` | NI output queue errors | three 4-bit fields at 7:4, 11:8 and 15:12 (lines 660 to 669) | unknown | software error writes; storage readback is model policy | none | |
| `0x700080` | LB output queue errors | 1:0 invalid crossbar select; 9:8 underflow; 13:12 tail timeout (lines 701 to 711) | unknown | software error writes; storage readback is model policy | none | |
| `0x700088` | LB input queue errors | 0 overflow; 4 deadlock timeout (lines 743 to 751) | unknown | software error writes; storage readback is model policy | none | |
| `0x700030`, `0x700038`, `0x700048`, `0x700050`, `0x700068`, `0x700070`, `0x700078` | PI1 output, PI1 input, MD-to-PI1, MD-to-LB/NI/II, NI input, II output, II input queue errors | **no structure in the header** (lines 1260 to 1273 list them without a union) | unknown | writable masks unresolved | none | The former 16-bit storage rule was inferred from siblings and is withdrawn as hardware behavior. Width, reserved bits and error semantics remain unresolved. |
| `0x700090` | first error | 3:0 type; 7:4 channel; 11:8 source (`POQ0=0xf`, `PIQ0=0xe`, `POQ1=0xd`, `PIQ1=0xc`, `MP0=0xb`, `MP1=0xa`, `MMQ=0x9`, `MIQ=0x8`, `NOQ=0x7`, `NIQ=0x6`, `IOQ=0x5`, `IIQ=0x4`, `LOQ=0x3`, `LIQ=0x2`; `hubxb_next.h` lines 13 to 26); 12 valid | unknown | read; not writable, but set when software writes any error register | none | Latched when valid is clear and an error occurs; **any valid bit interrupts both processor interfaces**; persists until the specific error register is cleared and then this one, in that order (lines 760 to 784). The public header establishes these source values but does not define type or channel values. |
| `0x700098`, `0x7000a0` | debug data control, debug arbitration control | debug | unknown | R/W | none | |
| `0x700120` + 8n, n 0 to 13 | error clear for each of the fourteen queue error registers, in address order of their base registers | as the base register | unknown | write (consumer); the mechanism is not stated | Itanium writes 0 to each at probe | "Clears" the base register (lines 163 to 258); each clear's narrative repeats its base register's. |
| `0x700190` | first error clear | as first error | unknown | write (consumer) | Itanium, last in the same loop | |

## Behavioral contract

### Reviewed control fields

The following field map fills details omitted from the summary rows. Root read
the public `linux-history` Library root at revision `7570df54ef8c`,
`include/asm-ia64/sn/sn1/hubxb.h`, lines 280 through 459 and 800 through 929.
These are field definitions, not measured reset values or proof of writable
storage and hardware side effects. The
independent control-field review (citation: `docs/reviews/bedrock-crossbar-control-fields-review.md`)
clears these field definitions with those limits.

| Register offset | Defined field mask | Field meaning |
|---|---|---|
| `0x700000` | `0x000f_ffff_ffff_fff1` | Bypass enable bit 0, age period bits 11:4, deadlock period bits 31:12, tail period bits 51:32. |
| `0x700008` | `0x0000_0000_ffff_ffff` | Eight four-bit grant-period fields. From low to high: LB, II, NI, MD's combined output, MD-to-PI1, MD-to-PI0, PI1, PI0. Bypass grants are unaffected. |
| `0x700010` | `0x0000_0000_0000_07ff` | Eleven traffic-observation selectors. At most one should be selected. A selected stream uses PI1 as a debug connection. This is distinct from the debug-data selection register. |
| `0x700018` | `0x0000_0000_0000_000f` | Number of clocks the I/O output queue waits before starting crossbar arbitration. |
| `0x700098` | `0x0000_0000_0000_ffff` | Bits 13:0 select one of fourteen queue observation points. Multiple selected points make debug output undefined. Bit 14 selects source-field observation and bit 15 selects low-data observation. |
| `0x7000a0` | `0x0000_0000_0000_0007` | Three-bit selection for queue and arbiter debug output. The referenced header does not supply the selected output formats. |

The masks describe fields present in the public definition. They do not prove
reserved bits read zero, every defined bit is writable, or that storage-only
readback implements grant scheduling, traffic observation or debug output.

### Approved initial behavior

For one chassis:

- Parameters reads the tree's reset constant `0x0008_0000_8000_0021` after reset
  and retains a write. The value is a software constant, not a header statement,
  and is the best public number; the qtest records it as such. Whether the PROM
  writes the active value is settled by a trace of the held PROM's writes to
  `0x700000`, or a read after hand-off.
- Zero-reset error storage is an explicit compatibility policy, not an observed
  hardware reset value. Cold reset and Bedrock local reset restore the parameters
  software-reset constant, clear every queue error and first error, clear the
  per-queue service state, and deassert both processor-interface outputs.
- MMIO writes are serialized in access order. A 64-bit write to a structured
  queue error register ORs its documented bits into the retained word. Reserved
  bits read zero. Every write containing a documented nonzero bit marks that
  queue unserviced, including a reinjection after its clear alias was used. A
  write containing no documented bit changes no error or service state.
  The seven structureless registers have no writable masks or readback behavior;
  reads return zero and writes are ignored with `LOG_UNIMP`. Their clear aliases
  accept 64-bit writes but have no state to change.
- First error starts with valid clear. The first nonzero structured error write
  latches that queue's documented source, type zero and channel zero, and raises
  level-1 bit 54 in both processor interfaces. Type and channel zero are model
  compatibility values, not measured hardware meanings. Later writes accumulate
  in their queue registers without replacing a valid first error.
- Any 64-bit write to a structured queue's matching clear alias clears that
  queue word and records that the alias serviced it. A first-error-clear write
  succeeds only after the clear alias for the recorded queue has been written.
  A premature first-error clear changes nothing and both processor inputs remain
  asserted. After an eligible first-error clear, the model scans the remaining
  structured queue words in ascending register-address order. It immediately
  latches the first nonzero queue with compatibility type and channel zero, or
  clears first error and deasserts both outputs if none remain. This priority is
  deterministic model behavior, not a hardware claim.
- The unit accepts exactly 64-bit accesses in both `valid` and `impl`. A bad-width
  access returns `MEMTX_DECODE_ERROR` without splitting or changing state. Its
  MMIO region is `0x198` bytes.
  Register holes read zero and ignore writes with `LOG_UNIMP`. Queue-clear and
  first-error-clear registers are write-only; reads return zero with `LOG_UNIMP`.
  First error is read-only; writes are ignored with `LOG_UNIMP`.
- Migration preserves retained registers, the first-error record and per-queue
  clear-service state, then recomputes both outputs without invoking reset.
- Parameters retains only `0x000f_ffff_ffff_fff1`; slow grant only
  `0x0000_0000_ffff_ffff`; spew control only `0x0000_0000_0000_07ff`; the I/O
  arbitration trigger only `0x0000_0000_0000_000f`; debug-data control only
  `0x0000_0000_0000_ffff`; and debug-arbitration control only
  `0x0000_0000_0000_0007`. Other bits read zero. All these controls except the
  parameters register reset to zero. Retained values do not claim arbitration,
  timeout, traffic-observation or debug behavior.

## Interrupts

Crossbar errors raise level-1 bit 54 in both processor interfaces (`sn1/intr.h`
line 206; OpenBSD `hub.h` line 139); the Itanium port panics on it (`error.c` lines
112 to 115). Both processor-interface destinations are required. These consumers
do not establish whether a stock-PROM boot generates or injects an error.

## DMA

None; the unit moves the other units' traffic and has no bus-facing address.

## Remaining hardware and diagnostic work

Arbitration, ages, bypass, queue backpressure, deadlock and tail timeouts,
traffic-generated errors and debug output are outside the initial register
implementation. They remain unresolved for complete machine and SGI diagnostic
qualification. The diagnostic inventory includes standalone system tests and
external scan coverage, but does not yet map their workloads to individual
crossbar registers. No diagnostic pass may be inferred from register tests.

## Behavioral notes

**The clearing mechanism is not stated.** The header says each clear register
"clears" its base register without specifying a write mask. The inspected
consumer writes zero during initialization. That error-free probe works under
both any-write-clear and write-one-to-clear behavior. Any-write-clear remains
an explicit model choice, not a mechanism established by the probe.

**Both PIs on first error is a statement, not a consumer expectation.** No consumer
handles it successfully. That limitation does not permit a master-only route.
The model must preserve both processor-interface destinations.

**Twenty-one of the thirty-six registers are never touched by any public consumer.**
That coverage limit does not establish their masks or side effects. Retained
storage, where explicitly cleared, is initial model behavior rather than proof
that the corresponding hardware function works.

## qtest checklist

- Parameters reads `0x0008_0000_8000_0021` after reset; a write of
  `0x0008_0000_ffff_f021` reads back.
- Under the explicit zero-reset policy, a write of `0x1` to the PI0 output queue
  error register reads back, sets first error valid with source `0xf`, type zero
  and channel zero, and sets level-1 bit 54 in both processor interfaces'
  pending registers. This test does not assign masks to the seven structureless
  registers.
- A premature first-error clear leaves the record and both outputs asserted.
  Writing any 64-bit value to that queue's clear alias zeroes the queue register;
  first error stays valid until first-error clear is written, then reads zero and
  both outputs deassert.
- While PI0 output remains the first error, inject documented bits into two later
  structured queues. Service PI0 output and clear first error; the lower-address
  remaining queue relatches. Service it and verify the last queue relatches, then
  service that queue and verify both outputs deassert.
- Bad-width accesses fail without changing state. Structureless registers and
  holes return zero or discard writes with `LOG_UNIMP` as specified. Reset and
  local reset restore the approved initial state. Migration preserves an occupied
  first-error record, accumulated later errors and clear-service state, then
  re-drives both outputs.
- The fifteen clear registers written 0 in address order on a fresh machine leave
  the modeled error state clear: a replay of the Itanium probe. This does not
  prove that zero clears an existing error.
- Slow grant retains a write to bits 31:0; bits 63:32 read 0.
