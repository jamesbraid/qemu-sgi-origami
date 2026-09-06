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

QEMU models the two halves separately, each testable on its own:

* ``sgi-xbridge-head`` (``hw/pci-host/sgi-xbridge.c``) is one Bridge
  section -- the PCI host bridge with its register block, device and
  configuration windows, DMA translation and interrupt path;
* ``sgi-xbridge-xbow`` (``hw/misc/sgi-xbridge-xbow.c``) is the
  crossbar half as widget 0 presents it -- identity, crossbar-wide
  status and control, fences, lock and one register set per link.

Composing them into the chip, with packets routed by destination
widget, is not yet modeled. Everything the Bridge page states carries
over to the head unless this page says otherwise; the deltas come
from the XBridge ASIC specification through the project's reviewed
datasheet.

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
stored.

Errors of the head's own
========================

The first invalid access latches its address into the error address
registers whether or not bit 24 is enabled: enabling gates delivery,
not ownership of the record. With bit 24 enabled the head sends a set
packet to its interrupt destination carrying the host-error field's
number with bit 8 set. A later invalid access neither overwrites the
record nor sends again until the request-dispatch group strobe
(reset-interrupt-status bit 3) clears bit 24 and rearms it.

The latched value is the offset inside the head's 16 MB window, not
the 48-bit crosstalk request address a Bridge captures: the head
does not know its own address in the crosstalk space. That is model
behavior, kept until the crossbar composition can supply the full
address.

Configuration and device windows
================================

Type 0 and type 1 configuration space and the eight device windows
follow the Bridge page. One thing differs: a configuration read of
an empty device raises the master-timeout bit in the error-interrupt
view, whether or not it appears in the status register, because the
SN1 consumer probes with a guarded read and then reads the view.

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

Not modeled, and logged when touched: link training timing,
software-disabled links, arbitration, performance counting, buffer
flushing with traffic (an empty flush completes at once), injected
link errors, the link-reset one-shot's effect on a link, and the
one-wire pulse behind widget 0's NIC register.

Migration
=========

The head's stream carries its register file, the translation table,
the pin levels and the raw INTx levels, which are re-driven on load.
The crossbar's stream carries its register file and its latched
status, so a pending assertion still clears on the far side.

Not yet modeled
===============

* the chip composition and destination routing
* the 4 MB flash section behind port 0xf
* the performance counters and address-match buffers
* PCI memory space above the head's 16 MB register window
* Super Bridge mode and the PIC
