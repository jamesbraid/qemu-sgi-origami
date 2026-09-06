==================================
SGI XBridge crosstalk-PCI bridge
==================================

The XBridge is a Crossbow crossbar fused with two Bridge sections in
one chip. A mode pin straps it either as a crossbar with the two
bridges on internal ports 0xe and 0xf and six external crosstalk
ports (XBridge mode), or as two independent bridges with the crossbar
disabled (Super Bridge mode). The Origin 300 runs XBridge mode: its
Bedrock sits on port 0xa, the crossbar answers at widget 0, and the
two PCI buses are widgets 0xe and 0xf.

QEMU models the two halves and the chip that joins them, each testable
on its own:

* ``sgi-xbridge-head`` (``hw/pci-host/sgi-xbridge.c``) is one Bridge
  section -- the PCI host bridge with its register block, device and
  configuration windows, DMA translation and interrupt path;
* ``sgi-xbridge-xbow`` (``hw/misc/sgi-xbridge-xbow.c``) is the
  crossbar half as widget 0 presents it -- identity, crossbar-wide
  status and control, fences, lock and one register set per link;
* ``sgi-xbridge`` (also in ``hw/pci-host/sgi-xbridge.c``) is the chip:
  one crossbar and two heads at widgets 0xe and 0xf, and the routing
  between them and one host port.

Everything the Bridge page states carries over to the head unless this
page says otherwise; the deltas come from the XBridge ASIC
specification through the project's reviewed datasheet.

The head's register block
=========================

Registers are 32 bits in the low half of an eight-byte slot, reached
by a 32-bit access at the slot's offset plus four. Any other width
or an undefined offset is the invalid-address error (status bit 24),
posted rather than faulted, as on the Bridge.

The identification register reports part ``0xd002`` under
manufacturer ``0x024``, revision 2; OpenBSD keys its attach on that
pair, and the Bridge's ``0xc002``/``0x036`` would attach nothing.
The status register's low byte is eight input pins, so bit 5 -- the
Bridge's PCI-not-GIO flag -- is a pin the SN1 consumer still tests,
and it reads one. The control register's PCI speed field is
read-only, fed from a ``pci-speed`` property; its widget-ID field is
writable and reads back what was written. The direct map register
resets with the add-512 MB bit set and has no remote-map bit; the
device registers have no page-mapped write-gather or swap bits (both
are accepted and dropped, since both public drivers still write
them). There is no external SSRAM window: that range is reserved and
an access to it is the invalid-address error.

Two registers are new. The error-interrupt view at ``0x170`` mirrors
the status register's error bits whether or not they are enabled,
and the same group strobes clear it. The multiple-interrupt register
at ``0x178`` is read-only and resets to zero; the specification does
not say what detects a recurrence, so nothing sets it. Eight
force-always registers send a pin's set packet on any write, and
eight force-with-pin registers do so only while the pin is asserted;
a force on a masked pin is logged and not delivered, because the
specification does not say which of the two happens.

The bridge-half NIC register does not drive the one-wire pin in
XBridge mode -- widget 0's does -- so its writes are logged and not
stored. The registers the datasheet carries over from the Bridge
unchanged -- test pin control, arbitration, PCI timeout, interrupt
device and the two read response buffer registers among them -- are
stored with the Bridge's reset values and fields. The address-match
buffers, performance counters, the eight-byte interrupt-acknowledge
slot and the 4 MB section are defined but deferred: reads return
zero, writes are dropped, both are logged, and none is an
invalid-address error. The rest of the acknowledge slot's page is in
no map entry and is one.

Errors of the head's own
========================

The first invalid access latches its address into the error address
registers whether or not bit 24 is enabled: enabling gates delivery,
not ownership of the record. With bit 24 enabled the head sends a set
packet to its interrupt destination carrying the host-error field's
number with bit 8 set. A later invalid access neither overwrites the
record nor sends again until the request-dispatch group strobe
(reset-interrupt-status bit 3) clears bit 24 and rearms it.

The latched value is the request's widget-relative address, bits
47:32 in the upper register and 31:0 in the lower, without the
widget number: inside the face it equals the face offset, above the
face the full value. Through the chip it is at most 44 bits wide.

Configuration and device windows
================================

Type 0 and type 1 configuration space and the eight device windows
follow the Bridge page. One thing differs: a configuration read of
an empty device raises the master-timeout bit in the error-interrupt
view, whether or not it appears in the status register, because the
SN1 consumer probes with a guarded read and then reads the view.

Space above the face
====================

A widget's space is 48 bits wide and the head decodes its lower
8 GB. Above the 16 MB face, ``0x4000_0000`` to ``0x7fff_ffff`` is
PCI memory ``0x0`` to ``0x3fff_ffff`` for a 32-bit target,
``0x8000_0000`` to ``0xbfff_ffff`` the same gigabyte for a 64-bit
target, and ``0x1_0000_0000`` to ``0x1_ffff_ffff`` the whole of PCI
I/O space; the offset within a range is the PCI address. Byte
placement follows control bit 22 for the memory ranges and bit 23
for the I/O range: with the bit clear a narrow access lands mirrored
within the target's width, as a device window places it without its
swap bit; with the bit set it lands where addressed. That the alias,
not a device register, selects the 64-bit mirror is the datasheet's
inference, and the test asserts it as the model's reading.

Everything else in the widget's space, the gaps between the ranges
and all of it above 8 GB, is an undefined address: the
invalid-address error of the head's own, with the address recorded.
Through the chip a widget-relative address is at most 44 bits, so
the datasheet's rule for bit 47 has no reachable case.

DMA
===

A device's 4 GB of PCI space divides by its top bits:

* below 1 GB, nothing answers, except the flush range at
  ``0x3fff0000`` which completes at once with nothing to drain;
* the second gigabyte's first 8 MB goes through the translation
  table (below);
* the upper 2 GB is the direct window: target widget and crosstalk
  bits 47:31 from the direct map register, byte swapping from the
  device register's direct-swap bit;
* at and above ``1 << 60`` a 64-bit cycle carries target widget in
  bits 63:60 and swap in bit 55. Target 0 is not supported and gets
  no response, and neither does any address from ``1 << 32`` up to
  the 64-bit window.

Each target widget is a link to a memory region; the head keeps a
plain and a byte-swapping address space for each, so byte placement
follows the XBridge's own swap tables.

Translation table
=================

The table holds 512 entries of 64 bits, written whole and read back
as two 32-bit words through a second region; writes to that region
are ignored. The specification says 512 in three places; both public
headers say 1024, and the model serves 512 and ignores writes to the
next 4 KB rather than guessing which is right.

Translation is per page, 4 KB or 16 KB by control bit 21: the index
is PCI bits 29:12 or 29:14 and is rejected above 511 rather than
wrapped, the target widget is entry bits 11:8 (zero allowed here,
unlike the 64-bit window), the crosstalk address is entry bits 47:12
with bits 13:12 ignored for 16 KB pages, and swapping follows entry
bit 5. An invalid entry or an index past the table sets status bit
30 and its view mirror and issues no transaction; reset bit 1 clears
it. Addresses from ``0x40800000`` to ``0x7fffffff`` are refused,
because the specification's flow chart still routes them to the
mapper and the reviewed datasheet leaves them open. The map fault
address register exists as storage only: nothing publishes its
layout.

Interrupts
==========

Eight pins per bus. Status bit *n* follows pin *n* even while
masked. A pin's rising edge sends a set packet -- the host address
register's number with bit 8 set -- to the interrupt destination,
with the host register's bits 17:8 overlaid on crosstalk address bits
47:38; the falling edge sends a clear packet only when both the
enable and the mode bit are set.

Which slot's INTx reaches which pin is board wiring the specification
does not predefine. The head therefore exposes 32 named outputs,
``pci-intx``, one per slot and pin, carrying each device's level out
unchanged for a board to wire to the pins; the head assigns nothing.
The levels are migrated and cleared on reset.

The crossbar half
=================

The block answers on one MMIO view per port, because the status
register reports in bits 9:6 which port a read arrived on, and the
Bedrock and both heads read it. A view is 16 MB; only ``0x300`` of it
is registers.

Every register is a 32-bit access at its slot's offset plus four. Any
other width, a reserved offset, or a write to a read-only or
read-and-clear register sets the register-access-error bit (status
bit 5). The chip reports the error rather than faulting the access,
so the model does too.

Reset values follow the XBridge datasheet where it departs from the
Crossbow: control resets to ``0x80`` (the link timeout counters
enabled), the interrupt destination to target 8 with bit 19 fixed at
one, the performance counter selects are three bits wide and reset
to link 8, the packet timeout to ``0xfffff``, LLP control to
``0x03ff1810``, the arbitration reload to 2, and every reset fence to
all ones. The lock accepts a non-zero write only while it reads zero
and is released through the lock-clear register, whose read returns
the lock.

An enabled condition sends one set packet -- the destination's vector
with bit 8 -- and sets the widget-0 request bit. Further conditions
accumulate without another packet. Reading the status register's
clearing alias returns the status, clears the widget-0 conditions and
the request, and sends the clear packet if one was outstanding.
Writing the error command word rearms error logging and clears both
error address registers. The data is discarded.

Links carry the population as a property. An attached link comes up
alive and reports presence in its auxiliary status. An empty port
goes to initialization failure (maximum-retry and failure-mode set,
alive clear) and stays there. Link control's force-bad-packet bit is
a one-shot that reads zero. A warm reset arriving on a port that the
widget-0 fence admits resets the core and goes out on every other
port whose fence admits the source, judged by the fences as they
stood before the reset returned them to all ones.

A read of a write-only link-reset register is a register-access
error like the others, condition and interrupt; what data the chip
answers is not specified, and the model returns zero.

Not modeled, and logged when touched: link training timing,
software-disabled links, arbitration, performance counting, buffer
flushing with traffic (an empty flush completes at once), injected
link errors, the link-reset one-shot's effect on a link, the address
and command word a condition would log in the error registers, which
are storage here, and the one-wire pulse behind widget 0's NIC
register.

The chip
========

``sgi-xbridge`` owns a crossbar and two heads as children and is what
a machine instantiates. Two properties are required and realization
is refused without them: ``fabric``, a link to the memory region that
receives everything the chip sends toward its host (interrupt and
error packets, DMA), and ``host-port``, the crossbar port the host
occupies, 8 to 0xd. The Origin 300's Bedrock sits on 0xa and the
machine must say so. The crossbar's presence map is the host port and
the two heads.

The chip's first MMIO region is the crosstalk space as the host sees
it: sixteen slots of 16 TB, one per widget. Slot 0 is the crossbar's
view for the host's port, so the status register reports the host's
port number; slots 0xe and 0xf are the heads' whole widget spaces,
the 16 MB face and the PCI ranges above it. Every other slot reaches
nothing: reads return zero, writes are dropped, both are logged.

A host that decodes the widget number itself, as the Bedrock does,
links one region per widget instead. ``sgi_xbridge_widget_view``
returns the chip's own view of widget 0, 0xe or 0xf: the widget's
whole slot. The three views are also MMIO regions 1 to 3. They exist from instance init, so
the host's link to a view and the chip's ``fabric`` link to the host
can both be set before either device is realized.
``sgi_xbridge_pci_bus`` returns a head's PCI root bus by widget once
the chip is realized; the buses are also named ``head-e.0`` and
``head-f.0`` after the children.

Each head's interrupt pins and raw INTx sources are the chip's GPIO
groups ``head-e-pin`` and ``head-e-pci-intx``, and their ``head-f``
twins, carrying the head's lines unchanged. Which source drives which
pin is board wiring: the machine connects the two groups to each
other, and the chip assigns nothing.

Widgets 0xe and 0xf are fixed destinations. Rewriting a head's
widget-ID field changes what it reads back, not where it answers; the
source identity a real head puts in its packets is not observable in
this model, whose packets are plain writes.

A warm reset arriving on the host's port resets the crossbar core, as
the widget-0 fence allows, and is forwarded to the ports whose fences
admit the source. Inside the chip the forwards for 0xe and 0xf reset
the heads. The chip's ``warm-reset`` inputs and ``warm-reset-out``
outputs carry the six external ports only, 8 to 0xd at index port
minus 8; the heads' lines stay inside the chip, so a machine has no
alias to avoid. Whether a warm reset the widget-0 fence rejects still
forwards to the links is not settled by the datasheet, and the model
forwards nothing in that case. The 1024-cycle window in which a port
ignores further reset patterns is not modeled.

Migration
=========

The head's stream carries its register file, the translation table,
the pin levels and the raw INTx levels, which are re-driven on load.
The crossbar's stream carries its register file and its latched
status, so a pending assertion still clears on the far side.

Not yet modeled
===============

* routing between the two heads
* the 4 MB flash section behind port 0xf
* the performance counters and address-match buffers
* Super Bridge mode and the PIC
* the 1024-cycle reset-ignore window
