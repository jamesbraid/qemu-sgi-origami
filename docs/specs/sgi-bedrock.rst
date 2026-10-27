.. SPDX-License-Identifier: BSD-3-Clause

.. _sgi-bedrock:

=================================================================
SGI Bedrock node controller (Origin 300, Fuel, Origin 350 family)
=================================================================

The Bedrock is the node controller of the Origin 300, Origin 350, Origin
3000 and Fuel generation, the successor to the Origin 200's Hub. It is a
single register window of seven units. QEMU models the units as separate
devices composed by a parent (``hw/misc/sgi-bedrock.c``): two processor
interfaces (``sgi-bedrock-pi.c``), the local block (``sgi-bedrock-lb.c``),
the network interface (``sgi-bedrock-ni.c``, ``sgi-bedrock-ni-regs.c``),
the I/O interface (``sgi-bedrock-io.c``), the memory and directory
interface (``sgi-bedrock-md.c``) and the internal crossbar
(``sgi-bedrock-xb.c``). Where each unit sits in the address space is on
:doc:`sgi-ip35-address-map`. Ratings follow :ref:`sgi-ip30`.

Every register offset and field below comes from SGI's own generated Linux
SN1 headers, ``linux-history:7570df54ef8c`` (tag v2.5.40,
``include/asm-ia64/sn/sn1/``), read with the named consumers: the Itanium
port, OpenBSD sgi at ``openbsd-sgi:2df91d80c104636deeef8124252ddd5951b55fbf``
and the out-of-tree MIPS port ``linux-ip35:9f613a321ea0``. The unit roles are
from *Origin 3000 System Architecture* (108-0240-002). **The headers state
no reset values** for the processor interface, network interface or I/O
interface and only two software constants for the crossbar parameters
register (``hubxb_next.h`` lines 29-30, used by no consumer), so every reset
value is *unknown* as hardware and an emulator choice (zero unless stated)
in the model. A register capture of a real Bedrock would settle them.
The silicon revision is likewise unknown (see :ref:`sgi-bedrock-lb`).

.. _sgi-bedrock-pi:

Processor interface
===================

Two interfaces, PI0 and PI1, each face two processors, A and B, over the
system bus (Linux ``sn1/arch.h``: four processors per node, two per
interface; 108-0240-002 printed page 2-12). Each holds the interrupt
pending banks and per-processor masks, the real-time counter and
comparators, region and cached-alias controls, protection registers and
error capture. Every register is 64 bits. PI1's registers are PI0's at
hub offset ``+0x200000``. The header defines 88 registers (``hubpi.h``).

.. list-table::
   :header-rows: 1
   :widths: 14 26 60

   * - Offset
     - Register
     - Contract (established by the header unless marked; reset unknown)
   * - ``0x00``, ``0x10``
     - CPU protect, I/O protect
     - 64 bits, one per region; denied accesses get an error reply
       (``hubpi.h`` lines 557-564, 604-611); retained across a soft reset.
   * - ``0x18``
     - region present
     - A zero bit blocks speculative coherent reads to a region
       (lines 630-640); OpenBSD writes all ones to both interfaces
       (``ip27_machdep.c`` lines 338-344).
   * - ``0x20``
     - CPU number
     - bit 0 processor A or B, bit 1 interface PI0 or PI1 (lines
       660-689); OpenBSD's first hub access (``ip27_machdep.c`` line 159).
   * - ``0x28``
     - cached-alias size
     - 4-bit code, 0 off, 1 to 15 for 4 KB doubling to 64 MB
       (``hubpi_next.h`` lines 200-215); every interface in a system
       carries the same size (``hubpi.h`` lines 701-702).
   * - ``0x40``, ``0x48``
     - CPU present A, B
     - Set by hardware when the interface sees the processor's first
       transaction (lines 811-813).
   * - ``0x50``, ``0x58``
     - CPU enable A, B
     - While clear the interface never grants the processor the bus
       (lines 883-887).
   * - ``0x70``, ``0x78``
     - NMI A, B
     - A write issues a non-maskable interrupt (lines 961-994). The exact
       trigger predicate is in no public source (unknown).
   * - ``0x90`` to ``0xc0``
     - interrupt pending modify, pending banks, masks
     - Write ``0x100 | level`` to set and ``level`` to clear (OpenBSD
       ``hub.h`` lines 98-100; ``sn1/intr.h`` lines 122-143); two pending
       banks at ``0x98``, ``0xa0`` and four mask registers
       ``0xa8`` to ``0xc0``.
   * - ``0x30100``
     - real-time counter
     - The counter the processors read; its frequency comes from
       ``ip27config.h`` and the local block's clock-control registers
       decide what it counts (unknown: no public source records what
       firmware programs there).

.. _sgi-bedrock-pi-presence:

Presence of the second processors
---------------------------------

On an observed two-processor Origin 300 board, PI0/A and PI1/A are fitted
and PI0/B and PI1/B are empty: an owner's inventory of an IP45_2CPU system
reports processors in slices A and C, with PROM 6.210 and IRIX 6.5.29m
(established, ``web:nekonomicon-16726611``). That capture does not
establish the placement on every board revision or any register's reset
value. Four-processor products populate both slices of both interfaces
(established, 108-0240-002 printed page 2-12 and the model's two-slot
interface naming from ``sn1/arch.h`` lines 79-85). The model derives the
present bits from the configured population.

.. _sgi-bedrock-lb:

Local block
===========

The local block services processor register accesses to the network,
crossbar and memory-directory units, holds its own registers, generates the
real-time clock signal the interfaces count, replies to programmed I/O,
initiates vector operations across the fabric during initialisation and
services local invalidates (108-0240-002 printed page 2-17). The Origin 200's
Hub has no such unit. The header defines 34 registers (``hublb.h``).
Three are on the boot path:

- ``0x600000``, revision and identity: bit 0 reserved, 11:1 manufacturer,
  27:12 part number, 31:28 revision, 39:32 node number, 47:46 region
  size (``hublb.h``, ``hublb_next.h`` lines 19-32); the Itanium port reads
  the node number as a fence before each posted-write flush
  (``pio_flush.h`` lines 43-54) and the region size as the mode test
  (``ip37.c`` lines 33-39). The stock IP35 PROM reads bits 39:32 as its
  own node number and writes the node number and region size with one
  64-bit store of the whole word (measured from the stock image,
  ``firmware:ip35-prom-6.210``).
  The silicon revision in bits 31:28 is unknown: IRIX inventories print
  "Bedrock revision 2" (Fuel) and "HUB revision 2" (a revision G Fuel
  board) as software labels, not register reads
  (``web:sgi-sh-fuel-repair-attempt``, ``web:tingo-fuel-irix-console`` lines
  132-145).
- scratch register 2 at ``0x608010``, the reboot request mailbox OpenBSD
  writes (:ref:`sgi-ip35-address-map-handoff`);
- scratch register 3's read-set-if-zero alias, which the Itanium port uses
  as a hub-wide lock around the L1 console.

The vector engine's route, status-clear and write behaviour and the node
number assignment policy come from the stock PROM's own accesses and have
no public source; the model's behaviour there is an emulator choice
(``firmware:ip35-prom-6.210``). The MicroLAN controller register follows
the XBridge specification, revision 0.5, printed pages 79-80
(``web:xbridge-asic-spec-0.5``); its behaviour on a Bedrock is inferred.

.. _sgi-bedrock-ni:

Network interface
=================

The network interface is the NUMAlink 3 port (108-0240-002 printed page
2-14). On a one-chassis Origin 300 it is cabled to nothing: the port status
reads no link and M mode, and it carries the node reset. Two chassis
joined by a NUMAlink 3 cable are a shipping configuration ("A single
system image (SSI) of 4, 6, or 8 processors is available by interconnecting two
SGI Origin 300 servers via a NUMAlink 3 cable", 007-4365-002 printed page
1; the rear LINK connector takes the cable at 1.6 GB/s each way, page 8;
the operator sets the slave's system id above the master's and cables
both LINK connectors with both servers off, page 38); established. No
public source holds the link protocol beyond the manual's summary (a
CRC-16 over 128 data bits, retry to a limit), so the register contract does
not model it. The header defines 142 registers, 128 of them a routing table
(``hubni.h``). Boot-path registers:

- ``0x680000``, port status: bits 1:0 port status, bit 2 remote power
  (``hubni.h`` lines 1015-1033); OpenBSD takes bit 18 as "more nodes than M
  mode" (:ref:`sgi-ip35-address-map-node`);
- ``0x680008``, reset: bit 0 link reset, bit 1 port reset, bit 2 local
  reset; ``0x680010``, reset enable bit 0 (:ref:`sgi-ip35-address-map-handoff`).

.. _sgi-bedrock-io:

I/O interface
=============

The I/O interface is the Bedrock's crosstalk port: it translates a device's
DMA request into the crossbar's format, forwards it to memory, and holds
the two block-transfer engines (``IIO_BASE_BTE0`` and ``IIO_BASE_BTE1``,
``hubio_next.h`` lines 78-79). The header
defines 135 registers (``hubio.h``). The inspected public MIPS consumers
use three: the seven translation entries (``0x400160`` to ``0x400190``,
:ref:`sgi-ip35-address-map-io`); the link control and status register at
``0x400128`` whose bit 13 is link up and which the out-of-tree MIPS port
tests before walking the crosstalk (``ip35-xtalk.c`` lines 159-163); and
the widget control register at ``0x400020`` whose low four bits that port
reads as the Bedrock's own crossbar port number (lines 165-179). Widget
control bit 17 set means a widget is directly connected with no crossbar
(Itanium ``ip37.c`` lines 24-31, ``io/io.c`` lines 783-796); whether an
IP35 PROM sets it is open. The model reports link up after reset and the
port number the machine is built with. The block-transfer engines are not
exercised by any inspected MIPS consumer; what the stock PROM, IRIX or
diagnostics do with them is unknown. The model's engine timing is an
approximation with no public source.

.. _sgi-bedrock-md:

Memory and directory interface
==============================

The memory and directory interface is the Bedrock's memory controller and
coherence directory, with issue, memory and directory blocks, page
migration, DDR SDRAM refresh and a built-in self test (108-0240-002
printed page 2-13, established). The header defines 45 registers
(``hubmd.h``). No public MIPS consumer touches any register of this unit on
IP35: OpenBSD's one MD access is the Origin 200 LED write (``ip27_machdep.c``
lines 426-432) and every consumer takes memory size from the configuration
records the PROM leaves in memory (OpenBSD ``sginode.c`` through
``mnode.h`` lines 362-365; linux-ip35 ``ip35-memory.c`` lines 148-166). The
unit's boot-path contract is therefore what the PROM programs, which no
public source records (unknown), plus the four error-clear registers the
Itanium port reads at probe. The model keeps the registers as storage with
the header's masks and idle replies (an approximation), models the
directory as one 64-bit word per 128-byte line at the directory backdoor
(:ref:`sgi-ip35-address-map-special`; zero initialised, a model policy), and
gives the current-cell register the access-rights role for the Hub special
windows that hold the L1 UART (inferred from the register's description).

.. _sgi-bedrock-xb:

Internal crossbar
=================

The crossbar is an 8-input, 6-output switch connecting the Bedrock's
units, with arbitration and some error handling (108-0240-002 printed page
2-11). The header defines 36 registers (``hubxb.h``). The Itanium consumer
writes 0 to all fifteen error-clear registers at probe (``huberror.c`` lines
95-100); OpenBSD defines only the base; the MIPS port has no matching
definitions. Register storage and software error injection through the
processor-interface-0 output are modelled; traffic-generated errors,
arbitration and structureless-register masks are unknown and not modelled.
This is the crossbar inside the Bedrock; the one at crosstalk widget 0 is
the XBridge's (:doc:`sgi-xbridge`).
