.. SPDX-License-Identifier: BSD-3-Clause

.. _altix350:

Altix 350
=========

The ``altix350`` machine models an SGI Altix 350 node: a SHub 1, one or two
Itanium 2 (Madison) processors, and the IO9 base I/O card, with a PIC, an
IOC4 (serial port, IDE channel, time-of-day chip), a QLogic ISP12160 SCSI
adapter and an optional BCM5701 Ethernet adapter. The processor executes the
unmodified SGI PROM image from its reset entry. The PROM prints its console
text through the L1 controller to the first serial port, tests memory and the
processor, and reaches its EFI shell. A Linux kernel booted from there
reaches a login prompt: in our runs a community 7.0 kernel with a Debian
userland on an ISP12160 disk did so at ``-smp 1`` and ``-smp 2``. The machine
supplies no PAL, SAL, EFI or boot tables and does not change the processor's
reset entry.

Behaviour that no source describes is an *approximation* or *inferred*, and
the table near the end of the page lists each one with its evidence.
Whether other guest kernels work is not claimed.

Sources
-------

The chips repository's firmware qualification
(``software/firmware/snprom/qualification.md``) identifies the image, its
flash placement and the early RAM accesses. It covers the firmware's
accesses, not undocumented SHub reset values or read/write semantics. The
chips repository's SN2 address map (``platforms/sn2/address-map.md``) and
SHub description (``devices/shub/README.md``) distinguish public Linux
definitions from unknown hardware behavior.

Machine and firmware
--------------------

An IP57 node board carries one SHub and one or two Itanium 2 processors
(*established*: SGI 007-4660-001, printed pp. 100 and 104). The model takes
one or two (``-smp 1`` or ``-smp 2``); with two, each runs on its own host
thread (``-accel tcg,thread=multi``). The actual product also requires a
PIC riser, a base I/O card and an L1 controller (the guide names the IO9 card
with its IOC4 and the PIC on pp. 104--105). The PIC is present, linked to the
SHub with its crossbow in small windows 0 and 15, using the shared PIC model's
Bedrock-compatible profile. Whether that profile's Origin 350 quirks apply to the Altix riser is
*unknown*, so only the PROM's crossbow reads, an
interrupt round trip and a bus master's DMA are tested.

The base I/O card is modelled as far as the firmware needs: head 0 carries the
IOC4 at PCI device 1 and a QLogic ISP12160 SCSI adapter at device 3, the
windows the firmware scans for them. The other slots of both heads are empty.
The ISP's two buses are ``scsi.0`` and ``scsi.1``, and a disk goes on one with
``-device scsi-hd,drive=...,bus=scsi.0,scsi-id=1``. The model accepts ids 0
to 15 and reserves none for the adapter; without ``scsi-id`` the lowest free
id, 0, is taken. The EFI shell booted from a disk at id 1 and at id 3 and
listed a CD-ROM at id 2 (*observed*). A disk at id 0 has not been tried and
the id the EFI driver gives the adapter is *unknown*, so use an id from 1 up.

The qualified input is the unmodified 6,291,456-byte ``shub1snprom.bin``
from snprom 10.54 build 70399, SAL 5.04. Its SHA256 is
``51ccf9ad830b85ee6c88d6d73d2512322a86454e63db51c57454679283a34617``.
Product compatibility is vendor intent, not boot proof. No firmware bytes
are distributed with QEMU.

The model requires ``-bios`` and exactly 2 GiB of RAM. It rejects direct
kernel loading. Firmware size is checked, but image identity remains the
caller's responsibility.

The SHub's 8 MiB PROM flash is RAM-backed by default: it starts erased (all
ones) with the firmware image copied in at offset ``0x200000``, and what the
firmware writes is lost when QEMU exits. ``-drive if=pflash,format=raw,file=...``
backs it with an 8 MiB file instead, so the firmware's changes persist. The
file is used as it is and the image is not copied into it.

A flash file therefore needs the firmware and the PROM's variable area built
into it. The PROM keeps its variables, such as ``NoACPIIO``, in the 2 MiB
below the firmware. The layout is *inferred* from the PROM's reader (the
chips repository's ``software/firmware/snprom/efi-boot.md``, commit
``efef3fe93b11``), not measured except where the text says *observed*: two
64 KiB blocks, at ``0`` and ``0x10000``, each with a 0x100-byte header, and
the block with the higher generation word is active. The header is 16 erased
bytes, the magic ``IOLP`` at ``0x10``, the version word ``1`` at ``0x14`` and
a generation word at ``0x18``, then erased bytes. The records are fixed
64-byte entries from ``0x100`` to the end of the block. Byte 0 has the type
in its low nibble (1 is a live variable, ``0xf`` ends the log), bytes 1 to 14
hold the name, at most 14 characters and a NUL, and bytes ``0x10`` to
``0x3f`` the value, at most 47 characters and a NUL, with every other byte
erased. There is no checksum and no length field. The blank image
is the firmware at ``0x200000``, block 0 with the generation left erased and
the variables in order from ``0x100``, and every other byte ``0xff``. The
``sgi_altix350_initial_flash`` function in ``hw/mips/sgi/initial-flash.c``
builds exactly this, and ``qemu-sgi-machine-init --prom-variable`` will use it
once the machine catalogue lists the flash. EFI boot entries are in a separate
EFI store that this image does not hold.

On the model, with a stock PROM and an image built that way holding
``NoACPIIO=1``, the DSDT that the XSDT reaches has zero bytes where the AML
starts, after its 36-byte header (*observed*). The same image without the
record has the AML there, which opens a ``\_SB`` scope (*observed*). So the
PROM took the record from block 0 with the generation erased and block 1
erased (*observed*). The PROM leaves block 0 unchanged during a run to the EFI
shell (*observed*) and writes about 700 bytes elsewhere between ``0x100000``
and ``0x1f0000``, which are *unknown*. The console showed no ``NoACPIIO``
message at the shell. What would settle the layout on real hardware is a
capture of a real flash's variable area.

The first record's slot is the open question. At the EFI shell, ``pod``
reaches the PROM's POD prompt (*observed*), where ``setenv NoACPIIO "1"``,
with the value quoted, makes the PROM write the record itself and takes effect
on the next boot (*observed*). On a flash holding only the header, that record
landed at ``0x140`` and left ``0x100`` erased, and a later ``setenv`` of
``Verbose`` went to ``0x180`` (*observed*: stock PROM
``snprom-10.54/shub1snprom.bin``, SAL 5.04, one run each). The PROM does read
a record built at ``0x100``: ``printenv`` lists it and the console omits the
``SAL ACPI`` generation messages, as for the record at ``0x140`` (*observed*,
one run each), so an erased slot 0 does not end its search, which the ``0xf``
end-of-log reading did not predict. Changing ``NoACPIIO`` from ``1`` to ``0``
rewrote the value in place at ``0x100``, and changing it from ``0`` to ``2``
set the old record's type byte to ``0`` and appended the new record at
``0x140`` (*observed*, one run). Why the first write skips ``0x100`` is
*unknown*, and the builder keeps ``0x100``. Whether the PROM appends correctly
after two builder-made records is untested (*unknown*).

The firmware's console text goes through the L1 controller to the first
``-serial`` device, so ``-serial stdio`` shows the PROM's messages, and typed characters go back
through the L1 as console input (*inferred* frame format).

Address map
-----------

======================= ===================== ====================================
Physical start          Size                  Model backing
======================= ===================== ====================================
``0``                   2 GiB                 RAM
``0x100000000``         28 GiB                Compatibility hole: loads read 0, stores refused
``0x800000000``         32 GiB                SHub 1 global MMR, moves with NASID
``0x3000000000``        2 GiB                 RAM again, cacheable space, NASID 0
``0x10000000000``       2 GiB                 RAM again, local memory (space id 4)
``0x8000000000``        32 GiB                SHub 1 local MMR (:ref:`sgi-shub`)
``0xffa00000``          6 MiB                 Top 6 MiB of the PROM flash
``0x6ff800000``         8 MiB                 Read-only alias of the PROM flash
======================= ===================== ====================================

Flash placement is a *software expectation*: the qualified image's FIT
pointer at ``0xffffffe0`` names ``0xffa00000``, and its component addresses
lie in this window. A 2.6.5 kernel reads the FIT pointer through the alias at
``0x6ff800000`` (see :ref:`sgi-shub`), which is *inferred* to ignore bit 32.
The reset window reads the flash's array bytes whatever state the chip is in,
and a CPU write to it is refused. A write to the ``0x6ff800000`` alias is
refused, because what it does is not known.

The ``0x10000000000`` alias is the address-space-0 local-memory form
(space id 4 in the node field, any NASID). The SAL shadow copy of the firmware
image stores through it. That it is the same memory as the low and cacheable
forms is *inferred* from SGI's Linux v5.3 SN2 header wording, not stated by
it.

RAM from reset is an *approximation*, not a memory-controller reset claim.
SAL 5.04 at ``0xffa1de00`` uses a table at ``0x1000000`` before memory
initialization. Ordinary RAM preserves those stores without replacing the
firmware's table builder. It does not qualify DRAM training or cache-only
execution before training.

The processor owns interpretation of the IA-64 physical uncached attribute
(bit 63). The machine maps physical addresses, not virtual region aliases.

Inferred native reset diagnostic
--------------------------------

Madison starts at physical ``0xffffffb0`` in IA-64 mode for a bounded stock
PROM diagnostic. This is *inferred*, not an established Madison reset
contract. Intel patent US7305544B2 gives that IPF vector as an example; a
published Merced ITP trace and the common Intel/SGI PAL_A vector layout
support it. The source analysis is the chips repository's
``software/firmware/snprom/boot-sequence.md``. Intel 245318-002 section
11.1.6 identifies ``0xfffffff0`` as IA-32 reset code, not this native entry.

The supplied image's vector branches to ``0xffff83a0``. Its prefix writes
MSR 212 and 1225, then executes ``br.ia``. MSR 212's effect is unknown and
MSR 1225 is kept as storage (see below). Each CPU stores MSR indices 0 to
2047. The model stores other unknown MSR writes; unknown reads return the last
value written, or zero. An index of 2048 or more reads zero and drops the
write: the PAL executes a ``mov`` with index ``0xf80001`` and goes on (PAL_B
5.73 near ``0xfff01370``), and raising Reserved Register/Field for it stalls the
stock PROM at "Switching to RAM and testing CPU" (*observed*, ``-smp 1`` and
``-smp 2``). Which indices a Madison implements is *unknown*. Every
unimplemented access is logged under ``-d unimp`` with the PC and value, and the
``ia64_unimplemented_msr`` trace event records every access, with the value a
read returned. The PROM reads MSR 1536 and MSR 1715 and branches on them
(PAL 520's size check and the PAL_B version check). Their reset values are
*unknown*, so those reads return zero until written and show in the trace,
except for MSR 1536 bits 59:48 (see "MSR 1536 L3 ways"). This is a
diagnostic approximation, not implemented MSR hardware or firmware
acceptance. The image check returns a result only by verifying the signature (see below), and there is no shortcut to ``0xffffff70`` or
SALE_ENTRY. The initial zeroed register/PSR defaults remain unqualified
reset-state assumptions, not the PAL exit state specified for SAL.

Inferred br.ia service
----------------------

The PROM writes MSR 1225 with 4 and then executes ``br.ia`` with a data
address as its target. The chips repository's
``software/firmware/snprom/boot-sequence.md`` and
``software/firmware/snprom/pal-msrs.md`` read that
as a call into resident processor code, not an IA-32 switch, and the SDM
describes PAL authentication as a PAL procedure, not a ``br.ia`` entry. The reading follows Intel patent US 6,542,981 only
partly: the patent uses two branch registers, while the generic PAL_A sites
use one and a general register (``r5``) that looks like a selector. All of
this is *inferred*.

While MSR 1225 is 4, the model keeps the MSR as plain storage and treats
``br.ia`` as a service call. With ``r2`` zero it resumes at the next
bundle, which is what the chips repository's reset-site notes ask of a
model and what the two no-image sites need (``b0`` is the next bundle there). With a
nonzero ``r2`` and ``r5`` other than 0 or 1 it checkstops the CPU (see
Unsupported accesses) and logs the site. The ``r5`` 0 case is described below.

With ``r5`` 1 and an image address in ``r2`` the model checks the image, as
the PAL_A image check at ``0xffff8720`` asks (*inferred*, matching the
notes in the chips repository's
``software/firmware/snprom/pal-authentication.md``). The check is a
DSA signature (1024-bit modulus, 160-bit order, SHA-1) over a component of ``r3`` times 16
bytes. The digest covers bytes 16 up to the last 64. The last 48 bytes hold
``r`` and ``w`` (``w`` is ``s`` inverted modulo ``q``), each little-endian in
24 bytes. The digest does not cover bytes 0 to 15 or the 16 bytes before the
signature, so changes there are not detected.

The key is the one in the image's own generic PAL_A: ``p``, ``g`` and ``y``
at ``0xffffa1d0`` and ``q`` at ``0xffffa350``, which the model reads from
guest memory. The CPU property ``pal-auth-key-addr`` holds the address of that
block (``q`` follows ``p``, ``g`` and ``y`` at +384), the machine sets it to
``0xffffa1d0``, and with the default of 0 every image check fails with ``r1``
1 and a ``-d unimp`` line. Because the key comes from the guest, a degenerate key (for
example ``g`` and ``y`` both 1) accepts any image. The key and algorithm
come from verifying the stock image's six PAL components, not from any
statement about the processor, and whether the microcode behind this
``br.ia`` runs the same check with the same key is *unknown*.

``r1`` is 0 when the signature verifies and 1 otherwise, then execution
continues at ``b0``, where the PROM's caller tests ``r1``. The error code
the processor returns on failure is *unknown*. A rejection logs a warning
that says whether the size, the memory reads or the signature failed. The
verdict, digest and size are traced as ``ia64_br_ia_image_check``. Sizes
are limited: ``r3`` times 16 must exceed 80 bytes (``r3`` of 5 or less is
rejected) and must not exceed 1 MiB, and ``r3`` is range-checked before the
multiply so a huge value cannot wrap into a valid size.

PAL_B calls ``br.ia`` with ``r5`` 0, a nonzero ``r2`` that points at a table
in PAL_B, ``r3`` 1 and ``r1`` preset to ``0x99`` (the chips notes read this
as a table load for the processor, not authentication). The table load is *not
modelled*. The model resumes at ``b0`` with ``r1`` set to 0, which the firmware reads as loaded, and writes no
MSR and applies nothing. The model stands for a processor that accepts a
valid table. This is an *approximation*: whether the load succeeds on hardware and what it changes is *unknown*. The
call is traced as ``ia64_br_ia_service`` with "table load reported done" and
logged once.

Any other MSR 1225 value would be the architectural IA-32 transition, which
is not modelled, so it checkstops the CPU, as does any image call with ``r5``
other than 0 or 1. The architectural checks come first in all cases: Illegal
Operation when ``ar.bspstore`` differs from ``ar.bsp``, and Disabled
Instruction Set Transition when ``psr.di`` is set.

Reserved instruction words
--------------------------

The PROM's PAL executes two words the SDM leaves reserved, an M-unit slot
``0x108000000`` and a B-unit slot ``0x48000000``, in the processor-specific
PAL_A and in PAL_B, after ``srlz`` and writes to MSR 1544 to 1547 (the chips
repository's ``software/firmware/snprom/pal-instructions.md``). The architecture raises
Illegal Operation for them, and the firmware completes them, so the real
Itanium 2 must do something else. What is *unknown*. Any other reserved word
faults.

Both words are an *approximation* in the firmware components. The M word
runs as described below and the B word returns from interruption: the
model loads PSR from ``cr.ipsr`` (which sets the slot from ``ipsr.ri``) and
IP from ``cr.iip``, and treats ``cr.ifs.v`` as clear so the frame stays. That
is *inferred*: the chips notes find 12 PAL_B sites, each ending a block, three
of them straight after writes of ``cr.iip`` and ``cr.ipsr``, so falling
through would run into the next routine or into filler. The stock PROM boot to
the EFI shell at ``-smp 1`` and ``-smp 2`` reaches none of the 12 sites. Both
words run, with no fault, on a CPU whose ``cpuid[3]`` has family 0x1f, at
privilege level 0 with qp 0, wherever they are fetched (*approximation*). The
PROM runs the same words from its RAM copy of the PAL at an address the SAL
picks, so no fetch address says whether the code is Madison firmware: a
translation block is keyed by RAM page, and the machine maps RAM at three
addresses and the flash at two. On another family, above privilege level 0 or
with a predicated slot the words take Illegal Operation. In a throwaway build
with no address test, the stock PROM to its last point of progress and the SLES
9 SP3 sn2 kernel to a shell, at ``-smp 1`` and ``-smp 2``, ran 86 reserved-word
sites per CPU, all at privilege level 0, and returned a modelled cache-array
line to a load 634 times per CPU, all from firmware code. A Debian 7 kernel and
a compiler run were not covered.

Only one site has grounded context: ``0xfffca400`` in PAL_A 5.37, the first
on its entry path (after the write of MSR 1546 at ``0xfffca3f0``), read in
the chips repository's ``software/firmware/snprom/pal-instructions.md``.
Every other site is an *approximation* from the same shape, a command write,
``srlz.d`` and the slot, and may be wrong. PAL_B sites follow the same
rule, and some PAL_B code polls status registers that this model completes
as a labelled approximation (below).

Limits: the chips notes grade the M word's no-op reading *inferred*, not
established. The M word
has no register operand and nothing after it tests a result from it, but the
encoding is reserved, so hardware may give it a side effect, and the code
reads status MSR 1608 to 1612 afterwards, where a non-zero value takes the
error exit on hardware. The model reads those MSRs as zero under the
diagnostic MSR policy. The hardware effect is *unknown*. Sites whose
context was not read follow the same rule and may be wrong.

A third word is an *approximation* in the same way, with the same conditions: opcode 1,
``x3`` 0, ``x6`` 0x36 in an M slot with ``r3`` set and ``r1``, ``r2`` and
``qp`` zero (``0x21b1c00000`` with ``r3`` = 28 is the first one the firmware
reaches). Table 4-45 of the SDM (vol 3) marks that cell reserved, so the
architecture raises Illegal Operation for it, and the firmware executes it
anyway. It sits next to ``fc`` (``x6`` 0x30). The PAL images contain it in three copies, in a loop that
steps an address 128 bytes at a time between an MSR 1546 command write and
a bound check, and the RAM copy of the PAL code (``0x1000000``) runs it
too. The reading is a cache-line operation on the address in ``r3``, which
has nothing to do in a model without caches. It is *inferred* from the
operand shape and the loop, and its hardware effect is *unknown*. Any other
operand setting of that cell faults.

MSR 1632 to 1635 are written by the firmware and read by no image. The
model accepts those writes, traces them and keeps no value (*inferred*
harmless; function unknown).

MSR 1212 completion
-------------------

After a write to MSR 1546 whose top 16 bits are ``0x9021``, reads of MSR 1212
return bit 14 set until MSR 1212 is next written. This is an *approximation*:
no source describes the command or the bit. The
``ia64_msr_estimated_completion`` trace event records each read it changes.
For a CPU whose ``cpuid[3]`` revision byte is 4 to 8 (0 reads as 1; the
Madison model is 5) the model also stands for a processor that passes
the PAL's built-in known-answer test: MSR 1212 bits 13:0 read ``0x317c`` and
MSR 1226 reads ``0x82518167826f836c`` until MSR 1212 is next written. The
constants are the ones the PAL compares against (the same for these revisions in the PAL_B 7.79,
5.73 and 2.25 images, which hold them only inside that compare); what the
registers hold on hardware is *unknown*. This makes the self-test pass by
construction, so it is the first approximation to revisit. Other revisions get only
bit 14.

ALAT
----

The PAL's CPU test (PAL_B 5.73, test at ``0xffef9160``) allocates 32 entries
with ``ld8.a``, checks them with ``chk.a.nc`` and ``ld.c``, and expects stores,
``invala`` and ``invala.e`` to remove them. The CPU keeps a table of 32
entries keyed by register and physical range. A lookup fails when a store
overlaps the entry, ``invala`` or ``invala.e`` removed it, or another vCPU
stored to the entry's physical range since it was made; with one vCPU only the
first three apply. Each vCPU logs the physical range of its last 1024 stores,
before the store, and another vCPU drops only the entries those stores overlap.
A store whose range is unknown, a log that lapped before it was read, drops the
whole table, which the SDM allows. A lookup that always failed would be allowed by the SDM (Vol. 1,
4.4.5.3) but not by the PAL's test. The CPU is unmigratable, so entries are never carried. A store is compared
with an entry on bits 19:0 of the physical address, so a store a multiple of
1 MiB away also removes it (*established*, Itanium 2 reference manual 5.1).
The manual's cross-check that a store within two cycles of a check compares
only 12 bits depends on cycle counts and is not modelled. A new entry takes
the entry of the same register, else the first invalid one (*established*,
5.1.1), else a valid one chosen round robin. The manual calls the last step
"approximately FIFO" with pointers tied to the two memory ports and gives no
advance rule, so the single round-robin pointer is an *approximation*.

Itanium 2 performance monitor registers
---------------------------------------

The PAL's register test (PAL_B 5.73 at ``0xffefdc80``) writes one pattern to
PMC0 to PMC15 and PMD0 to PMD17 and reads each back, and a second check writes
small values and reads them back. The Madison model keeps the bits that test
expects: PMC0 ``0xf1``; PMC1 to PMC3 read zero and ignore writes; PMC4
``0x0fffff7f`` and PMC5 to PMC7 ``0x0f7fff7f``; PMC8 ``0xffffffff3fffffff``
and PMC9 ``0xffffffff3ffffffc``; PMC11 ``0x3ffffff``; PMC12 ``0xffcf``;
PMC10 and PMC13 to PMC15 keep every bit. PMD1 keeps 13 bits, PMD3 16 and
PMD16 36; PMD4 to PMD7 are 47-bit counters whose bits 63:47 read as copies of
bit 46; the other PMDs keep every bit. No counter counts: the registers hold
what was written, which is all the PAL's checks need.

Where the Itanium 2 Processor Reference Manual (Intel document 251110-003, chapter 10)
differs, the model follows the PAL. PMC4 to PMC7 (Figure 10-11), PMC9 bits 1:0,
PMC12 (Figure 10-22) and PMC11 agree with the manual. The conflicts are PMD4
to PMD7, where the manual has a separate overflow bit 47 and sign bits
63:48; PMC8 and PMC9, where bit 32 reads back although the manual lists bits
32:30 as reserved; and PMC13 to PMC15, which read back in fields the manual
calls reserved. PAL 5.73 shipped and ran on real Madison parts, so these are
software expectations of that PAL; the manual probably describes an earlier
revision (*inferred*). The PMD1, PMD3 and PMD16 widths are not in the manual.

Cache-array port
----------------

PAL_TEST_PROC's cache-array tests (PAL_B 5.73, sub-tests ``0xfff00b30``,
``0xfff02b00``, ``0xfff015b0``) drive the Madison caches' array debug port.
The model implements the smallest port they need and nothing else, on a
Madison CPU model. It is an *approximation*: nothing public describes the
port, and the layout below is read from a disassembly of the PAL image (Ghidra), so
it rests on one firmware image.

The reserved M word ``0x108000000`` runs the command last written to MSR 1546:
bit 63 set, bits 60:58 the type (2 read, 3 write, *inferred*), bits 57:53 the
array (8 data, 9 check bits, 10 tag). MSR 1544 names the line
(``addr[14:7] | addr[17:15] << 8``). MSR 1600 to 1627 (``0x640`` to ``0x643``,
``0x648`` to ``0x64b``, ``0x650`` to ``0x653``, ``0x658`` to ``0x65b``) are
the 16 dword staging words, one per dword of the 128-byte line, and the
commands move them to and from the line's arrays. A data write stores the
dwords and their check bits: the Hsiao (39, 32) code per 32-bit half, check
bit *i* the parity of the half ANDed with column *i* (``0x0003c3ff``,
``0x12c87c0f``, ``0x25549c71``, ``0x49a42592``, ``0x8e392aa4``,
``0xf03e1348``, ``0xffc3c000``), placed in bits 19:13 (low half) and 51:45
(high half). Writing array 9 stages an injection: the first four words, XORed
into the check bits of every dword whose index matches their position modulo 4
at the next data write, until array 9 is written with zeros.

The tag array takes its word from the first staging word: the tag
``addr[49:15]`` bit-reversed in bits 34:0, the valid bit in bit 35, four parity
bits in 39:36 and the state in 43:40. A tag read puts the tag, not reversed, in
bits 53:19 of MSR 1545, the state in bits 3:0, the four parity bits as stored
in bits 8:5 (the model computes none) and the set's replacement mask in bits
16:9. The valid bit is held but not read back, as nothing known places it.
Index bits 15:13 are ignored: the PAL writes a line with them clear and reads
it back with them equal to the column in bits 10:8, so a line is keyed by its
set (bits 7:0) and column.

A doubleword load that hits a line (data written through the array, matching
tag, valid bit set, any column of its set) returns the line's data and not
RAM's, which is what the PAL checks in sub-test 5 (``0xfff01cf0``). A store
that hits a line copies the stored dword from RAM into it with fresh check
bits, sets its state to 7 and sets its column in the set's replacement mask,
which starts over at zero once all eight columns are set. The PAL writes MSR
1552 bit 4 at the start of each sub-test, and the model clears every
replacement mask on that write. All of this is read from sub-test 8
(``0xfff04730``) and rests on one firmware image and the PAL's own reading of
the fields.

A tag is *bad* when it is valid and its four stored parity bits differ from
the parity of its fields: bit *i* covers tag bits 7:0, 16:8, 25:17 and 34:26
for *i* = 0 to 3, and bit 0 also covers the valid bit. Any load or store to a
set (``addr[14:7]``) that holds a bad tag stores ``0x4005`` in MSR 1509, which
is what sub-test 6 (``0xfff03e80``) expects once it has flipped a parity bit.
The bad tag need not be the accessed address's own: the PAL writes it at a
different index column (bits 10:8) than the line it then loads, so the model
checks every tag of the set. Which of MSR 1509 to 1512 reports it, what
the log's bits mean, and whether MSR 1504 must enable the check are not
known: the model logs always, in MSR 1509.

The PAL's second cache group (group 11, ``0xfff04ff0``) drives arrays 4 (data)
and 6 (tag) through the same MSR 1544 and 1546 port, with the commands read from
the PAL's disassembly. MSR 1544 holds the set (address bits 17:7) and the
way (bits 15:11). A line is 128 bytes in four 32-byte granules, each with a
10-bit check code over its four dwords (ten masks per dword, from the PAL).
Staging MSRs 1600 to 1627 hold the dwords, and MSR ``1604 + 8 * g`` the check
word of granule *g*. The commands (bits 63 and 60:58 as above) are:

* ``0x88800052c0000000`` reads a line and its check words, and
  ``0x88880052c0000000`` does the same (what bit 51 changes on a read is not
  known);
* ``0x8c980052c0000000`` writes the data with generated check words, and
  ``0x8c880052c0000000`` writes them as found in the check registers, which
  is how the PAL injects an error;
* ``0x8cc0001800000000`` writes the tag from MSR 1545 (tag ``addr[49:18]`` at
  bits 53:22, 12 extra bits at 21:10, a 7-bit code at 9:3, the state at 2:0)
  and MSR 1547 (the high extra bits), and ``0x88c0001800000000`` reads it.

A doubleword load that hits a line (data written, state not zero, tag equal)
returns the line's data. If its granule's stored code differs from the code of
its data, the load stores ``0x080a2000`` in MSR 1558 for one differing bit and
``0x080b2000`` for more, which is the pattern the PAL compares with under the
mask ``0x1c1f2000``. The tag code is held as written and not checked. A store
to a hit line copies the dword in from RAM and regenerates the granule's code.
MSR 1536 bits 59:48 mark ways the PAL skips; see below for what the model
reports.

Private L3 ways
---------------

The PAL builds its early table at ``0x1000000`` in the L3 itself, before any
DRAM works, with procedure 520 (index in ``r28``, range in ``r29`` and ``r30``;
the PAL's reading of it is in the chips repository's ``software/firmware/snprom/pal-msrs.md``). Each CPU
runs the same table builder, and the builder swaps entries in place without a
lock, so the range must be private to each CPU. The model gives each CPU an
address space of its own: a range tag write (command ``0x94c00000000007ff``
for way 0, a way index in bits 15:11 of MSR 1544, tag in MSR 1545) with state 3
puts 256 KiB of zero-filled RAM at the tag's address, over system memory and
for that CPU only. A write of MSR 1504 that clears every one of bits 16, 24,
32, 33, 40 and 41 after one was set (the PAL's enable bits, ways 24, 32 and 33
on one CPU id path and 16, 40 and 41 on the other) removes every way; a system
reset does too. The PAL's own state 0 range writes, an invalidate sweep, and
its other changes to MSR 1504 (bit 48, say) leave the ways alone. Nothing is
written back to system memory, and other CPUs, DMA and the host see only
system memory. The way count is 24, the largest part the model sizes (6 MiB).

This is an *approximation*. What the PAL's state 3 means to the L3, whether
lines are written back on disable, and what the other bits of MSR 1504 and
1040 do are unknown, and so is whether the real range is private or only
coherent in a way the PAL's single-writer use hides. The entry keys on the tag
install and the exit on the MSR 1504 clear (inferred from the PAL's order of
writes, not from a description), and it accepts any memory attribute. The
page-table walker, ``fc.i`` and the physical-range invalidation, and the
``br.ia`` image check, still use system memory and do not see the ways.

The PAL's data tests store with ordinary stores and then read the arrays back
(sub-test 3, ``0xfff015b0``). A line the port does not hold therefore reads as
the RAM contents of the latest line stored to its set, for either way, with
the Hsiao check bits of that data in array 9. This is a convenience mapping of
the PAL's own way argument, not a cache model: no allocation, replacement or
eviction rule is known, a read of a set nothing was stored to returns zeros,
and a line written only through the tag array has no data of its own. The port
watches stores only after its first array command (a pointer test per store
otherwise), through the ALAT write-end helper.

A load from a line the port holds recomputes the check bits of the dword and
XORs them with the stored ones. No difference logs nothing. An odd number of
differing bits (a single flip) stores ``0x100001`` in MSR 1505 and an even
number (two adjacent mask bits, also across the halves) ``0x080001``. The PAL
clears the log itself. Eviction and state transitions other than a store are
not modelled. The port warns once, traces
``ia64_msr_estimated_completion``, and is on the revisit list.

MSR 1536 L3 ways
----------------

PAL_CACHE_INFO for the L3 (PAL_B 5.73 at ``0xffea0430``) computes its
associativity as twice the number of *clear* bits of MSR 1536 bits 59:48 and
its size as 256 KiB per way, and PAL group 11 skips ways 2*i* and 2*i*+1 for
each *set* bit *i*. The PAL never writes these bits and no document gives
their value. A read therefore reports the model's L3: the CPU variant's L3
size divided by 512 KiB gives the number of clear bits, and the bits above them
are set, so ``madison-1600-3m`` (3 MiB, 12 ways) reads ``0xfc0``. This is an
*approximation*: which bits a real part sets is unknown, only their count is tested
by the PAL. The model warns once and traces
``ia64_msr_estimated_completion``. The other bits of MSR 1536 stay plain
storage.

MSR 1559 clock ratio
--------------------

The PAL's CPU test (PAL_B 5.73 at ``0xffe8e890``) reads MSR 1559 once and
dispatches on bits 26:22 through a table of bundles. A value below 8 lands
back in the dispatch code, which then loops on itself without reading the MSR
again. The model reports ``itc_frequency_hz / frequency_base_hz`` in those
bits, 16 for the 1.6 GHz Madison, and warns once. Every value from 8 to 23
takes the same path in the table, so the choice does not change what the PAL
does next. What the register holds is *unknown*; reading it as the processor
to base clock ratio is *inferred* from the table, not from a source.

MSR 1552 completion
-------------------

The PAL writes a command to MSR 1552 and then polls it until bit 2 is set
(PAL_B 5.73 at ``0xffe89ff0``). The model reports bit 2 set in every read of
MSR 1552 once the register has been written, as if each command completed at
once. Nothing says what the bit means or how long a command takes, so this
is an *approximation* and the ``ia64_msr_estimated_completion`` trace event
records each read it changes. Without it the poll never ends.

MSR 1730 training nibbles
-------------------------

PAL_A and PAL_B run two loops on MSR 1728 and MSR 1730. Both set a bit of
MSR 1728, write MSR 1730 with bit 48 set (a strobe) and bit 49 set or clear,
and poll bits 3:0 (or 11:8) of MSR 1730: the first loop until they read
``0xf`` after the strobe is gone, the second until they read 0. The model
latches the nibble that MSR 1728 selects (bit 48: bits 3:0, bit 49: bits 11:8)
at the strobe write: ``0xf`` when bit 49 of the write is set, 0 when it is
clear. Everything else is plain storage, so a read-modify-write keeps the
latched value. This is an *approximation* read from the loops' own writes: no
source says what the bits mean. A real part steps one count per strobe
(*inferred* from the servo loops in the PAL's disassembly) and the model jumps to
the loop's end state, so a loop that needs an intermediate target (8, in the
PAL_A 1.27 and PAL_B 2.25 nibble 11:8) is not exercised on this machine's 5.37
and 5.73 path. Each latch is traced as ``ia64_msr_estimated_completion``.

Processor identity
------------------

``cr.lid`` bits 31:16 (id and eid) are assigned by the platform through the
CPU's ``sapic-id`` property, or ``ia64_cpu_set_sapic_id()`` before the first
reset, and survive reset. They read back exactly as written, as the PAL's
self-test needs. On this machine the firmware reads them as the NASID in bits
22:16 and 31:27 and the slice in bits 26:23 (*software expectation* from the SAL's
use and the SDM's id/eid split), and only slices 0 and 2 occur.

CPUID register 3 reads family 0x1f, model 1, revision 5 and four further
registers (Itanium 2 reference manual Tables 12-1 and 12-2: 6 MB-class
model). CPUID register 4 reports only the long-branch bit. The manual's
Table 12-3 defines the bits (long branch, spontaneous deferral, 16-byte
atomic operations) but not what a given part sets. That Madison sets only
long branch is *inferred*: a 2006 linux-ia64 thread presents the 16-byte
atomic operations feature as new with Montecito. So ``ld16``, ``st16`` and ``cmp8xchg16`` raise Illegal Operation here.

Guest time
----------

The ITC is QEMU's virtual clock scaled to the part's frequency (1.6 GHz) plus
a per-CPU offset, and the interval timer is a QEMU timer per CPU on the same
clock that also wakes a halted CPU. With ``-icount`` the whole machine is
deterministic; without it the virtual clock runs at host time, so a guest
that executes far slower than silicon (by a factor no source gives) sees time
pass relative to its own instructions. A known consequence: a 2.6.5 timer
handler can outlast a tick, and the kernel logs ``Oops: timer tick before it's
due`` and goes on. The SHub real-time clock runs from the same clock.

The PROM's PAL_B self-test needs an *approximation*. It reads the ITC, writes
the ITC and ``cr.itm`` 100 ticks ahead, rewinds the ITC, spins on it and
requires exactly one delivery of the interrupt (PAL_B 5.73 at ``0xffeebde0``,
read in the stock image). Silicon covers the first writes in a few cycles;
the model, on a wall-clock-driven clock, can take thousands of ticks between
those instructions (*observed* under host load). So a write of the ITC arms the
match from the value written, not from the clock after the write; a write of
``cr.itm`` that the ITC has already passed arms nothing, as the SDM says; and a
read of ``ar.itc`` that returns a value at or past an armed match raises the
interrupt before it returns, ends its translation block so the interrupt is
taken before the next bundle, and disarms the match until the next write of
``cr.itm`` or the ITC. The QEMU timer is the backstop. The hardware rule for a
match the ITC has passed is *unknown*. Under ``-icount shift=0,sleep=off`` at
``-smp 1`` the stock PROM reached the EFI boot selection countdown (3 seconds
of 10 left) when a 580 second wall limit ended the run; it did not reach the
shell in that time. The reset write of the ITC does not arm the timer.

Floating-point Software Assist
------------------------------

The PAL's floating-point self-test runs famax, famin, fcmp, fcvt.fx, fma,
fmax, fmin and frsqrta on unnormal operands with the D trap enabled and expects
the Software Assist fault (ISR.code 0x08, vector 0x5c00), not the
Denormal/Unnormal fault. The SDM lets a processor signal SWA at any time for
operations that consume denormals (Vol. 1 5.4.1.2), and no public Itanium 2
document says which operations Madison sends to SWA. The model raises SWA, with
the FPSR flags untouched, when exactly those instructions consume an unnormal
operand, whatever the D trap mask. fnorm, the add and multiply family, and
frcpa keep their SDM D response: SAL_B runs ``setf.sig`` then ``fnorm`` on
integers before anything can print, so SWA there would be wrong. Normal
operands never take SWA. This is an *approximation*: the PAL's own test expects SWA from these
instructions and nothing says which others Madison sends there. It is
traced as ``ia64_fp_swa_estimated``.

Unsupported accesses
--------------------

An access the SHub or another device does not implement is logged with its
address, direction, width and write value (``-d unimp``) and fails with
``MEMTX_DECODE_ERROR`` where nothing is modelled or ``MEMTX_ERROR`` where the
register is but the effect is not. A qtest access sees only the log and the
error; a processor access that fails is a checkstop (below), and ``-action
panic`` decides what the VM does. An address no region covers is
unassigned memory and fails the same way, unlogged. This is model
instrumentation, not a claim about the hardware's error response. Unknown
registers never return successful zero reads or acknowledge writes as if they
had implemented hardware effects.

A failed processor access is a checkstop, not a machine check. The CPU logs the
cause under ``-d unimp``, halts, and QEMU reports a guest panic, so
``-action panic=pause|shutdown|exit-failure|none`` decides what the VM does (the default
shuts it down); this is an *approximation* (see the estimates table). Nothing
else stops the VM on guest behaviour: a guest that loops on a fault or an MSR
is the guest's business.

What the stock PROM does with a machine check is read from its code in the
chips notes (``software/firmware/snprom/pal-b-halts.md`` and
``software/firmware/snprom/smp-and-timers.md`` in the chips repository, at
``efef3fe93b11``); these are *software expectation* from the shipped image and
the addresses are PROM image addresses. The PALE_CHECK vector (*inferred* to be
``0xffffff90``) branches through ``0xffff8a80`` to a PAL_B stub at
``0xffe88080``, which requires bit 56 of MSR 1640 and otherwise takes the error
exit at ``0xffff8120`` with ``r30`` set to 2. The handler at ``0xffeca570``
takes the pointer to the processor's minimum state from MSR 1640 bits 49:9; the
chips notes match the layout it writes to SDM figure 11-2 (p. 2:304); that is
the notes' reading, not checked against the SDM here. Stage 2 (``0xffe99bc0`` to ``0xffe9b6c0``)
decodes the error from about ten MSRs: it reads MSR 1558 (bit 13 valid, bits
20:16 a cause code, bits 25:21 a structure code) and MSR 1417, 1087, 1039, 1537,
1282, 1279, 1779, 1168 and 1572. The path then calls SAL with function 1
through the pointer at ``0xffffffe8`` (SAL_A at ``0xffc4e000``). SAL returns to
the interrupted code through ``PAL_MC_RESUME`` (PAL_B index 26 at
``0xffe9e3e0``) when its classification returns 4, which needs the processor
state parameter's uncontained and corrected-error bits clear and the saved
instruction pointer inside a registered no-fault range; a classification of 2
reaches the same routine with a different argument (``new_context`` 1 where 4
uses 0), and the notes do not say what it means.
The model delivers none of this, because the values a bus error latches are
*unknown*: the notes list the meaning of MSR 1640 bits 4, 7, 53, 56 and 57, of
MSR 1558 and of MSR 1417 as not settled, and which combination an empty-slot
read produces as unknown. A guest that expects to recover from a bus error sees
a panic instead.

The chips repository's PROM reset-path register list
(``devices/shub/prom-reset.md``) records the SHub accesses found by reading the calls from reset to the
console start. Method tables it did not trace may touch more. The :ref:`SHub model <sgi-shub>` answers those accesses with what
the list says the code needs. Each answer is *software expectation* for
what the PROM requires, not a hardware reset value, and the model page
rates the approximations. Registers outside that list, such as the
directory contents and the network interface, are refused at the
first access.

The node UART's interrupt path, the DIMM contents, the memory controller and
the PCI discovery results are approximations, each rated in the table below.
They are enough for the PROM to print its console, test memory and the
processor, and reach its EFI shell (the functional test below runs exactly
that), but no hardware measurement backs them.

Behaviour to revisit
--------------------

Every behaviour the model supplies without a source is listed here with its
rating, the evidence for and against and what would settle it.

.. list-table::
   :header-rows: 1
   :widths: 30 14 32 24

   * - Behaviour
     - Where
     - Evidence
     - How to settle
   * - Reset vector ``0xffffffb0``
     - ``cpu.c`` reset, ``altix350.c``
     - Inferred. For: patent US7305544B2, a Merced trace, the PAL_A layout, and the processor state PAL_A expects at reset entry. Against: no Madison manual states it.
     - A Madison reset trace or manual text.
   * - Unknown MSRs: indices 0 to 2047 are write-through storage, reads return the last write or 0; an index of 2048 or more reads 0 and drops the write
     - ``arch/system.c``
     - Approximation for the storage, inferred from the PROM for the range. For: nothing needs more so far; the PAL executes a ``mov`` with index ``0xf80001`` and goes on, and a Reserved Register/Field fault there stalls the stock PROM. Against: which indices a Madison implements is unknown, and MSR 1536 and 1715 reset values are unknown and steer PAL branches. MSR 1715 bits 15:8 are compared with the PAL_B header revision byte by a signed less-than: 0 accepts every entry, as does any value up to 5, and a value above 5 skips the PAL_B 5.73 entry (inferred).
     - A rated description per register; replace each one that gets one.
   * - ``br.ia`` with ``r5`` 0 and a table in ``r2`` reports the load done (``r1`` 0)
     - ``arch/rse.c``
     - Inferred. For: the first 256 qwords of the table match what the PAL's MSR 1224 loader writes, so the use reads as a table load. A nonzero ``r1`` on return sets MSR 1640 bit 59. Against: what the callee does with the table is unknown.
     - Hardware behaviour or a source for the PAL table format.
   * - ``ar.rnat`` keeps its value across a ``mov ar.bspstore``
     - ``arch/system.c``
     - Software expectation. For: Linux's syscall entry from kernel mode (``.break_fixup`` in ``arch/ia64/kernel/ivt.S`` in Linux, used by ``kernel_execve``) writes ``ar.rnat`` and then ``ar.bspstore``, and needs the NaT bits it wrote. Against: the SDM (Vol. 2 6.5.3) leaves RNAT undefined after the write.
     - A test on a part.
   * - Reserved M word ``0x108000000`` halts until an interrupt is pending (whatever psr.i says) when MSR 1553 bits 43:42 are set, else runs the cache-array command armed in MSR 1546 bit 63, else does nothing. The model never clears the arm bit, so the halt check comes first. The B word ``0x48000000`` returns from interruption (IP from ``cr.iip``, PSR from ``cr.ipsr``, ``cr.ifs`` ignored). Both run on a family 0x1f CPU at privilege level 0 with qp 0 wherever they are fetched, and fault otherwise
     - ``translate.c``, ``exception.c``
     - Inferred for the M word. For: all five PAL sites that run the word with no command armed (PAL_HALT_LIGHT, PAL_HALT and three others) set MSR 1553 bits 43:42 just before it, and the PAL's wake logic reads no wake register. Against: nothing documents the word, the bits' meaning as halt depth is a reading, and whether hardware needs a mode the PAL sets is unknown. No fetch address can gate it: the PROM runs the same words from its RAM copy of the PAL at an address the SAL picks. The B word is inferred rfi-like (12 sites in PAL_B 5.73, each ends a block, three after writes of ``cr.iip`` and ``cr.ipsr``). The stock PROM boot to the EFI shell reaches none of them.
     - What the words do on hardware (PAL source or a test on a part); a guest path that reaches the B word.
   * - Reserved word ``0x21b0000000`` (opcode 1, ``x6`` 0x36, ``r3`` set) runs as a no-op
     - same
     - Approximation. For: sits in a 128-byte-stride loop next to ``fc``. Against: reserved in SDM vol 3 Table 4-45 (Illegal Operation architecturally); the firmware executes it; run as a no-op because the model has no cache state to write back; effect unknown.
     - As above.
   * - MSR 1212 bit 14 after a ``0x9021`` command; bits 13:0 ``0x317c`` and MSR 1226 constant (cpuid revision 4 to 8)
     - ``arch/system.c``
     - Approximation. For: the PAL's own compare constants. Against: makes the known-answer test pass by construction, and no source describes the bit or the command.
     - The real register contents.
   * - MSR 1552 bit 2 reads set once written
     - ``arch/system.c``
     - Approximation. For: the PAL polls it after each command. Against: no source on the bit or a command's duration. Groups 4, 10, 11 also poll bit 4.
     - A description of the register.
   * - MSR 1559 bits 26:22 report ``itc_frequency_hz / frequency_base_hz`` (16)
     - ``arch/system.c``
     - Approximation. For: values 8 to 23 take the same PAL path and 16 matches the model's clock ratio. Against: the register is unknown.
     - The register's definition.
   * - MSR 1730 training nibbles jump to the loop's end state
     - ``arch/system.c``
     - Approximation. For: read from the loops' writes; thirteen copies of the loop reach target 8, so the end states are exercised. Against: a real part steps one count per strobe (inferred from the servo loops) and the model jumps to the end state; the step size and the reset value are unknown.
     - Servo loop description; test on a part.
   * - MSR 1536 bits 59:48 report the L3 ways of the modelled part
     - ``arch/system.c``
     - Approximation. For: the PAL derives both PAL_CACHE_INFO and its group 11 way scans from the count of clear bits, so only the way count matters. Against: which bits a real part sets is unknown.
     - The register's description, or a hardware read on a 3 MiB part.
   * - Cache-array port: array commands run by the reserved M word, check codes, tags, state, replacement and the logs in MSR 1505, 1509 and 1558 (Madison)
     - ``arch/cache-array.c``
     - Approximation. For: the reading of the PAL and the compares in PAL groups 10 and 11. A physical-mode load sees a modelled line from the first array command until reset, wherever it is fetched from (a throwaway build saw no such load outside firmware code, on the PROM and a SLES 9 kernel boot). Against: one firmware image, and the PAL's own invalidation is the only thing that retires a line, so a stale line could reach a later load (a Debian 7 kernel and a compiler run were not covered). The bit meanings of MSR 1505 to 1512 and MSR 1558 are unknown, and so are tag fields the PAL does not compare, the way index, which of MSR 1509 to 1512 logs and the dword-to-injection mapping.
     - Read of the helper against hardware behaviour or a part.
   * - Floating-point Software Assist for unnormal operands in the listed instructions
     - ``arch/fp.c``
     - Approximation. For: the PAL's self-test expects it. Against: no public document says which operations Madison sends to SWA.
     - Intel Itanium 2 documentation or a test on a part.
   * - PMU register bits and widths where they differ from 251110-003 (PMD4 to PMD7 bit 47, PMC8/9 bit 32, PMC11 25:0, PMC13 to PMC15, PMD1/3/16 widths); counter width 47
     - ``arch/system.c``
     - Software expectation. For: the model's values equal the masks the PAL itself expects, and the PAL shipped and ran on real Madison parts. Against: the manual says otherwise; the manual may describe an earlier revision.
     - The Madison revision's manual or a register dump.
   * - A full ALAT replaces a valid entry round robin, one pointer for both memory ports
     - ``arch/alat.c``
     - Approximation. For: the manual gives the priority (same register, first invalid, then an approximately FIFO pointer) and the PAL's tests never need the third step. Against: the manual names two advancing pointers, one for each of ports M0 and M1; the model has one because it has no ports; the advance rule is unknown.
     - The pointer rule from a test on a part.
   * - The ITC is the virtual clock at 1.6 GHz; a write of the ITC arms the interval timer from the value written, a read at or past an armed match raises the interrupt before it returns, and a ``cr.itm`` write the ITC has passed arms nothing
     - ``arch/interrupt.c``, ``translate/gen-system.c``
     - Approximation. For: the PAL programs the ITC and ``cr.itm`` 100 ticks ahead, rewinds the ITC and requires exactly one delivery (PAL_B 5.73 at ``0xffeebde0``), which silicon covers in a few cycles and a wall-clock-driven clock does not; measured from the clock after the write, the rewind armed nothing under host load and the PAL stalled in its halt at ``0xffe7f100``. A guest that delivers the interrupt on a read is indistinguishable from one that takes it a few instructions later. Against: the SDM delivers nothing for a past value and an OS that deliberately writes one gets no interrupt; the hardware rule for a match the ITC has passed is unknown; without ``-icount`` the guest executes far slower than the clock, so a 2.6.5 kernel logs ``Oops: timer tick before it's due``.
     - A measured gap between the ITC read and the ``cr.itm`` write on a part, or a description of the PAL test.
   * - A range tag write with state 3 puts 256 KiB of zero-filled RAM at the tag's address for one CPU, and clearing the MSR 1504 enable bits drops it with no write-back
     - ``arch/cache-array.c``
     - Approximation. For: the PROM's table builder races on shared RAM and faults with two CPUs, and with private ways both reach the shell. Against: the meaning of state 3, write-back at disable and coherence with other CPUs are unknown, and the 24 ways, created when the CPU realizes (6 MiB of lazily committed host memory per CPU), assume a 6 MiB part.
     - A description of PAL procedure 520, or an L3 way test on a part.
   * - Ordinary RAM answers from reset, so the SAL's table at ``0x1000000`` works before memory initialization
     - ``altix350.c``
     - Approximation. For: the PROM stores and reads that table before it trains memory. Against: DRAM training and cache-only execution are not modelled.
     - A description of the memory controller's reset state.
   * - ``br.ia`` with ``r5`` 1 and an image in ``r2`` checks a DSA signature and returns ``r1`` 1 on a bad image, ``r1`` 0 and resume at ``b0`` on a good one; with ``r2`` 0 it resumes after the branch
     - ``arch/rse.c``
     - Inferred. For: the PAL_A image check asks for exactly this verdict, and the algorithm and key are the PAL's own, read from the block at the CPU property ``pal-auth-key-addr`` (``0xffffa1d0``, set by the machine; with the default 0 every check fails). Against: the real service also does work the verdict hides.
     - The service's description.
   * - MSR 1632 to 1635 writes are accepted and discarded
     - ``arch/system.c``
     - Approximation. For: the firmware writes them and nothing reads them. Against: their function is unknown.
     - A description of the registers.
   * - CPUID register 4 reports only the long-branch bit
     - ``cpu.c``
     - Inferred. See the processor identity section.
     - A register read from a Madison.
   * - The node's L1 chassis area (device 1, selector 1) and board information area (selector 2) are format-valid IPMI FRUs with every field empty
     - ``sgi-l1.c``
     - Approximation. For: the ColdFire handler's trace gives the window reply and the format, the PROM goes on with any valid area, and ``software/firmware/snprom/boot-sequence.md`` in the chips repository says the SHub's reader asks for the chassis area (selector 1) and then the board area (selector 2). Against: a real node serves its own manufacturing record, which no source holds. A read of any other selector still stops the guest as unsupported.
     - A capture of the node EEPROM, or the record's source.
   * - The ISP12160's firmware is not executed: the driver's microcode download is accepted and its mailbox and queue commands are answered at the register level
     - ``qlogic-isp.c``
     - *approximation*, reported at run time by every download command. For: the model has no vendor firmware and no public RISC specification, and the EFI driver and Linux's qla1280 only use the commands it answers. Against: any firmware-visible behaviour beyond those commands is not modelled.
     - A specification of the ISP's RISC core, or a trace of what the firmware does for each driver command.
   * - Bus configuration 1 bit 3 of the ISP12160 stores and selects nothing
     - ``qlogic-isp.c``, ``altix350.c``
     - *software expectation*: the EFI driver stores it, reads it back and restores it (``software/firmware/snprom/efi-qlogic.md`` in the chips repository, read from the driver binary), and what it does on this part is unknown. For: the EFI driver writes it and requires it to read back. Against: on the ISP1040 it selects the SXP bank, and the 12160 uses bits 9:8; what it does on the 12160 is unknown.
     - The ISP12160 data sheet's register description, or the EFI driver's later use of the bank.
   * - A little-endian host's DMA takes bytes in order with address bit 55 clear
     - ``sgi-pic.c``, ``altix350.c``
     - Inferred. For: the Altix drivers never set bit 55 and expect descriptors in order. Against: the PIC's own rule is from its big-endian MIPS hosts, and no source states the net lane rule for this host.
     - A PIC specification, or a trace of what the SHub does to inbound data.
   * - The SN2 L1 answers the PROM's version report (``100b``), which the real L1 leaves unanswered, with status 0
     - ``sgi-l1.c``
     - Approximation. For: the PROM parks without an answer. Against: what ended the wait on hardware is unknown.
     - A trace of the L1 around the report.
   * - The SN2 L1's plain replies (module word and type letter, slab, slot, partition, debug switches, sharing mode, maximum nodes, serial) and the processor-presence word
     - ``sgi-l1.c``
     - Inferred. The layout is from ``software/firmware/snprom/l1-requests.md`` in the chips repository and the values are those of one node. Against: a deployed L1 reads these from the board and its settings.
     - A capture of a real node's replies.
   * - DIMM serial presence detect is one chosen part, bank 0 only, and device 98 answers "absent"
     - ``sgi-l1.c``
     - Approximation. For: the PROM sizes 2 GiB from it. Against: the DIMMs and riser of a real 350 are unknown.
     - A capture of the node's EEPROM reads.
   * - Console input frames carry the last output index with no class bits; the display and event-subchannel requests are acknowledged and the L1 sends no events
     - ``sgi-l1.c``
     - Inferred from the PROM's receiver and the kernel's console driver; the display and event-subchannel acknowledgements are a software expectation, since the PROM's runtime takes exactly those replies. Against: the real L1 forwards typed input only in a pass-through mode and sends its own events.
     - A trace of the L1's console and event traffic.
   * - Link status bit 13 reads set, the credit count reads 8, the bus clock select reads 0, the slice record reads 0, the slice-present word has one bit per front-side bus that has a processor and the directory result reads 0
     - ``sgi-shub.c``
     - Approximation. For: each is the smallest value the PROM's code accepts. Against: none is sourced as a hardware value.
     - The registers' definitions or a trace of a real SHub.
   * - PIO write status reads idle, a lock read takes the lock, the memory test engine always passes, and the SHub identification word resets to NASID 0 and revision 3
     - ``sgi-shub.c``
     - Approximation. PIO writes complete synchronously, the lock rule follows the header's wording only, no failure is modelled, and the reset value is not sourced.
     - The registers' definitions or a trace of a real SHub.
   * - The PROM flash reports AMD manufacturer and device ID words ``0x0001`` and ``0x227e``
     - ``sgi-shub.c``
     - Inferred: the PROM keeps but never compares them.
     - A read of the real flash's ID.
   * - The reset window at ``0xffa00000`` reads the flash's array bytes whatever command state the chip is in
     - ``sgi-altix350.c``
     - Approximation. For: the PROM issues flash command cycles through the SHub window while it still fetches from the reset window, and a window that followed the chip's mode stops the boot with a bus error on the next fetch. Against: a chip in its autoselect or unlock state answers a read of its array with ID or status words.
     - A trace of a real board's fetches during the PROM's flash probe, or a read of the window while the chip is in autoselect mode.
   * - An II interrupt packet goes to the processor the pid in bits 19:4 of the matching ``SH_II_INT0_CONFIG`` or ``SH_II_INT1_CONFIG`` (``0x1_1000_0080`` and ``0x1_1000_0180``) names. Until the PROM has written that register the packet goes to the first processor; with one processor every pid names it
     - ``sgi-shub.c``
     - Approximation. For: the packet carries no processor field, and the PROM programs the two config registers with the pids of slice 0 and slice 2, so the register layout and the roles come from its stores. Against: whether the real II also reads the config's other fields (agent, type, base) for delivery is not modelled, and a pid no processor has stops the guest.
     - The II's behaviour for a config pid no processor answers, or a capture of a packet's delivery.
   * - SH_SCRATCH2 holds the node's identity after reset: rack in bits 34:26 (and rack >> 6 in 35:32), bay in 25:20, slab 0 in 18:16, partition in 15:8 and ``0x6c`` in 7:0, from the L1's ``rack``, ``bay`` and ``partition`` properties (``0x0410006c`` for rack 1, bay 1, partition 0)
     - ``sgi-l1.c`` (pushes at reset release), ``sgi-shub.c``, ``altix350.c``
     - Approximation for the values: the mechanism and layout come from the shipped L1 firmware, the rack, bay and partition are a choice. The same properties give the L1's rack, bay and partition replies, so the two cannot disagree. The register is plain storage, so the SAL's later read-modify-write that sets bits 41:39 after the processor election keeps the identity fields.
     - A real node's rack, bay and partition, or a read of its SCRATCH2.
   * - The node's 16550 raises the transmit-empty interrupt again when the enable is written while it is already set and the transmitter is empty
     - ``sgi-shub.c`` (``sgi-shub-uart``)
     - Inferred from the PROM and the Linux console driver: the PROM's console code reads the interrupt status, then writes the enable back, and the kernel's next console chunk depends on the interrupt that write re-raises, since the driver's polling timer stops once the interrupt is granted. No datasheet or capture covers a same-value write. Without it the console stops after one 127-byte frame. The ``sgi-shub-uart`` device passes such a write to the 16550 as the enable with THRI clear, then the value, except while the divisor latch is open, when the offset is the divisor's high byte. The chip is not shown to be a PC16550D, and a real one may differ.
     - A trace of the node UART's interrupt line around an enable write, or its datasheet.
   * - A load from local compatibility space above 4 GiB (``0x1_0000_0000`` to ``0x7_ffff_ffff``, no node RAM or register there) completes with zeros
     - ``altix350.c``
     - Approximation. For: the address is a hole, and a real kernel (3.2's unwind-table setup) loads one byte from it and uses the value only as a table bound, so any completed value boots. Against: what the chip returns is unknown, and the PI's address-access error summary bit (masked by the PROM unless ``AttrCheck`` is set) is not set. Stores there still stop.
     - A trace of the chip's response, or the PI error summary after such a load.
   * - The IOC4's byte bus carries an M48T35-style time-of-day chip: 32 KiB of RAM at BAR 0 + ``0x80000``, the clock in the top eight bytes, time as host UTC (shifted by ``-rtc base=``) with the year stored as years since 1970
     - ``altix350.c``
     - Approximation for the part. The PROM's ``GetTime`` takes the chip path whenever its PCI scan finds an IOC4, and never asks the L1 for the time then. It probes the layout at run time and supports an M48T35-style and a DS1286-style map, and the IO9 owner's guide lists a time-of-day clock on the IOC4, but which part a real board carries is unknown. ``SetTime`` is not kept: writes to the clock registers are ignored, and the rest of the window is RAM. The RAM is migrated. A multi-byte clock field is read byte by byte, so a read that straddles a second can be torn. The 32 KiB window is not aliased above ``0x8000``: the PROM's probe at offset ``0xf7e0`` reads zero and logs a guest error, which the PROM tolerates.
     - A capture or parts list of the base I/O card's IOC4 byte bus.
   * - A PIC interrupt packet aimed at node memory, as the kernel's DMA flush aims one, stores its 64-bit data word (``0x10f`` for level 15) in the node's little-endian byte order
     - ``sgi-pic.c`` (``pic-rev3-shub-compat``)
     - Inferred. The 3.2 kernel's flush forces such an interrupt and waits for ``0x10f`` at the word, which only reads back if the word is stored in the processor's order. The SHub's real byte order for the data of an interrupt written to memory is not documented.
     - A trace of the data word a real node's memory receives, or the hub's interrupt-packet format.
   * - The IOC4's ATA channel is a programmed-I/O ATA and ATAPI bus on QEMU's memory-mapped IDE device, one register per word, with the drive's interrupt in bit 0 of the IOC4's other status word; the first two ``-drive if=ide`` drives are its master and slave. With no drive attached the task file reads as a floating bus (a byte ``0x7f``, the data register ``0xff7f``) and writes go nowhere, so the old IDE driver's probe ends at once. There is no DMA
     - ``sgi-ioc4.c`` (``ata``), ``altix350.c``
     - *approximation*. The register map, the lane (a byte at the base of its word, the 16-bit data register at the base of word 0, as the PIC swaps lanes) and the interrupt come from ``devices/ioc4/ata.md`` in the chips repository and the public drivers. The IOC4's own DMA block at ``0x140`` is not modelled, so a driver that starts a DMA transfer (the 2.6.5 IDE driver, which sets multiword DMA mode 2) will not finish it, and the 7.x libata port, which forces PIO, works. Whether the chip clears its status bit when the drive's status is read is unknown; here the line follows the drive's own request. The drive's interrupt level (bit 0 of the IOC4's other status word) is not in the migration stream.
     - The DMA block's behaviour, which needs the table walk the contract describes and access to the IDE core's buffer state.
   * - The IOC4's INT_OUT register (BAR offset ``0x28``), with the ``attach-compat`` property, keeps its control bits and, in toggle mode (mode field 7), reads bit 31 as a square wave off the virtual clock, one half-period every 520 IOC4 clocks; the other modes read the stored value
     - ``sgi-ioc4.c``, ``altix350.c``
     - Approximation. The kernel's clock probe (``drivers/misc/ioc4.c`` in Linux, ``ioc4_clock_calibrate``) writes toggle mode and counts the output's level changes to measure the PCI clock. Against: how the real output behaves in the other modes, and the 520-clock period, which is chosen to give a plausible clock, are not sourced. Without the property the offset is unsupported, as on the MIPS machines.
     - A trace of the real INT_OUT output, or a description of its modes.
   * - SIO_CR (BAR offset ``0x20``) reads bits 7 and 8, the serial and ATA request-idle indications, set
     - ``sgi-ioc4.c``, ``altix350.c``
     - Bit 7 is established as polled until set (``devices/ioc4/registers-extra.md`` in the chips repository); bit 8's role is inferred from the public driver's name for it (``drivers/tty/serial/ioc4_serial.c`` in Linux). The model reports both set because nothing ever has a request in flight there. Rated *inferred*.
     - The register's description, or a read during DMA on a real board.
   * - The IOC4 (device 1) and the ISP12160 (device 3) drive PIC inputs 0 and 2 with their INTA; a card plugged into device 2 drives input 1
     - ``altix350.c``
     - *software expectation* for the rule, *inferred* for the riser. The stock PROM (firmware registry entry ``firmware/snprom-10.54/shub1snprom.bin``, SAL 5.04, SHA-256 ``51ccf9ad830b85ee6c88d6d73d2512322a86454e63db51c57454679283a34617``, loaded at ``0xffa00000``) routes the INTA and INTC of PCI device *d* to PIC pin *d* - 1 and the INTB and INTD to pin (*d* + 3) & 7, for devices 1 to 4 only, in its routine at ``0xffb6c460``; the rule is the same on every PIC bus, and the model wires INTA only. The SAL 4.43 image, which has no registry entry, has an identical body in a reading that has not been independently reviewed. The Altix riser's wiring is unmeasured on hardware. Device 2 holds no card of the base I/O board.
     - The PROM's per-device interrupt records (the input each device reports), or a capture of the PIC's interrupt-device register.
   * - A force-always write (``+0x180`` plus 8 per input) sends its interrupt packet whether or not the input is enabled. A force-with-pin write (``+0x1c0`` plus 8 per input) sends only for an enabled input that is high
     - ``sgi-pic.c`` (``pic-rev3-shub-compat``), ``altix350.c``
     - Approximation. The kernel's DMA flush programs an input's destination to a node memory word, forces it and waits for the packet's value there, and never sets the input's enable bit itself (a software expectation about Linux). The PROM builds the flush record and may set the bit, and that the chip sends for a disabled input is not even inferred. The MIPS PIC models keep sending nothing.
     - The PROM routine behind SAL service ``0x5a`` (the DMA flush list, near ``0xffb3d290``) to see whether it sets the enable, or a force write to a disabled input on a real PIC.
   * - The node's 16550 console UART follows the stock 16550 core's rules: a 16-byte FIFO, the transmit-holding interrupt only when the FIFO is empty, the interrupt identification read clearing it, and a 115200 baud clock
     - ``sgi-shub-uart.c``
     - *inferred*. The firmware and the Linux console driver run on it, and ``software/firmware/snprom/l1-requests.md`` in the chips repository describes the behaviour the console code relies on without naming the part. The real part, its FIFO depth and its clock (the notes call the clock unknown, 12 MHz weak) are not known, and no qtest would fail if the FIFO depth or the clock changed.
     - A datasheet or capture of the node's console UART.
   * - A forced PIC packet is delivered 1000 ns after the force write, not in the same instruction
     - ``sgi-pic.c`` (``pic-rev3-shub-compat``), ``altix350.c``
     - *approximation*. A guest that forces an input from its interrupt exit, as the SN2 kernels do, would otherwise take the interrupt again before it ran another instruction. The value is a choice and the real flight time is unknown. Forces in flight migrate with the PIC.
     - A trace of a force write and the resulting interrupt on a real node.
   * - SH_EVENT_OCCURRED bit 20 latches a rising edge of the node UART's interrupt line, sends one message, and an acknowledge through the alias clears it even while the line is still high (no re-latch)
     - ``sgi-shub.c``
     - *approximation*. For: it is an edge latch, one message per setting, which is what every kernel we run needs. The chips notes (``software/firmware/snprom/l1-requests.md``, the Linux 2.6.18 and 2.6.5 subsection, revision 809eb279bb54) read the public kernel source: from 2.6.x the interrupt entry acknowledges first (reads the register, writes every pending bit in the mask to the alias), runs the driver handlers, and then ``sn_end_irq`` reads the register again and sends itself a fresh message if bit 20 is set. A re-latch on an acknowledge with the line still asserted therefore loops until something clears the UART source. On 2.6.18 the console driver, whose handler reads the console status first, registers last, after ``snsc`` and ``mmtimer``; the handlers registered before it on that vector do not read the console status, and whether the PROM's interrupt-router status call drains the UART is unread. So a re-latching UART has nothing known to clear it in that window, which is the *inferred* mechanism for a guest stalling before ``SGI Altix RTC Timer``: it is derived from mainline 2.6.18 source and was not seen in a run, and the RHEL tree itself was not read. The notes also say 3.2.78 has no end hook and never re-checks the bit, and that 3.4 and later drain in the handler; those two and the 2.6.5 reading come from the notes and were not independently confirmed (*inferred*). The model's rule suits CentOS 4.7 (2.6.9), RHEL 5.11, SLES 9, the Debian 3.2 kernel and a 7.0 kernel, which reach a login in our runs. Against: no source line gives the re-latch rule, so the real chip may behave differently. A source left asserted and undrained stays quiet until it drops and rises again; no kernel here relies on that. Whether the real bit is a latch or a sticky level is unknown.
     - The SHub's event semantics for the UART source, or a trace of the real bit after an acknowledge with the line still high. A run of the RHEL 5.11 guest with a re-latching build that logs the alias write, bit 20, IIR and IER per message at the stall would confirm the loop.
   * - The IO9's NIC is a BCM5701 at PCI device 4, its INTA on PIC input 3, present only when a ``-nic`` names the model (``-nic user,model=bcm570x``)
     - ``altix350.c``
     - *inferred*. The model reuses the BCM570x device the Origin 350's IO9 uses, at the device and PIC input that board's model gives it (input = device - 1). The evidence for a BCM57xx on this board is that the SLES 9 kernel builds the ``tg3`` driver, the PROM carries a Tigon 3 builder for its configuration records, and the Origin 350's IO9 capture shows ``14e4:1645`` at window 4. The part is a BCM57xx, modelled as a BCM5701. The Altix 350's slot is unmeasured.
     - The PROM's per-device interrupt record for the NIC, and a read of the real board's MAC.
   * - The NIC's station address is ``08:00:69:E1:``\ *emu-id >> 4*\ ``:``\ *(emu-id & 15) << 4* from the machine's ``emu-id`` property (0 to 4095, default 0), or the ``mac`` property when given; a ``-nic`` with a different ``mac=`` is refused, as on the Origin 350's IO9. The machine takes only ``emu-id`` and ``mac`` and refuses ``serial`` and ``hostid`` as unknown properties, because no guest here reads a host id. It resolves the identity as the text family, whose explicit ``mac`` need not start with 08:00:69 (the address family requires that, for IRIX's host id)
     - ``altix350.c``, ``board-mac.c``
     - *approximation*. For: the project's identity rule (see :ref:`sgi-identity`) gives each run a stable address in the block reserved for the emulator, so two guests do not share one generated address. Against: no source gives an Altix 350 address. The ``08:00:69`` prefix is the rule's and not a measurement of this board, and ``mac`` is not required to carry it.
     - A read of a real board's NIC address.
   * - An unclaimed PIC type 0 configuration read completes with all ones
     - ``sgi-pic.c``, ``altix350.c``
     - Approximation. For: the PROM reads an empty slot as vendor ``0xffff`` and cannot recover a bus error that is not a machine check. Against: no source says the PIC returns all ones. The chips notes recommend a synchronous machine check (the PIC's error latch, then a bus-check machine check through PALE_CHECK, contained), SGI's SN2 error handling expects a bus error or machine check, and all-ones sends the PROM down a different path (the slot is marked in its valid-device mask).
     - A trace of what the PIC returns for an empty slot, or the machine-check route through PALE_CHECK.
   * - A write to ``SH_SCRATCH1_ALIAS`` takes effect only while ``SH_SCRATCH1`` reads 0
     - ``sgi-shub.c``
     - Approximation. For: the PROM's election needs exactly one winner among the processors, and it writes the alias, then reads ``SH_SCRATCH1``. Against: how the pair relates in hardware (alias, latch or separate stores) is not known.
     - A read of both registers after two processors have written the alias on a real node.
   * - A block transfer finishes inside the control write and, with the notify bit set, writes 0 to the notification address
     - ``sgi-shub.c``
     - Approximation for the timing, inferred for the word. For: Linux's ``bte_copy`` polls the word for completion and the PROM clears it first. Against: the hardware's final status word and the transfer's duration are not documented, and errors, which reach the kernel through the CRB, are not modelled.
     - A trace of the notification word and the length register across a transfer on a real SHub.
   * - Write-only registers (``error_clear``, ``SH_JUNK_BUS_TIME`` and the others the page lists) accept any write and ignore it; reads are refused
     - ``sgi-shub.c``
     - Approximation. For: the PROM only writes them, with zeros, masks or all valid bits. Against: write-one-to-clear and mask effects are not known, and nothing here clears.
     - The registers' definitions with their write semantics.
   * - The error, mask and alias words the PROM writes at set-up (blind cells) read back what was written and reset to the header's values
     - ``sgi-shub.c``
     - Inferred. For: the PROM sets bits in them by read-modify-write. Against: their effects are not modelled and the header's reset values are the only source.
     - A trace of the registers' behaviour, or their definitions.
   * - The I/O interface registers the PROM sets up (``iio_*``) are stored 64-bit cells that read back what was written
     - ``sgi-shub.c``
     - Approximation. For: the PROM writes them and reads some back. Against: the access protection, the CRB words and the performance counters have effects that nothing here applies.
     - The I/O interface register definitions with the effects of each field.
   * - A store to the unnamed register at ``0x1_1005_0608`` raises a PIO write error and latches the address
     - ``sgi-shub.c``
     - Approximation. For: the PROM's self-test expects that write to fail and reads the error status, and the header names the latch fields. Against: no register is named there and what the chip does is not known.
     - A trace of the self-test's status reads on a real SHub.
   * - A PIC bus master's request to node memory is accepted when the widget's inbound access bit is set and the NASID names the node, and refused otherwise
     - ``sgi-shub.c``
     - Inferred. For: Linux and the PROM's own DMA reach memory this way. Against: no SHub specification gives the I/O interface's address decode, and what a requester sees from a refused request is not known.
     - A SHub specification, or a trace of a refused inbound request.
   * - A CPU write to the reset window at ``0xffa00000`` is refused
     - ``sgi-altix350.c``
     - Approximation. For: the window is a read view of the flash array bytes, so a write has nothing to act on. Against: on a real board a write there may be a flash command cycle, as it is through the SHub window. A flash programmed through the SHub under code the CPU is running from the window takes effect: the ``flash-program-reaches-running-code`` qtest shows the new instructions run under TCG, and fails if the flash model's translation-block flush is removed. Only a program is tested, not a sector erase.
     - A real board's behaviour for a store to the window.
   * - A processor access that fails (a refused load or store, a failed fetch, the IA-32 transition) is a checkstop of the CPU, not a machine check
     - ``target/ia64``
     - *approximation*. For: the PAL's machine-check handler branches on MSR values (1640 bit 56, about ten MSRs) that no source gives for a bus error, so any delivered check would be a guess, and a guest that cannot recover is stopped visibly instead of running on with wrong data. Against: an error a guest could recover from would panic the VM instead. The PROM's own path (PALE_CHECK, SAL function 1, ``PAL_MC_RESUME`` for a contained check in a registered no-fault range) is a *software expectation* from the chips notes, with the entry address *inferred*.
     - A capture or document of the MSR values (1640, 1558, 1417) a PIO bus error latches, or a trace of the PROM's PALE_CHECK path on a real board.

Tracing SAL calls
-----------------

``-trace ia64_indirect_call`` logs every indirect ``br.call`` with its
target and ``r32`` to ``r35``, the callee's first four inputs, and
``-trace ia64_br_ret`` logs each ``br.ret`` with the return address and
``r8`` to ``r11``. A call is paired with the first return to its call
site's next bundle. The SAL entry
point is the second address of the SAL system table's entry point record, and
a SAL call is a call to it whose ``r32`` is the service index. The event is
off unless selected, and a running guest makes millions of such calls a
minute, so filter the log as it is written (for example through a FIFO read by
``grep``) or switch the event on with the monitor's ``trace-event`` only
around the operation of interest.

Running it
----------

A PROM boot to the EFI shell, with the console on the terminal:

.. code-block:: shell

   qemu-system-ia64 -M altix350 -m 2G -bios shub1snprom.bin \
       -display none -serial stdio

A fuller machine: two processors, a disk on the ISP12160, an IDE CD-ROM on the
IOC4, a persistent 8 MiB PROM flash image and a BCM5701 network adapter:

.. code-block:: shell

   qemu-system-ia64 -M altix350 -m 2G -smp 2 -bios shub1snprom.bin \
       -display none -serial stdio \
       -drive if=none,id=d0,file=disk.img,format=raw \
       -device scsi-hd,drive=d0,bus=scsi.0,scsi-id=1 \
       -drive if=ide,file=cd.iso,media=cdrom,readonly=on \
       -drive if=pflash,file=flash.img,format=raw \
       -nic user,model=bcm570x

Tests
-----

``tests/qtest/sgi-altix350-test.c`` checks the image endpoints and early RAM
without executing its test data as firmware. It also covers the machine's
wiring: the SHub and PIC interrupt routes, the IOC4's serial, IDE and
time-of-day paths, the ISP12160 and BCM5701 adapters' placement and DMA, and
that firmware writes and unsupported accesses are refused and logged.
``tests/qtest/sgi-shub-test.c`` covers the SHub's junk-bus UART, flash,
comparator gate, stored cells and two processors on a fixture machine, and the
machine test above the rest of its registers. ``tests/qtest/ia64-cpu-test.c``
covers the processor. A CPU-target build is required
to run them.

``tests/functional/ia64/test_altix350_prom.py`` boots the stock PROM to the end
of its processor test and to its EFI shell prompt. ``ALTIX350_PROM`` names
the image, which is never part of the tree. Without that variable the test is
skipped silently, so CI does not run it. Run it by hand with the variable set
before changing anything the PROM touches.
