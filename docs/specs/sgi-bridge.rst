===============================
SGI Bridge crosstalk-PCI bridge
===============================

The Bridge ASIC is a crosstalk widget on one side and a PCI host
bridge on the other. QEMU models it in ``hw/pci-host/sgi-bridge.c``:
the widget-local register block, eight device windows, a PCI
configuration window, an address translation table, the interrupt
path into the Hub, and the one-wire bus to the board identity part.

Values below marked as specification facts come from the Bridge ASIC
specification; the public Linux and OpenBSD drivers supply bit
positions but no power-up values.

Register block
==============

Every register is 32 bits or narrower and occupies data bits 31:0 of
an eight-byte slot, reachable only by a crosstalk double-word packet
-- a 32-bit access at the slot's offset plus four. Linux's header
names that +4 address; OpenBSD names the slot and folds the four
bytes in at access time. The two sources look four bytes apart
everywhere and are not. The other half of a slot holds no register.
Any other access width is an error, in both directions: an over-wide
access is not split and a narrow one is not widened.

The reset state is heavily non-zero. The control register alone
comes up with seven fields set; a Bridge resetting it to zero would
report widget 0 and little-endian byte order. Distinguish two kinds
of zero in the model's reset table: registers the specification
resets to zero (asserted by qtest) and registers the specification
leaves blank, where zero is the model's choice.

An access to an undefined address inside the 64 KB register block is
*posted*, not refused: the access completes and the invalid-address
error latches in the interrupt status register (bit 24), which is
how the real chip behaves and what the firmware's own self-test
requires -- it provokes such an access deliberately and fails on any
synchronous exception. The latched bit clears through the
reset-interrupt-status register's group strobe for its group only.

Reading the target-flush register is an operation, not a query: the
read completes once every earlier programmed access to the widget
has, and its value means nothing. It must never fault. The bus
time-out register's retry hold is five bits at 20:16 -- a four-bit
model would read a guest's value back with its top bit missing -- and
the control register's two link-retry-counter strobes accept a write
and read back zero.

Widget 0 and the crossbar
=========================

An Origin 200 has no crossbar: the Hub's one crosstalk link ends at
the Bridge, so every widget address the Hub emits reaches the same
chip, and widget 0 -- the address a real Crossbow's own registers
answer at -- is the Bridge answering a second time. Guests read
widget 0's identification register first and decide the machine's
shape from it, so the identity presented there is a machine
property: an Origin 200 reports the Bridge's part and manufacturer,
where a real Crossbow reports part 0 and manufacturer 0 as its true
identity. The crossbar model (``hw/misc/sgi-xbow.c``) exists for
machines that have one: its registers are crosstalk double-words at
their slot's offset plus four, the slot's upper half faults, and it
claims its whole widget space so an undecoded address is a reported
error rather than an answer from whatever sits underneath.

Device windows
==============

Each of the eight PCI slots is reached through a window in the
widget's space. The first two windows are 2 MB and the remaining six
1 MB -- they are not evenly spaced. Each window is aimed by its
slot's Device(x) register: the base field supplies PCI address bits
31:20 (one count per megabyte) and a bit selects PCI memory or I/O
space. For the 2 MB windows, bit 20 arrives from both the base and
the offset and the chip adds them, so a window based on an odd
megabyte reaches across the boundary above it -- OpenBSD computes
the same addresses.

The Device(x) reset values are a firmware fact, not a specification
one: the register table gives no power-up value for the base field,
but the firmware's sanity diagnostic states all eight expected
values, which lay the default regions end to end from PCI address
zero (device *n* at 2\ *n* MB for the first two, *n*\ +2 MB after).
Three bits reset set, including memory-space select. The
page-cross-check bit is one the specification calls an enable and
Linux a disable; the descriptions agree, only the names differ.

I/O space windows address the full 32 bits, as memory windows do:
this chip does not narrow I/O space to 64 KB the way a PC chipset
does.

Configuration space
===================

Type 0 configuration space is decoded as eight devices 4 KB apart,
eight functions 256 bytes apart. A configuration double-word is
returned as a value, so on this big-endian machine a guest reaching
for one byte aims at the window address whose byte lanes carry it:
Linux composes the register offset exclusive-or (4 - width), and the
decode undoes exactly that. OpenBSD only issues 32-bit configuration
reads, for which the correction is zero.

A configuration cycle no device claims -- an empty slot, or a
function a card does not have -- is a master time-out. When the
time-out's enable bit (interrupt enable bit 11) is set, the chip sets
interrupt status bit 11, latches the cycle's PCI address (the
device's ID select line, 24 plus the device number, with the
function and register word) and the device number in the PCI error
address registers, and sends the error interrupt. A second PCI-group
error before software clears the group sets the multiple-error bit
(31) and keeps the first address. With the enable clear nothing is
recorded, which is how every guest probes. An unclaimed read also
fails back to the processor as a bus error; the data floats all ones
for a caller that sees a value without the failure. An unclaimed
write posts. The IP27 PROM, IRIX, Linux and OpenBSD all read empty
slots inside a bus-error guard and take the fault as "nothing
there"; the PROM also tests bit 11 after a read that completed. The
type 1 window is not modelled (no PCI-to-PCI bridge on this machine)
but its steering register exists as storage.

Interrupts
==========

The chip has eight interrupt pins for eight slots of four INTx lines
each: pins A and C of slot *n* reach chip pin *n*, pins B and D
reach *n* xor 4. Neither guest reads this mapping from hardware --
Linux and OpenBSD both compute it, with no shared code, which is
what makes it a determined fact a model may implement. The base I/O
controller's second interrupt is not part of this mapping: it is
board wiring, not a PCI interrupt, and reaches its pin directly.

The interrupt status register is the one register that must not be
storage. Its low eight bits report each pin's level and must follow
it, never latch: guests re-arm in software by re-reading the
register at the end of each service, and a latched bit would tell a
guest that a device with a second condition pending had gone quiet
-- permanently. The model composes the register on read from the
live pin levels, ORed with the latched error bits above them (a past
event a driver reads after the fact). PCI-driven and wire-driven
levels are tracked per source domain and ORed per pin, because both
domains can drive one pin at once and each must hold it on its own.

Interrupt delivery is a write across the fabric into the Hub's
interrupt-modify register, sent on a level *transition* rather than
while a level holds. The per-pin destination register holds a
compressed address -- bits 17:8 are crosstalk address bits 47:38,
bits 7:0 the interrupt number -- a layout three sources state
identically. A pin whose destination was never programmed sends
nothing. On a falling edge a clear packet is sent only if the guest
selected that mode; otherwise the guest clears the Hub bit in its
own handler. Bridge-internal errors are reported separately, on the
host bit named in the control register's host-error field, to the
full address in the widget interrupt-destination pair.

DMA address map
===============

A device sees 4 GB of PCI address space divided by its top two bits:

* below 1 GB: local, not carried to crosstalk. The model passes
  local-window DMA through to system memory, which real hardware
  does not -- a recorded fidelity gap the byte-lane swap rule
  deliberately does not mask;
* the second gigabyte: through the translation table;
* the upper 2 GB: the direct window -- the direct-map register's
  offset field supplies crosstalk bits 47:31 and the low 31 PCI bits
  pass through. The add-512-megabytes bit resets *set* and, when the
  offset field is zero, steps the window over the processor's low
  compatibility segments.
* at and above ``1 << 32``: a 64-bit cycle. Bits 47:0 are the
  crosstalk address and bits 63:60 the target widget; the offset
  registers are bypassed, so a NASID at bit 32 survives.

Translation table
=================

An entry is 64 bits, written whole -- the one exception to the
32-bit access rule -- and read back in halves: the upper half at the
entry's offset plus four, the lower half through a second, read-only
region of 128 32-bit words. Any other access shape is refused,
including a 32-bit write (it would leave half an entry) and a write
to the read-only region (there is nothing there to change).

Translation is per page, 4 KB or 16 KB by a control-register bit,
and the model returns a one-page mask from its IOMMU translate hook
so a transfer crossing pages is translated once per page -- a
translate-once model is correct inside one page and wrong for the
tail of every crossing transfer. An entry whose valid bit is clear,
or an index past the 128 internal entries (this machine fits no
external table SRAM), refuses the transfer. Reset clears whole
entries, not just valid bits: a stale page address behind a clear
bit is a working translation waiting for one write.

Map notifiers are refused: nothing invalidates a cached mapping when
the guest rewrites an entry, so a consumer that caches (vfio, vhost)
would run on stale translations.

Byte lanes
==========

The chip's per-window swap bits (Device(x) bits 19 and 20, one for
the direct windows and one for the translated window) enable a
swapper that turns the crossed wiring between the big-endian
crosstalk side and the little-endian PCI side into straight-through
byte numbering. A byte-addressed model is straight-through already,
so the model corrects the *disabled* case -- the reset state, and
the state both guests run in. The correction moves only the low two
address bits: a transfer never crosses the 32-bit word its lanes are
defined within (specification Tables 65 and 73).

Which window applies is judged on the raw PCI address the device
issued, before any translation rewrites it. Local-window transfers
are never corrected -- local space is not carried to crosstalk and
has no swapper in its path.

The IOC3 is exempt: it is a big-endian PCI part and both public
drivers clear its swap bits. The exemption is board policy handed
down per slot, not something the chip could know -- real silicon has
no notion of PCI identity.

A corrected transfer that the fabric refuses is refused to the
device: the swap path forwards the inner access's result rather than
answering with zeroes, so a device's DMA error handling sees the
same refusals on a swapped slot as on a direct one.

Board identity part
===================

A one-wire bus master register drives the board identity EPROM one
bit slot per write: two timing fields (pulse and sample delay, in
microseconds) start a slot, bit 0 returns the sampled level and
bit 1 reports completion. The model completes each slot within the
write. Slot kinds are recovered from timing *thresholds*, not exact
values -- Linux resets with a 520 microsecond pulse where OpenBSD
uses 500, and a read uses the same short slot as a written one, so
the part answers whatever the conversation expects next.

The part is a DS2505, the 2 kbit member of the Dallas add-only EPROM
family, which is what its family code 0x0b names. It answers both
discovery forms: a search (each identity bit offered with its
complement, the guest's choice written back) and a direct match
against the 64-bit identity -- family code, six serial bytes from
the machine, and the standard Dallas/Maxim CRC-8. The family code is
what a guest matches to decide it knows how to read the memory.

Either form leaves the part selected, so a memory function follows
without a second reset -- the datasheets' ROM-function flow converges
on that for a search, a match, a Read ROM and a Skip ROM alike. Only
a match is a decision the part can fail, and it drops out at the
first identity bit that fails to compare rather than clocking all
sixty-four before deciding. An unimplemented command leaves the wire
idling high and is logged, because silence is otherwise
indistinguishable from an absent part; a match naming some other
identity is not logged, since dropping out is exactly how every part
but the named one is meant to behave.

The data memory is 2048 bytes, 64 pages of 32. The board record
occupies the first two pages and every byte past it reads
unprogrammed. Two commands read it with different framing. The
extended read sends, per page, a redirection byte, a check pair over
the request and that byte, the page's 32 bytes, and a check pair
over the page -- then carries straight on into the next page's
redirection byte for as long as the guest keeps reading, to the end
of the memory. The plain read sends nothing but stored bytes, and
only a read that runs through the last byte of the memory is
followed by a check, over the command, both address bytes and every
byte read. Neither command wraps: an address the record does not
cover reads unprogrammed, and a start address above the memory has
its high address bits forced to zero.

A page's own embedded check -- the one the board-identity reader
validates before trusting a part number -- is *stored* in the page's
last two bytes, and is not framing that either command adds. A
part's own check always covers a whole page, so a check embedded
inside one is a software record convention rather than anything the
silicon computes. The check is the reflected CRC-16 with the result
stored inverted: the guest runs the check across the whole exchange
and requires a fixed residue.

The identity page serves two readers at once: one takes the part
number positionally from offset 11, the other scans for
``name:value;`` fields. The field name occupies offsets 6-10 so its
value lands at 11, satisfying both; the semicolon at offset 5 is
load-bearing, because the scanner skips past the next semicolon on a
miss and without it never reaches offset 6. Byte 0 is the vendor
manufacturing-data format and must be 1 or the PROM rejects the page
before constructing fields. The part number is the one a real
Origin 200 reports, traced through the firmware that reads it.

The status memory is separate, read by its own command eight bytes
at a time with a check pair after each (the first page's check also
covers the command and address; later pages are checked over data
alone, because status memory is field-programmable and a stored
check could not stay valid). It is decoded across the same 2048
bytes of address the data memory is, of which only four small
regions are physically programmable. This model can program none of
them, so every status byte reads unprogrammed -- this is EPROM, bits
leave the factory set -- and an all-ones redirection byte is exactly
the value that says a page is valid where it sits. Past the
programmable regions the part keeps answering framed pages of ones
rather than falling silent, and a start address above the field has
its high address bits forced to zero the way the data memory's does.

External SSRAM window
=====================

The board fits no external SSRAM. The window completes every access
with reads floating high: the PROM sizes the RAM with an unguarded
write-and-read-back probe expecting a completing access, and
all-ones differs from any probe pattern, so the size probes as zero.
Writes are dropped.

External flash port
====================

The same SSRAM pins double as a flash port: two 2 MB chip selects, a
strap (``FLASH_SELECT``, mirrored in the status register) choosing
which one answers at the window base, and the control register's
write-enable bit gating writes. Unlike the external SSRAM window,
an unfitted flash port decodes as an error rather than completing --
the specification ties the flash port to a fitted part, where the
SSRAM port stays wired regardless. A fitted part answers reads of
any crosstalk size, repeating its sixteen-bit words across the
access; writes complete at sixteen bits only, on account of the
parts' differing command sequences, and are logged and dropped,
without an error, while the write-enable bit is clear. The model
reuses ``pflash_cfi02``, the AMD command-set part QEMU already
carries, rather than a bespoke device: the fitted card's own class
builds it sized to the part and hands the Bridge the resulting
memory region.

Migration
=========

The migration stream carries the register file; the device windows'
aim and enable state are re-derived from it on load, exactly as
reset derives them from reset values. A stream from before the pin
word split into its two source domains attributes the carried levels
to the PCI side -- a wire level is re-driven by its source device's
own restored state. The one-wire conversation and the stored
identity image both migrate, in the identity part's own stream
rather than the chip's. The chip's stream keeps sixty bytes of
padding where its own responder's state used to sit, so a stream
written before the part became a device of its own still loads.
