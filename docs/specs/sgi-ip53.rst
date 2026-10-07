==========================================
IP53 IO and compatibility components
==========================================

This page records the component boundaries used by the IP53 module profiles.
Those profiles are software constructions for the Origin 350 and Tezro, and
for proposed captured Onyx 350 compatibility work. They do not establish that
every selected device, setting, or connection matches a physical board.
Chip-level behavior and evidence limits are stated below; the detailed
reviewed Origami contracts remain the authority for experiment-specific
policies.

.. _sgi-ip53-pic:

PIC PCI host bridge
===================

The profile exposes one PIC with two PCI heads. Public OpenBSD sources show
the second head at offset ``0x800000`` within the same XIO widget; public
Linux sources describe the PIC configuration-device numbering used by these
profiles. The two-head layout is selected from those software sources.

The Origin 350, Tezro, and proposed Onyx 350 profiles select PIC widgets and
direct-DMA settings as explicit software inputs. In particular, the supported
direct-DMA fields and the direct64 path are emulator policies for those
experiments. They do not establish the complete PIC revision 3 register
contract, reset image, or physical Bedrock-to-PIC wiring. Unsupported fields
and untranslated DMA remain observable. See the PIC model and qtests for the
implemented boundary.

.. _sgi-ip53-ioc4:

IOC4 on IO9
===========

The selected IO9 component uses one IOC4 PCI function, with PCI identity
``0x10a9:0x100a``. The profile selects the existing QEMU revision value
``0x53``; this is not a measured revision for either selected IO9 board.
The experiment profiles enable the existing serial-DMA option. The Origin
350 and proposed Onyx 350 profiles choose a 33,333,333 Hz clock; Tezro chooses
66,666,667 Hz. These are software inputs, not claims about board straps or
the complete IOC4 reset state.

The public serial-console contract covers byte-wide UART windows and the
IOC4-local interrupt state used by the selected subset. ATA, keyboard and
mouse, general-purpose pins, full byte-bus behavior, and board-level
interrupt routing are outside this profile page. Unimplemented accesses
must remain visible. Exact reset values, the complete access-width matrix,
and the physical interrupt route remain unresolved.

.. _sgi-ip53-bcm570x:

BCM5701 Ethernet controller
===========================

The profile selects the existing generic BCM570x model as a single-port
BCM5701, PCI identity ``0x14e4:0x1645``. The selected PCI revision value is
an emulator setting, not an observed IO9 card revision. The model uses
QEMU's network backend interface; the IP53 profile does not claim that a
particular host network backend or physical network route exists.

The QEMU BCM570x model's register, DMA, and interrupt behavior is exercised
by ``tests/qtest/bcm570x-test.c`` and ``tests/qtest/sn0-bcm570x-test.c``.
Those qtests establish model behavior, not physical BCM5701 behavior on IO9.
Startup timing includes a bounded model approximation. Physical startup
timing and the exact fitted IO9 Ethernet component remain unverified. DMA
must continue through the selected PCI device's bus-master address space.

.. _sgi-ip53-ds1742:

DS1742-compatible clock resource
================================

The profiles link an existing DS1742 model to IOC4 as a synthetic software
construction. They expose the eight clock-byte addresses used by the selected
compatibility contract and retain the model's 2 KiB backing image. They do not
establish the fitted IO9 package, full IOC4 byte-bus decode, SRAM aliases, or
physical address wiring. The reported IOC4 clock label does not identify the
fitted part. The broader storage span and PROM environment/checksum area
remain outside this model.

.. _sgi-ip53-io-profile:

Profile boundaries
==================

These component selections describe the software model projection. The
Origin 350 and Onyx 350 captures differ in board identity and context; their
topologies and unmeasured board behavior must not be inferred from one
another. The PIC attachment, IOC4 settings, PCI revisions, and linked clock
are profile choices where explicitly identified above. Physical behavior
that lacks matching evidence remains unknown.
