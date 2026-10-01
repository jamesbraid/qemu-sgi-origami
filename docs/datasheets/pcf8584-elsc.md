# Datasheet: PCF8584 I2C-bus controller and the ELSC system-controller protocol

References rendered as citation identities name material outside this document
set. Those targets are not included or cleared for redistribution. Review and
runtime statements retain their recorded dates and scope.

**Status:** the original device contract is reviewed and reconciled. The added IRIX 6.5.30
early-boot and two-module arbitration contract is independently reviewed and reconciled in
the two-module contract review (citation: `docs/reviews/pcf8584-elsc-two-module-review.md`). The original
review transcripts are committed beside
the original reconciliation (citation: `docs/reviews/pcf8584-elsc-gate.md`). The base PCF8584 and ELSC devices
and the Origin 200 two-module arbitration behavior are implemented on the maintained QEMU trunk.
The Origin 2000 shared-management behavior has its own reviewed contract and implementation
boundary. A question this datasheet cannot answer is a defect in the document and should be raised
here rather than answered informally.

**Scope: which firmware this describes, and which firmware we boot — they are not the same
build.** The source-derived PROM and controller claims come from IRIX 6.5.7. **The PROM binary
this project boots is IRIX 6.5.30**, and we hold no release for which we have both the source
and the binary we run. A statement here about source behaviour therefore describes 6.5.7.
Claims about 6.5.30 come from disassembly and live captures of the binary we boot.

**That is a stated scope and not a hedge, and the distinction matters.** Nothing measured has
contradicted anything specified here. Where this document has been checked against the running
binary — the arbitration wait, which has an instruction-level witness — **the source reading
and the measurement agree about the contract** and differ only in how the code is factored,
which is what one expects between builds and is also what a compiler does on its own. What a
model must satisfy is the observable contract, and that is what every claim below is written
as. The way to close a doubt about any of it is the way the arbitration wait was closed:
measure it against the binary we boot.

**Fork files — both built and on the trunk.** `hw/i2c/pcf8584.c` with
`include/hw/i2c/pcf8584.h` (the controller chip — a generic third-party part, named by part
number like the existing `hw/gpio/pcf8574.c`, no `sgi-` prefix), and `hw/misc/sgi-elsc.c` with
`include/hw/misc/sgi-elsc.h` (the module system controller — an SGI board with an
SGI protocol, named like `sgi-hub`/`sgi-bridge`; the `sgi-elsc` id keeps SGI's older name for
it, ELSC). The proposed naming was confirmed at the gate
and is what landed; `tests/qtest/sn0-elsc-test.c` covers both parts. The taxonomy rows for both
now read "modelled, on the trunk".

**Sources used:**
- The IRIX 6.5.7 PROM, PCF8584, ELSC, and controller-firmware sources named here are
  **protected source** under the provenance policy (citation: `docs/provenance.md`). The facts derived from
  them are independently restated interface behavior. The source reader for this addition did
  not implement the corresponding QEMU behavior.
- IRIX 6.5.7's PCF8584 driver — the chip driver linked into every production PROM build.
  Its own header names four public Philips references directly ("The Philips I2C bus",
  "Application Hints for the PCD8584", "Application Report PCF8584 I2C-Bus Controller",
  "Philips PCD8584 I2C-bus controller datasheet") — a third-party chip with a public
  interface, read and transcribed in our own words, not SGI's.
- IRIX 6.5.7's ELSC driver and its structure/constant header, plus a short plain-text usage
  guide for the ELSC package covering polling cadence, TTY buffering, and warning-message
  handling. SGI's own code; read directly, nothing pasted, facts restated in our own words
  per the source-use policy.
- IRIX 6.5.7's IP27 PROM console-selection routine and the startup file that calls it
  (`stand/arcs/IP27prom/pod.c` and `stand/arcs/IP27prom/main.c`), for the order in which
  traffic reaches this bus and for what gates the system-controller candidate. Both were read
  for the review gate; the second was added after the first pass, because the routine's own
  callee lives there and a file list that stopped at the caller missed sixteen boot-path
  EEPROM transactions.
- The stock IRIX 6.5.30 IP27 PROM payload, SHA-256
  `c2c69bf33c375b43ec7ac511fab41ddf595be1b6838ae717e58a9a1273437fdd`, disassembled and
  checked against live register captures. It supplies the early PCF liveness probe's address
  formation, timeout, and control flow on the release this project boots.
- **"ELSC" is a public name and is used throughout here on that basis.** Mainline Linux's
  `arch/mips/include/asm/sn/sn0/addrs.h` defines the PROM's per-processor ELSC scratch bases
  and their size, so the name is a shared one for a thing rather than SGI's private label.
  The PROM's internal routine names are a different matter and are not reproduced — see the
  note in the command/response section.
- The Module System Controller research document (citation: `docs/library/msc-module-system-controller.md`)
  (`docs/library/`, irix-structs, commit `dfbe7e0`) already covers most of this ground from
  the same primary sources plus the Origin 200 Owner's Guide and the IP27 PROM Technical
  Reference Manual — cited throughout rather than re-derived; this datasheet's job is the
  formal register-map table and the qtest-facing spec, not a second narrative.
- Existing register-address facts (Hub offsets `0x22_0000`/`0x22_0008`, the S0/S1 access
  matrix, the control/status bit lists) are already established in
  [the Hub memory/directory datasheet](hub-memory-directory.md) from the Hub register-block
  work; this document does not restate that address mapping, only the chip- and
  protocol-level behavior built on top of it.
- SGI's *Origin 200 and Origin 200 GIGAchannel Owner's Guide*, `007-3708-002`, held in the
  reference corpus at `refs/techpubs/techpubs.sgi.com/library/manuals/3000/007-3708-002/`.
  A machine-specific vendor manual for our exact machine, read end to end; it supplies the
  operator-facing half of this controller — the rear serial port, its line settings, its escape
  prefix and its input-routing rule — which no source read for the first draft covered. Every
  claim taken from it is recorded with its printed page number in
  the Owner's Guide diagnostics read (citation: `docs/library/origin200-owners-guide-diagnostics.md`).
- The module controller's own microcontroller firmware source was read for its
  I2C-visible grant transaction, cadence, priorities, recovery, power, and reset
  behavior. The source identities and bounded reads are recorded in
  the shared-management activation investigation (citation: `docs/research/origin2000-shared-management-guest-activation.md`).
  Internal board wiring outside those observable behaviors remains outside this
  datasheet.
- The Origin 200 read of the ELSC's 17C44/16C74 microcontroller firmware establishes which
  controller emits the rotating grants in a two-module machine and that the slave participates
  in the same five-position schedule. The observable schedule is in scope. Microcode
  implementation, private state, board wiring, and electrical details remain out of scope.

## Shared-module bus: supported chip behavior and open runtime questions

The [Origin 2000 module wiring](origin2000-module-management.md) places four
PCF8584 interfaces on one bus. The public behavior evidence (citation: `docs/reviews/pcf8584-shared-bus-review-20260905.md`)
and independent reconciliation (citation: `docs/reviews/pcf8584-shared-bus-independent-review-20260905.md`)
clear the following chip rules for that shared attachment. They do not clear
a complete module scheduler or extend the earlier private-bus model policy.

Each interface retains its own register and status state. BB records observed
bus conditions: START makes it zero and STOP makes it one. Repeated START
keeps the bus occupied. Hardware reset sets the local BB and PIN flags to
one. A detected misplaced START or STOP sets BER, makes BB one and makes
PIN zero. A late-powered interface can miss an earlier START. Consequently,
a local BB value of one does not prove that no peer owns an active transfer.
Philips recommends an initialization wait covering the longest transfer for
that case, but this contract selects no duration.

Repeated START by the active master retains bus ownership. The documented
master-mode simultaneous START/STOP request chains transfers without
relinquishing control. Ordinary STOP ends the transfer. These rules apply
to the requesting adapter's transfer: another adapter's active transaction
is not evidence that a local START request is a repeated START.

Arbitration compares transmitted bits against the shared data line while
the clock is high. A contender that sends high but observes low loses and
releases its data output, preserving the winner's transfer. Identical
transfers can remain concurrent until they differ, or finish identically.
There is no fixed adapter priority. LAB reports actual arbitration loss.
An attempt made while another master is busy does not by itself establish
LAB. A host callback order or a first-arrival mutex is not evidence of the
physical winner. The public protocol leaves specified combinations of data,
STOP and repeated START during continuing arbitration undefined.

Addressed slave service holds the common clock low while PIN is active.
Address matching and general-call reception differ from passive monitor
mode. With own address zero, the PCF8584 monitors received data without
acknowledging, interrupting or stretching the clock for pending service.
Normal nonzero-address adapters do not automatically copy every byte.
These chip receive modes do not specify the MSC's scheduling broadcasts.

Hardware reset affects local adapter state. It does not establish a reset
of the MSC, NVRAM or peer adapters. A losing or reset non-owner must not
terminate the winner through a shared transaction cleanup operation.
The cited hardware reset requires at least 30 controller clocks with the
clock running. This is distinct from a PIN-one software-reset write.

The smallest supported implementation subset is shared module population
and storage, separate adapter state, observed START/STOP status with the
reset/error exceptions, normal addressed transfers, ownership-preserving
repeated START, and the documented receive modes. This is partial behavior,
not a complete shared-master runtime. The following still require evidence:

- How an active owner's reset changes wire levels and the peer/slave
  transaction, including any resulting completion or recovery.
- How a PIN-one software reset changes local BB while a peer remains active.
  The general status-clear wording and BB's specific rules do not settle it.
- The silicon mechanism by which a nonzero-address adapter exposes and retains
  an unmatched grant address. The source-reviewed software contract requires
  this result, but the public chip sources do not establish the mechanism.
- A simultaneous-transfer representation that preserves bitwise arbitration
  and clock synchronization. Serializing arrivals does not resolve it.

The earlier arbitration-pulse policy describes the existing Origin 200
model. It does not establish a four-node Origin 2000 scheduler. The reset
values recorded for that model likewise do not make physical bus occupancy
an unconditional function of one interface's reset state. Shared-bus qtests
must preserve these limits rather than treating a private-bus passing
sequence as full multi-master acceptance.

### Clocked START and unequal-master synchronization

Status: independently cleared for the stated public behavior and evidence
boundaries. The independent review (citation: `docs/reviews/pcf8584-clocked-start-general-call-independent-review.md`)
preserves the explicitly unresolved timing and request-handling questions.
The existing S2 access row specifies the clock selectors. Philips PCF8584
product specification, 1997-10-21, PDF page 8 calls the selected SCL rates
approximate and conditions them on programming the prescaler for the actual
input clock. A nominal frequency therefore does not by itself specify exact
SCL low/high durations or a control-write-to-START delay.

PDF page 16, Figure 6, gives the normal software sequence: poll BB until
free, write the address byte, then request START through the control
register. The chip emits START, the address and its acknowledgement clock.
PDF page 12 says starting a transmission makes PIN inactive. Completion of
a byte including its acknowledgement clock makes PIN active. These are
serial-transfer events, not evidence for completing the byte at the instant
of a host register write.

The cited flowchart avoids requesting START while a peer already occupies
the bus. It does not define that out-of-precondition request's handling.
PDF page 11's STA/STO operation table likewise supplies no explicit rule for
retaining such a request until STOP, clearing STA, or reporting a particular
status transition. A queued START, discarded request, immediate arbitration
loss or synthetic error cannot be chosen from this evidence. The existing
BB reset/error exceptions still apply independently.

The PCF8584's PDF page 24 specifies these bus timing limits:

| Quantity | Published bound |
|---|---|
| SCL frequency | At most 100 kHz. |
| Bus-free interval | At least 4.7 microseconds. |
| START setup interval | At least 4.7 microseconds. |
| START hold interval | At least 4.0 microseconds. |
| SCL low interval | At least 4.7 microseconds. |
| SCL high interval | At least 4.0 microseconds. |

These limits describe bus signals under the stated operating conditions.
They do not assign the number of input clocks between accepting the parallel
control write and driving the initial START. Nor do they establish a 50%
SCL duty cycle. The input clock's duty-cycle requirement is a separate
parallel-interface parameter.

For the shared-module model, use a period rounded upward to a whole nanosecond
at the selected approximate rate, then split it into low and high halves that
differ by at most one nanosecond. At 90 kHz, use 5,556 ns low and 5,556 ns high.
Use one high interval for the START hold. This exceeds the published 4.0
microsecond minimum. It is a deterministic approximation grounded in the
published nominal rate, timing bounds, and absence of the predecessor's
adjustable asymmetric-clock function. It is not a measured duty cycle.

NXP UM10204 revision 7.0, PDF pages 10–11 and Figure 7, supplies the common
I2C synchronization rule for unequal masters. An observed falling SCL edge
starts each participating master's low-period count. A master that finishes
its low period releases SCL, but a peer still driving low keeps the shared
line low. The masters start their high-period counts when the shared line
actually rises. The first master whose high period expires pulls SCL low
again, starting the next synchronized cycle. Thus the common low period
follows the longest participating low period and the high period follows
the shortest participating high period. A slave holding SCL low also delays
the actual rising edge and subsequent high-period count.

That event ordering can guide a clocked shared-line model once each adapter's
phase durations are justified. It does not justify selecting one master's
whole clock, averaging rates, or advancing a master's high timer while a
peer holds SCL low. The existing bitwise arbitration rule samples SDA while
shared SCL is high. NXP page 11 permits overlapping START conditions within
the START hold interval, but does not specify how PCF8584 host writes enter
that physical interval. The PCF8584 busy-START and write-to-pin timing gaps
remain distinct from supported wire-level synchronization.

Sources: the retained
PCF8584 specification (citation: `docs/library/cache/datasheets/pcf8584-i2c-bus-controller-philips.pdf`),
SHA-256 `481c7ea1eacd8d92fc23be4c92f8feb600dee057e51316747783a37e106db365`,
and [NXP UM10204 revision 7.0](https://www.nxp.com/docs/en/user-guide/UM10204.pdf),
SHA-256 `dc91f00f65584e06ef36e26c93bf9d91a95fb3c8a1830a9223e53caf678b36af`.
PCF8584 pages 8, 16 and 24 and NXP pages 10–11 were rendered and checked.
The later NXP text is used for ordinary shared-line I2C synchronization,
not later optional features or undocumented PCF8584 register behavior.

### General-call acknowledgement and receiver selection

Status: independently cleared for the stated general-call behavior by the
independent review (citation: `docs/reviews/pcf8584-clocked-start-general-call-independent-review.md`).
NXP UM10204 revision 7.0, PDF pages 16–17, describes a general call as an
address phase followed by data for the receivers accepting that address.
This general call uses address byte `0x00`, including R/W zero. Table 4 on
page 16 assigns address zero with R/W one to the `0x01` START byte, which
no device acknowledges. It is not a general-call read.

A device that declines the general-call address does not acknowledge it.
A device that accepts acknowledges and participates as a target receiver. This selection
is per receiver, while the master's acknowledgement observation is shared.

One or more receivers acknowledging produces ACK at the master, even when
another receiver declines. A NAK from one receiver must not override another
receiver's ACK. If none acknowledges, the master observes NAK. The master
cannot infer the number or identity of accepting receivers from that result.
The same aggregate rule applies to each subsequent data byte: any accepting
receiver acknowledging masks the NAK of another receiver that cannot process
that byte.

A receiver that ignored the general-call address is not thereby selected
for the following addressed data because a peer acknowledged. This does not
prohibit a separately enabled passive monitor from observing those bytes.
Nor does a receiver's NAK of one data byte establish that it has permanently
left the transaction: the cited text specifies ignoring that byte, not a
mandatory permanent deselection. No MSC scheduling interpretation or optional
general-call reset behavior is derived from these rules.

Discriminating tests should include two accepting receivers with opposite
data-byte ACK results, an address-declining receiver alongside an accepting
peer, and a transfer with no address acknowledgements. Repeat the mixed-ACK
case with receiver order exchanged. The bus result must follow the shared
line, while payload delivery follows each receiver's address acceptance.
The original NXP PDF pages 16–17 were rendered and checked for this addition.

### Origin 2000 address-only grants

Status: independently source-reviewed implementation contract. The
source-clearance audit (citation: `docs/reviews/origin2000-module-management-source-clearance-review-20260908.md`)
binds this text to the earlier independent scheduler and reset review.
The activation investigation (citation: `docs/research/origin2000-shared-management-guest-activation.md`)
records the complete source identity and exposure boundary.

The module controller publishes one raw byte, `0xe0` through `0xe7`, as the
address and direction phase of a transaction. A pending-message indication sets
bit 3 and produces `0xe8` through `0xef`. The controller requests START after
loading the byte, waits for its nine address and acknowledgement clocks, then
requests STOP. No data byte follows. These are otherwise-unmatched addresses,
not a general call. The descriptive source comment using general-call language
does not override the register operations.

The Hub-side software uses a nonzero own address and nevertheless requires the
last unmatched grant address to be readable from its PCF8584 data buffer while
BB reports free. It first requires its own value to be absent, then requires its
value to remain unchanged for more than 250 microseconds. The module scheduler's
one-millisecond cadence supplies that stable interval. The next processor's
grant supplies the later absence without a zero-valued gap transaction.

The producer and consumer establish this software-visible result. The public
PCF8584 specification expressly guarantees automatic passive buffering only
when own address is zero. It does not state whether a nonzero-address adapter
copies an unmatched address into its parallel read buffer, whether STOP retains
it, or what PIN does in this case.

The Origin 2000 model adopts this narrow approximation: after the ninth clock of
an unmatched address phase, a nonzero-address adapter copies that address byte
to its parallel read buffer and retains it through STOP and idle until a later
completed address phase replaces it. This observation does not ACK or select
the adapter, set addressed-as-slave, activate PIN, request an interrupt, or hold
SCL. It does not copy payload bytes from an unmatched transfer. Tests must keep
address observation separate from addressed reception and zero-address monitor
mode.

A later matching register-and-wire capture may replace this approximation. Until
then, documentation and tests must call it a software-required approximation,
not measured PCF8584 behavior.

For addressed service, PIN becomes active only after all nine clocks, including
ACK. Receiver data moves into the read buffer during that ACK phase. The
addressed receiver then holds the next SCL low phase until software reads S0.
An addressed transmitter holds it until software writes S0. Each adapter owns
its hold, and servicing one adapter cannot release another adapter's hold.

On arbitration loss, LAB becomes set at the mismatching sampled bit and the
adapter releases SDA master drive. PIN does not become active at that loss
point. It becomes active when the winning byte finishes after ACK. If the
winning address matches the loser's own address or general call, addressed
slave status is set as specified for that address. The public chip sources do
not distinguish immediate SCL release from clock contribution through the rest
of the winning byte, so that PCF8584-specific choice remains an explicit model
policy.

BER reports a misplaced START or STOP, makes PIN active, and makes local BB read
free. A valid repeated START from the active master is not BER. A START request
made while another master is busy is not by itself LAB or BER. Stock software
checks BB before its initial START, and the chip sources do not define whether a
busy nonparticipant request is retained, rejected, or later emitted. The model's
bounded compatibility behavior for this case is recorded under
[the busy-START approximation](#compatibility-approximation-retain-one-busy-initial-start).

## Role

Two devices work together to give the Hub a path to the Origin 200's entry-level system
controller (ELSC): the PCF8584, a Philips I2C-bus controller chip wired to the Hub's local
register space, and the ELSC itself, a separate board sitting on the same I2C bus as an
addressable slave. The PCF8584 is the only way the Hub reaches the bus, and the ELSC is the
device on it this datasheet specifies — but it is **not** the only device the PROM driver
addresses: a serial EEPROM holding the machine's identity and configuration answers on the
same bus, at its own slave addresses, and firmware reads it during boot. Real firmware uses
the ELSC itself for two distinct purposes: a lightweight, checksummed ASCII command/response
protocol (front-panel display, power control, reset, switch readback), and — separately — an
alternate console path, where a serial terminal connected to the ELSC's own rear diagnostic
connector is multiplexed onto the same I2C link as queued input messages. The
second purpose is why this datasheet exists: the console fan-out identifies a real-answering
ELSC as the higher-leverage fix for the console-selection problem this project has been
chasing, ahead of the Bridge/IOC3 path.

**The base devices and Origin 200 two-module grant sharing are implemented on the maintained QEMU
trunk.** The Origin 2000 shared bus, scheduler, and machine attachment remain separate from that
landed behavior. The sections below specify those additions without claiming that the complete
Origin 2000 module is on the trunk. What this datasheet leaves unspecified remains a documentation
defect rather than permission to invent hardware behavior.

**The state it was written against, kept because it explains the shape of what follows.** The
Hub-side register block answered as a fixed, always-idle controller — enough to stop firmware
spinning through its full I2C timeout on every access, but not a real controller: it could not
run a transaction, and nothing answered as the system controller itself. **The chapters below are
organised around closing exactly that gap**, which is why they lead with the register contract and
the arbitration wait rather than with the packet protocol.

## PCF8584: register map

The Hub-local addressing (`0x22_0000` data, `0x22_0008` control/status, the write-only vs.
read-only split on the control/status register once `ESO` is set) is already documented in
[the Hub memory/directory datasheet](hub-memory-directory.md) and is not repeated here. That
document also names the control bits `PIN`/`ESO`/`ES1`/`ES2`/`ENI`/`STA`/`STO`/`ACK` and the
status bits pending-interrupt-not/stop-detected/bus-error/last-received-bit/addressed-as-
slave/lost-arbitration/bus-not-busy — the chip's own public register vocabulary, not an SGI
naming.

What it does not yet break out is that the data register (`0x22_0000`) is not one register —
it is **one of four**, selected by `ESO` together with the control register's own `ES1`/`ES2`
bits. All four selections are listed in the chip driver's own access-conditions comment, and
the table below is that matrix restated:

| `ESO` | `ES1` | `ES2` | Selected register | Purpose |
|---|---|---|---|---|
| 0 | 0 | 0 | Own address | The chip's own I2C slave address. Firmware's init sequence writes and reads this back as a liveness check before writing the real address. |
| 0 | 1 | 0 | Clock control | Two packed fields: a main-oscillator divider (`0x00`/`0x10`/`0x14`/`0x18`/`0x1c` for 3, 4.43, 6, 8, 12 MHz) and an SCL bus-speed select (`0x00`/`0x01`/`0x02`/`0x03` for 90, 45, 11, and 1.5 kHz). Firmware always programs `12 MHz` / `90 kHz`, which is the single byte `0x1c`. |
| 0 | 0 | 1 | Interrupt vector | Never written or read by the PROM driver; `ENI` (interrupt output enable) is never set, so this register has no observed behavior to model. |
| 1 | either | 0 | Data | The working data register for an in-progress transaction — the byte shifted in or out. |
| 1 | either | 1 | Interrupt vector | The same register again, still reachable with the serial interface enabled. |

**`ES2` keeps selecting even after `ESO` is set, and `ES1` stops.** That asymmetry is easy to
lose: with the serial interface enabled, `ES1` no longer distinguishes anything and `ES2`
chooses between the data register and the interrupt vector. A model that treats "`ESO` set"
as meaning "this address is the data register" answers the interrupt vector's selection
wrongly. Nothing in the PROM driver ever sets `ES2` while running, so the error would not
show up against this firmware — which is the reason to get it right in the model rather than
discover later that a different guest sets it.

**The own-address register holds the seven-bit address in its low seven bits, and the chip
shifts it left by one onto the wire.** Firmware writes `0x7f` and then its real address with
no shift of its own, while every *transaction* address in the same driver is explicitly
shifted left before it is written — the two conventions sit a few lines apart and the
difference is real rather than an inconsistency. Linux's own PCF8584 algorithm driver states
the same thing beside the same write-then-read-back init step, calling the effective address
the register value shifted left by one (`drivers/i2c/algos/i2c-algo-pcf.c`, mainline
`refs/linux`), so this is a public third-party statement about the chip rather than an
inference from SGI's usage. An earlier draft of this table called the field left-justified;
that was wrong, and a model storing it shifted would put every node on the bus at the wrong
address.

## PCF8584: behavioral contract

**Initialization** is nine register writes and one read, in this order, and a model has to
survive all of them:

1. Write the control register with `PIN` alone (`0x80`) — reset, and select the own-address
   register by leaving `ESO`, `ES1` and `ES2` clear.
2. Write `0x7f` to the selected register, a placeholder address.
3. Write the control register with `PIN | ES1` (`0xa0`) to select clock control.
4. Write `0x1c` — the 12 MHz divider with the 90 kHz bus-speed select.
5. Write the control register with `PIN` alone again, re-selecting the own address.
6. **Read** the selected register and compare it against `0x7f`. A mismatch aborts
   initialization with a chip-initialization error and nothing else on the bus is attempted.
7. Write the control register with `PIN` alone a **second** time. This write is redundant
   against the state step 5 already established, and it is in the firmware, so a model whose
   qtest replays the sequence write-for-write will see it.
8. Write the node's real address, `0x09` plus the node's own slot index.
9. Write the control register with `PIN | ESO` (`0xc0`), enabling the serial interface.

**A model's own-address register has to store what is written and give it back**, or the
sequence dies at step 6. **Firmware reads back only the placeholder** — it writes the real
address in step 8 and enables the interface immediately, never checking it. That asymmetry
matters twice: it is why a model returning a constant fails initialization outright, and it
is why our own qtest asserting that the *real* address round-trips too is asserting something
the firmware never exercises. That is deliberate; a register test that only replays what one
guest happens to check is passed by wrong implementations that a full round-trip catches.

**The bus liveness probe reads the data register and nothing else.** Immediately after
initialization the firmware samples the data register once, then re-reads it in a tight loop
for `5,000,000` units of its converted Hub counter, and returns as soon as the value *differs*
from that first sample. It
never looks at the control/status register and never consults bus-not-busy; a changing byte
is the whole test, and its comment says what it is for — waiting for the system controller to
start sending its tokens. So **a data register that answers a fixed value tells this probe
that nothing is on the bus**. The historical fixed-idle stub produced that result. The current
single-module ELSC model does not make fixed-idle behavior the hardware contract. An
earlier draft of this paragraph said the probe combined bus-not-busy with the data register.
It does not, and the distinction is the difference between a model that has to drive one
register and one that has to keep two consistent.

### The early 6.5.30 liveness probe and its address

The 6.5.30 PROM forms this probe's address from the PCF data register in the Hub's
**explicit-node** window, then inserts the NASID reported by the processor executing the
probe. For NASID `n`, the data address is `0x9200000001a20000 + (n << 32)` and the
control/status address is eight bytes higher. The equivalent **Hub-local** data alias is
`0x9200000001220000`, also with control/status eight bytes higher. The probe uses the
explicit-node form, not the local alias.

This distinction matters before node numbering. Both Hubs initially report NASID zero,
so a processor in the slave module computes `0x9200000001a20000`, the same numeric address as a
processor in the master module. In the current direct-link model, duplicate initial NASID
claims resolve to the issuing Hub: the slave processor's access reaches the slave module's own
PCF. This routing is an observation of the model, not proof of the real fabric's duplicate-ID
rule. The PROM's computation does establish its intent: at this stage it asks for the PCF of
the Hub whose NASID the caller currently reports. Once the slave Hub reports NASID one, a
newly computed explicit address is `0x9200000101a20000`. A retained explicit-zero address would
instead name the master Hub.

The probe's deadline argument is exactly `5,000,000` in the units returned by the firmware's
Hub-counter conversion. Firmware treats those units as microseconds, making this a nominal
five-second deadline. The physical duration remains unknown until the Hub counter's real tick
rate is established. The probe samples the low data byte once and then succeeds on the first
different low byte. If the byte remains fixed, it returns failure when the converted deadline
expires. The startup caller discards that return value and continues. An inert PCF therefore
delays that processor for the whole deadline. This probe neither waits for a processor-specific
grant nor disables a processor directly. Any lost early rendezvous is a consequence of the
delay around this call, not a failure branch inside the probe.

The observed stop at `0xc00000001fc5ad68` lies in the Hub real-time-counter conversion used to
check that deadline. It is not a PCF load and not the arbitration wait. The probe's first data
load is at `0xc00000001fc584b0`. Its repeated data load is at `0xc00000001fc584ec`.

The available captures bracket this probe without recording its byte stream. In failed run
`irix-20260911T134931-574612`, CPU 3 stopped in the timer conversion with
`s1=0x9200000001a20000` and was disabled later in early initialization. Healthy run
`irix-20260911T141620-49017d` enabled all four processors, but its retained state is from after
node numbering and shows the slave PCF idle. The first capture places the processor that later
fails inside the probe, but does not record the byte that kept it there. The second proves that
late slave-PCF idleness alone does not distinguish success from failure.

#### Healthy-versus-failed capture

Capture the missing discriminator at the two data loads. Start a fresh two-module run and plant
a breakpoint on slave CPU 3 at the first load:

```sh
tools/origami qmp RUN x-mips-debug-breakpoint \
  '{"cpu-index":3,"pc":13835058055815201968}'
```

When it stops, capture the machine state before attaching GDB, because GDB owns the run's QMP
connection while attached:

```sh
tools/origami qmp RUN x-query-origin-state '{"event-limit":128}'
tools/origami gdb RUN --batch \
  -ex 'info threads' \
  -ex 'info registers pc ra s1 s2 s5 a0 a1'
```

Remove that breakpoint, plant one at the repeated load, resume, and capture several successive
hits:

```sh
tools/origami qmp RUN x-mips-debug-breakpoint \
  '{"cpu-index":3,"pc":13835058055815201968,"remove":true}'
tools/origami qmp RUN x-mips-debug-breakpoint \
  '{"cpu-index":3,"pc":13835058055815202028}'
tools/origami qmp RUN cont
```

At every repeated-load stop, record the PCF state and stopped CPU's registers with these
commands:

```sh
tools/origami qmp RUN x-query-origin-state '{"event-limit":128}'
tools/origami gdb RUN --batch \
  -ex 'info threads' \
  -ex 'info registers pc ra s1 s2 s5 a0 a1'
```

After collecting the CPU 3 samples, remove its repeated-load breakpoint and let the boot
finish:

```sh
tools/origami qmp RUN x-mips-debug-breakpoint \
  '{"cpu-index":3,"pc":13835058055815202028,"remove":true}'
tools/origami qmp RUN cont
```

Collect fresh CPU 3 runs until the archive contains one boot that enables all four processors
and one boot that disables CPU 3. Compare the PCF byte sequence and NASID state at
`0xc00000001fc584b0` and `0xc00000001fc584ec`.

Capture slave CPU 2 as a control on a separate fresh run. Plant its first-load breakpoint with
`{"cpu-index":2,"pc":13835058055815201968}`. After recording the state and registers, remove
that breakpoint with
`{"cpu-index":2,"pc":13835058055815201968,"remove":true}`, plant the repeated-load breakpoint
with `{"cpu-index":2,"pc":13835058055815202028}`, and resume. Record several PCF states at the
repeated-load stop, then remove it with
`{"cpu-index":2,"pc":13835058055815202028,"remove":true}` and resume the boot. A late healthy
capture is not a substitute: one retained four-processor boot shows an idle slave PCF after
node numbering, but says nothing about the byte stream during the early probe.

**Starting a transaction as bus master** (transmit, receive, or a combined write-then-read):
write the target 7-bit address plus a direction bit to the data register, then write the
control register with `PIN|ESO|STA|ACK` to generate a start condition. The driver then polls
the status register in a tight loop until `PIN` clears (meaning the byte went out) or an
error bit (`bus-error`/`addressed-as-slave`/`lost-arbitration`) appears. Each subsequent data
byte follows the same write-then-poll-for-`PIN`-clear pattern. A combined write-then-read
transaction issues a **repeated start** (`STA` set again without an intervening stop) to
switch direction rather than releasing the bus.

**The receive half has three mechanics a model must reproduce or every response misframes.**
After the read-direction address goes out, the driver **reads the data register once and
throws the byte away** — that discarded read is what advances the chip's receive pipeline, so
a model that answers it with the first response byte shifts the entire packet by one and the
length and checksum both fail.

**What that discarded byte *is* decides where a model puts its fetch, and it is worth stating
because getting it backwards produces a defect that spreads to every other device on this
bus.** The firmware's own comment calls it **the slave address** — the stale content of the
chip's own register, left there by the address write, not a byte the responder sent. **The chip vendor's own product specification settles this, and we hold it.** Philips'
PCF8584 datasheet, 1997, acquired from the manufacturer's successor and recorded with its
checksum in the acquisition note (citation: `docs/library/pcf8584-datasheet-acquisition.md`), describes the
mechanism in three places — a remark under the register description, a footnote to the
master-receiver figure, and a bus-sequence diagram — and **all three attach the transfer to the
read of the data register rather than to the start condition.** In our own words: the first
read after a read-direction address **returns what the register already holds, which is the
address byte**, and *that read* is what causes the first real data byte to move into the
register. The next read returns it. The datasheet's own figure marks the point after the
address acknowledgement as where the dummy read must be executed.

**So the fetch belongs on the read, not on the start**, and it is now a vendor statement rather
than an inference. Two lesser sources agree and are recorded only because they were what this
document rested on before: the firmware author's comment naming the discarded byte as the slave
address, and a public third-party model that declares a read-side buffer and a write-side shift
register as separate fields — the latter being much weaker than it looks, since that model
implements no bus engine and never writes its read buffer at all, so it evidences a naming
rather than a pipeline.

**The failure this prevents.** A model that fetches from the bus when the start is issued
consumes the responder's first byte into its holding register, so the discarded read eats real
data and everything after is off by one. **From the responder's side that is fixed by emitting
a throwaway byte ahead of the real packet — a fix that works and encodes the bug.** Written
into a bus contract it would make every later device on these eight-plus-two slave addresses
prefix a byte to match a defect in the controller, at which point the model is self-consistent,
every test passes, and only real hardware disagrees.

**So: no responder on this bus should need to prefix a byte, and a model that requires one has
its fetch in the wrong place.** **Settled by the chip vendor's product specification**, which we hold —
not by the driver comment and third-party naming this paragraph previously rested on. An
earlier revision said that datasheet was the instrument that would settle it and that we did
not hold it. **That was true when written**: the holdings were checked, the document was not
there, and it was acquired minutes later *because* this document named it as the thing that
would settle the question. **Recorded because the sequence is the useful part** — a datasheet
naming its own missing instrument precisely enough to be actioned is how the instrument got
acquired, which is worth more than the paragraph it corrected. If someone acquires it, this is the paragraph to
check first. Before reading what will be the **last** byte, the driver
rewrites the control register with `ESO` alone, clearing `ACK`, so the final byte is NAKed;
for a one-byte read it does this before the pipeline starts at all. And when a transaction is
told the length is embedded, the driver reads byte zero and **replaces its own byte limit**
with that byte plus a fixed adjustment, so the packet's own first byte decides how many more
reads happen.

**Every transaction that gets as far as moving data ends with a second, deliberate repeated
start to an invalid address**, which every PCF8584 on the bus NAKs; the driver's own comment
documents this as a system-controller arbitration hack that forces a known, fixed value into
every chip's data register before the real stop condition is written. Three details of it are
easy to model wrongly, and all three are visible in the register trace:

- **The byte written is `0xfe`, not `0x7f`.** The invalid address is `0x7f`, and it goes onto
  the wire shifted left like any other address, with a clear write-direction bit.
- **The control register is written *first*, then the data register** — `ESO | STA | ACK`
  (`0x45`, with `PIN` deliberately absent), and only then the address byte. That is the
  opposite order from a normal transaction start, which writes the address first and the
  control register second. It is the same control-first order the direction-changing repeated
  start uses, so a model driving order off the normal-start description gets both wrong.
- **"Success or failure" overstates it.** An earlier draft said every transaction ends this
  way. It does not: arbitration failure, a no-message return, a bad initial chip state, an
  address NAK, a bus error and any timeout all leave by paths that skip the invalid-address
  step, and the earliest of them skip the closing stop condition too. Only a transaction that
  completed its data phase performs it.

After the invalid-address start is acknowledged as NAKed, the driver writes
`PIN | ESO | STO` to generate the stop.

The invalid address must always NAK because `0x7f` does not name a module endpoint. The
Hub software also expects every controller to reach the same known data-register state. That
second effect uses the same software-required unmatched-address approximation as the grant
addresses above. It is not independently established PCF8584 silicon behavior.

**The arbitration protocol**, run immediately before every real transaction, is what makes
multiple Hub nodes and the ELSC share one bus without a real I2C multi-master mechanism — the
same "super-master" mechanism the MSC research document (citation: `docs/library/msc-module-system-controller.md`)
identifies from the driver's own comment. The controller publishes one of two signal values as
an otherwise-unmatched address-only transaction, and each node watches its own PCF8584 read
buffer for that value after STOP. The two values are derived, not fixed:
`signal1 = 0x70 * 2 + processor_code`,
`signal2 = signal1 + 8`, where `processor_code = 2 * (physical module slot - 1) + local CPU
number`. The CPU number comes from a live read of the Hub's CPU-number register (`PI_CPU_NUM`,
a name Linux and OpenBSD both use for the same processor-interface register). Each processor
therefore watches for a distinct pair of bytes, and `signal2` is `signal1` with bit 3 set. In a
two-module Origin 200, CPU 3 is local CPU 1 in physical module slot 2: its processor code is 3,
and its pair is exactly `0xe3` and `0xeb`.

**The wait is three loops, not two, and the loop most easily missed is the first one.** The
routine runs, in order:

1. **Drain a stale token — the liveness phase.** Spin until the bus reports not-busy *and* the
   data register presents **a valid byte that is neither of this processor's two signal
   values** — both halves load-bearing. The **valid strobe must be set**, and the byte may be
   anything but your own pair. Another processor's grant supplies that transition in the
   source-reviewed scheduler. The existing private-bus model's zero value also made measured
   boots leave this phase, but that observation does not establish a hardware gap transaction
   or a sixth position in the controller's five-position ring.
   The two halves are the liveness check between them: the valid strobe
   proves a byte is genuinely being presented, and the not-yours half that it is not a stale copy
   of your own signal. That the ring is *turning* is the phase's purpose, which the pair serves
   without either half establishing it alone. The comment says why the byte must not be yours:
   if your signal is already present on entry, this node missed the start of its own time slice
   and must let it pass. **The hang case is precisely a valid strobe that never
   sets** — a controller presenting nothing, no valid byte at all, parks every processor here
   whatever value it shows. A live rotation of different processor grants releases them.
2. **Wait for the signal to arrive.** Spin until the bus reports not-busy and the data
   register holds `signal1` or `signal2`, remembering which.
3. **Hold it.** Record `converted_start = floor(8 * raw_start / 10)`, then spin until
   `converted_now > converted_start + 250`, where
   `converted_now = floor(8 * raw_now / 10)`. Require on every pass that the bus still reports
   not-busy and the data register still holds the *same* value. Any change, or the bus going
   busy, jumps back to loop 2 — not to loop 1 — and the hold restarts. The physical duration is
   unknown until the raw counter's tick rate is established.

Success returns whether the held value was `signal2` rather than `signal1`, which is the
message-waiting indication described below. The transaction callers give this wait a deadline
of `1,000,000` firmware-converted counter units, nominally one second. Expiry returns an error.
A caller that retries starts again at the stale-token drain with a fresh deadline. This grant
deadline is separate from the startup liveness probe's `5,000,000` converted units. Neither
number proves physical elapsed time without the Hub counter's tick rate.

**The consequence for a model is that one processor's value must persist long enough for the
hold check, then be replaced by another value before that processor's next turn.** A controller
that parks one processor's value forever never escapes loop 1. The Origin 200's five-position
rotation and the Origin 2000's eight-code rotation both supply an absent interval and then hold
the processor's value past the converted-250 threshold. Neither needs an invented zero gap or
repeated publication within a slot. The hold is measured against the Hub real-time counter, so
that counter's rate still determines the observed duration. The clock-control register that
makes it settable is implemented in fork commit `74af325d4b`.

### Measured against the running firmware, not only read from source

**A disassembly-plus-capture pass has since observed this wait executing on our own machine,
which makes it the only part of this datasheet with an instruction-level witness rather than a
source reading.** Three things it establishes, and one warning that matters more than any of
them.

**The register roles are confirmed by pairing an instruction with the address it issued**,
rather than by recognising an address and assuming. The load that reads the status register
was observed issuing accesses to the control/status address, and the load that reads the byte
was observed issuing accesses to the data address. **The status bit the loop tests is bit 0** —
bus-not-busy — exactly as specified above.

**The wait compares the data byte against two expected values and leaves by one of two exits**,
which is the shape this datasheet gives the two arbitration signals and their different
meanings.

**The counter's conversion is now a measured quantity rather than an assumption.** The firmware's
time helper reads a Hub register, shifts the value **left by three and divides by ten** — a
multiply by `0.8`.

**State the threshold as the converted-counter predicate, not as a duration, because the
duration depends on a number nobody has established.** The firmware computes
`converted = floor(8 * raw / 10)` and succeeds only when
`converted_now > converted_start + 250`. Because conversion floors each absolute reading, no
single raw-tick boundary is equivalent for every starting residue. A qtest that does not
control `raw_start mod 5` must advance the raw counter by at least 314 ticks to guarantee
success. A boundary test may instead fix the starting residue and calculate the exact
transition. Physical duration remains unknown until the raw counter's tick rate is established.

**The 1.25 MHz that follows from reading `0.8` as microseconds-per-tick is the firmware's
assumption, not a measured property of the hardware.** Linux's own IP27 timer says so
explicitly: its author calls the equivalent constant a hack, records that it needs figuring out
dynamically, says 800 ns was chosen because it **works well across** Hub frequencies of 360,
380, 390 and 400 MHz, and asks outright which clock rate feeds the counter. **So a second guest
hard-codes the same number while stating it does not know the rate and picked one tolerant of
variation** — which is a source recording a non-answer, and it would invert if cited as
agreement. This datasheet cites it as neither.
The conversion is the part the measurement establishes. The 250 threshold is the consumer's
validation that a sampled address remains visible. It is not a publisher dwell time. It does not
require an absent or zero-valued transaction between grants. A later address-only grant supplies
the required replacement. The scheduler supplies eligible publication ticks independently of this
Hub-counter check.

The caller-supplied deadline is separate from the 250 threshold. On expiry the wait returns its
error and the caller retries from the liveness phase. Both values are measured against the Hub
real-time counter, not a controller clock. The counter rate therefore affects the consumer wait,
but it does not define the controller's one-millisecond publication cadence.

**One entry-time early-out, observed but never seen to fire — tagged uncertain.** Before the
liveness phase the routine reads a flag word and a failure counter, and if the flag is set with
the counter at five or more it returns a distinct error immediately, ahead of phase A. Its
meaning is not established — it reads like a *controller-has-given-up* latch after repeated
failures — and it has not been observed firing on any of our boots, so a model need not
reproduce it until something is seen to trigger it. Recorded as a mechanism so that a distinct
early error, if one ever appears, is not mistaken for a new defect.

**The warning, and it is counter-intuitive enough that it will otherwise be "fixed".** On our
machine today that status register answers a constant `0x81` — the pending-interrupt and
bus-not-busy bits — on every one of tens of thousands of reads, and the data register answers
zero on every one. **Do not treat the constant `0x81` as the defect.** Had bit 0 been clear
instead, the loop would have skipped the data read and **timed out on exactly the same
schedule**. The outcome is decided by no expected byte ever arriving, not by which idle status
is presented. Anyone who reads the invariant status as the bug will change it, watch nothing
move, and conclude the whole approach is dead — having only swapped one path to the same
timeout for another. **The status value is not load-bearing; the absence of a signal is.**

### The signal formula, confirmed across a release boundary, and the Origin 200 pairs

**An earlier revision of this section recorded an open question — whether different callers
could expect different pairs — on the strength of a first disassembly reading that the values
arrived as caller arguments. That reading was withdrawn by the person who made it. The values
are computed inside the wait, from machine state, so all three callers get the same pair and
the question is closed.** The correction is recorded rather than silently dropped because a
model was very nearly specified around the open version.

**What replaced it is the strongest confirmation anything in this document has.** The
disassembly followed the computation through and observed its terms; independently, the source
this datasheet was written from gives a formula. **They agree exactly, once one accounts for the
compiler having folded a subtraction into a constant** — the source subtracts one from the slot
index before doubling it, and the binary carries a base constant two lower with the raw slot
index instead. Same arithmetic, different factoring, identical output. **That is a source
reading from one release and a measurement of a different release's binary agreeing to the
byte**, which is worth more than either alone and is the kind of independence this project
usually cannot get.

The valid Origin 200 assignments follow directly from the formula:

| Physical module | Local CPU | Processor code | Grant pair |
| --- | ---: | ---: | --- |
| 1, master | 0 | 0 | `0xe0` / `0xe8` |
| 1, master | 1 | 1 | `0xe1` / `0xe9` |
| 2, slave | 0 | 2 | `0xe2` / `0xea` |
| 2, slave | 1 | 3 | `0xe3` / `0xeb` |

### How wide the encoding is, and why the Origin 200 schedule is narrower

**The processor-code field has exactly eight values, and the protocol's own encoding fixes
that bound.** A product's physical schedule may use fewer processor codes and may include a
controller position that is not a processor grant.

The message-waiting indication is **bit 3** of the signal byte: the second value is the first
with `8` added. **So the processor code must fit in three bits**, or one processor's
"message waiting" byte would be identical to another processor's "nothing waiting" byte. That
bounds the code at **0 through 7**, which makes the first signal `0xe0` through `0xe7` and the
second `0xe8` through `0xef`.

**Eight is also the full range the wider software interface can supply**: the code is twice the
node's slot index plus the processor number, and four node slots with two processors each produce
eight distinct codes.

**And the firmware confirms the bound from a third direction, which is what makes this a
derivation rather than an arithmetic coincidence.** The one configuration that has *more* node
slots than that carries an explicit rule **forbidding the extra slots from talking to the
system controller at all** — the two slots whose codes would exceed seven and collide with
another processor's message-waiting byte. **The protocol cannot represent them, so the firmware
excludes them.**

The encoding range is eight processor codes: first signal `0xe0`–`0xe7`, second
`0xe8`–`0xef`. That does not make every product's physical schedule eight processor grants. A
two-module Origin 200 schedules codes 0 through 3 and a fifth position for the slave controller.

### Origin 2000 grant handoff and the duplicated-slot collision

The [address-only grant contract](#origin-2000-address-only-grants) defines the
wire transaction and the model's bounded observation approximation. The module
controller publishes one grant on each eligible one-millisecond tick. A busy
tick does not advance its processor counter, and a processor's STOP only makes
a later tick eligible; it does not publish or consume a grant synchronously.

**Established — the grant is a promise of exclusive identity, not a lock consumed by START.**
The PROM waits for the bus to be free and its own byte to remain unchanged for more than 250
microseconds. Once that wait succeeds, the transaction routine samples status once and rejects
bus error, addressed-as-slave, or lost-arbitration. It does **not** reject bus-busy at that
point. It neither tests the `BB` bit in that sample nor reads status again between loading the
target address and commanding an ordinary START. This is deliberate: the source requires the
caller to begin immediately and relies on only one processor matching the held grant. Source:
IRIX 6.5.7 `stand/arcs/lib/libkl/ml/i2c.c`, lines 378–495, 519–688, 718–908, and 943–1120.

**Established — a successful wait creates no durable reservation.** The module controller
receives no acknowledgement that a processor accepted its grant, and START does not consume a
grant bit or token object. The PROM's protection is the wire observation itself: before
accepting a matching token, it first requires an earlier occurrence of that same token to
disappear while the bus is free; during the matching-token hold, any busy indication or token
change restarts the wait. After the hold succeeds, the source warns that the caller must begin
the transfer immediately. The two processors on one Hub also share a Hub-local software lock,
but no corresponding lock spans Hubs. Therefore exactly one processor may act on `0xe0` when
slot and processor identities are unique: physical slot 1, processor 0. The same processor
cannot reuse a stale `0xe0` because its next wait must first observe that signal gone. Sources:
IRIX 6.5.7 `stand/arcs/lib/libkl/ml/i2c.c`, lines 378–495 and 519–579; and
`stand/arcs/lib/libkl/io/elsc.c`, lines 280–313, 340–371, 409–468, and 485–534.

**Established — one processor transfer is one uninterrupted busy interval.** The granted
processor's ordinary START must make every attached PCF8584 report the bus busy. Direction
changes and the closing invalid-address operation use repeated START and retain bus ownership;
they do not create a free interval. The final repeated START clocks `0xfe` as an unmatched
address so every PCF8584 data register no longer contains a grant-like byte. Only after that
address completes does the processor issue STOP, which makes the bus free. The module-controller
source explicitly requires repeated START here so it cannot mistake a direction change for a
free bus and publish a token during another master's operation. The next grant can begin only
on a later one-millisecond tick that observes this final free state. Sources: IRIX 6.5.7
`stand/arcs/lib/libkl/ml/i2c.c`, lines 498–505, 641–676, 692–700, 861–896, and 913–922;
`stand/arcs/elsc/i2c.c`, lines 242–269, 456–483, and 507–579; and Philips
[*I2C-bus controller PCF8584*](https://www.nxp.com/docs/en/data-sheet/PCF8584.pdf), Rev. 4.0,
21 October 1997, sections 6.8.1–6.8.2 and Figure 8.

**Inferred model obligation — preserve the rendezvous timing without inventing consumption.**
Normal PROM execution depends on the granted processor reaching START before another eligible
one-millisecond controller tick can publish a different grant. A model must not publish a new
grant directly from a guest STOP, expose a free interval at a repeated START, or delay the
shared busy indication at some PCFs. Its normal PROM schedule must also avoid turning host
descheduling into several concurrent post-hold requesters. An implementation may need internal
scheduling state to preserve that hardware-visible order, but guest-visible behavior remains
the source-backed wire protocol: periodic address-only grants, no acknowledgement or
consumption operation, and busy deferral from START through the one final STOP.

**Established software precondition; silicon result unknown.** The Philips product
specification defines `BB=0` as bus busy, says access is then impossible, and has the master
flowcharts wait for a free bus before requesting an initial START. Its control-table entry for
`STA=1, STO=0` in slave-receiver mode says to transmit START and the address, but does not define
what happens when the host writes that command after `BB` has become zero. The status-register
descriptions specify that setting `STA` sets `PIN=1`, but do not define whether a request made
while `BB=0` is retained, ignored, or emitted, nor whether that busy condition produces `BER`
or `LAB`.
AN96040 discusses missed START detection, the PCF8584's blind interval, bus status, and lost
arbitration. Its power-up example warns that a controller which missed an ongoing transfer may
disturb that transfer by trying to start its own. That controller does not report busy, so the
example does not define the result when `BB=0`. The published PCF8584 evidence therefore does
not distinguish retaining the request until STOP, ignoring it, attempting it before or after
STOP, or reporting an error. Sources: Philips
[*I2C-bus controller PCF8584*](https://www.nxp.com/docs/en/data-sheet/PCF8584.pdf), Rev. 4.0,
21 October 1997, pages 11–13 and 16–17, sections 6.8–6.9 and Figures 5–6; Philips
[AN96040](https://www.nxp.com/docs/en/application-note/AN96040.pdf), NXP catalog Rev. 2.0,
16 February 2010; publication-body version 0.3, April 1996, with page headers dated
6 January 1997, pages 13–15, sections 6.1–6.4 and 7.1–7.2. Philips
[AN10148](https://www.nxp.com/docs/en/application-note/AN10148.pdf), Rev. 1.0,
22 November 2004, page 9, explicitly gives the later PCA9564 a retained
master-in-waiting request, but does not establish that behavior for the PCF8584.

#### Compatibility approximation: retain one busy initial START

The QEMU model retains one initial START written while another master owns the shared bus. It
sets `PIN`, keeps the address already staged by the host, and waits for that transfer's sole
closing STOP plus the normal
bus-free time (`tBUF`) before starting with the staged address. A STOP command from the waiting
controller, controller disable, reset, or a wire error cancels the request. The one-bit pending
state migrates. This approximation exists to preserve stock PROM forward progress across the
documented interval in which its last `BB`-free observation can become stale before its START
write. It may differ from PCF8584 silicon, whose behavior in this case remains unknown. A clean
implementation review must check the discriminating coverage in
`tests/unit/test-sgi-o2000-module-mgmt-real.c`,
`tests/unit/test-sgi-o2000-module-mgmt.c`, and
`tests/qtest/sn0-o2000-module-mgmt-test.c`.

When several adapters retain initial STARTs behind the same busy transfer, the model releases
them in stable wire-master order, separated by one nanosecond, starting one nanosecond after
the bus-free time. A fresh START written inside the bus-free time is placed at its end and so
goes ahead of every released START, which then re-queues behind it; the processor a grant has
just named is primed to write exactly then, and a released START placed at the same instant
would arbitrate an equal address byte down to a real `LAB` for one of the two. This is
deterministic scheduling for the busy-START compatibility approximation, not measured PCF8584
arbitration or fixed hardware priority. Initial STARTs requested while the bus is free retain
ordinary equal-deadline bitwise arbitration.

A module-controller grant published by the STOP that would release a retained initial START
does not refuse it. The retained START runs after that grant's STOP and bus-free time like any
other, so the transaction runs in the slot the newer grant announced. From 2026-09-11 to
2026-09-14 the model instead discarded the staged address, reported lost arbitration through
`LAB` and cleared `PIN`, on the assumption that software would repeat its grant wait.

**Established on the model.** Byte-level traces of four-node cold boots on QEMU `28bb059757`
(runs `irix-20260914T061939-a29858` and `irix-20260914T062356-325d62`, Origami issue
jamesb/qemu#577) show stock PROM 6.156 reading the controller-reserved block one byte per
transaction, with a START request made while a grant held the wire (status `0x88`, the
controller's participant bit among the contenders) never opening the bus and the processor
moving on to the next offset without returning for the refused one. The status the firmware
read back, `0x0b` (LRB and LAB set, BB free), is established by the model's code path and the
register-contract test that failed on it, not by the traces, which carry no register reads. In each run the node whose
first processor skipped byte 8, the module number, printed the flash-log fallback's vote and
later wrote 1 back into byte 8; nodes whose first processor skipped another byte, and a node
whose second processor skipped byte 8, did not vote. **Inferred.** The refused byte leaves that
processor's cached copy of the block stale, which is how the fallback is reached; no trace event
shows the value the firmware used. **Reported, not retained with a trace.** One boot in twelve on
`ea7e8d747b` (run `irix-20260914T051641-d6d277`) had two nodes vote `-1`, come up in module 0
and stop at the global barrier, the outcome expected when two first processors lose byte 8 and
the four advertised values split two against two. **Unknown.** Published PCF8584 evidence still
does not establish the silicon result for either policy. Running the late START is the
compatibility choice that preserves the firmware's read, and `LAB` is left to report actual
bitwise arbitration loss against a peer that starts at the same instant.

**Evidence needed — observe the command on a PCF8584.** The smallest discriminating capture
uses a second master to hold the bus between START and STOP after the PCF8584 has observed a
free bus. While the PCF8584 reports busy in slave-receiver mode, the host writes the target
address and the ordinary START command. A logic analyzer records SDA and SCL, while the host
records `PIN`, `BB`, `BER`, and `LAB` before the command, during the other master's transfer,
and after its STOP. Whether a START and address appear, when they appear, and which status bits
change would distinguish all four candidate behaviors. The part marking and date code must be
recorded so the result is bounded to the tested silicon revision.

**Established retained run; cause remains bounded.** Run `prom-20260910T095145-add723` used stock
Origin 2000 PROM 6.156, eight R10000 processors, QEMU revision
`2ba590dc18bcb9416e949e1c78185a389081e17d`, and four node-local PROM images. Its retained
2,869-byte trace ends with one initial START attempted while another master occupies the shared
bus, followed by one repeated START attempted part way through a byte. The console and QEMU
stderr streams are empty. The canonical retained `run.json` record supplied the launch identity
and arguments; all streams were read through the supported Origami log interface on 2026-09-10.

**Reported detail from the same run; adapter identity unavailable.** A bounded parent trace
shows the losing path polling a stable `0xe0` with status `0x81`, loading target byte `0xae`,
and then issuing the ordinary START command. Status `0x81` says the adapter still observed the
bus free immediately before the address and control writes. Another transaction subsequently
reaches an NVRAM-page write and repeated START, but the trace does not identify either adapter
or master. The retained evidence therefore does not show two processors accepting `0xe0`.
Source says only slot 1, processor 0 can legitimately accept that token, and its next operation
cannot accept the same stale occurrence. The evidence instead bounds the failure to the
rendezvous: a post-hold processor overlapped another master's START, whether from another
processor or the next controller tick, or the shared busy transition was not visible to every
adapter before the losing START. It does not distinguish those cases.

**Established — the current Origin 2000 model gives every node the same arbitration identity.**
Origami revision `f0942bb4301f86821f3ab4d232cbc93f4245142c` generates the four-node
composition with physical node slots 1, 2, 3, and 4. The generated
`hw/mips/sn0-origin2000-model.c`, SHA-256
`474b5878a37d916347e71125c6eef7a61f417465619a01fffd9224909ce3fcee`, is not tracked in
QEMU. QEMU revision `975239d39b9f7db7ccb7a7ff535d684620fba877` consumes it but does not
pass the slot from the descriptor to the Hub. Its Hub slot/status read instead returns the same
low three bits, binary `101`, for every node; a separate master-module bit differs but is
outside the field the Origin 2000 decode uses. IRIX 6.5.7 maps that low-three-bit value to
physical node slot 3. The live processor-number register correctly distinguishes the two
processors within each Hub, so all four first processors compute code 4 and all four second
processors compute code 5. Sources: the generated descriptor named above, QEMU
`hw/mips/sn0.c`, lines 1134–1178, and `hw/misc/sgi-hub.c`, lines 131–136, 1643–1651, and
1994–1999; IRIX 6.5.7 `stand/arcs/lib/libkl/ml/slots.c`, lines 135–155, 267–290, and 396–408.

**Inferred — that duplicated identity explains the collision exactly.** When grant `0xe4` or
`0xe5` completes, one processor on each of four Hubs can pass the same 250-microsecond hold.
The Hub-local I2C lock serializes the two processors on one Hub, but it does not serialize four
different Hubs. The first node to issue START makes the shared bus busy. The other nodes follow
the established post-grant path, which checks the three error bits but does not recheck `BB`, and
issue their STARTs into the occupied bus. The repeated-START failure is a later consequence of
those interleaved transfers. The lock scope is established by IRIX 6.5.7
`stand/arcs/lib/libkl/io/elsc.c`, lines 280–313, 340–371, 420–468, and 500–534.

**Reported runtime observation — not independently verified.** A parent investigation reports
that stock Origin 2000 PROM 6.156 run `prom-20260910T080522-b5a63c` observed grants including
`0xe4` and `0xe5`. Immediately before a collision it reportedly read status `0x80`, meaning
`PIN` was inactive and `BB` reported busy, loaded `0xae` (the write-direction address byte for
management NVRAM page 7), and wrote control `0xc5`, the ordinary START command. MTTCG run
`prom-20260910T080016-3e58a0` reportedly produced five attempts to START while another master
owned the bus, followed by one partial-byte repeated START. Single-threaded control run
`prom-20260910T074908-8da556` reportedly produced one shared-wire failure delivered to four
PCFs, followed by six module-management reinitialization warnings. The supported launcher
removed these runs, so neither this author nor the independent reviewer could re-read their
artifacts.

**Inferred — the differing error counts reflect scheduling.** Both runs show one logical
transfer being attempted by several nodes; the exact number of reported losing attempts is not
a fixed part of the hardware contract.

**Unknown — no retained evidence establishes a second cross-node lock or a firmware fallback
for duplicate physical-slot identities.** The source has a per-Hub lock and the common grant
scheme. It contains no later bus-free validation that would repair duplicate identities. A
hardware capture could test electrical timing, but it is unnecessary to diagnose this model:
the generated composition already supplies four distinct slots and the Hub currently discards
them.

**Clean implementer handoff.** Preserve one address-only grant on the shared module bus and
make its completed byte visible to every attached PCF8584. Preserve busy deferral, the
one-millisecond controller cadence, the 250-microsecond stable-token check, and the normal
START/repeated-START/STOP ordering. A STOP makes the next periodic tick eligible; it must not
publish a grant synchronously. Keep every PCF busy across repeated START, deliver the closing
invalid address before the final STOP, and do not let host scheduling turn successive grants
into concurrent permissions. Supply
each Origin 2000 Hub with the physical slot identity already present in the machine composition:
the firmware's low-three-bit decode maps raw values 7, 6, 5, and 4 to physical slots 1, 2, 3,
and 4. Keep the live processor number at 0 or 1 within each Hub. A discriminating test should
show that one full `0xe0`–`0xe7` rotation releases exactly one of the eight processors per grant,
while all four adapters still observe every completed grant. The source reader who added this
handoff is exposed to the PCF8584, module-management, and boot-discovery sources named here and
must not implement or independently clear those device changes.

#### Compatibility approximation: place a mid-byte STOP after the byte

This entry rests on the shipped PROM's disassembly and on public Philips documents only. It
uses no vendor source.

**Established from the shipped PROM's disassembly.** PROM 6.156's I2C write routine
(`0x1fc3958c` in `tools/origami disasm`) reads the Hub real-time counter once on entry and
measures the grant wait and every byte after it against that single deadline. When the
deadline passes while `PIN` still reads inactive, it writes `PIN | ESO | STO` (`0xc2`)
immediately and returns an error. The data byte it last wrote can still be on the wire.

**Unknown on silicon.** PDF page 11's STA/STO operation table (Table 7) gives `STA=0, STO=1`
in master-transmitter or master-receiver mode as "transmit STOP" and names no byte boundary.
Nothing published says whether a STOP written during a byte is generated at once, after the
byte, or ignored.

**The model.** An adapter that receives a STOP while its own START, address or data byte is
on the wire holds the request and places the STOP after that byte's ninth clock, where the
chip otherwise stops and waits for its host. `PIN` goes active for the finished byte as usual.
Losing arbitration, a bus error, a wire error or reset drops the held request. So does a START
that goes back to waiting before it reaches the wire, when a peer adapter's START lands first or
the wire refuses the line change: the held STOP then counts as a STOP from the waiting
controller, which cancels the request (see
[the busy-START approximation](#compatibility-approximation-retain-one-busy-initial-start)), and
no transfer opens. The one-bit state migrates. Trace event `pcf8584_stop_deferred` records
each use. A STOP written during a repeated START or another STOP is still unsupported and
logged under `-d unimp`.

Placing the STOP at once would also end the transfer. After the byte is a modelling
preference: it keeps every byte on the wire whole, so no peer adapter or endpoint ever sees a
STOP cut into a byte, a case the wire model does not otherwise represent.

**Why an approximation is needed.** Before it, the adapter dropped the STOP. The byte finished,
the adapter held SCL low between bytes, and the transfer never ended, so the module controller
never published another grant. Every later grant wait on every node ran to its timeout. On
eight-processor io6g Origin 2000 boots this happened in all six traced boots, 7 to 41 seconds
after launch, and was the cause of the peer-discovery timeout in Origami issue #1621. With the
change, 16 of 16 boots passed discovery and the grant ring ran to the end of every trace
(QEMU `db8bae1e74`, measured on its parent). On QEMU `7dae4db1c7`, which adds the
withdrawn-START case, 24 of 24 boots passed discovery; none of them held a STOP.
`tests/qtest/sn0-module-management-test.c`, `/sn0/o2000-module-management/mid-byte-stop`,
writes STOP directly after a data byte and requires the bus to come free and grants to resume.
The same file's `stop-withdraws-released-start` and `withdrawn-start-migration` cover a held
STOP whose START goes back to waiting.

**What would settle it.** On a PCF8584 in master-transmitter mode, write a data byte and then
`0xc2` before `PIN` goes active, and record SDA and SCL with a logic analyzer. Repeat with the
`0xc2` written during the START and during the address byte. The PROM's address loop
(`0x1fc39648`–`0x1fc396b0`) writes `0xc2` on its timeout there too. For each case, record
which bit was on the wire when the write landed, and read `PIN`, `BB`, `BER` and `LAB` back
before the write and after the bus goes quiet. A STOP after the ninth clock with a free bus
and no `BER` or `LAB` confirms this model. A STOP cut into the byte, or none, would need the
model changed. Record the part marking and date code so the result is bounded to that silicon
revision.

### Which pairing carries the message-waiting gating

**Not a fixed one — it is per-processor, and the model must track it per-processor.**

Each processor watches only its own pair of bytes and takes the second of them as *"there is
something queued for me."* The firmware's polling command is issued **only if** that
processor's own arbitration saw its own second value; otherwise the transaction is abandoned
before anything reaches the bus. **So the gating is not a property of the controller's state
but of the pairing between a queue and a processor.**

The retained module controller has one alternate-console output queue and a processor
selection. It raises the second signal for the selected processor while that queue has data.
Automatic selection follows the processor that most recently sent console output to the
controller. A separate pending controller interrupt can raise the second signal for every
processor in its destination mask.

The output command supplies the processor identity used by automatic input selection.
The input response prefix instead encodes the selection mode. The
[console selection contract](#console-selection-and-response-prefix) distinguishes
these fields and their announcement behavior. A fixed slot or callback CPU cannot
replace the output command's selector.

The publisher must derive its broadcast from machine state and not hardcode `0xde`. That
value is correct for this machine as it currently answers, and it moves the moment the
slot-identity register is corrected — which is a change somebody else has good reason to make.
Rotating through the whole set of processor codes is immune to that. A hardcoded pair is one Hub
fix away from a silent hang. In a two-module Origin 200, the master controller is the publisher.
The slave consumes controller traffic but does not advance or publish this rotation.

The address-only scheduler requires a discriminating test that observes one completed address on
each free eligible one-millisecond tick. A busy tick publishes no address and does not advance the
rotation. The consumer must observe retention past the hold threshold, replacement by the next
processor's address, and no zero gap transaction. A steady single value remains a useful negative
case because it must time out in the initial stale-value loop.

**One master owns the Origin 200 sequence.** It presents five positions in order: processor
codes 0, 1, 2, and 3, followed by the slave-controller position. Both PCF data-register views
must expose that one sequence. Two independently advancing PCF-local rotations can disagree
about the active position and do not satisfy the hardware contract. Each processor's first or
second byte still depends on whether that processor has a queued message. Sharing the schedule
does not merge the per-processor queues.

**This is the one part of the protocol only a real ELSC, or a model of one, can drive.** The
historical fixed-idle stub satisfied loops 1 and 3 trivially and could never satisfy loop 2.
The wait runs before *every* transaction, including a single three-byte version query, so a
model that never drives the signal times out before firmware attempts to send anything,
however correctly the rest of the protocol is modelled.

**One path returns without polling the bus at all.** A configuration check runs first, and in
the large partitioned configuration it excludes two node slots from talking to the controller
and returns a no-controller error immediately. It does not apply to our machine, and it is
worth knowing because it is the one way this routine can fail without any bus traffic to
observe.

## What a model must decide that the firmware never reveals

Everything above is what the firmware does. Five things a model needs are **not** observable
from watching it, because the firmware either never exercises them, supplies them itself, or
acknowledges an action before it performs it. They are settled here so that an implementation
does not have to invent them, and so that two implementations would agree.

**Reset values.** Out of reset the chip reports a **local idle/free indication** — the
pending-interrupt bit set, meaning no operation is outstanding, and bus-not-busy set. Every
other status bit is clear: no bus error, no lost arbitration, not addressed as slave, no stop
detected. The own-address, clock-control, interrupt-vector and data registers all read zero,
and the control register is zero, which selects the own-address register. **This matters more
than it looks**: the firmware's very first action is to write the control register and then
read the own address back, so a model whose registers do not start from a defined state has
its liveness check decided by uninitialised memory. A device that is mapped and answering has
not necessarily been reset — reset reaches a device through the bus it sits on, so this model
must be a system-bus device realized on a bus, or every one of these values silently reverts
to whatever zero-initialisation produced.

**Access widths, and the direction the mechanism runs.** The guest issues eight-byte accesses
to both addresses. **The data register's read has a side effect** — it advances the receive
pipeline — so the model must declare the widths its handler is written for, explicitly, up to
eight. Leaving that undeclared caps the implementation width at four while leaving the minimum
at one, and the memory system then splits each eight-byte access into two calls, running the
pipeline advance **twice** and discarding the first result. Nothing reports it: the second call
overwrites the first with a plausible value, every assertion passes, and the packet is short by
one byte per access with no error anywhere. This is the same mechanism that has already cost
this project a firmware-call region, pointing the same way.

**How a first start is told from a repeated start.** The chip sees only register writes, and
the firmware issues starts in **two opposite orders**:

- A **first** start: the address-and-direction byte is written to the data register, *then* the
  control register with the start bit.
- A **repeated** start — both the direction change inside a combined transaction and the closing
  invalid-address step — writes the control register with the start bit *first*, and the address
  *after* it.

**The discriminator is whether a transfer is already in progress, and it is exact.** A control
write carrying the start bit with **no** transfer active means the address is already sitting in
the data register from the write before it, so the start is issued now with that byte. The same
write with a transfer **active** is a repeated start whose address has not arrived yet, so the
intent is latched and the start is issued on the next data-register write, using that byte.
**Both orders occur in every combined transaction the firmware runs**, so a model that acts on
whichever write arrives second fails on the very first command rather than in some corner.

### Shared-module grant pacing

The retained Origin 200 controller source configures a one-millisecond periodic interrupt and
attempts one address-only grant from that handler. The counter covers processor codes 0 through
3 and the slave-controller position, so an idle bus produces a nominal five-millisecond
rotation. A busy bus or controller-owned transaction skips that tick without advancing. The
controller waits for the next periodic interrupt rather than publishing when the bus becomes
free. It advances before an eligible transmission attempt and restarts from code zero after its
five-second recovery reinitialization. This is evidence from the retained 6.5.7 controller
source; no matching 6.5.30 controller image has been recovered.

On a free and idle tick, a pending controller transfer precedes a pending serial
command, and both precede grant publication. A pending controller interrupt
suppresses those two handoffs while an eligible processor grant can still be
published. The [module-management contract](origin2000-module-management.md)
owns the recovery counter and reset interaction.

The firmware polls without a delay. Each processor's four non-own positions provide the interval
in which its own signal is absent, and its own one-millisecond position must remain stable past the
hold threshold. A model must not lengthen a position or insert a sixth timed gap to compensate for
emulator scheduling.

**Drive one master sequence through both PCFs.** The master controller advances through
processor codes 0, 1, 2, and 3, then the slave-controller position. The data byte visible at
both PCFs must represent the same current position. Processor slot decoding determines which
pair a processor waits for. It does not authorize either PCF to synthesize its own schedule.

**When a destructive command's action actually happens.** `rst`, `pwr d` and `pwr c` answer
`ok` and only then carry out the reset, shutdown or power-cycle. That order is a contract, not
an implementation nicety: a caller treats the `ok` as proof the action happened and does **not**
fall back to its own hardware reset — a panic reboot or fatal-error recovery that has seen `ok`
will not try again. So a model must **defer** each destructive action until its `ok` has been
delivered to the caller: performing it earlier tears the machine down before the caller reads
the acknowledgement, and never performing it at all leaves the caller trusting a reset that did
not happen. The exact deferral timing is a model choice with no firmware behaviour behind it.
The deferred-action state machine, and the resolution table for commands that conflict while one
is still pending, live in the fork's implementer spec, `docs/specs/sgi-elsc.rst`. (`rst`'s
specific failure mode — a panic reboot that then does neither — is spelled out where the command
set defines it.)

## The ELSC: command/response protocol

Everything above is how the Hub reaches the bus. This section is what has to answer on it —
the ELSC itself, at I2C address `0x08` — for any of it to matter.
The MSC research document (citation: `docs/library/msc-module-system-controller.md`) already traces the
console-selection outcome-decider in full — which candidate letter reaches the ELSC probe, and
that acceptance is two steps rather than one: the packet's framing and checksum first, then a
separate parse of the response text, which is the step that decides. This section summarizes
only what a model needs to implement and adds the one mechanism that document does not cover
in protocol-mechanical detail — the ongoing input-polling path.

**Every ELSC command is a short ASCII mnemonic, sent and answered inside a checksummed
packet, over one combined write-then-read I2C transaction to address `0x08`.** The request
and response share one framing: a length byte, that many bytes of ASCII text, and a checksum
byte chosen so the whole packet (length byte through checksum byte, inclusive) sums to zero
modulo 256. On transmit the firmware computes the checksum as the two's complement of the sum
of the length and text bytes; on receive it re-adds the length, the text and the checksum and
requires the low eight bits to be zero. Those are two spellings of the same rule, and both
hold.

**Three limits sit alongside the checksum, and a model that ignores them produces packets the
firmware rejects or, in one case, packets that corrupt it.**

- **The received byte count must equal the length byte plus two, exactly.** A short or long
  packet is a format error before the checksum is even consulted.
- **The text length must be under 96.** The firmware's packet buffers are 96 bytes.
- **Nothing bounds the length byte before it is used.** The transfer starts with a 96-byte
  limit and then *replaces* it with the packet's own length byte plus two, unclamped, so a
  length byte above 94 makes the firmware read past its own buffer before any of the checks
  above run. **A model must never emit a length byte above 94**, which caps response text at
  94 bytes and the whole packet at 96. This is a defect in the firmware rather than a
  protocol feature, and it is named here because the model is the thing that decides whether
  it is ever reached.

Nothing validates that the text is ASCII; length and checksum are the only structural checks.

A response is parsed as either `ok` (success, no data), `ok` followed by up to three
space-separated parameters, or `err` followed by a short reason token (`csum` — the ELSC
rejected the command's own checksum; `cmd` — unrecognized command; `arg` — bad parameter;
`perm` — not permitted; `pwr_state` — wrong power state for this command). **The parser is
looser than that grammar** — it skips a fixed four bytes past `err` without checking that the
fourth is a space, matches reason tokens by prefix, and ignores parameters past the third —
so a model should emit the strict forms and must not rely on the parser to reject a sloppy
one.

**The version command is the simplest real exchange and the natural first target for a
model to answer correctly**: firmware sends the three-byte command `ver` inside this framing
and expects back `ok` plus a version string. **The presence probe is defined as "did the
version command succeed", and succeeding takes more than a valid checksum.**

The version call is two steps and the second one decides. First the framed exchange, which
checks the length and the checksum. Then a **parse of the response text**, and the version
call returns the parse's error whenever that is negative. **The parse requires the response
to begin `ok`, followed by either a string terminator or a space**; a leading `err` maps to a
reason-specific error code, and anything else is a response-format error. Either outcome is
negative, and the probe is a not-negative test on the version call, so either rejects the
controller. Both steps live in the ELSC driver `stand/arcs/lib/libkl/io/elsc.c` and the error
codes in the header `irix/kern/ksys/elsc.h`, IRIX 6.5.7;
the module system controller research document (citation: `docs/library/msc-module-system-controller.md`)
records the line numbers.

The routine names are deliberately not reproduced here. They appear only in SGI's own tree,
name nothing a guest can see, and no public source contains them — so under the source-use
policy they belong in a citation rather than in a datasheet, and the file, release and
behaviour above are what a reader needs to check the claim. An earlier draft spelled the call
chain out in SGI's symbols; that was the same class of slip as an internal codename, caught
during this document's review gate.

**So the version string's content is irrelevant and the response's shape is not**, and those
are two claims rather than one. A model that answers `ver` inside correct framing but with
text that does not begin `ok` is rejected by real firmware. This paragraph said otherwise
until 2026-08-12 and the framing is rewritten rather than corrected underneath it, because
what it licensed — a correctly framed answer of any shape — is precisely the model somebody
would have built from it.

**Two people have now read that call chain and agree, and that is worth exactly what it is
worth.** ds-hub found the dropped parse step; irix-structs then read the same driver
independently and reported the same chain with line numbers. Both readings are of one source,
so what they establish is that neither misread it — a weaker claim than the conclusion holding
of the real controller.

**A second and genuinely independent source agrees about that shape, and that is what carries
the acceptance condition rather than the two readings above.** SGI's Origin 200 Owner's Guide
is a vendor operator manual written for a different audience by different people, and it documents
the same controller's replies to an operator typing at it: `ok` on success (page 130), and on
failure `err` with a reason token — `err perm`, `err cmd`, `err arg` (page 130) and `err
pwr_state` (pages 128 and 129). **Four of the five reason tokens agree exactly with the
driver's, from a source that is not the driver, and the `ok`/`err` shape the parse requires is
what an operator is shown.**

The fifth token, `csum`, is absent there, and its absence is what the framing predicts rather
than a discrepancy: a human typing on a serial line sends no checksum for the controller to
reject. **What this does not do is settle the behaviour** — a manual describing what an
operator sees is still a description, and the check that would settle it is the one already
registered: a non-`ok` responder observed rejected against an `ok` responder observed accepted.

**The command vocabulary beyond `ver` is smaller than it looks, because half of what the
driver offers is not an ELSC command at all.** The checksummed ASCII channel carries: the
version query; the message poll and its console-input companion; a test-and-set lock and its
release; front-panel display of a character, a digit or an eight-character message; power
query, power-down with a delay, power cycle and system reset; a DIP-switch readback; a
one-wire identity passthrough; and a heartbeat control. Each is a small, independent
request/response pair in the same framing, with no timing behaviour beyond the transaction
sequencing above.

**Identity and configuration are not in that channel. They are reads and writes of a separate
serial EEPROM on the same I2C bus**, driven by raw transactions the ELSC never sees: the
module, partition, domain, cluster and cell numbers, the supervisor password, the debug-switch
values and the built-in-self-test flag all live there. The EEPROM answers **eight consecutive
I2C slave addresses starting at `0x50`**, with the top three bits of an eleven-bit byte address
choosing among them and the low byte addressing within one. **Whether that is one 2 KB part
consuming eight addresses or several smaller parts is not established here** — the addressing
arithmetic is identical either way and the firmware cannot tell. The eight regions are called
pages below purely as a way to name them; a model may implement it either way. **The ELSC's own reserved
area is page 7**, which independently matches SGI's PROM Technical Reference stating that
NVRAM addresses 1792 through 2047 are reserved for the system controller's use: 1792 is the
first byte of page 7. Two sources of different kinds agreeing on the same boundary.

**A third address is driven directly too**: a single-byte read/write target at slave address
`0x20`, used by a pair of routines the interactive monitor calls. What it is is not established
by the driver, and this datasheet does not guess.

An earlier draft described NVRAM as a page within the ELSC command channel. It is a different
device at a different address reached by a different mechanism, and the correction matters
because a model implementing only the ELSC leaves those addresses unanswered.

**Two notes for anyone checking this list against the Owner's Guide, because two absences there
look like problems and are not.** That manual documents no debug-switch command and no NVRAM
command. Neither absence contradicts anything here: the operator-facing command set and the
firmware-facing one are **different sets**, which this datasheet's own comparison of the two
already establishes, and the switch is something the manual shows an operator setting by hand
rather than reading from a terminal. **Nor is the NVRAM absence evidence for the separate-device
finding** — an operator manual omitting a maintenance command is expected either way. That
finding rests on the driver reading alone and should be cited that way.

**A caution about real-machine boot captures, because the obvious ones are the wrong
generation.** Boot logs from later SGI machines are far easier to come by than ours, and they
print a revision belonging to **a successor ASIC rather than a later revision of the Hub** — a
different part in a different architecture. **So a phrase in such a log naming where the firmware
obtained something is not evidence about how our machine obtains it**, and the gap is bigger
than formatting: the quantity may be sourced differently rather than merely printed differently.

**This is the adjacent-thing trap in its capture-shaped form**, and it is worth stating
separately from the specification-shaped one below, because a boot log feels like an observation
of a running machine rather than a citation — which is exactly what makes it easy to lean on.
**A capture from a successor machine is not a capture of ours with cosmetic differences.**
Established by d1-machine, who supplied two such captures in support of a claim and then
withdrew them.

**One thing the machine's own manual independently places on the controller is the physical
module role.** Every CPU module ships configured as a master. Before connecting two modules, the
operator leaves one master and sets switch 10 on the other module's controller board to Off for
slave; On means master. The switch belongs to a ten-position block on each module, with positions
1 through 9 defaulting Off and position 10 defaulting On. In the current two-module machine
description node 0 is the master and node 1 the slave. Source: SGI's *Origin200 and Origin200
GIGAchannel Owner's Guide*, `007-3708-002`, revision 002, printed pages 53, 56-59 and 77, PDF
pages 79, 82-85 and 103, SHA-256
`ae6857707a113bd63b77a3a8187c81beed76cea1406592a38a735c4aa2c462dc`.

**The Owner's Guide never mentions `rsw` or says how firmware reads switch 10.** It therefore
establishes no encoding for the role in the eight-bit debug-switch value specified elsewhere in
this datasheet. Do not invent one; carry the already-declared per-module master/slave role to the
controller without claiming how the real board conveys it internally.

The two retained RAT guides do not bridge that gap. Both say RAT supports Origin 200 and describe
its MSC as similar to the Origin 2000 controller, but each `rsw` discussion takes its switch
meanings from the *Origin2000 IP27 PROM Technical Reference Manual*. Revision 001 covers switches
1 through 8 and assigns switch 8 to the IP27 global-master choice. Revision 002 extends that list
beyond switch 8 and calls switch 10 unassigned. That list describes Origin 2000 PROM/debug
settings, not an encoding of the Origin 200 chassis-role selector. Sources: SGI's *Remote Access
Tool (RAT) Reference Guide*, `108-0168-001`, printed pages 1-1, 2-1, 4-3, and 4-6, PDF pages 11,
13, 25, and 28, SHA-256
`05b30c9917c9da4fcad31d2691102174a6f5c2824a307309c7a4e955ad6633bf`.
`108-0168-002`, printed pages 1-1, 2-1, 4-9, and 4-12, PDF pages 11, 13, 31, and 34, SHA-256
`c93ab969844e0c72ad9535f4a6b3975412d8841d43dd25ad662b2d03173fd6fd`.

**The Owner's Guide proves the external role effect at this device boundary.** Table 1-1 says a
two-module system contains two controllers but only the master's is active, and explicitly labels
the second AUX port inactive. The guide also tells a two-module operator to attach the AUX cable to
the master. The slave front-panel power switch is inactive, and the master controls the system
power sequence. Source: the same Owner's Guide, Table 1-1 on printed page 5, PDF page 31, and
printed pages 56, 102, 120 and 122, PDF pages 82, 128, 146 and 148. A two-module model must
therefore expose an external AUX character backend only on the master.

**The controller firmware establishes the internal arbitration relationship that the Owner's
Guide omits.** In the 6.5.7 controller firmware, the master emits the rotating processor grants.
The slave does not run an independent local rotation. It waits for its place in the master's
five-position schedule. The positions cover the four processors and the slave controller. The
master and slave PCF data-register views must therefore expose the same master-owned sequence,
including CPU 3's `0xe3`/`0xeb` grant. A permanently inert slave-local bus or two independently
advancing rotations is not consistent with this contract. Such a model may still boot: a
retained healthy run enabled all four processors even though a late snapshot showed an idle slave
PCF. That late state is not the early probe, and it does not license silence for the startup
probe's full converted deadline.

**The cross-module wire path remains an inference.** The Owner's Guide shows only a grounding
lead and the CrayLink cable connecting the two modules. It shows no separate management-bus
cable on printed pages 77-78, PDF pages 103-104. Combined with the shared five-position schedule,
that supports routing the arbitration visibility through sideband conductors in the CrayLink
cable assembly. Neither the guide nor the firmware evidence identifies the pins, electrical
levels, buffering, or whether raw I2C lines or encoded grant state cross the cable. Model the
observable shared rotation, not an unevidenced electrical implementation.

A model does not need to implement all of the ASCII vocabulary to unblock the console path,
but **each unimplemented command should answer `err cmd`** — a real, protocol-legal response —
rather than timing out. That is load-bearing rather than tidy: the console-selection routine
issues a DIP-switch read before it ever probes the ELSC as a console, and both callers of that
read tolerate a negative return but not a stalled bus.

### Console input: `get`, and the signal that gates it

The polling routines the console driver calls whenever it needs fresh input send `get`, and
interpret a response beginning `acp ` as terminal input.

**`get` is issued in the "only if a message is waiting" mode, and that mode is enforced by the
arbitration token rather than by the ELSC.** The transaction helper checks what the
arbitration wait returned and, if it saw `signal1` rather than `signal2`, returns a
no-message error **without putting anything on the bus**. So the sequence a model must drive
is: present `signal2` in the slot of the processor a response is waiting for, and only then
will the firmware transmit `get` at all. **A model that broadcasts only `signal1` never sees a
`get` command, no matter how correctly it would have answered one** — and the correct behaviour
for an empty input queue is therefore to *withhold* `signal2`, not to answer `get` politely.

**The wait returns *which* signal it matched, and the caller decides what that means.** Every
processor watches its own slot for its own pair — a code-2 processor's pair is `0xe2`/`0xea` —
and the wait leaves by one of two exits, first signal or second, handing the caller which one
it was: the second-signal flag is bit 3 of the matched byte. The console `get` is one caller
that *requires* the second — it is issued in the "only if a message is waiting" mode, so a
first-signal exit returns its no-message error. Other callers do not require it: the send path
proceeds on a first-signal exit, its own slot being enough. So `signal2` is not a signal every
processor must be handed or else stall; it is the flag one receive path demands, and the
console-input `get` is that path's code-0 instance. The measured wait is described below.

**Another one-driver reading, and its settling test is the same shape**: a model broadcasting
only `signal1`, observed to receive no `get` on the bus at all, against one broadcasting
`signal2`, observed to receive it. That test is worth running rather than assuming precisely
because a model answering `get` correctly and never seeing one looks, from the guest side,
identical to a model that answers it wrongly.

The response prefix is six bytes. The firmware checks `acp ` and copies the payload
starting at byte six, skipping the mode byte and space. That consumer behavior does
not establish the skipped byte's value. The retained controller contract follows.

### Console selection and response prefix

For the four-node module, a supported console output command carries one decimal
digit from zero through seven, identifying physical slot and local CPU through
the reviewed pairing map. Record that identity as the most recent console
output sender. It is distinct from the input selection mode, which can be
automatic, disabled, or one explicitly selected processor.

When input is queued in automatic mode, announce it only in the most recent
output sender's grant. An explicit input selection instead announces it in that
processor's grant. Disabled input selection announces no queued console input.
Controller interrupt messages retain their separate destination-mask rule.

The retained controller's console input response has a six-byte prefix. In
explicit selection mode, its fifth byte is the selected processor's decimal
digit. In automatic mode, that byte is a period, ASCII 0x2e. In disabled mode it
is a slash, ASCII 0x2f, if software explicitly fetches console input despite the
absence of a console-input announcement. These are source-backed protocol
values for the retained controller revision, not measurements of every shipped
MSC firmware. The prefix does not carry the automatic mode's last output sender.
Do not derive announcement ownership from the input response's fifth byte.

Controller initialization selects automatic input and has no previous output
sender. Queued input therefore does not produce a console announcement until a
supported processor sends output or explicit selection is made. This describes
the announcement condition, not a claim that queued characters are discarded.
The retained fetch path can construct a console response without first proving
that the fetching processor was the selected recipient. Do not infer access
control from the scheduler's announcement.

A well-formed output frame updates the last sender from the wire selector even
when its submitting adapter or callback CPU differs. It does not change an
explicit or disabled input selection into automatic mode. Save and restore the
selection mode and last sender separately. Ordinary non-console commands do
not choose their incidental callback CPU as the console recipient.

The examined output parser performs no selector range or separator validation
at this point. It therefore does not support a claim that malformed output
frames receive a particular argument error. The supported output form remains
one digit zero through seven followed by a space and characters. Malformed,
truncated and other selector forms need separately reviewed behavior before a
model advertises support for them. Do not fabricate an error response to make
an invalid-input test pass.

### Console selection across protocol reinitialization

Protocol reinitialization preserves input selection mode and the most recent
console output sender. It clears the controller's I2C command and queue state
through the existing reinitialization path. This is distinct from controller
initialization, which selects automatic mode with no previous sender. A module
reset or NMI that invokes protocol reinitialization must not be treated as a
fresh controller initialization merely to clear those selection fields.

Tests must distinguish retained selection from retained input. Establish
explicit or disabled mode and a different last output sender, reinitialize the
protocol, and verify the old queued input is gone. Queue new input and verify
the retained mode controls its announcement and prefix. Switch to automatic
mode and prove that the previous output sender was also retained. Test the
automatic mode directly with a nonzero sender as well. These checks do not
establish any reset of a peer adapter, Hub, CPU or peripheral.


The independent reinitialization review (citation: `docs/reviews/origin2000-module-console-reinitialization-source-review-20260908.md`)
clears this retention rule without adding peer reset effects.

### Required discriminating tests

Exercise all eight output selectors through different submitting adapters and
with no executing CPU callback context. For automatic mode, assert the selected
grant and the period prefix independently. Change the last sender and prove the
prefix remains a period while the announced grant changes. For explicit mode,
submit output from another processor and prove both the explicit grant and digit
prefix remain unchanged. Disabled mode must suppress the console announcement.
An explicit fetch in disabled mode is a separate test of the source-backed
prefix, not evidence that ordinary software polls without a grant.

Test delayed completion and migration with different explicit selection and last
sender values. Their meaning must survive independently. Preserve controller
interrupt priority and destination masks. Do not use a fixed response prefix or
callback CPU as an oracle for the announcement expectation.


The independent console selection review (citation: `docs/reviews/origin2000-module-console-selection-source-review-20260908.md`)
clears these bounded source-backed claims and records the exact revision and evidence.

### The arbitration wait, measured on the release we boot

**This is observed behaviour of the 6.5.30 firmware with an instruction-level derivation, not a
source reading.** The 6.5.7 source read here drives the second signal only through the
console-input `get`, so this datasheet had described it as "the processor that has input
waiting." The running 6.5.30 image shows the same wait run by every processor, console or not,
before every exchange — it is the bus-arbitration gate, and the console-input `get` is one
caller of it, not its only purpose.

**The derivation.** The wait-setup code computes both bytes from the processor's own code — a
code-derived first signal, and that value plus eight for the second — then spins until the
arbitration byte matches one of the two, leaving by which it matched. This is the same
two-value, two-exit wait the arbitration protocol derives from the 6.5.7 source, now witnessed
executing on 6.5.30 (observed disassembly, addresses `0xbfc397c0`–`0xbfc39824` in the running
image).

**The freeze evidence, and what it does and does not show.** A code-2 processor on the second
node was caught parked in this wait with both of its signals staged — its first signal `0xe2`
in one register, its second signal `0xea` in two others — its programmed node number already 1.
That confirms the measured shape: a processor watching its own two bytes, either of which
releases it. It does **not** show a stall. The boot reached the console menu with all four
processors, and a re-examination found this was ordinary idle polling of the gate. An earlier
draft read the second signal as an "exit onto the launch path" and this freeze as a slave
stranded for want of it; that reading is withdrawn. The wait releases on either signal, so a
processor is not stranded by the absence of its second one — and what the model did or did not
present at this byte is a fact about the wait, not about a launch.

### When the announcement stands and drops

The retained 6.5.7 controller source recomputes the second signal when it publishes each code.
A pending controller interrupt selects codes through its processor destination mask. Fetching
the message clears the fetching processor's destination bit, while the global pending state
expires after about one second. Otherwise queued alternate-console input selects one processor
explicitly or selects the processor that most recently sent console output to the controller.
Clearing either pending condition removes the indication from later grants.

An unconsumed ordinary command response does not itself select the second signal in this source.
The earlier model behavior that kept the indication armed for a response owner is therefore not
part of this source-reviewed contract. The measured 6.5.30 PROM wait establishes how a Hub
consumes the two values. It cannot establish the scheduling policy inside the separate physical
module controller.

### Console output: `acp` in the other direction, and the count that must not be zero

**The output direction was missing from this datasheet entirely, and its omission livelocks
the firmware.** Output through this path is line-buffered because writing to the controller is
slow; the flush routine drains its queue by sending `acp <selector> <characters>` — the same
mnemonic, now as a command — with at most 86 characters per transaction.

**It then parses the first `ok` parameter as a hexadecimal count of characters the controller
accepted, and advances its output queue by exactly that many.** The loop repeats while the
queue is non-empty.

So **a model that answers a non-empty write with a bare `ok` parses as zero accepted, advances
nothing, and the firmware spins forever** — after the console has apparently started working,
which is the worst time to find it. The contract is: **answer `ok <count>` with `count` in
hexadecimal, greater than zero and no greater than the number of characters submitted.**
Accepting all of them is the simple correct answer.

**The hexadecimal part has independent support; the count semantics do not, and the two should
not be run together.** SGI's Origin 200 Owner's Guide documents the delayed power commands as
taking a delay of "a hexadecimal value from `5` to `258`", glossing that range as 5 to 600
decimal — and `0x258` is 600. That is a vendor operator manual, different authors and
audience, stating that this protocol's numeric arguments are bare hex with no prefix and no
marker, in the opposite direction of travel. **So "the numbers in this protocol are
unprefixed hex" is a property of the protocol on two sources of different kinds.** That the
number in *this particular reply* is a count of characters accepted, and that the queue
advances by it, is a reading of one driver and nothing else.

**Unprefixed hex is worth the emphasis because writing decimal fails silently in both
directions**: a count of ten characters answered as `10` advances the queue by sixteen, and a
delay a model reads as decimal turns 600 seconds into 258.

**What would settle the count semantics** — named rather than left implied, because it comes
almost free once the responder exists: a model answering a bare `ok` to a non-empty write,
observed livelocking, against one answering a hex count, observed draining its queue. Until
then this is a driver reading, which is strong evidence about what that driver's authors
believed and weaker evidence about the part.

**Two further sources of bytes on this line, from the same Owner's Guide, that a model of the
output path should expect.** Command echo is on by default after reset, so the controller emits
characters the firmware never sent; and the manual's reset-to-defaults command lists "no
heartbeats" among the defaults it restores, which implies the controller can be configured to
emit unsolicited periodic output. **The heartbeat is a named thing and not a mechanism** —
the manual never mentions it again, and nothing else read here describes it. Recorded so that
unexplained bytes on this line are a known gap rather than a surprise.

This is also why the console path never touches a UART register, which is what this project's
own keystroke experiment measured: the ELSC console is I2C data and control registers plus
this protocol, and there was never a UART on it to touch.

**Where the characters come from is no longer an open implementation question.** The driver's
own comment names this path as pass-through terminal I/O to the UART on the ELSC — the
Alternate Console Port — and SGI's manuals put that port's second connector on the rear of the
module, which is the AUX connector the next section specifies. The firmware end and the
operator end are two ends of one line, so a QEMU chardev on the ELSC device model is what
feeds the `acp` input queue and drains the `acp` output command. **Stated as a reading of two
documents rather than as an observation**: the driver comment and the manual name the same
port, and nothing captured from hardware has been checked against it. What would settle it is
a session driven end to end through a modelled controller.

### The kernel-triggered exchange, and why the write to processor-interface register `0x90` is not a second transmit channel

**The single-CPU IRIX bringup wedge runs exactly the console-input path above, and a capture
of it looks like evidence for a second, non-I2C transmit channel — the firmware stores a byte
to a Hub processor-interface register while it polls the PCF8584 for the reply. There is no
such channel, and a model must not grow a path to carry it.** What our own capture measured
(the black-box wedge work and the committed boot record — our binary, not source): the
kernel-triggered exchange stores a data-dependent byte to Hub processor-interface register
`0x90` — `0x74` or `0x73`, chosen by a selector, with a sibling routine storing `0x72` to
`0x90` and the neighbouring `0xa0` — while polling the PCF8584 data/status pair
(`0x22_0000`/`0x22_0008`) for processor code 0's arbitration signals, first signal `0xe0` and
second signal `0xe8`. Read as ASCII those bytes are `t`/`s`/`r`, which is what first suggested
a transmitted command.

**Read against the register map, that write is interrupt bookkeeping and nothing crosses to
the bus.** Hub-local offset `0x90` is the processor-interface interrupt-pending *modify*
register — a **public** name and behaviour: mainline Linux and the stale linux-mips SN0 header
both place `PI_INT_PEND_MOD` at `0x90` with the comment "write to set pending ints", and both
spell out its encoding, which is that a value with bit 8 set (`0x100 | level`) *sets* pending
interrupt `level`, while a bare `level` with bit 8 clear *clears* it. The captured bytes carry
no `0x100` bit, so `0x74`/`0x73`/`0x72` are **clears of pending interrupt levels 116, 115 and
114**, not characters. Those three levels are reserved by the firmware for **mutual exclusion
between the two processors that share one Hub**, and the selector that chooses 116 versus 115
is the processor-number register (`PI_CPU_NUM`, the same term the arbitration signal itself is
built from): processor 0 clears level 116, processor 1 clears level 115. The firmware brackets
**every** ELSC I2C transaction between taking and releasing this two-processor lock, and taking
and releasing it is precisely what emits these writes. So the byte at `0x90` is the lock, the
poll at `0x22_0000` is the transaction the lock guards, and the two are unrelated stores and
loads. Source for the lock and its reserved levels: SGI IRIX 6.5.7 PROM Hub-lock driver
(`stand/arcs/lib/libkl/ml/hub.c`, `$Revision: 1.63 $`), the write encoding
(`stand/arcs/include/hub.h`, `$Revision: 1.35 $`), and the register definition
(`sys/SN/SN0/hubpi.h`, `$Revision: 1.28 $`) — cross-checked against public Linux
`arch/mips/include/asm/sn/intr.h` and `.../sn0/hubpi.h`.

**"Different register spaces" is real but smaller than it sounds: it is one Hub, one access
window, two chiplets.** Both `0x90` and `0x22_0000` are reached through the single
Hub-register window this datasheet's addresses already assume (the local-hub register alias
SGI reaches as the hub register widget); `0x90` lands in the processor-interface block and
`0x22_0000` in the memory-directory block that carries the PCF8584. No crosstalk to another
device and no I2C touches the `0x90` write — it is a local store to a local interrupt register.

**This is a source reading of one release confirmed by a disassembly of the release we boot,
which is the strongest kind of agreement this document gets.** The committed capture's
disassembly loads the immediate `116`, then `115`, then a sibling `114`, and stores each to
offset `0x90` — the exact level numbers the 6.5.7 firmware computes for the two-processor
lock, from a 6.5-base binary never compared against that source. One caution for whoever builds
on it: in the **kernel's** build of this same driver the lock bracket compiles to nothing (the
kernel serialises the two processors another way) *and* the arbitration processor code is fixed
at 0, whereas the PROM's build does the lock *and* computes the code from the slot. The running
image measured here shows both a code-0 wait (a kernel-build trait) and the lock writes (a
PROM-build trait), which fits the handoff's account of the kernel driving the exchange through
the resident PROM. **Marked open:** which binary emits which in the shipped 6.5.30 kernel plus
6.5-base PROM is not fully settled from the 6.5.7 source held here, and it does not need to be
for the fix — both traits are individually source-explained, and neither depends on the `0x90`
write meaning anything to the ELSC.

**So the fact this section was asked to add is really a subtraction: there is no transmit
channel to model, and the fix is the one already specified here.** The ELSC command channel
stays I2C-only (address `0x08`); nothing a model does at `0x90` reaches the ELSC. The wedge is
the message-waiting gate the console-input path above already describes — the arbitration wait
never saw the awaited second signal *held*, because the model then raised the second signal
only for a non-empty console-input queue, which the kernel's exchange never provides. That
console-input-only raise was the wedge as diagnosed; the corrected second-signal presentation
is now implemented, and current model behaviour is owned by the fork spec
`docs/specs/sgi-elsc.rst` (fork pull request 288). **The source-settled number an implementer
needs here is the processor code the kernel waits on: zero.** The IRIX 6.5.7 kernel-side build
fixes the arbitration processor code at 0, so it waits for first signal `0xe0` or second signal
`0xe8`. The Origin 200 master's five-position sequence includes code 0 directly. No rotation
through unused processor codes is required. Raising `0xe8` for code 0 when that processor has
queued input is the whole fix. No `0x90` handling participates.

**One aside that belongs to the interrupt model rather than to this datasheet.** The model
answers `0x90` as an interrupt-modify that clears a bit — the *clear* half of a set/clear
register. Whether it also implements the *set* half (`0x100 | level`) and reflects both in the
pending-register reads (`PI_INT_PEND0`/`PI_INT_PEND1` at `0x98`/`0xa0`) is an interrupt-model
question, independent of this wedge: the two-processor lock runs only when the firmware believes
more than one processor is present, and a single-processor machine clears bits that were never
set. Recorded so the coincidence of `0x74` with `t` does not send someone building an I2C path
off this register.

## What the console-selection routine does before it probes the ELSC

The version query is not the first traffic the console-selection routine puts on this bus, and
a model that implements only `ver` and `get` meets something else first.

**Sixteen EEPROM reads come first, and they are on the normal boot path.** Before either of
the boot's two console-selection calls, firmware fills a sixteen-byte NVRAM cache by issuing
sixteen separate combined write-then-read transactions to EEPROM slave `0x57` — page 7,
offsets 0 through 15 — each with its own full arbitration wait. **So the first sixteen
arbitration cycles of console bring-up are not addressed to the ELSC at all.** The bytes it
collects are used, so a model answering that page needs its layout. The sixteen bytes are, by
offset: a **magic value at 0**, which the firmware compares against the fixed constant `0x37`
and treats the whole page as untrustworthy if it differs; a **four-byte password at 1**;
**debug-switch values at 5 and 6**, which the console-selection routine reads straight out of
this cache; a **configuration byte at 7**; a **module number at 8**; **self-test flags at 9**;
and identity bytes at 10 through 13.

**Offset 12 is a field name that was inferred once and then read as settled, and this
datasheet will not resolve it.** The driver's own accessors place the partition identifier at
**offset 10** and a cluster identifier at offset 12. The startup code reads **offset 12** and
calls it the partition — twice, once to build a front-panel message labelled for a partition
and once in a range check. **Two consumers of the same byte in the same firmware disagree
about what it means.**

**The layout has since been settled by a third party against a fourth instrument, and it is
the accessors that are right.** The identity fields were measured out of a *compiled* SGI
kernel from a later release, by reading the offset each accessor loads before its page read:
partition at 10, cluster at 12 — agreeing with the header, in a different release, from a
binary rather than from source. The controller's own firmware library defines the same
constants. **Three independent instruments agree on the layout; one site disagrees.**

**Why that one site was hard to find, which is the part worth carrying.** It does not use the
accessor and does not name the constant — it **subscripts the raw sixteen-byte cache with the
literal 12**. A search for the constant, or for callers of the accessors, cannot reach it by
construction, and one such search came back empty and was correctly reported as bounded rather
than as a refutation. Two searches with different reach, both sound, opposite results.

**So the reading that fits is a long-standing defect at that site rather than a layout that
moved** — it reads the cluster byte and labels it a partition, in a message and in a range
check. Stated as the best available reading and not as a certainty: nobody has observed the
consequence, and the site's own module number, read from the same cache two entries earlier,
is correct.

**What that means for a model is smaller than it looks, and worth stating so nobody
overreaches**: only the magic byte has to be right, because it is the only one whose value
changes what the firmware does on this path. The identity bytes are read and displayed rather
than acted on, so any coherent value works — and **none of them should be presented as
authoritative about our machine.** That scoping is now measured rather than argued: **all four
identity accessors have no callers anywhere in the shipped kernel outside the driver that
defines them.** The whole group is write-and-display, which is both why a naming disagreement
could survive in it indefinitely and why nothing a model answers there can mislead a guest.
A model that NAKs those reads leaves the cache holding whatever was on the firmware's stack,
because the return value is discarded and the buffer is never written on failure. **Answering
that page with a coherent sixteen bytes is therefore the better default than staying silent**,
and choosing its contents is an implementation decision this datasheet flags rather than
settles.

**On real hardware those reads always succeed, and that reframes every tolerant path in this
section.** SGI's Owner's Guide states that the power supply feeds the system controller at all
times while the master switch is on, and that the controller stays active until that switch is
turned off — the front panel switch then tells the controller to power everything else. **So the
controller is powered and answering before any processor executes an instruction**, and a real
machine's sixteen EEPROM reads and its switch command all succeed. The failure paths this
section describes — the discarded return value, the buffer left unwritten, the ignored switch
result — are paths **real hardware essentially never takes, and which nobody had reason to
test.** That is the argument for answering the page rather than staying silent: a coherent
answer is the faithful behaviour, not merely the convenient one. It is also a caution, because
a silent model is exercising firmware error handling that may never have run anywhere else.

**Then the routine reads the debug switches, still before the candidate loop.** That read
issues the DIP-switch command to the ELSC at `0x08` — a full arbitration, transaction and
closing invalid-address sequence. The three console-selection call sites differ in one
argument: both of the boot's calls hand the routine the already-filled NVRAM cache, so it uses
those bytes directly; **the interactive monitor's own call passes nothing**, and the routine
then goes to EEPROM `0x57` itself for the magic byte and, only if the magic matches, two
debug-switch bytes.

**The firmware has now said so itself, and this is the one claim in this document with a
guest-side observation behind it rather than a source reading.** With a bring-up serial console
fitted, the shipped firmware reaches a console and its **opening line reports that the system
controller's debug switches were unavailable**, prints its own table of what those switches
select, and then prints a switch value of zero — and carries on booting. **That is the firmware
telling us, in its own output, that it attempted the switch read against our unanswered
controller, failed, degraded to zeros, and continued.**

Worth being exact about what it does and does not establish. It **confirms** that a failed
switch read is survivable rather than fatal, which this document had been asserting from a
source reading of a discarded return value. It says **nothing** about the version query that
follows, which is a different command with a different consumer, and nothing about whether the
console candidate is subsequently reached. And the console it was observed on is one that wins
the selection outright and masks this device, so it is an instrument for watching the firmware
rather than a path this device competes with.

**One consequence reaches past this device and past this datasheet, and it is worth stating
because nobody has been stating it.** The value the firmware settles on after the failed read is
**zero**. So **every capture this project holds — every boot anyone has reasoned about — was
taken with the debug switches reading zero**, because this device does not answer. That is not
a defect in anything and it may well be the same value a real machine with all switches off
would produce. **But it is a condition, not a null**, and it has been going unmentioned in
measurements quoted as though unconditional. Anyone comparing one of our captures against a
real machine, or against another capture, should know that this term was fixed at zero by a
failure rather than chosen — and that it stops being fixed the moment a responder answers,
which makes it something to control for rather than assume.

**No failure here is fatal, and that is the useful part.** The switch read's result is folded
into a status register only if it succeeded; the candidate loop is not gated on it, and the
routine does not propagate its result at all. So a model may answer the DIP-switch command
with `err cmd` and the ELSC console candidate is still reached — **provided the transaction
completes rather than stalling.** This is the concrete case behind the rule that unimplemented
commands answer an error rather than time out.

So the forced sequence, for a boot that reaches and accepts the system-controller console, is:
sixteen reads of EEPROM `0x57`; then `rsw` to the ELSC at `0x08`, whose failure is ignored;
then `ver` to `0x08`, which must return a correctly framed, correctly checksummed response
beginning `ok`. **No `get` occurs before acceptance** — input polling belongs to the session
that follows. Each of those ELSC transactions internally ends with the closing repeated start
to the invalid address, which must NAK.

**That list is complete for the ELSC and the EEPROM and is not complete for the bus.** The
candidate letters ahead of the system controller — the bring-up UART always, the I/O
controller path on two of the three passes — are implemented elsewhere and may issue I2C
traffic of their own; nothing read for this datasheet establishes whether they do.

**Which determination this describes, because there are three accounts of console ordering and
they do not all agree.** This section describes the **candidate loop read from firmware
source**. SGI's manuals separately give a **diagnostics-mode** console table and a **numbered
boot sequence**, and those two give *opposite* orders — the diagnostics table prefers the I/O
controller over this device, the boot sequence reaches for this device first. **Both are
correct for when they run**: at the initial determination the Bridge and PCI do not exist yet,
so a console needing them cannot be a candidate, while diagnostics mode is entered from a
machine that has already got somewhere. **So no sentence here says the firmware "prefers"
anything without naming which determination it means** — a bare preference claim is wrong about
one of the two whichever way it is written, and both sentences come from one manual, so the
citation would look impeccable either way.

**Two of the three accounts agree in a way neither could have borrowed from the other, and that
is worth more than the ordering itself.** The firmware source's *first* candidate list contains
no I/O-controller entry at all, while its later list does; SGI's boot sequence independently
places the initial console determination **before** the step that initializes the Hub's I/O
section, the Bridge and PCI. **A source reading and a vendor manual, arriving at the same fact
from opposite directions: at the first determination, no Bridge-dependent console can be a
candidate.**

**One reading, marked as a reading because it joins two sources rather than observing
anything.** SGI says the initial console is *normally* this device — and the source's first
candidate list does place the bring-up serial port ahead of it. Those reconcile if the bring-up
port is normally absent, which would be what "normally" is doing in that sentence.

**That is now supported, and by the strong kind of negative rather than by a failed search.**
The Origin 200 Owner's Guide's connector appendix **enumerates** the external connectors on the
machine — it states that it describes the following connectors and then gives a closed list of
five, covering the serial ports, the parallel port, the Ethernet port, this controller's own
AUX port and the external interrupt ports. **No bring-up serial connector is among them.** That
negative is bounded by the source rather than by anybody's vocabulary: it does not depend on
guessing what such a connector would be called, because the manual says *these are the external
connectors*. A vendor board drawing agrees independently, its rear edge carrying serial,
parallel and the interconnect and nothing else.

**So a shipped machine has nowhere to plug one in, the firmware's first candidate probes and
finds nothing, and "normally" does exactly the work supposed.**

**State the boundary precisely, because it is narrower than it will be remembered as.** What is
bounded is **the set of external connectors on a shipped machine**. Nothing here says the Hub
lacks such an interface in silicon — it says there is nowhere on the chassis to reach it. For a
candidate that probes and finds nothing, the connector fact is the one that matters; **for any
claim about the Hub's own interfaces it is not evidence at all**, and this document makes none.

**And one trap for whoever checks this**, recorded because it looks exactly like a
contradiction: searching that manual for a third serial port **returns a hit**. Its two-module
block diagram numbers ports across the whole machine, so the second module's ports are numbered
three and four. That is machine-wide numbering, not a third port on one module.

**The candidate loop itself has two gates worth knowing.** It stops at the first candidate that
succeeds, so an earlier candidate winning means the ELSC is never probed on that pass; and the
ELSC case is guarded by a variable intended to carry the chip-initialization result. In the
firmware release read here that initialization call is compiled out and the variable is set to
zero instead, so the guard is open unconditionally — worth recording precisely because it looks
like a gate that could close and, in this release, cannot.

**The first gate is a live hazard for this device rather than a note, and it belongs to another
block of the same Hub register file.** The bring-up serial port is the **first** candidate in
all three of the firmware's device lists, and its presence probe writes a value to one of its
registers and reads it back. **So any model that answers that register block as plain
read-write scratch makes the probe pass by accident**, the loop stops there, and the system
controller is never probed on any pass — the console then points at a register file with
nothing behind it, which is a worse failure than the silence it replaced because the firmware
believes it succeeded. Those registers are a different block from the two this datasheet
specifies, so this is a fence and not a conflict; it is recorded here because the thing it
would silently disable is this device.

## The other end of the console: the rear serial port, and the rule that multiplexes it

Everything above is the firmware side — the Hub reaching the controller over I2C, and draining
terminal input out of a `get` response prefixed `acp `. **This section is where those characters
come from**, and it is the half no source read for the first draft covered. It is taken from
SGI's Origin 200 Owner's Guide, cited by printed page.

**The port is real, standard equipment, and this manual calls it the diagnostic port in its own
words** (page 86, listing what is at the back of the machine). It is the AUX connector, and
every Origin 200 ships with a cable for it — the packing lists for all four configurations
carry one (pages 40, 42, 44, 46).

| Property | Value | Page |
|---|---|---|
| Connector | 8-pin mini-DIN, rear of the CPU module | 18, 120 |
| Protocol | RS-232 | 194 |
| Line rate | 9600 baud | 122 |
| Frame | 8 data bits, 1 stop bit, no parity | 122 |
| Flow control | None, or XON/XOFF | 122 |
| Pinout | 1 DTR, 2 CTS, 3 TXD, 4 GND, 5 RXD, 6 RTS, 7 DCD, 8 GND | 187 |
| Reachable module | Master only | 120 |
| Login | Cannot be used to log in to IRIX | 121 |

**The cable SGI ships for a terminal presents no modem-control signals at all.** Part
`018-8114-001`, page 194, wires transmit, receive and ground and leaves DTR, CTS, RTS and DCD
not connected; only the separate modem cable wires them. **A model that waits on carrier
detect or clear-to-send before transmitting would never talk to the cable in the box**, so the
line has to work with three conductors.

**The escape prefix is Ctrl+T, the byte `0x14`, and it is the entire multiplexing rule.**
Page 122 has the operator test the connection by typing Ctrl+T and Enter and expecting the
prompt `MSC>`; page 123 states the command form as Ctrl+T followed by a command. Page 132, on
the input-routing command: *"Anything typed on the MSC that is not an escaped command is sent
through to the selected CPU."*

So a model of the controller has to make exactly one split on its serial input — a sequence
beginning `0x14` is the controller's own command, everything else is queued for a processor —
and nothing more elaborate than that.

**Input routing has three states and a power-on default that matters here.** The routing
command selects a processor by module and slice; queried bare it answers which is selected, or
`none` if none is; and its automatic mode, **which page 132 names as the power-on default,
selects the last processor to have produced output.** Output filtering defaults to showing
every processor's output, intermixed by line.

**That default leaves a window this datasheet cannot close, and it is the window our machine is
sitting in.** On a machine that has produced no output yet, nothing has been selected, so by
the routing command's own definition the bare query would answer `none`. **What a real
controller does with characters typed before any processor has output anything is not stated
anywhere in the Owner's Guide.** A model has to decide something, and the decision is visible
to the guest: characters could be dropped, or queued against the first processor to speak, or
queued for whichever processor next polls. **Recorded as an open question rather than answered,
because a model's choice here is not a fact about the hardware** — and it is the first thing a
console session through this path will exercise, since our firmware reaches its input wait
before printing anything.

**Supervisor mode gates the destructive commands**, page 124: `pas` followed by a
four-character password, factory default the word `none` on every Origin 200, and a protected
command attempted without it answers `err perm`. A model that implements power-down or reset
commands has to gate them the same way or it will accept a command real hardware refuses.

### The operator command list and the firmware command list are different lists

The vocabulary above comes from IRIX's driver. The Owner's Guide's command reference (pages
130 to 132) documents what an operator can type at an Origin 200's controller: automatic
power-on query and set, an option reset, command echo control, fan speed query and set, raise a
nonmaskable interrupt, supervisor mode and password set, power query and control including
delayed forms, redundant-supply status and configuration, hardware reset, the input-routing and
output-filtering commands above, temperature, and version.

**The two lists overlap in `ver`, power control and reset, and otherwise do not.** The Owner's
Guide documents no display command, no identity command and no NVRAM command — consistent with
an operator manual omitting them, and equally consistent with this machine's controller firmware
not having them. **Which list an Origin 200's controller actually answers is not settled by
either source and should not be asserted from either.** What is settled is that `ver` is in
both, and `ver` is the one command the firmware's presence probe depends on.

**One raw detail worth carrying into any argument parser**: the delayed power commands take
their delay as **a hexadecimal value from `5` to `258`, which the manual glosses as 5 to 600
decimal** (page 131). Arguments in this ASCII protocol are hex-encoded.

### Which firmware-facing commands a boot actually drives, and their wire contracts

The two lists above separate what an operator may type from what the driver defines. This
section is the single spec a model builds the firmware-facing dispatch from, and it is
release-tagged because controller-firmware generations differ. Its repertoire and framing are
authoritative from a disassembly of the **6.5.30** PROM we boot — its firmware-channel command
repertoire — per the release-authority rule. The **6.5.7** IRIX driver is the
cross-check, and reading it settles which commands the firmware and kernel actually *issue*. The
hardware guide's operator-command table is public corroboration where a verb appears in it.
Where a verb has neither public corroboration nor a live 6.5.7 caller, it is documented as
wire-protocol contract from the release we boot — the mnemonics are bytes the guest emits on the
wire, a device contract, in the same practice the console `get`/`acp` sections above already
follow.

**A third cut the two lists do not give: which of these a normal boot issues, versus which are
reached only at shutdown, reboot, or by operator action.** Unlike "which commands the controller
answers" — which neither source settles — this one *is* settled, by reading what the firmware
and kernel issue, and it says which commands must work for a boot and which need only exist.

| Command | Wire form | Driven on a normal boot? | By |
| --- | --- | --- | --- |
| version | `ver` | yes | PROM console probe |
| console input / output | `get` / `acp <selector> …` | yes | PROM and kernel console |
| DIP/debug switches | `rsw` | yes | PROM console selection |
| display char / message | `dsc <pos> <ch>` / `dsp <≤8ch>` | yes | PROM boot-status display |
| power-on query | `pwr q` | **yes** | PROM cold-power-on detection |
| heartbeat / NMI watchdog | `hbt <i> <d>` | **yes** | kernel watchdog init |
| test-and-set | `tas <ch>` / `tas 0` | **6.5.30: yes, in arbitration; 6.5.7: no** | firmware arbitration |
| system reset | `rst` | **no** — panic reboot and fatal-error recovery only | kernel / standalone |
| power-down / power-cycle | `pwr d <sec>` / `pwr c` | no — shutdown, reset recovery | kernel, firmware error path |
| redundant-power query | `pwr r` | no | kernel and operator |
| module one-wire identity | `nic <p> <s> <d>` | conditional — PROM module-inventory publication, skipped when the module has a populated non-memoryless first node | PROM and operator |
| flush (6.5.30 table) | `fl` | see release note | — |

**For a boot to work the model must answer `ver`, `get`, `acp`, `rsw`, `dsc`/`dsp`, `pwr q`, and
`hbt`; on the 6.5.30 firmware it must also answer `tas`, because the arbitration path drives
it.** Everything else can be a correct responder without being on the boot path — reset most of
all: a boot never issues it, so real reset is a feature-completeness matter, not a boot blocker,
but it is the one command that today *looks* done and is a no-op, which is worse than
unimplemented (below).

**The error vocabulary is a fixed set of `err` tokens.** Every command answers `ok` — alone, or
followed by space-separated result fields — or `err <token>`, with the token one of `csum`
(packet checksum bad), `cmd` (unknown command), `arg` (bad argument), `perm` (permission
denied), or `pwr_state` (not allowed in the current power state). `pwr_state` is newer than the
rest — it belongs to the 4.x-generation controller — so a model may answer it where a state
truly forbids a command, and a model that never emits it is consistent with older firmware.
Anything that is neither `ok` nor a recognised `err` token reads as a format error.

**Argument encoding is not uniform — three kinds.** Most arguments are hex — the `pwr d` delay,
the `nic` triplet, the `dsc` position — matching the manual's hex-arguments rule. Two request
fields are decimal: **`hbt`'s two arguments, and the processor selector in
`acp <selector> <chars>`** (a decimal digit). And one is neither hex nor decimal: **`tas`'s lock
byte is a raw literal character**, passed and returned as-is — `tas A` carries the byte `A`, not
the value `0x0A`, and any byte, non-hex ones included, is a valid owner token, because the caller
must recognise its own byte round-tripped exactly (hex-normalising it would break the lock; see
its contract below). Do not carry the decimal over to the response either: the **`acp` reply's
`ok <count>` — the characters-accepted the firmware advances its output queue by — is hex**, as
the console-output section above establishes. A parser that hex-parses the heartbeat arguments
misreads the interval; one that answers the acp count in decimal livelocks the flush; one that
hex-normalises the `tas` byte breaks the lock's round-trip.

**Two framing details beyond the packet layout given earlier.** The response count is below 96,
the packet maximum. And the controller's two bail paths differ: on an **error status** it writes
an I2C STOP; on a **timeout** it does not. A model that always issues STOP, or never does,
desynchronises a driver that distinguishes the two.

**The per-command contracts.** Console `ver`, `get`, `acp`, and `pas` are specified in the
console sections above; the firmware-facing remainder follows.

**`tas <ch>` / `tas 0` — test-and-set, the arbitration lock.** `tas <ch>` writes the literal byte
`ch` (a raw character, not a hex value) into the controller's lock cell and answers `ok <prior>` — the byte the cell held *before* this
request, a classic test-and-set returning the old value: `0` if it was free, so this caller has
now taken it, or the current holder's byte if it was not. `tas 0` releases it. A caller polls
`tas <ch>` until the response reads `0` (was free, now taken) or its own `ch` (already held),
retrying on any other value. On 6.5.30 the firmware's arbitration path drives this —
the live use of the mnemonic; see the release note. **Provenance:** the two request forms are
confirmed in the 6.5.30 disassembly; the response semantics stated here — the pre-write
`ok <prior>`, the prior-holder byte, the poll-until-`0`-or-own-`ch` loop — are read from the
6.5.7 driver's lock
wrappers, which are dead code on 6.5.7 and unverified on 6.5.30. What settles them: a
disassembly of the 6.5.30 call regions, or one measured exchange once a model answers `tas` at
all.

**`tas` is not the bus arbitration; it is a service carried over the bus like any other
command, and it sits at the top of a three-layer serialization stack a model must implement
coherently** — collapsing them into one is the trap:

- **The Hub-local two-processor lock** — the interrupt-level bracketing the kernel-triggered
  exchange section describes — serializes the two processors that share one Hub.
- **The bus-arbitration slot wait** — the signal-pair wait the arbitration section above
  specifies — arbitrates the bus per transaction among processors. Every exchange passes it,
  `tas` included: a `tas` transaction must itself first win the wait, which is why the lock and
  the wait cannot be one mechanism.
- **The `tas` lock cell** — the service on top, plausibly the cross-module or global election,
  though that purpose is not verified here.

The three have different scopes and must stay distinct.

**`dsc <pos> <ch>` / `dsp <chars>` — the alphanumeric display.** `dsc <pos> <ch>` sets one
character: `pos` a display position, a hex digit `0` to `7` (the eight positions), `ch` the
literal character; a digit is sent as its ASCII character. `dsp ` followed by **up to eight**
non-NUL characters overwrites the display from position `0`, leaving any positions past the
string unchanged — the guide's own example writes seven. Both answer `ok`. (The hardware guide
documents `dsp`/`dsc`; the landed display model implements the same 0-to-8 range.)

**`pwr q` — power-on query (boot-driven).** No argument; answers `ok <hex>`, `1` if **this
controller's own module** was just cold-powered-on and `0` otherwise, and the controller
**clears the flag once read** — a second `pwr q` to the *same* controller in one power cycle
answers `0`, so a model one-shots it per controller. The latch is **per module, not
machine-wide**. In a two-tower Origin 200, each node occupies its own module
and queries that module's controller. This per-node interpretation does not
transfer to the four-node Origin 2000 module's shared MSC. Its per-reader
notification behavior is not established by this topology argument. See the
[Origin 2000 module wiring](origin2000-module-management.md). The machine-wide state is then built by **advertise-and-record**, not
by one node telling the others — each node advertises its own power-on state in its discovery
scratch register, and peers read and record each other's. (The clear-on-read and the per-module
scoping are read from the 6.5.7 driver and boot path, unverified on 6.5.30; a measured exchange
settles the clear-on-read. An earlier "only the first node sees `1`" was the firmware header
comment's simplification, which the advertise-and-record path contradicts.)

**`pwr r` — redundant-supply query.** Answers `ok` followed by two per-module status records — a
master record and a slave record — each colon-separated as **tag : present-flag : health**:
`m:y:ok` (master present and working), `m:y:fl` (present but failed), and `m:n` (not present — no
health field), with `s:y:ok` / `s:y:fl` / `s:n` for the slave. A fully healthy answer is
`ok m:y:ok s:y:ok`. The driver takes the master record normally and the slave record only on a
single-node desktop non-master, reading health by **substring** (`fl`, then `ok`) within the
record and treating an unclassifiable record as unavailable — so the parse also accepts a shorter
form, but the firmware's real output carries the present-flag, which is what tells an absent supply
(`m:n`) apart from a failed one (`m:y:fl`). Kernel and operator, not boot. (Response format from
the public Origin 200 owner's guide, pages 129 and 131. The 6.5.7 driver's own test fixture used a
shorter one-colon `m:ok` form — a minimal input to its substring parse, not the wire format.)

**`pwr d <sec>` / `pwr c [<sec>]` — delayed power-down and power-cycle.** `pwr d <sec>` schedules
a power-down in `sec` hex seconds (`0` = immediate). `pwr c` takes an optional hex interval from
`5` through `258` (5 through 600 seconds), defaulting to 5 seconds. It queues local power-off
and starts its power-up timer at command processing. The timer uses one-millisecond controller
ticks, and expiry queues local power-up. The configured interval is therefore not an exact
physical off interval. On I2C, command processing begins at the write-to-read transition. The
power-off request and timer precede response construction, so unread `ok` does not delay the
cycle. Shutdown and reset-recovery paths, not boot.

In a two-module Origin 200, a power cycle issued through the master controller queues peer
power-down and power-up requests around its corresponding local actions. Those requests wait for
their controller bus grant and are retried at most five times. The master still performs its local
action when a peer request is dropped after those failures. The source does not establish
simultaneous power edges, an exact inter-module skew, or a peer power-on sequence and `pwr q`
re-arm after failed delivery. This is a cold restart, unlike `rst`. A direct I2C `pwr c` addressed
to the slave is accepted and cycles that module only. Only a master-originated cycle sends the
paired down/up requests to its peer.

Overlapping cycles retain each controller's own countdown rather than creating a combined
machine-wide hold. A second `pwr c` accepted by one controller replaces that controller's local
power-up countdown, including while its module is already off. A peer power-down request received
while the slave is already off does not replace the slave's earlier local countdown. The slave can
therefore power up on that earlier deadline before the master's later peer power-up request; the
later request is a no-op when the slave is already powered. It does not reinitialize the slave or
re-arm the slave's `pwr q` latch. A model must not extend this into reference-counted power holds.

**`rst` — module reset.**

`rst` takes no argument. It returns `err cmd` while the addressed module is unpowered. On I2C, the
write-to-read transition accepts the request. The controller then raises its interim Hub reset
indication and schedules a fixed 1,000 ms wait before constructing bare `ok`. An unread response
does not stop the timer or prevent the reset. `ok` acknowledges acceptance, not a completed reset.
The observed interim indication is outside the current model contract and must not be treated as
the reset pulse. The front-panel reset button is accepted only by the master controller. There is
no reset forwarding to the other module, so a two-module Origin 200 does not turn one controller's
`rst` into a whole-machine reset. A model must schedule the addressed module reset or answer an
`err` token.

After the fixed wait, the controller asserts reset for 100 ms. Processors must remain inactive
throughout the pulse. The evidence establishes reset only of the Hub's attached I2C interface and
does not settle the wider Hub, Bridge, or PCI-device domains. The controller remains powered and
runs its timer during the pulse. When the addressed controller is the master, eligible arbitration
publication continues while the bus is free. The slave controller does not publish the grant
rotation, so it has no local rotation to preserve. Resetting the slave leaves the master's
publication state unchanged. At pulse release,
the reset controller reinitializes its management-I2C timeout, command and response state,
queues, and grant position. Its first subsequent eligible publication is the first code in the
rotation. The reset does not re-arm `pwr q`.

Reset and module power remain separate state when their timers overlap. If `rst` is accepted while
the module is powered and `pwr c` removes power before the reset deadline, the reset pulse can run
and release while the module is still off. Releasing the reset signal does not make processors or
the Hub-side I2C interface active without module power, and it does not cancel or reload the
power-up countdown. The reset routine can delay when the controller services an already-expired
power-up countdown. A `rst` request first received while the module is off still returns `err cmd`.

This two-module reset and power-cycle contract is source-reviewed from the 6.5.7 controller
firmware and has passed independent review. The public Origin 200 Owner's Guide corroborates the
master/slave controller roles and the reset and power commands. The later firmware release used
by the emulator has not been observed exercising these destructive commands.

**`rsw` — DIP/debug switch read.** No argument; answers `ok <hex>`. The byte is active-low — the
driver complements it — so the wire value is the inverse of the logical switch state.

**`nic <p> <s> <d>` — one-wire identity access.** Three hex arguments (pulse, sample, delay)
bit-bang the midplane one-wire line; answers `ok <hex>` with the sampled bit, and a one-wire ROM
read is layered over repeated calls. The interactive monitor can drive it directly. Normal PROM
startup also drives it while publishing module identity, but only after a topology check. A module
with a populated, non-memoryless first node returns before the command. The Origin 2000 four-node
run recorded in the independent review took that return, so it does not establish whether the
controller accepts the configured one-wire image.

**`hbt <interval> <delay>` — heartbeat / NMI watchdog (boot-driven, by the kernel).** Decimal
arguments: enable monitoring so a missed heartbeat raises a non-maskable interrupt after
`delay`; `hbt 0` disables; `hbt` alone sends one heartbeat. All answer bare `ok`. The kernel
only enables it, so a model that answers `ok` and takes no watchdog action carries a boot.

**`fl` — 6.5.30 firmware table.** Present in the disassembled repertoire; its argument and effect
are not established from the 6.5.7 read, where `fl` occurs only as a `pwr r` response substring
(the failed-supply status), never as a command. Documented as a 6.5.30 verb pending a use site.

**Release notes — the 6.5.7 driver against the 6.5.30 firmware table.**

- **`tas` is driven in 6.5.30 and dead in 6.5.7.** The mnemonic is identical — `tas <ch>` and
  `tas 0` — but the 6.5.7 driver's test-and-set wrappers have no callers, so 6.5.7 never issues
  it, while the 6.5.30 firmware drives it in the arbitration path. The wire contract is
  release-stable; only whether the firmware exercises it changed. (This corrects an earlier read
  of the 6.5.7 lock API as merely dead: the mnemonic is live on the release we boot.)
- **`fl` is in the 6.5.30 table and not the 6.5.7 driver** — see its contract above.
- **The 6.5.30 repertoire read here is the disassembled firmware-channel set, not proven
  exhaustive.** Verbs the 6.5.7 driver defines but this 6.5.30 list does not name — `pwr c`, the
  module and partition identity setters, `pas` and its NVRAM backing — are documented from the
  6.5.7 read and left open as to whether the 6.5.30 firmware also answers them.

## The acceptance observable: a value the firmware itself will show, predicted in advance

**This device finally has an externally visible success signal, documented by the vendor and
fixed before any of it is built.** SGI's power-on diagnostics manual gives the firmware's
console-selection order as a table the firmware checks in sequence, and **states the condition
for choosing this device in its own words**: the system controller's UART is selected *if no
I/O controller is available and the controller is responsive*. Each selected console is then
**blinked as a two-value pattern on the node indicators**, and the manual gives the value for
each.

**So the firmware announces which console it chose, in a documented code, and the code for this
device is known.** Our machine currently blinks a value matching **neither** the I/O
controller's nor this device's.

**That gives a prediction rather than a hope, and the distinction is this project's own scar
tissue.** The earlier attempt on this bus published its kill condition as *"make the bus answer
anything and see whether the trace diverges"* — a condition a status-only stub satisfied while
the hypothesis went untested. **The condition here is a specific value: with a responding
controller, the blinked console code must change to the one the manual assigns this device.**
It cannot be satisfied by a model that merely stops timing out, it does not depend on reading a
trace for divergence, and it is registered here before the implementation exists so that nobody
can pick the target after seeing the result.

**The prediction has a precondition, and until it is met a run cannot test the prediction at
all.** The firmware only listens for the processor code its *own* arithmetic produces, and that
arithmetic runs on the Hub's slot-identity answer. **While that answer decodes to an invalid
slot, the firmware waits for a code outside the representable range — which a correct controller
never broadcasts, because it is not a code.** So a correctly implemented responder and a
correctly implemented firmware **cannot hear each other on this machine today**, and the console
will not be selected however right the controller is.

**That makes the controller and the slot-identity answer a co-requirement rather than two
improvements in sequence**, and it has a direct consequence for anyone running the acceptance
check: **a run showing the console code unchanged is not evidence against this device or against
the prediction.** It is the expected result while the precondition is unmet, and it discriminates
nothing — the subject cannot see the change the experiment is meant to detect. **That is the
mis-scoped experiment this bus has already produced once**, in the form of a stub that satisfied
a published kill condition without testing the hypothesis, and it would be produced a second
time by reading a null result here as a verdict.

**So the acceptance check is worth running only after the slot-identity decode is corrected**,
and until then the honest report of a boot is *"the precondition is unmet"* rather than
*"the console did not come up."*

**Three things to hold about it, in decreasing confidence.** The selection order and the code
for this device are **vendor statements**. That our machine currently blinks neither documented
value is a **measurement**. What our machine actually fell through to instead is an
**inference** belonging to whoever owns that decode, and this datasheet takes no position on
it — the prediction above works regardless of what the current value means, because it is about
the value *changing to a known target*, not about explaining the value we have.

**A corroboration recorded here earlier is withdrawn, and the reason is worth more than the
claim was.** It said the capture emitting this device's probe indicator three times
independently confirmed the source reading that the selection runs three times. **Two things
kill it.** SGI's own numbered boot sequence has **two** console determinations, not three, so
the number does not come from the vendor at all — either the source reading counts candidates
inside one determination rather than counting determinations, or our machine repeats something
a healthy one does fewer times. And **one of the two agreeing instruments is our own machine in
its failure state**: if the repetition is a retry driven by the defect, the count is a product
of the defect rather than a property of the firmware, and it corroborates nothing.

**It was withdrawn by the person who supplied it, and taken on trust here without the check
this document applies elsewhere** — that an observation of our own machine cannot be evidence
about the firmware unless it adds information we did not put in. A count produced by a failing
machine is exactly that shape. Kept visible rather than deleted because the appealing part was
the phrase "different instrument, same number", and that phrase is only worth something when
both instruments are independent of the thing being explained.

**What replaces it is stronger and needs no count at all.** SGI's numbered boot sequence states
that **the initial console is normally the alternate console port on the system controller**,
determined at a step that runs **before** the step initializing the Hub's I/O section, the
Bridge and the PCI bus. **So this device is not competing with the I/O controller for
selection — it is the one the firmware reaches for first**, at a point where no console needing
a Bridge can exist yet, and the later determination is a *switch away* to base-I/O if a console
port is found there.

**That changes what the model must do and when.** It has to answer at a point in the boot where
essentially nothing else is initialized — no crosstalk link, no Bridge, no PCI. And the step
immediately before the console determination is **the debug-switch read from this same
controller**, so a controller that does not answer loses the switch read and then the initial
console, in that order. **Our own capture shows exactly that order**: the first line the
firmware prints is the switch-read failure. A vendor sequence and a captured string agreeing
about ordering, from opposite directions.

**One scope caution, because these are two different accounts in one manual.** The
diagnostics-mode console table and this numbered boot sequence describe different situations,
and merging them would be the adjacent-thing error this datasheet has already had to correct
once. Both are recorded; neither is used to complete the other.

**And the release caveat at the top of this document is not hypothetical for this section
specifically — the two firmware images we hold disagree about this ordering, measured.** Their
progress indicators show the image we boot **initialising a serial port before locating any
console**, and the later image **announcing that it located a console first and then initialised
what it found**.

**That distinction between doing and announcing is not pedantry, and the later image's half of
the claim has since been qualified.** Counting device accesses in the window after the
console-located indicator, with one binary and only the firmware image differing: **the image we
boot writes to a serial region seventy-six times; the later image writes to it zero times, and
its only write target of any kind is the indicator register itself.** No unassigned accesses and
no unimplemented-register warnings either — **so it is not writing somewhere we fail to model.
It is not writing.**

**So on the later image, the console-located and UART-initialised indicators are backed by no
device work at all.** They are the firmware's claim about itself, and nothing on this machine
substantiates them. **The ordering difference between the images is therefore a difference in
what they announce, and only the earlier one's announcements are known to correspond to
actions.** Measured by d1-fault3, who corrected their own earlier reading of the same indicators
to produce it.

**The instrument lesson generalises past these two images and this project leans on that stream
constantly: a progress indicator can report work that did not happen.** It is one register write,
and whether anything followed it is a separate question with a separate answer. **Pairing an
indicator with the accesses attributed to it is what separates the two**, and it is cheap.
So *"the firmware locates a console at this point"* is a claim about **which image**, and this
document's account comes from source for neither of them.

**That is worth more than a caution because it is the first measured evidence that release
differences reach the console path at all**, rather than a general worry that they might.
Recorded from d1-machine's paired captures; the reading that an early unconditional output path
explains it is theirs and is held as inference, since the console descriptor the firmware reads
is ours and *"which console it chose"* is partly a claim about what we advertise.

**And one observation cutting the other way, carried because it was given honestly.** The
firmware never emits the probe indicator for the bring-up serial port — zero occurrences — while
emitting this device's three times. That weakens one reading of what our machine currently
selects without eliminating it, since firmware may select a console it did not announce
probing. Recorded because a finding that survives only by omitting its counter-evidence is not
a finding.

## What the vendor's own manuals say, and how far it carries

Three statements from SGI operator manuals, recorded because they are a **different class of
evidence** from the driver readings the rest of this document rests on — a vendor describing
its own machine to a customer, rather than us inferring from code. All three are read from
the Origin 2000 owner's guides (citation: `docs/library/origin2000-owners-guides.md`) with their printed
pages named there.

- **The name is the vendor's, not ours.** The controller is expanded as the entry-level system
  controller in the manual's own words. This datasheet was filed under that name on the
  strength of driver source; the manual uses it directly.
- **The bus and its rate are in the controller's own feature list**: an I2C link at 100 Kbps to
  Hub I/O space on each node board. **That is consistent with what firmware programs rather
  than in tension with it** — the chip's own speed options are 90, 45, 11 and 1.5 kHz, and 90
  kHz is its nearest setting to the 100 Kbps standard-mode nominal the manual quotes. Two
  descriptions of one link at the granularity each audience needs.
- **The fallback-console role is stated outright rather than inferred.** The manual says the
  controller also communicates with the node boards when the I/O console port or a system
  console is not available or functional. **That is the vendor describing this device's purpose
  as the path taken when the ordinary console is absent — which is exactly the state our
  machine is in**, and it is independent of every source reading that led us here.

**The caveat is not a formality and it is the one this project keeps being caught by.** Those
manuals are for the **Origin 2000**, a different machine in the same family, whose controller
carries a keyswitch, an alphanumeric display and a rack-level controller above it that the
Origin 200 has none of. **A statement true of the neighbouring machine, cited for ours, is the
error this project has already paid for more than once.** The bus, the rate and the console
role are plausibly family-wide and are recorded here as vendor statements about the family; the
front panel certainly is not.

**No pinout from those manuals is taken into this document, and the reason given here earlier
was wrong.** It said the guides contradict each other on two pins of the connector our AUX port
matches, so the match was only as good as the disputed figure. **Both halves of that were
mistaken.** The controller has *two* connectors on the same circuitry — a mini-DIN and a
rear DB-9 — and **the dispute is entirely in the DB-9, which is not the connector the match
rests on.** The mini-DIN figure is identical across every revision, including the one that
corrupts its neighbour on the same page. Recording the correction rather than quietly swapping
the reason, because a right instruction carried by a wrong reason survives every check until
the case changes.

**The real reason to take no pinout is better and it is about corroboration, not disagreement.**
Six figures across six books were checked pin by pin, and they are **not six sources**: every
one carries the same idiosyncratic mislabelling of a signal, so they descend from **one
drawing**, propagated with edits landing in different places. Three books show a
standard-conforming assignment and two show a rearranged one that disagrees with itself on two
pins — and counting them is counting copies rather than measurements. **A third source did not
settle it and could not have.** That is the same-source-agreement trap in its purest form: what
looked like independent manuals to compare was one document seen six times.

**So the mini-DIN comparison stands and is slightly better than before** — it survives the
revision that corrupted the connector beside it — while remaining a match between two figures
rather than an observation, and no pin from any of it reaches this specification.

**For a model that wants to answer the switch command with something meaningful rather than an
error**, SGI's complete eight-switch debug encoding and the commands that read and write it —
including the switch read this datasheet names and its two inversions — are recorded in
the vendor debug-switch encoding note (citation: `docs/library/msc-debug-switches-vendor-encoding.md`).
That is Origin 2000 controller hardware, and the same caveat applies to the switch block
itself; the firmware's *interpretation* of the bits is the part that is shared, and our own
machine's firmware prints its own table of exactly that when it cannot read them.

## Interrupts

The PCF8584 has a hardware interrupt output, enabled by the control register's `ENI` bit, and
**the PROM driver never sets that bit** — every wait in the chip's initialization,
arbitration and transaction routines is a polled read of the status register. No interrupt
line needs modelling for this firmware to function.

**The line does exist in the Hub, though, and the model should not claim otherwise.** The
Hub's slot-identity and micro-controller status register carries an I2C interrupt bit, named
in the public Linux and OpenBSD headers for this register alongside the slot-identity field
the arbitration signal derives from. So the chip's interrupt output has a destination on this
machine even though the firmware polls instead. Out of scope for the console work, recorded so
that a later guest driving `ENI` is a known gap rather than a surprise.

## DMA

None. I2C is a byte-at-a-time serial protocol on this chip; nothing here uses the Bridge's
DMA path.

## Out of scope

- Slave-mode reception on the PCF8584 (the driver never configures itself to be addressed as
  a target; `addressed-as-slave` is only ever checked as an *error* condition, never used).
- Temperature and fan-speed sensors on the shared I2C bus — SGI's own driver comments
  describe these as present on real hardware, and nothing in the PROM boot path traced here
  addresses them directly; the firmware asks the ELSC about temperature and fans through the
  ASCII channel rather than reading sensors itself.
- The single-byte target at slave `0x20`, reached by a pair of routines the interactive
  monitor calls. Not on the boot path, and what the device is is not established by anything
  read here.
- **The NVRAM EEPROM at `0x50`–`0x57` is *not* out of scope, and this list previously implied
  it was.** It is a real device this same driver addresses directly, it carries the identity,
  password, debug-switch and self-test state, and page 7 is read sixteen times on the normal
  boot path before console selection begins. A model can reach the system-controller console
  without it — those reads tolerate failure — but "any other device on the bus besides the
  ELSC is out of scope", which this document used to say, was wrong as a statement about the
  machine and would have sent an implementer past a device the firmware demonstrably uses.
- The controller microcode's instruction-level implementation, private state layout, board
  wiring, pin assignments, signal levels, and buffering. The controller firmware is not wholly
  out of scope: its observable master/slave grant schedule is part of this datasheet's contract.
- The full ELSC command vocabulary beyond `ver` and `get` — listed above for completeness,
  not required for the console-path fix this datasheet exists to support.

## Behavioral notes

**"Checksummed-version protocol," the phrase the console fan-out uses for the gating spec,
names this
packet framing plus the `ver` command — it is already committed, in
the MSC research document (citation: `docs/library/msc-module-system-controller.md`) (`dfbe7e0`), not a
separate spec this datasheet had to derive from scratch.** An earlier draft of this section
said the phrase named no document at all; that was wrong — a search of this stream's notebook
and of
irix-structs' own notebook came back empty, but the material lives in `docs/library/`, a
different directory neither search covered. Corrected directly by irix-structs before this
draft went further. Recording the correction rather than silently fixing it in place, per
this project's own rule about a wrong premise: the material is real, committed, and now cited
above throughout rather than re-derived.

**The I2C configuration NVRAM is not the machine's other non-volatile store, moved from
the machine catalogue: the two are easy to confuse, and this datasheet's own earlier
draft is why the entry spells it out.** The clock component is a battery-backed
Timekeeper on the I/O controller's byte bus at chip select 0. The configuration NVRAM is
a separate device on the Hub's I2C bus, reached by raw transactions the system controller
never sees. An earlier draft of this datasheet placed that storage inside the system
controller's command channel and had to be corrected, so the conflation has already
happened once.

**Every transaction's closing "invalid address" step is easy to model incorrectly as a
no-op.** It is not decorative: it is what resynchronizes every controller on the bus to a
known data-register state before the next arbitration round, and a model that skips it will
desynchronize its own data register from what a real bus would show after the first real
transaction.

## qtest checklist

- **Register selection, all five rows.** Each of own address, clock control, interrupt vector
  and data answers under its own `ESO`/`ES1`/`ES2` combination, and the interrupt vector is
  still reachable with `ESO` set. A test that only exercises the three selections this
  firmware uses passes on a model that ignores `ES2` once the interface is enabled.
- **Own address: full round-trip, both values.** Write `0x7f`, read `0x7f`; write the
  slot-derived address, read it back unshifted. The firmware checks only the placeholder, so
  the second half asserts more than the guest does — deliberately, because a register that
  answers a constant passes the first half alone.
- **Clock control: write and read back both fields independently**, so a model packing them
  into one opaque byte is caught.
- **The whole initialization sequence, in order, as writes.** Replay all nine steps including
  the redundant second control write, and assert the model is left with the serial interface
  enabled and the real address stored.
- **The liveness probe's shape:** the data register reads identically twice while the modelled
  controller is silent, and differently once it begins broadcasting. Assert the control/status
  register is *not* required to change for the probe to succeed — the firmware never reads it
  there, and a model that couples them is modelling a rule that does not exist. With no change,
  assert failure after `5,000,000` firmware-converted counter units and that the caller continues.
- **A full `ver` exchange against the modelled ELSC**: correct checksum accepted; a corrupted
  response checksum rejected; a response whose text does not begin `ok` rejected **even though
  its checksum is valid** — that last one is the acceptance condition's real content and a
  test that only checks the checksum path certifies the model this datasheet used to license.
- **Response bounds:** a length byte of 94 is accepted and 95 is never emitted by the model.
- **The closing invalid-address step**: the byte on the wire is `0xfe`, the control register is
  written before it, and the model NAKs. Assert the *order*, not just that both writes
  happened — order is the part a normal-start implementation gets wrong.
- **Arbitration, driven as the firmware drives it, not as the model would like.** The strong
  test runs the real three-loop wait: start with the signal *asserted*, and assert the wait
  does not complete until the model has presented another position, re-asserted, and held past the
  predicate `converted_now > converted_start + 250`, with each converted value calculated as
  `floor(8 * raw / 10)`. If the test does not control the starting residue, it advances the raw
  Hub counter by at least 314 ticks. A test that starts from an idle bus and simply asserts the
  signal skips loop one entirely — which is exactly the loop a naive model hangs in, so that
  test would certify the hang.
- **Eligible ticks preserve rotation on a busy bus.** Hold the bus busy across an eligible
  one-millisecond tick. Assert that no grant is published and the next free-bus grant uses the
  same code. In a two-module machine, assert that only the master controller publishes grants.
- **A signal that drops before the hold expires sends the firmware back to waiting**, and the
  wait then completes on the next full-length assertion rather than failing.
- **`get` is gated on the message-waiting signal, not on the response.** With an empty queue
  the model broadcasts only `signal1` and the test asserts **no `get` transaction appears on
  the bus at all**. With something queued it broadcasts `signal2` and the `get` arrives.
  Asserting that an empty queue produces a polite empty response tests a transaction that
  cannot occur.
- **The acceptance observable, registered before the model exists.** With the responder
  answering, the firmware's blinked console code must change to the value the vendor assigns
  this device. **Assert the specific value, not that it differs from today's** — a code that
  merely changes proves the model perturbed something, which is the weaker property that a
  previous experiment on this bus mistook for its own kill condition.
- **`acp` in both directions.** A queued message surfaces exactly once on the next `get`, with
  the payload starting at the sixth byte of the response. An `acp` *command* carrying text is
  answered `ok <hex count>` with a non-zero count, and the test asserts the firmware's output
  queue drains — a model answering a bare `ok` must fail this test, because that is the
  livelock.
- **External AUX follows the physical module role.** In a two-module Origin 200, node 0's master
  controller accepts and emits external AUX characters while node 1's slave AUX backend is
  inactive.
- **Two-module arbitration remains one schedule.** During the early 6.5.30 liveness probe,
  processors in both modules observe changing data bytes rather than an inert slave-local PCF.
  The master emits one five-position sequence, and both PCFs expose the same current position.
  CPU 3 then receives `0xe3` or `0xeb`, held unchanged with bus-not-busy until the
  predicate `converted_now > converted_start + 250`, where each converted value is
  `floor(8 * raw / 10)`, before the `1,000,000`-unit converted deadline. A raw-counter shortcut
  advances by at least 314 ticks when the starting residue is not controlled. Physical time is
  not asserted without a measured Hub counter rate.
- **Two-module PCF routing before and after node numbering.** While both Hubs report NASID zero,
  an explicit-zero PCF access from each module resolves to that access's issuing/local PCF, and
  both PCFs expose the master's one sequence. After the slave reports NASID one, an explicit-zero
  access from the slave reaches the master PCF and an explicit-one access reaches its local PCF.
  Both paths still expose the same master-owned sequence.
- The escape split on serial input: a byte sequence beginning `0x14` is consumed as a
  controller command and never appears in a `get` response; every other byte is queued and does
  appear. **Assert both directions** — a test that only checks that typed text reaches `get`
  passes just as well on a model that never implements the escape at all.
- Supervisor gating: a protected command answers `err perm` before `pas` and succeeds after it,
  and `pas` rejects a password that is not exactly four characters.
