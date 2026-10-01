# Datasheet: AT&T/Lucent `11c1:5802` two-port OHCI controller

References rendered as citation identities name material outside this document
set. Those targets are not included or cleared for redistribution. Review and
runtime statements retain their recorded dates and scope.

**Status:** cleared at author commit `4b9038d30405ca53a1f97c76ef221a47a11957cb`
by independent review commit `3eeb77ba8891f78e021e14aaecbe1b8bac615dcc`.
The PCI configuration Status amendment below remains unreviewed and does not
extend that clearance. The HcInterruptEnable reset amendment is covered by the
IO-8 board test review (citation: `docs/reviews/io8bt-ioc3-isp12160-xbridge-review.md`), not by
that clearance. The FrameNumber latch and the root-hub port power amendments are
covered by the FrameNumber and port power review (citation: `docs/reviews/2026-09-27-lucent-ohci-fm-number-review.md`).
This public-source handoff defines the Origin 300 PCI identity, board placement,
two-port topology and the narrow contract for reusing QEMU's existing OHCI core.
OpenHCI 1.0a is now held and read for the registers the IO-8 board test's reset check
reaches; the rest of its register contract has not been reviewed against the core, so
this is still not a complete standard register or reset contract.

**QEMU file(s) on `sgi-origami`:** `hw/usb/lucent-ohci.c`, the `lucent-ohci`
QOM type, a subtype of the existing `pci-ohci` wrapper rather than a second OHCI
transfer engine, with its register-contract qtests in
`tests/qtest/usb-lucent-ohci-test.c`.

## Sources used

- *OpenHCI: Open Host Controller Interface Specification for USB*, release 1.0a,
  Compaq, Microsoft and National Semiconductor, September 1999, `docs/library/cache/io-chip/openhci-usb-host-controller-spec-1.0a.pdf`,
  SHA-256 `588a27e934e3fd17ade596321d482ffb2269db17576293cd646065abd59f2a98`,
  acquired 2026-09-24 through `docs/library/docs.tsv`. Sections 5.1.2.1, 7.1.3,
  7.1.5, 7.1.6, 7.3.1, 7.3.3 and 7.4.1 to 7.4.4.
- *USS-312 Two-Port PCI-to-USB OpenHCI Host Controller*, Lucent Technologies
  Advance Data Sheet Rev. 5, March 2000, DS99-275CMPR-5,
  `docs/library/cache/io-chip/lucent-uss312-two-port-ohci-advance-data-sheet-rev5.pdf`,
  SHA-256 `0752255bb111833180e57c8c8d09e12a5a18fd465e59a4bc755e63f7b23af5eb`,
  acquired 2026-09-27 through `docs/library/docs.tsv` after a Library query for
  `USS-312` returned no holding. Its PCI configuration summary (Tables 3 and 8)
  gives vendor `11C1h`, device `5802h` and revision ID `10h` for Revision C, the
  function both Origin 300 captures report, so this is the part's own datasheet.
  Tables 42 and 45 to 48 give the HcFmNumber and root-hub register fields, their
  access and their reset values.

- *SGI Origin 300 User's Guide*, revision 002, document `007-4365-002`, Library
  root `techpubs`, exact path
  `library/manuals/4000/007-4365-002/pdf/007-4365-002.pdf`. Printed pages 9, 12
  and 14 describe the two 64-bit universal PCI option slots, the IO-8 assembly,
  the USB connectors and the one-chassis I/O inventory.
- *sMDK-based Field Diagnostics for SGI Origin 300 System and SGI 3000 Family*,
  document `108-0264-004`, Library root `origami-docs`, exact path
  `library/cache/sn1sn2/108-0264-004-origin300-smdk-field-diagnostics.pdf`,
  SHA-256 `535bda2d0de4ee9a86d79c0d1ebbb1d54ceadd52677d3395e430f50f39ebfcc7`.
  Printed pages 2-10 through 2-14 define the IO-8 diagnostic sections, select
  USB as PCI device 5 and define pass and failure reporting.
- *Online Diagnostics*, document `108-0286-007`, Library root `origami-docs`,
  exact path `library/cache/prom/108-0286-007-online-diagnostics.pdf`, SHA-256
  `79e91fd162c16ed36d0fc4694af06afbdba23b9433841cd4e4adab82ff3af9b8`.
  Printed pages 2-30 through 2-32 define the `olusb` host-controller and optional
  inventory tests for IP45.
- The public 2009 Origin 300 OpenBSD capture, Library root `origami-docs`, exact
  path `library/cache/boot-logs/openbsd-sgi-list/marc-125897882029839-origin-300-installation-issues.html`,
  lines 55 through 90. It reports the controller at PCI device 5 function 0 as
  `AT&T/Lucent USB 2-port`, PCI revision `0x10`, XBridge interrupt pin 5, OHCI
  version 1.0 and legacy support. The same capture reports option cards at head E
  devices 1 and 2 with XBridge interrupt numbers 1 and 2.
- The public 2014 Origin 300 OpenBSD capture, Library root `origami-docs`, exact
  path `library/cache/boot-logs/openbsd-sgi-list/marc-139223952727384-origin-300-numalink-1.html`,
  lines 150 through 183. It reports the IO-8 bus at 33 MHz, the controller at
  device 5 function 0 on XBridge pin 5, and an empty 66 MHz option bus.
- OpenBSD `origin/sgi-is-alive-at-7.9`, commit
  `2df91d80c104636deeef8124252ddd5951b55fbf`: `sys/dev/pci/pcidevs` lines
  191 and 7819 through 7851 identify `0x11c1:0x5802` as the AT&T/Lucent USB
  two-port product. `sys/dev/pci/ohci_pci.c` lines 74 through 160 match the OHCI
  PCI class, map BAR0, use the PCI DMA tag, establish the PCI interrupt and check
  controller revision and legacy ownership. `sys/dev/usb/ohcireg.h` lines 39
  through 139 gives the public register and descriptor interface the consumer
  uses. `sys/dev/usb/ohci.c` lines 708 through 917 allocates the host-controller
  communication area, resets the controller, installs the endpoint-list heads
  and enables the interrupt sources used by that consumer.
- The retained public IRIX `hinv` capture in Library root `origami-docs`, exact
  path `library/cache/boot-logs/nekonomicon-thread-17314-origin300-8cpu-hinv-prom.html`.
  It independently reports decimal PCI IDs 4545 and 22530, which are
  `0x11c1:0x5802`, at PCI device 5 and names the interface as OHCI.
- *PCI Local Bus Specification* revision 2.2, Library path
  `library/cache/io-chip/pci-local-bus-spec-2.2-1998.pdf`, SHA-256
  `7ab2ea9d141716f4c0b8f9bb188797727da192fea2fbc8d3e87bcbd2bbb01354`.
  Printed pages 196-198 define the Status fields.
- Public `lspci -v` output for a matching `11c1:5802`, Revision ID `0x10`
  function, Library Mail aggregate `all`, Message-ID
  `1014289078.15561.26.camel@ukabzc383.uk.saic.com`, lines 69-81. It reports
  medium DEVSEL and no capability list before the next function begins.

The Library was searched first for the PCI identity, product name and OpenHCI
specification. It resolved the retained machine documents, captures and public
drivers. The complete all-backend query `Open Host Controller Interface
Specification for USB Release 1.0 OpenHCI OHCI` returned no match. Complete
artifact searches for `OpenHCI Specification for USB Release 1.0a`,
`ohci10.pdf` and `OpenHCI 1.0a` also returned no match. `OpenHCI Specification
Release 1.0` and `openhci.pdf` returned only the unrelated PCI Local Bus 2.2
specification. The refined artifact query `Open Host Controller Interface
Specification Release 1.0` timed out and is not evidence of a miss. These
bounded results did not establish a Library-wide absence. Two further scoped
searches on 2026-09-24 also missed, and the specification was then acquired
from outside, as the first source above records.

## Role and placement

The controller supplies the two ordinary USB ports on the Origin 300 IO-8. It is
PCI function `00:05.0` on XBridge head F, widget `0xf`, beside the ISP12160 at
`00:01.0` and IOC3 at `00:04.0`. The 2014 capture reports this bus at 33 MHz.
The controller asserts PCI INTA, which the IO-8 wires to physical XBridge
interrupt pin 5.

Head E, widget `0xe`, carries the two option slots. The base machine leaves both
empty and the captured empty bus runs at 66 MHz. A separate capture associates
PCI device 1 with XBridge interrupt number 1 and device 2 with interrupt number
2. It does not report either card's PCI Interrupt Pin field. No source read for
this handoff establishes the riser's source-to-pin swizzle, so the machine must
not synthesize option-card interrupt wiring.

## PCI identity

| Field | Value | Status and evidence |
|---|---:|---|
| Vendor ID | `0x11c1` | Established by the public captures and OpenBSD's PCI product table. |
| Device ID | `0x5802` | Established by the public captures and OpenBSD's `USBHC2` entry. |
| PCI revision | `0x10` | Observed in both Origin 300 OpenBSD captures. |
| PCI function | `00:05.0` | Observed in both captures. SGI's IO-8 diagnostic independently selects device 5. |
| Class | serial bus, USB, OHCI interface `0x10` | Public OpenBSD consumer match requirement. |
| Root-hub ports | 2 | Product identity and both captures call it a two-port controller. |
| OHCI revision register | 1.0 with legacy-support indication | Observed in both captures. This is distinct from PCI revision `0x10`. |
| Subsystem vendor/device | hardware unknown, initial emulator `0xffff:0xffff` | No configuration-space dump or published source read for this handoff reports either hardware field. The emulator values mean unobserved and unavailable. |

The exact controller marketing part beyond the public AT&T/Lucent PCI product
name is unknown. Do not infer a subsystem identity or a more specific part name
from the primary vendor and product IDs.

## PCI configuration Status

The first implementation uses PCI Status `0x0200` after cold reset as an
evidence-bounded compatibility value. Its only set field is DEVSEL bits 10:9,
whose value `01b` means medium timing under PCI Local Bus Specification
revision 2.2, printed page 197. It leaves every capability and event bit clear.
In particular, bit `0x0010` is clear and the function does not expose a PCI
capabilities list.

The public `11c1:5802`, Revision ID `0x10` comparison capture reports medium
DEVSEL. Its Lucent block has no capability line, while the immediately following
Realtek block displays a Power Management capability. This is indirect evidence
for the fixed Status fields because the capture contains no raw Lucent
configuration bytes and was taken after initialization on another system.
Revision ID `0x10` is the separate byte at configuration offset `0x08`. It is
not Status bit `0x0010` at offset `0x06`.

No permitted source read for this handoff directly observes the Origin 300
function immediately after PCI reset. The exact hardware reset word therefore
remains unknown. A raw IO-8 configuration-space capture after PCI reset would
replace this compatibility selection. The register check must require the full
Command/Status dword `0x02000000` after reset, `0x02000006` after software
enables memory space and bus mastering, and `0x02000000` after another reset.
This also requires a clear capability-list bit throughout the sequence.

## Reuse boundary in current QEMU

At QEMU commit `ebeef315652bae8abfee0038ee18680950f90ef0`,
`hw/usb/hcd-ohci-pci.c` lines 62 through 80 already realizes the OHCI core with
PCI-bus DMA, PCI INTA, a PCI-allocated interrupt and BAR0. Lines 103 through 125
provide reset and migration, and lines 112 through 116 expose only `masterbus`,
`num-ports` and `firstport`. Lines 129 through 151 hard-code Apple
`0x106b:0x003f`, so the current `pci-ohci` type cannot enumerate as
`0x11c1:0x5802` through properties.

The implementation should add an identity subtype in the same file, overriding
the PCI vendor, device and revision while selecting two ports. This is an
implementation inference: the public evidence establishes the OHCI interface and
two-port identity, while the existing QEMU core supplies the behavior. No source
read for this handoff requires a second transfer engine or Lucent-specific
register behavior.

The initial subtype sets subsystem vendor and device to `0xffff:0xffff`. This is
emulator policy for unobserved and unavailable values. It prevents current QEMU
from injecting its unrelated `0x1af4:0x1100` defaults when both class fields are
left zero. The `0xffff` values are compatibility placeholders, not observed IP45
hardware. A configuration-space capture from an Origin 300 IO-8 replaces both
placeholders. The identity qtest must inspect the resulting configuration space.

## Consumer and reuse boundary

The public OpenBSD consumer requires a memory BAR at PCI configuration offset
`0x10`, OHCI interface value `0x10`, HcRevision 1.0, 32-bit descriptor addresses
and a root-hub descriptor reporting two ports. Its common OHCI driver disables
the master interrupt before initialization, handles legacy ownership, issues
host-controller reset through HcCommandStatus, builds the host-controller
communication area and endpoint/transfer descriptors in DMA memory, then
enables the interrupt sources it services. Those actions establish consumer
use. The specification is now held, but it has been read only for the reset
registers below, so those actions still do not clear the complete
standard register file, access semantics or reset values as a hardware contract.

DMA must use the endpoint's PCI bus address space so XBridge direct-map and
translation-table processing remain in the path. A system-memory DMA shortcut
would bypass the bridge contract. This is both a QEMU integration requirement and
the only route by which the PCI device can reach node memory. It is not evidence
for an unpublished Lucent-specific DMA extension.

`io8bt` section 0 establishes a board-level reset requirement: PCI reset,
propagation of reset to the IO-8 devices and access between devices are tested.
The subtype must therefore participate in the existing PCI OHCI reset path. A
qtest of that reuse covers the emulator lifecycle only. Complete Lucent reset
semantics remain open. The SGI manuals do not publish the operations `io8bt` USB
section 2 performs. The part's USS-312 datasheet gives the operational registers'
reset values, which the root-hub port power section below uses. Do not turn the
other QEMU reset values into hardware facts.

### What the diagnostic expects of HcInterruptEnable after a reset

`io8bt` section 0 sub-section 1 requires HcInterruptEnable, the register at
operational offset `0x10`, to read `0x00000000` after a host-controller reset
through HcCommandStatus. It reports a mismatch as "Reset error on the USB,
register #0x4 at loc ... should have reset to zero", with the received and
expected words and its own `sec0_1.c line:421`.

The sequence is observed directly, tracing `usb_ohci_mem_read`,
`usb_ohci_mem_write` and `usb_ohci_reset` through a whole diagnostic run
of the two-processor `origin300` machine (run `prom-20260923T153122-b167b0`).
The diagnostic parks HcControl, writes HcInterruptEnable `0xc000007f` — every
source plus ownership change and the master enable — writes HcCommandStatus
`0x1` to assert HostControllerReset, writes HcCommandStatus `0x0`, and reads
HcInterruptEnable back. The model answered `0x80000000`.

That is the shared OHCI core's own reset value, not anything this board's
wrapper chose: `hw/usb/hcd-ohci.c`'s `ohci_soft_reset()` assigns
`ohci->intr = OHCI_INTR_MIE`, and `ohci_hard_reset()` calls it. The same run
shows the diagnostic's earlier `0xcf` write to HcCommandStatus taking the same
path.

**The specification's reset tables give zero, and one sentence of it says otherwise.**
OpenHCI 1.0a (the Open Host Controller Interface Specification for USB, release 1.0a,
September 1999, held as `docs/library/cache/io-chip/openhci-usb-host-controller-spec-1.0a.pdf`)
gives every HcInterruptEnable field a reset value of `0b`, MasterInterruptEnable at
bit 31 included (section 7.1.5, Figure 7-5 and its table, printed page 115), and the
HcInterruptDisable table, which reads back the same register, repeats `0b` for every
field (section 7.1.6, printed page 116). Its HostControllerReset description says the
reset moves the controller to USBSUSPEND "in which most of the operational registers
are reset except those stated otherwise", naming HcControl's InterruptRouting as the
example exception (section 7.1.3, printed page 112), and HcInterruptEnable is not
excepted. But the MasterInterruptEnable row of the HcInterruptDisable table ends: "This
field is set after a hardware or software reset" (page 116). Read literally, that sets
bit 31 after a reset, and it is the likely source of the core's `0x80000000`. Two reset
columns against one sentence of prose, which may mean that disabling is in effect
rather than that the enable bit reads one: the SGI board test is what decides it for
this part, since it requires zero after a HostControllerReset. *(zero inferred from
the specification's two reset tables and the diagnostic's expectation; the
specification is not self-consistent here)* The core's reading is a candidate for an
upstream report, citing that conflict; this project has not reported it.

The model restores the specification's value in this project's `lucent-ohci`
subtype rather than in the shared core, so no other OHCI backend changes: after
the device's own reset, and after a guest's HostControllerReset, through a
four-byte overlay on HcCommandStatus that passes the access to the core's own
handler first and then clears the register. `tests/qtest/usb-lucent-ohci-test.c`'s
`reset-clears-interrupt-enable` case fails on the core's value and passes with
the subtype's.

**The diagnostic's further USB checks, and what the subtype does for each.** After
the interrupt enable, `io8bt`'s reset check and its USB register section reach four more
registers where the shared core differs. The subtype handles each with at most one
four-byte overlay that hands every access to the core first, so the core's traces,
side effects and other registers are unchanged. With all four, section 0 passes with the
USB included and the USB section's register write-and-read test runs to HcFmNumber (run
`prom-20260925T005138-cafe68`) and then fails on HcFmNumber, as below.

- **HcFmInterval's FSLargestDataPacket resets to zero.** The reset check requires
  `0x00002edf` where the core answered `0x27782edf` (`sec0_1.c line:421`, register
  `#0xd`, run `prom-20260924T210825-2299a2`). FrameInterval's `0x2edf` and
  FrameIntervalToggle's `0` are the specification's reset values, but it gives
  FSLargestDataPacket's as "TBD" (section 7.3.1, figure on printed page 120, table on
  page 121), leaving it to the part; the core's `0x2778` is the value Linux programs.
  The subtype sets zero with the interrupt enable, after the device's reset and after a
  HostControllerReset. *(inferred from the diagnostic alone)*
- **HcRhDescriptorB keeps what the driver writes.** DeviceRemovable and
  PortPowerControlMask are read/write by the host controller driver, one bit per port
  from bit 1 of each half, with implementation-specific power-on values (section 7.4.2,
  table on printed page 126), and a HostControllerReset does not reset the root hub
  (section 7.1.3). The core discards writes. The subtype stores the bits for this part's
  two ports from a full-width write. *(writable and kept across a controller reset:
  established by the specification; bits 1 and 2 of each half from the established
  two-port count)*
- **HcRhDescriptorB also survives USBRESET.** The reset check writes `0x3ffff`, commands
  USBRESET through HcControl, resets the controller and requires `0x00020006` — the
  write's two port bits of DeviceRemovable and its one of PortPowerControlMask — where the
  core answered zero, because entering USBRESET makes it reset the root hub (register
  `#0x13`; the sequence traced in run `prom-20260925T003450-609115`). The specification
  leans the other way: it leaves the root-hub registers out of what USBRESET preserves,
  says the root hub "is being reset" in that state (section 5.1.2.1), and has a hardware
  reset "also reset the Root Hub" (HcControl, section 7.1.2). What makes keeping them
  permissible is that it gives the descriptor registers' reset column as "Power-On
  Reset" and says they are written during initialization (sections 7.4.1, 7.4.2 and the
  introduction to 7.4). The subtype's overlay on
  HcControl hands each write to the core and then puts HcRhDescriptorB back; the core's
  port resets stand, and the device's own reset still clears it. *(inferred from the
  diagnostic alone; the specification's prose leans against it, its reset column permits
  it)*
- **HcRhDescriptorA keeps its driver-written fields.** NoPowerSwitching,
  PowerSwitchingMode, OverCurrentProtectionMode, NoOverCurrentProtection and
  PowerOnToPowerGoodTime are read/write by the driver (section 7.4.1, printed pages 124
  and 125); the core's write mask is zero. The USB section writes zero and requires
  those fields back as zero under its mask `0x03001b00`, failing with "USB register 0x12
  has unexpected data after writing it", received `0x00000202` (`sec2_0.c line:247`, run
  `prom-20260925T003830-63bdc3`). The subtype stores the fields from a full-width write.
  What the two power-switching fields do to the ports is in the root-hub port power
  section below. The diagnostic's mask covers only bits 25 and 24 of
  PowerOnToPowerGoodTime; bits 31 to 26 are writable on the specification's authority
  alone. Unlike HcRhDescriptorB, these fields are not kept across USBRESET: entering it
  returns the register to the part's power-on value, where the core used its own.
  *(writable: established by the specification; the power-on value established by
  the USS-312 datasheet; returning to it on USBRESET is the core's choice, not
  observed on this part)*

**HcFmNumber keeps a driver's write while the controller is not operational.** With
those four, the USB section's write-and-read pass writes `0xffff` to HcFmNumber and
requires it back under a mask of `0x0000ffff`, and failed with "USB register 0xf has
unexpected data after writing it", received `0x00000000`, expected `0x0000ffff` at
`0x920000000f90003c` (`sec2_0.c line:247`, sub-section 0, condition 1, run
`prom-20260925T010746-093d27`; the write and read are in run
`prom-20260925T005138-cafe68`'s trace). The diagnostic writes it after a controller
reset with HcControl at `0x50`, USBRESUME, and again in USBRESET. OpenHCI 1.0a makes
FrameNumber read-only to the driver and written only by the controller, reset 0
(section 7.3.3, printed page 122), and the USS-312 datasheet's Table 42 repeats that
access. Neither says what a driver write does.

Three things point to this part keeping the write, all found in the diagnostics'
disassembly by the work that added the latch (QEMU pull request 915). The IO-8
test's register table gives FrameNumber the full mask `0xffff`. The IO-7 board test
for the same PCI function checks the same field. And the IO-8 table carries a second,
narrower mask for four other fields this part evidently does not keep
(HcInterruptEnable's UnrecoverableError enable, HcFmInterval's FrameIntervalToggle,
HcFmRemaining and the top of PowerOnToPowerGoodTime), while FrameNumber keeps its
full mask. The subtype therefore latches a four-byte write to
FrameNumber while HostControllerFunctionalState is anything but USBOPERATIONAL. A
write while operational is discarded, as the core does, and logged as unimplemented:
what the part does then is unknown. A controller reset still clears the field.
`frame-number-latches-when-not-operational` and
`frame-number-write-ignored-while-operational` in the qtest cover both. *(latch while
not operational inferred from the diagnostic's expectation and its masks, not
measured; it goes beyond what the specification states, which is silent on driver
writes; discarding while operational is the core's behaviour, unknown on the part)*
A register read on a real Origin 300 IO-8, writing `0xffff` and reading it back with
the controller not operational, would settle it.

### Root-hub port power

With the FrameNumber latch the USB section's register pass completes, and its next
sub-section checks port power. It commands USBRESET, a controller reset and then
USBOPERATIONAL, waits 20 ms and requires both HcRhPortStatus registers to read
`0x00000000`. The core read `0x00000100` from the first port, failing with "USB
register HcRhPortStatus indicates power is on the first usb port (bit 2^8), but power
should be off after the just completed reset" (`sec2_0.c line:319`, section 2,
sub-section 0, condition 4, run `prom-20260927T122504-c0e752`). The diagnostic then
writes HcRhDescriptorB `0x00060000` and HcRhDescriptorA `0x00000100`, selecting
per-port switching with both ports masked. It switches the first port off with
ClearPortPower and on with SetPortPower, 400 ms apart, and requires it powered and
the second port not; then it does the same to the second port and requires it
powered. It ends with another
USBRESET and controller reset and requires both ports unpowered again. The sequence
is read from the `io8bt` disassembly, not traced.

The shared core sets NoPowerSwitching at power-on and on USBRESET, reads every port
as powered whatever the port holds, and acts on both global and per-port power
commands for every port.

**What the part's datasheet and OpenHCI establish.** The USS-312 datasheet gives the
power-on values. HcRhDescriptorA is `0x10000902` (Table 45): NumberDownstreamPorts
`02h`, PowerSwitchingMode `1b`, NoPowerSwitching `0b`, DeviceType `0b`,
OverCurrentProtectionMode `1b`, NoOverCurrentProtection `0b` and
PowerOnToPowerGoodTime `10h`. HcRhDescriptorB's DeviceRemovable resets to `0000h` and
PortPowerControlMask to `0006h`, register value `0x00060000` (Table 46). Table 46 prints
the mask's bit range as "17:16", which cannot hold `0006h`; OpenHCI puts
PortPowerControlMask in bits 31:16 with port *n* at bit 16+*n* (section 7.4.2), and
`0006h` there masks ports 1 and 2, the part's two, register value `0x00060000`. Every HcRhPortStatus field resets
to `0b`, PortPowerStatus included (Table 48). So this part powers on with
per-port power switching, both ports controlled only by their own commands, and
both ports unpowered. OpenHCI 1.0a sets the switching rules (sections 7.4.1 to 7.4.4).
With NoPowerSwitching set the ports are always powered and PortPowerStatus always
reads one. With it clear and PowerSwitchingMode clear, only SetGlobalPower and
ClearGlobalPower, written through HcRhStatus's LocalPowerStatusChange and
LocalPowerStatus bits, switch the ports, all together. With PowerSwitchingMode set,
a port whose PortPowerControlMask bit is set answers only SetPortPower and
ClearPortPower, written through its own PortPowerStatus and LowSpeedDeviceAttached
bits, and a port whose bit is clear answers only the global commands.

**What the subtype does.** After the device's own reset it sets both descriptors to
the datasheet's values. On entering USBRESET it returns HcRhDescriptorA to the
datasheet's value, where the core used its own, and keeps HcRhDescriptorB as before.
A HostControllerReset leaves both alone (OpenHCI 1.0a section 7.1.3). Overlays on
HcRhStatus and the two HcRhPortStatus registers apply the switching rules: a power
command the selected mode does not let reach a port is dropped before the core sees
the write, and a global command that may reach a port switches it in the overlay.
With NoPowerSwitching clear, PortPowerStatus reads the port's own bit. The qtest
cases `root-hub-power-on-values`, `port-power-per-port-switching`,
`port-power-global-switching` and `port-power-without-switching` cover this; each
fails on the core. *(HcRhDescriptorA and PortPowerStatus power-on values
established by the USS-312 datasheet, Tables 45 and 48; PortPowerControlMask
`0006h` established by Table 46, its placement in bits 18:17 inferred from OpenHCI
1.0a section 7.4.2 because the table's printed range cannot hold it; switching rules
established by OpenHCI 1.0a, except that ignoring every power command while
NoPowerSwitching is set is the model's reading, and what a port holds when switching
is selected again is not specified; returning HcRhDescriptorA to its power-on value on
USBRESET is the core's choice with the part's value, not observed on the part)*

With both changes the USB section passes: in run `prom-20260927T124905-d888d4` the
default device mask looped through 23 passes, and the run screen counted `00000017`
passes and no failures for section 2 (console in
`docs/boot-logs/firmware.smdk-io8bt-usb-section-passes.2026-09-27T12:49:05+00:00`;
the harness's `smdk-result.json` kept the run screen). That run's overall row is not
a pass: section 1's serial receive-timer check failed once, at pass 23, the
limitation `docs/NOW.md` records for `sec1_3.c line:1077`.

**Limits of the port power model.** A power-off clears what the core's own power-off
clears: PortPowerStatus, CurrentConnectStatus, PortSuspendStatus and PortResetStatus.
OpenHCI also names PortEnableStatus, which stays as the core leaves it. The core
sets CurrentConnectStatus when a device attaches, powered or not, and does not
restore it when a port is powered again, so a guest that powers a port with a device
attached off and on loses the device until it is reattached. Over-current reporting,
per-port or global, is not modelled; OverCurrentProtectionMode is stored and has no
effect. PowerOnToPowerGoodTime is stored and nothing waits on it. The core still decodes
the PXA27x HcHReset register at offset `0x64`, which the USS-312 does not have; a
write of its reset bit there takes the core's hard reset and its own root-hub
power-on values, bypassing the part's. No machine this
project boots attaches a USB device to this controller, and no guest other than the
diagnostic is known to switch its ports.

Migration is an emulator contract rather than hardware evidence. At QEMU commit
`ebeef315652bae8abfee0038ee18680950f90ef0`, the wrapper VMState contains the
PCI device and OHCI core. The core serializes controller and schedule registers,
root-port control, one conditional end-of-frame timer, its transfer buffer and
asynchronous-transfer bookkeeping. Guest HCCA and descriptor memory migrate as
guest RAM. The OHCI stream does not serialize an attached-device bus field.
Each attached USB device migrates through its own device contract. The OHCI
VMState also has no post-load hook that recomputes the PCI interrupt level.
Origami supports migration only between identical builds. Active transfers,
attached-device state and a pending XBridge-facing INTA therefore remain
uncleared until a targeted identical-build migration test observes them.

## Frame-counter timing fidelity

Real OHCI silicon advances HcFmNumber from its own clock, so a read at any
point mid-frame sees current progress. The reused core (`hw/usb/hcd-ohci.c`)
originally advanced `frame_number` only inside its `eof_timer` callback, which
the main loop runs some time after the frame deadline, not synchronously with
it. A guest register read landing in that gap saw a stale, possibly still-zero
count instead. The Origin 300 BaseIO PROM hit this directly: it writes
HcControl to Operational, waits about 1 ms, and reads HcFmNumber expecting it
to have advanced. Under host scheduling pressure it could still read 0, and
firmware's handling of that reading dereferenced a NULL object and crashed
the boot (a TLB Refill at EPC `0xc000000013073098`, BadVAddr `0x18`).

QEMU `5a70f0e462` makes every OHCI register read run any overdue frame
boundary synchronously first (`ohci_catch_up_frame()`), for every register, not
only HcFmNumber, so a read reflects the clock rather than whichever main-loop
iteration happened to service the timer. It calls the same
`ohci_frame_boundary()` the timer would have called, so HCCA writeback,
interrupts and `frame_number` update exactly as they would microseconds later,
and that function re-arms `eof_timer` off the new deadline, so the real timer
callback finds nothing left to do for any frame the read already caught up.
HcFmRemaining already derived from the clock directly
(`ohci_get_frame_remaining()`). HcFmNumber was the one register still tied to
when the timer happened to run.

This lands in the shared `hcd-ohci.c` core rather than as a Lucent-specific
override: `usb_ohci_init()` wires the shared `ohci_mem_ops` onto `OHCIState`'s
`MemoryRegion` before this device's own realize code runs, and nothing in the
tree reassigns a `MemoryRegion`'s ops afterward. The fix is therefore generic
OHCI behavior, not something specific to this two-port part, and applies to
every OHCI backend QEMU carries (PCI, sysbus, SM501, PXA27x), even though this
project only builds and tests `lucent-ohci`. It is recorded as an
upstream-submission candidate in
the upstreamable-fixes queue (citation: `docs/upstreamable-fixes.md#hcd-ohci-catch-the-frame-counter-up-to-the-clock-on-register-read`).
The commit itself flags a defensive cap on the catch-up loop for very large
clock gaps as worth adding first if it goes upstream, which this fork has not
added.

**Limitation.** The catch-up only runs on a register read. A guest that polls
HCCA memory directly by DMA without ever reading an OHCI register still
depends solely on `eof_timer`'s own main-loop schedule, so it can still see
`frame_number` lag real elapsed time by however long that callback is
delayed. No consumer this project boots is known to do this — the BaseIO PROM
and the OpenBSD `ohci` driver both read registers routinely — so the gap is
believed to stay well inside the wide windows those consumers already
tolerate, but it has not been independently measured against an HCCA-only
consumer.

## Interrupts

The controller's PCI INTA is level-sensitive through the ordinary PCI path. The
IO-8 connects that source to head-F physical interrupt pin 5. XBridge status and
enable state then own the pin. XBridge sends a crosstalk write to the target and
host pending-bit address programmed by the guest.

There is no fixed controller-to-Bedrock pending-bit number. The 2014 capture's
Bedrock interrupt number 59 is OpenBSD's allocation on that boot. Fixed machine
wiring ends at XBridge pin 5, plus the established XBridge crossbar route to
Bedrock at host port `0xa`.

## Published diagnostic boundary

The standalone `io8bt` test supports IP45. Its section 0 covers PCI
configuration-space access, PCI reset, device-reset propagation and
device-to-device reads and writes. Section 2 covers USB, section 4 runs all IO-8
activity concurrently, and its device mask selects USB as device 5. The manual
publishes the section selection and pass/failure accounting, but not the USB
register sequence or expected values. A skipped section, timeout or unavailable
attachment is not a pass.

The online `olusb` test also supports IP45. Its default host-controller check
performs register read/write tests. The optional inventory probe enumerates
attached devices and topology and reports nonresponsive devices and hub ports
that do not supply power. The published result is `RSLT olusb PASS` or `FAIL`.
The manual does not identify every register or reset value tested.

The identity and bounded reuse qtests must pass before qualification. An
`io8bt` acceptance record must select sections 0, 2 and 4 and record the exact
pass count for each selected section with zero failures. The default `olusb`
host-controller test must report `RSLT olusb PASS`. The inventory probe is a
separate acceptance case with a declared attached USB device and must record its
own topology and result. No reviewed source establishes a stock-PROM operation
called an IO-8 check, so PROM progress is not part of this diagnostic contract.

## qtest checklist

- Enumerate exactly `0x11c1:0x5802`, PCI revision `0x10`, the USB OHCI class and
  interface, function `00:05.0`, one memory BAR and PCI INTA. Read subsystem
  vendor and device as the explicit `0xffff:0xffff` compatibility placeholders
  and reject QEMU's unrelated `0x1af4:0x1100` defaults.
- Confirm the reused core reports HcRevision 1.0 and two downstream ports.
  Exercise both root-port status registers and reject a third fitted port as a
  wrapper-reuse regression, without treating unreviewed register semantics or
  reset values as the Lucent hardware contract.
- Exercise PCI reset and HcCommandStatus reset only to prove that the subtype
  retains the existing core lifecycle. Keep every value without a reviewed
  OpenHCI or Lucent reset contract labelled as a model expectation.
- Read the complete Command/Status dword as `0x02000000` after reset. Enable
  memory space and bus mastering and require `0x02000006`, then reset the
  function and require `0x02000000` again. Verify that Status never advertises
  a capabilities list.
- Place the host-controller communication area and a minimal descriptor chain in
  PCI DMA memory, verify completion through XBridge translation, and cover a
  translation failure without bypassing the PCI address space. This is a
  regression test for existing-core reuse, not a complete Lucent transfer contract.
- Assert and deassert a serviced OHCI interrupt. Verify PCI INTA drives head-F
  physical pin 5, the guest-programmed XBridge destination receives the set and
  optional clear writes, and no fixed Bedrock pending bit is assumed.
- Save and restore the controller in an identical build. Verify the serialized
  controller, schedule, root-port, timer and asynchronous fields. A separate
  active-transfer case must include a selected migratable USB child under that
  child's own migration contract and verify the pending XBridge-facing interrupt
  level, because the OHCI core has no post-load interrupt recomputation hook.
- Confirm HcFmNumber catches up to the clock when a register read lands after
  a starved `eof_timer` (see "Frame-counter timing fidelity" above): stop the
  VM, warp the clock across several frame deadlines, and read the register
  back before resuming. `tests/qtest/usb-lucent-ohci-test.c`'s
  `/lucent-ohci/frame-number-tracks-clock-while-timer-starved` and
  `/lucent-ohci/frame-number-never-lags-elapsed-time` cover this and the
  general non-lagging invariant.

## Open facts and assignments

- OpenHCI 1.0a is now held and settles HcInterruptEnable's reset value. Review
  the rest of its register, access-width and reset contract against the core
  before claiming more of it.
- The USS-312 datasheet gives values the model does not yet use: subsystem vendor
  and subsystem ID `11C1h` and `5802h`, read-only unless system firmware opens them
  through configuration register `4Ch` (Tables 3, 17 and 18), Min_Gnt `03h` and
  Max_Lat `56h`, and an HcLSThreshold reset of `628h` (Table 44). Whether the IO-8
  firmware rewrites the subsystem fields is unknown. Replacing the `0xffff`
  placeholders and checking the rest against the core is separate work.
- A real Origin 300 IO-8 read would settle the FrameNumber latch: write `0xffff` to
  HcFmNumber with the controller not operational and read it back.
- Obtain a public PCI configuration dump from an Origin 300 IO-8 to confirm the
  datasheet's subsystem identity on the fitted part and settle BAR size as
  reported by this exact function.
- The detailed `io8bt` USB operations and expected values remain unpublished.
  Treat the diagnostic as acceptance evidence, not as a source for invented
  register behavior.
- Independent re-review may clear the identity, placement, two-port topology,
  BAR0, PCI DMA and PCI interrupt reuse contract. Complete standard register and
  reset behavior, Lucent-specific behavior and active-transfer migration remain
  outside that clearance until their named evidence and tests exist. The author
  of this public-source handoff cannot supply the review.

## Clean-room disclosure

Sources are limited to published SGI manuals, public hardware captures, pinned
public OpenBSD source and current QEMU source for reuse inventory. No web source,
restricted IRIX source, firmware disassembly or vendor implementation was
opened. Later amendments add the published OpenHCI 1.0a specification, the
published Lucent USS-312 datasheet, and disassembly of the sMDK `io8bt` binary
for the diagnostic's register sequence; disassembly is not vendor source. The author is documentation-only for this controller and cannot
implement or independently review its QEMU work.
