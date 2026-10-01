# Origin 2000 module management wiring

References rendered as citation identities name material outside this document
set. Those targets are not included or cleared for redistribution. Review and
runtime statements retain their recorded dates and scope.

Status: independently cleared for population and shared attachment in one
four-node module. The independent evidence review (citation: `docs/reviews/origin2000-module-i2c-population-review-20260905.md`)
clears four logical adapters, one shared bus, one MSC and one shared logical
NVRAM space. The independent scheduler and reset review (citation: `docs/reviews/origin2000-shared-management-guest-activation-review-20260905.md`)
clears the shared grant scheduler and reset contract at revision
`5ace1c59f2d9f6b9b462681cdb4f4b29170f7343`. The shared grant scheduler and reset
requirements remain unchanged. The physical-slot identity mapping has separate independent clearance linked in
this contract. Physical EEPROM package count remains unestablished.
Implementation and stock firmware discovery require their own acceptance evidence.
The boot-time identity reconciliation added on 2026-09-12 is cleared for the
selected module-1 configuration by
the independent fresh-log review (citation: `docs/reviews/origin2000-module-fresh-log-independent-review-20260912.md`).

## Required population

| Resource | Four-node module population | Attachment and state scope |
|---|---:|---|
| Hub-side PCF8584 interface | 4 | One independently addressed controller interface per node's Hub. |
| Management I2C bus | 1 shared bus | Connects all four node interfaces to the module management resources. |
| Module system controller | 1 | Shared by Nodes 1–4, with one module identity and alternate-console endpoint. |
| Management NVRAM address space | 1 shared logical space | Accessible on the common I2C bus. Physical EEPROM package count is unestablished. |

SGI 108-0164-002, *Origin2000 and Onyx2 Diagnostic Roadmap*, printed B-1 /
PDF 55, Figure B-1, shows one MSC connected to one I2C bus with branches to
Nodes 1, 2, 3 and 4. Its NVRAM/voltage-margin/UST control block connects to
that same bus. The diagram does not place four independent MSCs or private
management NVRAM copies behind the four nodes.

The MSC has one RS-232 alternate-console interface. A terminal can attach in
a deskside configuration, while the rack arrangement can connect it to the
separate multimodule system controller. Four node interfaces do not imply
four physical alternate-console ports or four independent module controllers.

## Per-node interfaces and shared devices

The protected IRIX 6.5.7 driver describes PCF8584 interfaces attached to the
Hubs on this shared module bus. Its production register accesses select the
specified NASID's Hub. Initialization assigns an interface address derived
from the physical node slot. These source facts support four distinct
controller interfaces for four nodes, not one PCF8584 register file aliased
at every Hub.

For the four-node module's physical slots one through four, the IRIX 6.5.7
standalone firmware derives a zero-based slot index by subtracting one from
the decoded physical slot. It programs the interface's own I2C address as
`0x09` plus that index. The interface addresses are therefore `0x09` through
`0x0c`, one per physical Hub interface on the shared bus.

That firmware computes its processor grant code as twice the slot index plus
the live Hub processor-number value. The two processors in slots one through
four therefore use code pairs zero/one, two/three, four/five and six/seven.
The scheduler's eight-code sequence and addressing remain governed by the
shared grant contract. This mapping does not add another scheduler.

A generated descriptor's array position may stand for the slot index only if
the model checker or construction code verifies that correspondence. Reordering
node descriptors while preserving their physical slots must preserve interface
addresses and processor grant identities. A global CPU array index must not
replace the requesting processor's live local Hub identity without an equally
explicit checked correspondence.

This is the standalone firmware's addressing and grant-selection convention,
not a claim that every software consumer computes a grant identically. The
same source selects code zero in its non-standalone build. This clarification
does not redefine the Hub processor-number register or establish new remote
access behavior for that register.

Required tests must cover both local processor numbers in each physical slot
and a reordered descriptor arrangement. Existing scheduler, wire timing,
recovery and migration requirements remain unchanged.

The independent physical-slot grant review (citation: `docs/reviews/origin2000-module-slot-grant-review-20260908.md`)
clears this mapping against the identified IRIX 6.5.7 standalone source.

The existing [Hub controller register contract](hub-memory-directory.md)
places data at Hub offset `0x220000` and control/status at `0x220008`.
The logical registers carry eight-bit values. The production driver uses
64-bit Hub cycles, which the Hub adapter must preserve according to the
existing access contract. The [PCF8584/ELSC contract](pcf8584-elsc.md)
owns register selection and transaction behavior.

That reviewed protocol contract supplies the logical MSC endpoint at I2C
address `0x08` and a separate management NVRAM address space across
`0x50`–`0x57`, totaling 2048 addressable bytes. Combining that protocol with
the published shared bus means these are common module resources reached
from each node interface. A model must preserve their shared identity and
storage rather than silently constructing four private copies. This is a
logical population requirement, not a claim about the number or exact type
of physical EEPROM packages.

Per-node boot-flash logs are separate storage. The module's four node PROMs
each retain their own final two flash sectors for environment and messages,
as specified by the node flash evidence (citation: `docs/research/origin2000-firmware-configuration.md`).
Those four stores do not replace the shared I2C management NVRAM.
BaseIO nonvolatile storage is another separately owned resource and is not
counted as module-management NVRAM here.

## Numeric module identity

An assigned Origin 2000 module number is an integer from 1 through 255.
Zero represents an unassigned module. Assigned numbers identify modules to
firmware and IRIX and must be unique within a system. They are not node
NASIDs, physical slot numbers, graph identifiers or descriptor array indices.

The MSC keeps the number in its shared NVRAM. Node-card PROM logs hold backup
copies that firmware can consult if MSC NVRAM is unavailable. Firmware and
controller maintenance commands can change the number. The documented system
identity change takes effect on the next reboot. The PROM manual also
describes automatic assignment of an unassigned module. The model must not
replace those firmware procedures with generated discovery records.

### Controller storage access

The firmware's `elsc_module_get` and `elsc_module_set` names do not denote an
MSC command frame. They read and write the shared management NVRAM directly
over I2C. The sixteen-byte controller-reserved block starts at logical address
`0x700`. Byte 0 is the validity marker `0x37`, byte 8 is the module number, and
byte 10 is the partition number. A helper that reads the module number first
checks the validity marker. A model that seeds byte 8 but leaves byte 0 invalid
does not present an assigned module to that helper.

The module controller's serial `mod` command is a separate interface. It reads
or changes the same module-number byte. The IP27 POD `module` command also
changes the shared byte and, outside DEX execution, copies the value to every
node-card PROM log in the physical module. These maintenance paths converge on
one stored module identity. They are not independent module-number sources.

### Power-on selection and reconciliation

Each node's local master first reads the controller-reserved block into a
node-local firmware cache. The examined IRIX 6.5.7 path uses a positive cached
module byte as the primary identity without checking the validity marker. If
that byte is zero or negative when interpreted as a signed byte, the node reads
its own `LastModule` PROM-log backup, using zero when no valid backup can be
read. The cache fill discards the I2C transaction result, so this path does not
reliably distinguish a failed read from returned data. The node advertises both
the selected value and whether it came from the backup in its Hub scratch state
before fabric discovery. This source behavior is cleared for selected module
number 1. Values 128 through 255 need separate reconciliation with the manual's
published range before they are accepted as firmware-compatible configurations.

After discovery, firmware reads the shared controller storage for each Hub. A
failed controller read falls back to a vote among the non-headless nodes in
that physical module. Headless nodes do not vote, but receive the result. A
candidate wins only when strictly more than half of the eligible nodes
advertise it. Firmware then updates the discovered Hub records and attempts to
copy the module and partition values into that node's PROM log. This early
copy does not initialize an erased or corrupt PROM log, and this source path
does not act on its write error.

If a local master had to use its PROM-log backup, it repeats the module vote
after discovery. A successful result updates the local advertisement, every
discovered Hub record in the module, the local backup, and the shared MSC
NVRAM. Zero can win this vote when every eligible node advertises zero. The
later I/O-board firmware still treats zero as unassigned.

The I/O-board firmware groups node boards by the discovered Router topology.
It requires every node in one physical module to carry the same number and
requires different modules to have different nonzero numbers. For an
otherwise unnumbered one-module system, automatic assignment selects one more
than the highest number already present. With no assigned module, that result
is 1. It writes the value to each node's PROM log and requests a hard reset.
Any invalid node PROM log makes that write fail and sends the machine to POD.
The message names the first NASID whose log rejected the write. This automatic
path does not update the shared MSC NVRAM before the reset. On the next pass,
the valid per-node backups vote the value back into the MSC.

For the selected one-module machine, the stable result is therefore:

- one shared MSC module number, 1.
- node boards at NASIDs 0 through 3 and slots N1 through N4, all in module 1.
- Router boards R1 and R2, both in module 1.
- every other discovered board record attached to the module, including the
  two Crossbows and BaseIO, carrying module 1 in KLCONFIG.

The numeric module number is distinct from the module serial number exposed by
the one-wire identity device. All four node-card PROM logs are distinct stores.
Their backup values must converge even though the shared MSC has only one
module-number byte.

For the selected four-node emulation target, explicitly configure an initial
module number of 1 in the product model and carry it through the generated
descriptor. This is a selected installation value, not a hardware reset
default or a recovered manufacturing value. The first target requires an
assigned number from 1 through 255. Zero remains a documented hardware
state but is not an accepted initial configuration for this target.

The configured value seeds newly created emulated MSC NVRAM together with the
validity marker required to make that value readable. Once that
state exists, unrelated adapter register activity and ordinary reset must not
reseed or erase NVRAM. Explicit guest NVRAM writes update the shared contents.
Migration restores the saved NVRAM rather than reseeding it from the product
model. These are explicit emulator initialization and persistence rules.
They leave backup voting and automatic assignment to the firmware and add no
controller command. Any future external NVRAM-image input must define
its precedence separately before it is supported.

The compiler must reject a missing, repeated or unknown module binding,
duplicate assigned numbers, boolean or noninteger values, and values outside
the selected range. Changing
node or module array order while preserving graph identities must preserve
the configured number. Runtime tests must check the validity marker and initial
module byte, guest-written state across reset and migration, and unchanged
Origin 200 private-management behavior.

Each newly created node boot-flash image needs a valid empty PROM-log store if
the supported first boot includes firmware automatic assignment. An erased
`0xff` log sector is a real flash state, but it is not writable through the
PROM-log variable interface until the firmware's `initlog` operation formats
it. Ordinary reset, controller reset, and migration retain all four log stores.
An explicit `initalllogs` reinitializes all discovered node logs and removes
their saved module values. With a valid shared MSC module number, the next boot
repopulates each backup with 1 without changing the selected module identity.

The evidence is SGI 108-0170-001, *Origin200/2000 IP27 PROM Technical
Reference Manual*, printed pages 1-4 and 1-5 (PDF pages 18 and 19),
SHA-256 `08ad68675afffe7e00327b286c0586624f0b1a1f70878e34d3e1e18de423854e`.
The owning PCF8584/ELSC contract identifies the existing numeric NVRAM
location. Duplicate-number repair policy and complete power-sequencing
behavior remain outside this selected one-module configuration.

The independent numeric identity review (citation: `docs/reviews/origin2000-module-numeric-identity-review-20260908.md`)
clears the hardware claims and selected initialization policy.

The source-reviewed boot algorithm, fresh-log failure and additional tests are
recorded in
the fresh-log investigation (citation: `docs/reviews/origin2000-module-fresh-log-source-review-20260912.md`).
The corrected extension is cleared for the selected module-1 configuration by
the independent fresh-log review (citation: `docs/reviews/origin2000-module-fresh-log-independent-review-20260912.md`).

### Discriminating acceptance

- Read the shared management NVRAM through every Hub adapter. Each route must
  return validity marker `0x37`, module number 1 and partition number 0.
- Corrupt the validity marker while preserving module byte 1. The
  `elsc_module_get` helper must reject the controller value rather than
  accepting byte 8. The early cached-byte selection is a separate path and
  does not perform this marker check in the examined IRIX 6.5.7 source.
- Give four valid node logs the values 1, 1, 1 and 2 while making the shared
  controller storage unavailable. The three votes for 1 must win. A 1, 1, 2,
  2 split must not produce a winner. A headless node must receive the winning
  value without contributing a vote.
- Start with shared module number 1 and four valid empty node logs. Stock PROM
  6.156 must discover four Hubs and two Routers, write `1` into every backup,
  assign module 1 to all module-owned KLCONFIG boards, and reach the maintenance
  menu without a module-number reset.
- Start with shared module number 0 and four valid empty node logs. The first
  pass must assign 1 to all four logs and reset. The second pass must vote 1,
  store 1 in the MSC NVRAM, and reach the maintenance menu. No pass may enter
  POD or leave one node with a different backup.
- Start with an invalid log only on NASID 1 and shared module number 0. The
  automatic-assignment pass must fail on NASID 1. After `initalllogs`, the
  same machine must complete assignment and preserve the result across reset.
- Change the module number through the supported maintenance path. Verify one
  shared MSC value and four matching PROM-log backups after reboot.
- Migrate after guest writes to both the MSC byte and all four backups. The
  destination must restore those values and must not reapply the descriptor's
  initial value. Reset must retain the same state.

## Shared grant scheduler

The module system controller grants access by sending one address-only I2C
transaction on each eligible one-millisecond service tick. It loads one raw
byte, requests START, waits for the address and acknowledgement clocks to
finish, then requests STOP. It sends no data byte. The ordinary grant values
are `0xe0` through `0xe7`. Setting bit 3 announces a pending controller message
for that processor and produces `0xe8` through `0xef`.

These bytes are otherwise-unmatched addresses, not data following general-call
address zero. Their low bit remains the I2C direction bit. No module endpoint
acknowledges them, so the normal result is address NAK followed by STOP. The
[PCF8584 contract](pcf8584-elsc.md) owns the narrow passive-buffer approximation
that makes the completed address visible to each Hub adapter.

The scheduler's counter resets to the value immediately before processor code
zero. It advances modulo eight before each eligible transmission attempt, so
code zero is published first. It publishes at most one grant per periodic tick.
A later transmission failure does not roll the code back. A successful attempt
reloads the controller's five-second recovery counter.

Publication is eligible only when all these conditions hold:

- The controller has not granted the bus to its own pending transaction.
- Its addressed-slave protocol state is idle.
- The PCF8584 reports the bus free.
- Publication is enabled and no higher-priority serial-console command owns the
  controller's protocol path.

On an otherwise free and idle tick, a pending controller transfer has first
priority and a pending serial command has second priority. Neither handoff is
allowed while a controller-originated interrupt is pending. The requests remain
pending in that case, and an eligible processor grant can still be published.
A successful handoff skips publication without advancing the processor code.

A busy or slave-active tick does not advance the processor code. A guest may
therefore hold the bus across an ordinary or repeated-START transaction for as
long as required. The first eligible periodic tick after STOP advances to and
publishes the next code. STOP itself does not trigger publication. There is no
explicit zero-valued gap transaction, repeated presentation within a slot, or
ten-millisecond slot hold. Other processors' grant addresses make the previous
processor's own grant absent before its next turn.

The message indication is recomputed for the code being published. It is set
for a pending controller interrupt whose destination mask contains that
processor. That indication remains for the processor until it fetches the
message or the global pending state expires after about one second. Otherwise
the indication is set for queued alternate-console input assigned to that
processor. Automatic input selection follows the processor that most recently
sent console output to the controller. Input selection mode and the most recent
output sender remain separate state. The input response encodes selection mode,
not necessarily the processor whose grant announces input. The
[console selection contract](pcf8584-elsc.md#console-selection-and-response-prefix)
owns those values. An ordinary unconsumed command response does not by itself
create this indication.

Bus-busy, slave-active, already controller-granted, and failed-publication ticks
decrement the recovery counter. A successful handoff, an active serial command,
or disabled publication leaves the counter unchanged. At five seconds the
controller reinitializes its PCF8584 and protocol queues and resets the processor
counter. An already enabled publication state remains enabled, so the next
eligible tick publishes code zero.

**Inferred: the reinitialization releases the controller's own bus drive.**
The Philips PCF8584 specification (1997-10-21, section 6.8.1.2) says clearing
`ESO` disables the serial I2C-bus I/O, and a reinitialized interface starts
from that state. The controller therefore stops driving SDA and stops holding
SCL, including a data bit it was sending as a slave transmitter. No capture of
the controller's lines during recovery exists. What would settle it: hold the
module bus busy with an acknowledged controller read followed by a node STOP,
and record SDA and SCL across the five-second recovery.

This matters for a node that writes STOP after acknowledging a byte of a
controller response. The PROM's read loop (`0x1fc39aa8`–`0x1fc39ba8` in PROM
6.156, payload SHA-256
`c2c69bf33c375b43ec7ac511fab41ddf595be1b6838ae717e58a9a1273437fdd`) branches
on its deadline to `0x1fc39a38`, which writes `PIN | ESO | STO` (`0xc2`). After an acknowledged
byte the controller is already sending the next one. An ASCII byte's first bit
is zero, so SDA stays low, the STOP cannot form, and the bus stays busy. The
recovery reinitialization is the only release this contract describes; whether
the controller's firmware also gives up a stalled response on some timeout of
its own is unknown. A model whose recovery
leaves the slave half driving SDA keeps the bus busy for the rest of the boot.
That was observed in a four-node Origin 2000 cold boot at QEMU `fb4d6d3e3b`
(run `prom-20260926T193859-7756db`):
the stopping adapter waited for SDA to rise, the controller held the first bit
of the response byte `a`, and peer discovery timed out on node 2. The qtest
`/sn0/o2000-module-management/stop-after-acked-read-byte` requires the bus to
come free within the recovery period.

A successful module power-up initializes the interface after releasing reset,
then enables publication. Power-down disables publication before module reset
and power sequencing. Controller-executed reset and NMI do not clear an enabled
publication latch before their 100-millisecond pulses. Their blocking delay uses
the same periodic interrupt that services publication, so grant attempts
continue during each pulse. Both paths then reinitialize I2C, which resets the
processor counter while preserving the enable latch. They explicitly enable
publication when module power is on. The normal power-down path has already
disabled publication when power is off.

The retained source selects eight processor codes. Its optional fault-reporting
tables contain positions for four nodes and two Routers. This supports the
four-node module contract but does not identify the build used in a production
controller. Origami's stock PROM binary is from the 6.5 base release, while the
scheduling source is from IRIX 6.5.7. Guest activation must still show the stock
PROM observing all eight grants and discovering four nodes and two Routers.

Required scheduler tests must prove the address-only wire shape, code-zero
start, eight-code rotation, busy deferral without code advance, failed-attempt
advance, controller-before-serial priority, pending-interrupt request blocking,
per-processor message clearing, global message expiry, five-second
reinitialization, power/reset publication behavior, and migration without
duplicate publications or timer events.

## Limits that shared attachment does not settle

Existing two-tower Origin 200 guidance has one node per module. Its per-node
controller and alternate-console statements must not be transferred to the
four-node Origin 2000 module. The same limitation applies to a per-node
interpretation of a module-scoped cold-power query. The grant and
alternate-console selection rules above settle only their named state. Shared
attachment alone does not establish whether any other MSC command's state or
notification is global, per reader, or multiplexed by processor.

The driver observes scheduled access on a bus shared with the MSC and other
Hubs. The scheduler above does not settle simultaneous controller-transfer
semantics, interrupt wiring, electrical contention, or an active master's reset
waveform. Those require their own reviewed evidence. The source researcher has
not changed the emulator implementation.

## Provenance

The published Figure B-1 was rendered and inspected directly. Retained PDF:
Diagnostic Roadmap (citation: `docs/library/cache/prom/108-0164-002-diagnostic-roadmap.pdf`),
SHA-256 `fee8fe51c9bf7433dd04a155218fc0de552d68baadde84cb5de510c76ba33e6f`.
The diagram supports shared wiring and module ownership, not the chip count
inside each node or a physical EEPROM bill of materials.

Protected source: IRIX 6.5.7,
`stand/arcs/lib/libkl/ml/i2c.c`, lines 1–20 for attachment, 76–100 for slot
addressing, and 265–330 for node-qualified register access and initialization.
The scheduler and reset contract also uses `stand/arcs/elsc/i2c.c`, lines
1–110 and 480–600, `stand/arcs/elsc/i2c.h`, lines 39–95,
`stand/arcs/elsc/elsc.h`, lines 14–32 and 230–245,
`stand/arcs/elsc/main.c`, lines 285–337 and 370–413,
`stand/arcs/elsc/util.c`, lines 853–975, and
`stand/arcs/elsc/sequence.c`, lines 80–115, 400–470, and 675–748. Message
creation and per-processor clearing use `stand/arcs/elsc/cmd.c`, lines 235–263,
665–701, and 1667–1808. Source identities and whole-file hashes are recorded in
the activation investigation (citation: `docs/research/origin2000-shared-management-guest-activation.md`).
The source reader is excluded from implementing or independently reviewing the
PCF8584, Hub I2C, module-management, controller scheduling, and discovery paths.
The existing reviewed PCF8584/ELSC datasheet supplies protocol addressing
and the explicit physical-EEPROM-count uncertainty.
