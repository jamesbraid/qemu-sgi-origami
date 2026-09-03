=========================================
SGI Origin 200 machine and firmware model
=========================================

The Origin 200 is the one- or two-chassis member of SGI's SN0
family, built on the IP29 node board. The resolved Origami model is
the sole authority for the machine's canonical static topology:
profiles, components, ownership, buses, addresses, aliases,
interrupts, identities, and the firmware graph. Its compiler emits
``hw/mips/sn0-origin200-model.c`` as an ordinary committed QEMU build
input, so building QEMU needs neither Origami nor Python. The generic
SN0 model consumer constructs its selected profile from that
descriptor, and semantic adapters map the descriptor's component
kinds to QOM implementations. QEMU owns construction mechanics,
device behavior, register and DMA handling, reset, migration, and
object lifetime. The canonical topology has one
directly connected Bridge per chassis with two SCSI adapters and the
IOC3 base I/O controller on its PCI bus, four IP29 memory banks, and
no crossbar. The synthesized firmware
(``hw/mips/sn0-firmware.c``) builds what a real IP27 PROM leaves
behind for a guest -- the programmed boot flash and its configuration
block, the ARCS call vectors and the routines behind them, and the
KLCONFIG configuration graph -- and owns the direct ``-kernel`` and
sashARCS loaders, because loading a guest and handing it control is
the firmware's job. Device pages: :doc:`sgi-hub`, :doc:`sgi-bridge`,
:doc:`sgi-ioc3`, :doc:`sgi-qlogic-isp`, :doc:`sgi-elsc`.

No crossbar
===========

The Hub's single crosstalk link ends at the Bridge, with nothing in
between to route by widget number, so every widget address the Hub
emits reaches the same chip. SGI's own firmware exploits this: a
directly connected Bridge may be given whichever widget number the
firmware chooses, and it chooses 8, avoiding 0 because SGI's own
firmware cites an error-handling problem at widget 0 -- an account
never shown to be the same defect as any this tree has found. The
model declares the Bridge's whole widget face at widget 8 and aliases
at widgets 0, 2-7, and 9-15 (the IRIX miniroot stamps a device pointer
with widget 2). The generic consumer installs those mappings. OpenBSD
panics if widget 0's identification register does not read. Widget 1
is the Hub. One vocabulary trap spans the sources: the Hub translation
entry's bit 12 selects what Linux and
IRIX call "I/O space" and OpenBSD calls "device space", while the
Bridge's own "device space" is a third, unrelated usage.

The firmware enables PCI bus mastering on the adapters, which is
currently redundant rather than proven load-bearing: both guests
self-enable mastering, and no DMA has been observed to depend on the
firmware's write in this configuration.

Memory
======

The IP29 board has four banks, each a matched DIMM pair of 64, 128,
256 or 512 MB; bank zero must be present. The machine derives a
deterministic population from ``-m``, preferring larger banks and
fewer partially populated pairs, and rejects totals it cannot
express. IRIX decodes faulting addresses through the 512 MB bank
shift and mainline Linux indexes bank sizes by slot over four at a
128 MB stride, which is what sources the slot layout. Bank zero is
held at 256 MB or less: a 512 MB bank zero
would fill all four of its slots, including the fixed boot-flash and
firmware addresses at the top of the stride -- a model limitation,
not an IP29 rule.

The ARCS memory descriptor list a guest walks carries two
load-bearing facts. The base page of the first free entry is how a
guest tells this machine from its successor -- at or above page 0x20
it silently takes the later machine's boot path -- so free memory
starts at page 0x19, where a real machine of this generation puts
it. The walk stops at the first firmware-permanent entry, which the
guest rewrites to reach the 32 MB mark, so nothing after it is read;
pages 2 through 0x18 are described by nothing at all, as on a real
machine.

Base I/O
========

The IOC3 is a child of the node that carries it, not of the machine,
with its ethernet, transceiver and timekeeper children under it. Its
second interrupt is not a PCI interrupt: the Bridge's INTx fold would
land it on the ethernet's pin, so the model declares a direct
connection to the Bridge input the real board uses. Each node's base
I/O board carries its own laser-programmed one-wire serial, distinct
per node so that firmware and a guest's one-wire bus never merge two
physical nodes that fall back to their base I/O identities. The
timekeeper's battery is a per-node ``mtd`` drive.

The model declares the Bridge, both SCSI adapters, and IOC3 as
unconditional topology. The adapters occupy PCI devices 0 and 1, and
IOC3 occupies device 2. They are created with no drive: a disk arrives
on the command line, and the command line's disks are offered to every
adapter by bus number (node 0's first adapter is bus 0 and takes
``-hda``). The model also declares the Bridge register face and its
aliases. The removed ``io-bridge`` property cannot select a bridgeless
machine.

Boot flash
==========

Each node's boot flash is a standard AMD-command NOR device -- an
Am29F080, 1 MB in sixteen 64 KB sectors, byte-wide -- modelled by
QEMU's ``pflash_cfi02`` with no backing drive; the firmware builder
seeds the synthesized image after realize. Its region is never
mapped directly; every guest-visible view is a view of that one chip
-- through the Hub's flash controller decode between the 64-bit junk
bus and the byte-wide chip for the windows, or as a direct romd
alias where code must execute. Through the decode, reads pack the
chip 1:1, image byte *N* at bus
offset *N*, and the Hub does not address-error decode the upper
boot-space bits, so the 1 MB image repeats across the 256 MB LBOOT
and RBOOT windows -- guest code genuinely lands on offsets past the
first repeat. Command writes present the chip's byte-wide command
port on 64-bit cycles only, low byte used, chip byte *N* at bus
offset 8\ *N*, the mapping the vendor flash programmer states
directly.

Where code executes -- each node's own LBOOT window -- a direct romd
alias of the chip overlays the decode, because the PROM fetches its
relocated code there and an I/O region cannot back translated code.
The remote node's RBOOT window is only ever a maintenance read and
write path, never a fetch path, so it carries no overlay. The reset
vector view is an alias of the megabyte of Hub space at the flash
base, reached through the compatibility space; it overlaps RAM and
wins. Writes through it land on the chip's own byte-addressed decode
rather than the controller's strided command port -- an alias cannot
route reads and writes differently, and this divergence is deliberate
and confined: every observed command sequence, and the vendor flash
programmer the datasheet cites, drives commands through the Hub
window, and no source establishes the reset view's own write decode.

The chip migrates its own state through the qdev tree. The machine's
own flash descriptor keeps its wire name and a placeholder for the
bytes the retired state machine occupied, so earlier streams still
load; their legacy state is discarded rather than translated -- a
stream captured inside a flash command sequence would wake in
read-array mode with the sequence forgotten, a window a handful of
guest stores wide that no retained snapshot sits inside.

Cache-as-RAM boot phase
=======================

Before memory is configured, the PROM runs with the 32 KB at the
1 MB mark held dirty-exclusive in each R10000's data cache as its
stack. The model marks the phase in FPR 0 bit 1 -- the PROM's own
software convention -- disables the RAM overlay for the window
one-way, and migration reapplies the phase progress. The firmware's
memory sizing then probes through the back door: a pattern write and
zero read-back classifies an absent bank, the back-door address is
the physical address shifted right two so quarter-span aliasing
sizes a bank, the premium directory format keeps six bytes per line
where plain keeps two, and the probe's restore step writes all eight
bytes back.

Configuration graph (KLCONFIG)
==============================

The graph is a header carrying the console descriptor inline, then
the board records a single-node Origin 200 emits, in the order and
shape a real machine of this generation produces rather than the
tidiest one.

A component record carries three identifiers that must not be
confused. ``index`` is its position in the flat graph array, unique
and sequential across every type. ``virtual_id`` is numbered by
ordinal within the component's own type; IRIX requires a processor's
to be its processor number and checks it for range and uniqueness.
``physical_id`` means something different per type -- a processor's
slice, the Hub's node, a memory bank's index, the Bridge's widget
number, a controller's PCI device number -- and is written for every
component: the vendor firmware reads it, not the widget field, to
learn which widget the Bridge is, and a zero sends it to widget 0.
Two bytes the reference declaration calls padding are an OpenBSD
bitfield of PCI routing (controller port, bus, multifunction flag,
function) and are zeroed on purpose; the back-pointer to a matching
firmware component is zeroed for the same reason.

A component's port descriptor packs a sixteen-bit node identifier,
a flag byte, padding and a 32-bit offset. Zero is a live node
identifier, so a port that names no link carries the invalid marker
-- all ones in the node-identifier bits -- which every port on this
machine does.

Board records are a linked list terminated only by a zero next-board
offset, and a consumer reads a record before checking its link. Three
boards are emitted:

* the node board -- processors, Hub, memory. The processor's own
  fields are a guest's only source for its clock and cache (a zero
  clock breaks its delay loop and it dies after its last printed
  line); the identification field is sixteen bits and carries the
  low half of the processor identification register. The memory
  bank record uses the eight-bank layout the Hub's network-interface
  status register advertises; the other mode's record is eight bytes
  shorter and moves the directory attribute, so the two are asserted
  together;
* the midplane -- a module serial number and nothing else, with a
  *declared count of zero* over one populated component. Real
  firmware adds the component and decrements the count on purpose:
  OpenBSD's enumeration runs to the declared count and never visits
  it, while the firmware's own consumer reads it at the index equal
  to the decremented count. The midplane carries no crossbar
  component, which is why a guest assuming the Hub sits at a fixed
  widget is this machine's normal path;
* the base I/O board -- the Bridge, two disk controllers, then the
  I/O controller, matching a real Origin 200's captured inventory.

The console descriptor is the trap. A guest identifies the I/O
controller's generation from the low twelve bits of the serial-port
base and the register it writes from the low twenty; those are not
the same field. The descriptor's three base fields -- serial port,
configuration space, memory -- are all rooted at the *widget*, not
at the device window: the configuration base is a sibling of the
device window in the Bridge's widget space, unreachable from a
window-rooted composition. Linux reads only the memory base, adds
the chip's register offset itself and validates nothing, so it must
be an address the processor can issue.

ARCS firmware calls
===================

The flash holds one stub per vector entry: a stub forms the address
of its own slot in the trap page, loads from it, and returns, so the
routine's return value reaches the return register through the
translator's ordinary load path. The firmware call vector and the
private vector share one stub area and one trap page, told apart by
op number. The trap page sits in the compatibility segment because a
two-instruction stub reaches only sign-extended addresses.

The GDA's node table carries 64 entries -- the family kernel's
build-time count; the 128-entry variant belongs to the top-of-line
machine's own firmware image. The trap page is accessed in eight-byte
units, implemented at that
width: an unstated implementation width would let the memory core
split a doubleword load into two word accesses and call a routine
twice, visible only as every line of early output appearing twice. A
debugger examining the page must not trigger a call.

``Write`` caps the operation at what its 32-bit count field can
express -- the cap is also what bounds the loop for a hostile length
-- and translates the guest's buffer address through the processor's
own TLB. ``GetMemoryDescriptor`` walks the list above. The processor
launch routine enters a slave-loop processor through the firmware's
own 64-bit trampoline so the target makes the addressing transition
itself; the reset it performs is queued as a safe-run-on-CPU
operation so no peer's lock-free read of CPU state overlaps it.
Identification and environment lookups return null, which the guests
handle (the console rate falls back to 9600); the private vector's
capability level is three, because the SGI standalone shell is a
relocatable object with no program headers, and a lower answer could
not load it.

Kernel hand-off
===============

A ``-kernel`` file is loaded through the generic loaders: ELF first,
then the MIPS ECOFF loader (``load_ecoff()`` in ``hw/core/loader.c``,
bounds-checked on every header field), with both answers reported on
failure. A 32-bit ELF's entry point is sign-extended at this one call
site -- an IRIX-shaped kernel carries a physical load address and a
kseg0 entry. A kernel's headers name the unmapped 64-bit window as
both virtual and physical address; the physical address is the low
forty bits and an uncached window keeps its attribute.

The reset vector jumps to the hand-off stub with a two-instruction,
region-relative jump -- all the eight bytes between flash entry
points allow. An ELF kernel is entered in 64-bit kernel mode with
error level clear and the boot-exception-vector bit clear. A directly
loaded sashARCS instead gets the real ARCS ``client_start``
convention -- argc/argv/environ in a0-a2, a fresh stack, ``gp`` from
its own header -- with CP0 Status left at reset, because a 32-bit
link address needs unmapped kuseg and no TLB entry; its scratch
layout is computed from the image's extent, since the two IRIX
releases ship differently sized images.

Configuration block
===================

SGI ships a generic release image with Origin 2000 defaults; its
flasher copies the running node's configuration into the new image
and adjusts one byte so the block's byte sum is preserved. The
machine class supplies the same three values -- the machine-type word
and the two flash timings -- into a synthesized image that carries
this product's values directly, preserving every other field and the
old block sum. The factory-fresh environment log is retained state
outside the firmware payload, its erased first entry the log's end
marker.
