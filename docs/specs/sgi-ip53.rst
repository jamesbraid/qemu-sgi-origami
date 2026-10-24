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

.. _sgi-ip53-serial-profiles:

Headless serial profile choices
===============================

The machine catalogue selects the following inputs for serial IRIX experiments.
These values define the emulator profiles; a successful guest boot does not
measure a processor reset pin, PROM image fitted to a board, or PCI wiring on
the named product. The physical inventory facts cited by the catalogue have
separate public capture or manual references.

============================  =======================  ========  ===========  ============
Profile                       Processors               Memory    Cache each   Reset mode
============================  =======================  ========  ===========  ============
Origin 350                    four 700 MHz R16000       2 GiB     8 MiB        0x009cad85
Onyx 350 captured module      four 800 MHz R16000       8 GiB     8 MiB        0x009caf85
Tezro deskside, one CPU       one 700 MHz R16000        4 GiB     4 MiB        0x009bad85
Tezro deskside, four CPUs     four 700 MHz R16000       4 GiB     4 MiB        0x009bad85
Tezro rack, first enclosure  one 700 MHz R16000        4 GiB     4 MiB        0x009bad85
Onyx 4 host candidate        four 1 GHz R16000         8 GiB     16 MiB       0x009dad85
============================  =======================  ========  ===========  ============

The six listed compositions select the same internal R16000 experiment
properties with or without the legacy ``ip53-experimental`` machine option.
The machine checks the resolved processor population, fitted socket count,
memory, clock, cache and reset word before enabling the profile. Other
already-admitted experimental populations still require the legacy option and
explicit CPU properties. These are software inputs, not measured silicon
values. The Onyx 350 and Onyx 4 rows project a captured module into a
single-module host. The
Onyx 4 capture reports an IP59 board in a system called Onyx 350; assigning
that module to the Onyx 4 host is an inference. Its profile uses the IP53
guest-board path as a compatibility mapping, not a physical IP59 schematic.
The public IP59 module inventory reports HUB revision 2 at 200 MHz.
Mapping that report to Bedrock revision 2 is a profile choice; raw NI and
widget register identities remain unmeasured.

The Origin 350, Onyx 350 and Onyx 4 host profiles each select one PIC at XIO
widget 15. Tezro selects two PIC widgets, 11 and 15, and places the modeled
IO9 behind widget 15. The one-CPU and four-CPU Tezro deskside profiles share
the same selected board, memory and I/O construction. The rack profile models
only the first enclosure. No profile here attaches graphics, a G2/G2N brick,
or a NUMAlink peer. Direct-link XIO and the PIC interrupt and DMA settings
are emulator choices where physical routing remains unknown.

These profiles select an IP35 PROM 6.210 payload and a profile-specific flash
seed. Flash read and write cycle inputs are both 16, and the machine-type
input is 1. The Origin 350 and Onyx 350 profiles select a 33,333,333 Hz IOC4
clock; Tezro selects 66,666,667 Hz. The linked clock resource uses a 1968
fresh-year base. Platform identity ``0x2001`` is a selected software input.
Origin 350, Onyx 350 and the Onyx 4 host select L1 type ``0x37``. The Tezro
deskside and rack profiles select ``0x39`` and ``0x51`` respectively, following
the published Linux IP35 machine-name mapping. Physical L1 type replies
remain unmeasured. The public captures do not establish the selected clocks,
an installed flash image, or the complete PIC and IO9 interrupt wiring.
