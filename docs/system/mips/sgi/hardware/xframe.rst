.. SPDX-License-Identifier: BSD-3-Clause

.. _sgi-xframe:

===========================================================
Neterion Xframe 10-gigabit Ethernet adapter (PCI and PCI-X)
===========================================================

The Xframe is Neterion's 10 Gb/s Ethernet controller, sold earlier under
the S2io name, on PCI and PCI-X add-in cards. SGI sold one such card as
its 10-Gigabit Ethernet Network Adapter. It is not SGI silicon, so QEMU
models the chip rather than the SGI board: the ``xframe`` device in
``hw/net/xframe.c`` (header ``include/hw/net/xframe.h``) is a PCI
endpoint that any machine with a PCI bus can carry. The model is
Xframe I, codename Xena, PCI device ID ``0x5831``. A property selects the
Xframe II device ID but not Xframe II behaviour. No machine fits the card
by default. It goes in a free slot with ``-device xframe``, for example
at PCI device 5 on the ``origin200`` machine, the slot the model wires
to the Bridge's 64-bit data path (:doc:`bridge`). Facts are rated as
described in :ref:`sgi-ratings`.

.. _sgi-xframe-sources:

Sources
=======

- *SGI 10-Gigabit Ethernet Network Adapter User's Guide*,
  :src:`007-4669-003`, an installer's guide. It names the silicon and the
  supported systems and has no register map.
- OpenBSD ``xge(4)``: ``sys/dev/pci/if_xgereg.h`` and ``if_xge.c`` at
  ``2df91d80c104``, as software expectations. The manual page lists the
  SGI card among the supported boards.
- Linux ``s2io``: ``drivers/net/ethernet/neterion/s2io-regs.h``,
  ``s2io.h`` and ``s2io.c`` at ``c9bace5bce3e`` in the Linux/MIPS tree,
  and the interrupt-mask rationale in the commit message and
  ``drivers/net/s2io.c`` of ``596c5c97431e`` in the Linux history tree,
  as software expectations.
- Register offsets and names throughout are the public driver names.
  Neterion's programming manual is not a source for this page, so a
  register's reset value, a bit the drivers never use and the behaviour
  of an unprogrammed block are *unknown* unless a statement says
  otherwise, and the values the model gives them are *approximations*.

Identity and placement
======================

The SGI manual's trademark page credits Neterion and Xframe, and its
feature list gives a single LC multimode-fibre port (850 nm, 10GBASE-SR,
full duplex), PCI 2.3 and PCI-X 1.0a compliance with 64-bit operation at
66 MHz and PCI-X at 66, 100 and 133 MHz, universal 3.3 V and 5 V
signalling, jumbo frames to 9600 bytes, and 16 DMA channels, 8 for reads
and 8 for writes :src:`007-4669-003 Features`, *established*. TCP
large-send offload is a Release 1 feature, and TCP large-receive and UDP
large-send offload are Release 2 only :src:`007-4669-003 Features`,
*established*. The guide's supported-systems list, in its third
revision, names Altix, Silicon Graphics Prism, Origin 350, Onyx 4/350,
Origin 3000 with an IX or PX brick, Onyx 3000 and Tezro. It does not
list the Origin 200 or the Origin 2000 :src:`007-4669-003 Supported
systems`, *established*.

.. list-table::
   :header-rows: 1

   * - Field
     - Model value
     - Basis
   * - Vendor ID
     - ``0x17d5``
     - OpenBSD ``pcidevs`` and Linux ``s2io.h``, *software expectation*.
   * - Device ID
     - ``0x5831`` (Xframe I, Xena). Property ``device-id``.
     - Same sources.
   * - Device ID, Xframe II (Herc)
     - ``0x5832``, by setting ``device-id``
     - OpenBSD and Linux, *software expectation*.
   * - Subsystem vendor ID
     - ``0`` by default. ``subsystem-vendor-id=0x10a9`` gives the SGI
       board.
     - ``0x10a9`` is SGI's PCI vendor ID, *inferred* as the board's
       subsystem vendor.
   * - Subsystem ID
     - ``0``. Property ``subsystem-id``.
     - *Unknown*. No source states the SGI card's subsystem device ID.
   * - Class, revision
     - Ethernet controller ``0x0200``, revision ``0``
     - *Approximation*. The hardware listing above shows revision 3.
   * - BAR 0 (config ``0x10``)
     - 64-bit memory, 16 KiB, the PIF register window
     - Both drivers map it as memory. The size is an *approximation*:
       the smallest power of two that holds the register banks the
       model decodes.
   * - BAR 2 (config ``0x18``)
     - 32-bit memory, 1 MiB, the TxP transmit window
     - Eight 128 KiB FIFO windows, *approximation* for the size.
   * - Interrupt
     - INTA, level
     - Both drivers use INTx by default, *software expectation*. The
       model has no MSI or MSI-X.

That the SGI card's second-release board is an Xframe II is *inferred*
from the family. The guide prints no PCI IDs, and the release's device
ID is *unknown*. Both public drivers bind on vendor and device ID alone,
with no platform check, so the generic device binds OpenBSD ``xge`` and
Linux ``s2io`` without any SGI property set, *software expectation*.

The type declares the card a 64-bit PCI agent. In a slot wired to the
64-bit data path it runs 64-bit data phases, and the Bridge places its
bytes by that width (:doc:`bridge`).

Register map
============

Every register is 64 bits wide in a window declared little-endian, and
the model serves 4-byte and 8-byte accesses. A 4-byte access takes the
low half of a register at the register's offset and the high half four
bytes above it. Linux writes the key-locked MAC configuration register
on the Xframe I as two 32-bit writes, *software expectation*. The IRIX
driver reads the byte-swapper register with 32-bit cycles, *software
expectation*, observed on the model. Behind the Bridge a slot that
leaves the 64-bit-device bit clear delivers every register access as
32-bit cycles, and a window that served only 8-byte accesses returned
zero for the swapper constant and aborted the IRIX attach. A 1-byte or
2-byte access is outside the window's valid sizes and the memory system
refuses it.

A write composes the accessed half onto the register's current value. A
side effect that depends on the whole value fires only when the write
reaches the high half, where every trigger sits, so two 32-bit halves
trigger once and a write to the low half alone triggers nothing. The
triggers are the SW_RESET key, the RMAC_CFG_KEY unlock, the
address-table and traffic-memory command strobes, the scheduled
interrupt and statistics enables, and the transmit doorbell.

The reset column gives the model's value at cold reset and after the
SW_RESET key. Reset values are *approximations*, except where noted.
Bit positions count from the low end, so the public drivers' ``s2BIT(n)``
is bit ``63 - n``.

.. list-table::
   :header-rows: 1
   :widths: 14 22 18 46

   * - Offset
     - Name
     - Reset
     - Behaviour
   * - ``0x0000``
     - GENERAL_INT_STATUS
     - ``0``
     - Derived, never stored. Bit 23 receive traffic, 55 transmit
       traffic, 29 receive MAC, 63 PIC block. A bit shows when its
       source is pending and unmasked at every level below GENERAL.
       The GENERAL mask does not hide it. A write is accepted and
       ignored.
   * - ``0x0008``
     - GENERAL_INT_MASK
     - all ones
     - Active high. Zero enables a source. See :ref:`sgi-xframe-irq`.
   * - ``0x0100``
     - SW_RESET
     - ``0``
     - A write whose bits 63:40 are ``0xa5a5a5`` resets the device. Any
       other value is stored.
   * - ``0x0108``
     - ADAPTER_STATUS
     - ``0xf400ffc300000000``
     - Constant. Presents the quiescent and ready bits, with both RMAC
       fault bits clear. A write logs ``LOG_GUEST_ERROR``.
   * - ``0x0110``
     - ADAPTER_CONTROL
     - ``0``
     - Stored. Bit 56 is ADAPTER_EN. Its edges start and stop the link
       and interrupt timers.
   * - ``0x0800``
     - PIC_INT_STATUS
     - ``0``
     - Derived. Bit 59 is PIC_INT_GPIO, bit 63 PIC_INT_TX.
   * - ``0x0808``
     - PIC_INT_MASK
     - all ones
     - Bit 59 gates the GPIO link source, bit 63 the scheduled one.
   * - ``0x0810``
     - TXPIC_INT_REG
     - ``0``
     - Reads bit 59 while the scheduled interrupt is latched.
   * - ``0x0818``
     - TXPIC_INT_MASK
     - all ones
     - Bit 59 gates the scheduled interrupt.
   * - ``0x0830``
     - RXPIC_INT_MASK
     - all ones
     - Stored only.
   * - ``0x0890``
     - GPIO_INT_REG
     - ``0``
     - Link events: bit 62 down, bit 61 up.
   * - ``0x0898``
     - GPIO_INT_MASK
     - all ones
     - Gates the same two bits.
   * - ``0x08e0``
     - TX_TRAFFIC_INT
     - ``0``
     - Transmit completion, FIFO *n* at bit ``63 - n``. Write one to
       clear.
   * - ``0x08e8``
     - TX_TRAFFIC_MASK
     - all ones
     - Active high.
   * - ``0x08f0``
     - RX_TRAFFIC_INT
     - ``0``
     - Receive completion, ring 0 at bit 63. Write one to clear.
   * - ``0x08f8``
     - RX_TRAFFIC_MASK
     - all ones
     - Active high.
   * - ``0x0908``
     - SWAPPER_CTRL
     - ``0``
     - Stored. Several bits steer DMA byte order, see
       :ref:`sgi-xframe-dma`.
   * - ``0x0910``
     - PIF_RD_SWAPPER_FB
     - ``0x0123456789abcdef``
     - Constant, whatever SWAPPER_CTRL holds. A write logs
       ``LOG_GUEST_ERROR``.
   * - ``0x0918``
     - SCHED_INT_CTRL
     - ``0``
     - Bit 63 enables the scheduled interrupt, bit 62 makes it fire
       once.
   * - ``0x0960``
     - XMSI_ADDRESS
     - ``0``
     - Stored. Both drivers use it as a scratch to check byte order.
   * - ``0x09d0``
     - STAT_CFG
     - ``0``
     - Bit 63 enable, bit 62 one-shot, bits 31:0 the update period.
   * - ``0x09d8``
     - STAT_ADDR
     - ``0``
     - Bus address of the statistics block.
   * - ``0x1150``, ``0x1158``, ``0x1160``
     - TTI_COMMAND_MEM, TTI_DATA1_MEM, TTI_DATA2_MEM
     - ``0``
     - Transmit traffic-interrupt memory, see
       :ref:`sgi-xframe-coalescing`.
   * - ``0x19b8``, ``0x19c0``, ``0x19c8``
     - RTI_COMMAND_MEM, RTI_DATA1_MEM, RTI_DATA2_MEM
     - ``0``
     - Receive traffic-interrupt memory.
   * - ``0x1930``
     - PRC_RXD0_0
     - ``0``
     - Base address of receive ring 0.
   * - ``0x1970``
     - PRC_CTRL_0
     - ``0``
     - Bit 56 puts the ring in service, bits 49:48 select the
       descriptor mode.
   * - ``0x2008``
     - MAC_INT_MASK
     - all ones
     - Bit 62 gates the receive-MAC roll-up.
   * - ``0x2028``
     - MAC_RMAC_ERR_REG
     - ``0``
     - Bit 32 is the receive-MAC link-state-change latch.
   * - ``0x2030``
     - MAC_RMAC_ERR_MASK
     - all ones
     - Bit 32 gates the latch.
   * - ``0x2100``
     - MAC_CFG
     - ``0``
     - Key-locked. Bit 56 promiscuous, bit 54 accept broadcast. Other
       bits are stored and have no effect.
   * - ``0x2110``
     - RMAC_MAX_PYLD_LEN
     - ``1500`` in bits 61:48
     - Stored only. The receive path does not read it.
   * - ``0x2120``
     - RMAC_CFG_KEY
     - reads ``0``
     - Write only. A write whose bits 63:48 are ``0x4c0d`` unlocks the
       next MAC_CFG write.
   * - ``0x2128``, ``0x2130``, ``0x2138``
     - RMAC_ADDR_CMD_MEM, RMAC_ADDR_DATA0_MEM, RMAC_ADDR_DATA1_MEM
     - ``0``
     - The 64-entry address table, below.

An offset not in the table logs ``LOG_UNIMP`` on a read, which returns
zero, and on a write, which is dropped. The TxP window is described under
:ref:`sgi-xframe-tx`.

MAC configuration and the address table
---------------------------------------

A MAC_CFG write takes only after RMAC_CFG_KEY has been written with
``0x4c0d`` in bits 63:48, *software expectation* from both drivers. The
key unlocks one register write however many bus cycles it takes: the lock
re-arms when a MAC_CFG write reaches the high half. A write without the
key logs ``LOG_GUEST_ERROR`` and is dropped.

The address table has 64 entries. RMAC_ADDR_CMD_MEM carries the entry in
bits 37:32, a write-enable in bit 56 and a strobe in bit 48. The strobe
completes at once and reads back clear. A write strobe copies DATA0 into
the entry's address and DATA1 into its mask. A read strobe returns the
entry in DATA0 and leaves DATA1 alone, which both drivers treat as
write-only. An address is in bits 63:16, first byte in bits 63:56. Entry 0
holds the netdev's MAC address at reset, which is what both drivers read
at attach as the factory address, *software expectation*. The model
reloads it into entry 0 on every reset and
clears the other entries.

A frame is accepted when MAC_CFG promiscuous is set. A broadcast frame is
accepted only with MAC_CFG bit 54. Any other frame must match an entry on
every bit whose mask bit is clear. A clear mask bit means compare, a set
bit means ignore. Linux ``s2io`` relies on this polarity: it programs a
joined multicast group with mask zero and all-multicast with address
``01:02:03:04:05:06`` and mask ``0xfeffffffffff``, which then compares only
the group bit, *software expectation*. OpenBSD ``xge`` writes the
opposite polarity, all ones for a group join, so against this model it
receives every frame, which its stack filters in software. The model does
not special-case OpenBSD, *approximation*. Which polarity the silicon
implements is *unknown*. A manual's table description would settle it.

A frame the filter refuses is dropped and reported to the network backend
as consumed, so it never blocks the backend's queue.

.. _sgi-xframe-irq:

Interrupts
==========

The model drives one level-sensitive line, INTA. The line is high when
any of four sources is pending and every mask above it is open.

.. list-table::
   :header-rows: 1
   :widths: 18 26 26 30

   * - Source
     - Pending bit
     - Masks that must be clear
     - GENERAL bit
   * - Receive completion
     - RX_TRAFFIC_INT, ring 0
     - RX_TRAFFIC_MASK bit
     - 23
   * - Transmit completion
     - TX_TRAFFIC_INT, FIFO *n*
     - TX_TRAFFIC_MASK bit
     - 55
   * - Receive-MAC link change
     - MAC_RMAC_ERR_REG bit 32
     - MAC_RMAC_ERR_MASK bit 32 and MAC_INT_MASK bit 62
     - 29
   * - GPIO link event
     - GPIO_INT_REG bits 61, 62
     - GPIO_INT_MASK bit and PIC_INT_MASK bit 59
     - 63
   * - Scheduled interrupt
     - TXPIC_INT_REG bit 59
     - TXPIC_INT_MASK bit 59 and PIC_INT_MASK bit 63
     - 63

Masks are active high at every level: zero enables, one masks. The
public drivers establish this for GENERAL_INT_MASK and the GPIO masks
(Linux ``s2io.c`` at ``c9bace5bce3e``, and OpenBSD ``if_xge.c`` writing
``GENERAL_INT_MASK = 0`` after opening its lower masks),
*software expectation*. Writing all ones to GENERAL_INT_MASK lowers INTA
without clearing a pending source, and restoring the mask raises it
again. That rule comes from the Linux commit at ``596c5c97431e``,
*software expectation*. Whether the real GENERAL_INT_STATUS hides a
summary bit while its mask bit is set is *unknown*. The model shows the
bit whatever the GENERAL mask holds. It is also *unknown* what a masked
GPIO event does on silicon. The model keeps it in GPIO_INT_REG and
delivers it when the mask bit clears, an *approximation* that neither
public driver depends on.

Every mask register is written whole or in halves, and every write
recomputes the line. The two orders seen in the drivers both work:
Linux writes the low half then the high half, OpenBSD's 32-bit big-endian
path the reverse.

Link events
-----------

The model has no PHY, so it produces link events from the network
backend and from ADAPTER_EN.

- A rising ADAPTER_EN edge with the backend link up starts a 5 ms
  virtual-time timer. When it fires, if the backend link is still up, the
  model sets GPIO LINK_UP, clears LINK_DOWN and latches the receive-MAC
  link-state-change bit. Writing ADAPTER_EN again while it is set does
  nothing. The delay is an *approximation*: no source gives the hardware
  delay between enabling the laser and the first link event.
- It never raises a LINK_DOWN from an ADAPTER_EN edge. Linux starts its
  software link state at zero, so a first LINK_UP counts as a change, and
  on Xframe II it unmasks only LINK_UP and sets its adapter-enable bit
  from that event's handler (Linux ``s2io.c`` at ``c9bace5bce3e``),
  *software expectation*. An enable edge is therefore not a required
  cause of an earlier DOWN.
- A backend link-down, such as QMP ``set_link off``, raises LINK_DOWN
  and the receive-MAC latch at once, whether or not the adapter is
  enabled, and cancels a pending UP. A later link-up with the adapter
  enabled starts the same 5 ms timer.
- An ADAPTER_EN falling edge cancels the link timer and the scheduled
  interrupt and clears any link event not yet acknowledged.

GPIO_INT_REG clears when a read covers its high half. A write clears the
bits it carries. The receive-MAC latch clears by write one. When the
latch is masked, so it cannot reach the line, a read covering the high
half also clears it. The IRIX driver masks the MAC path, takes its link
interrupt through the GPIO path and still reads MAC_RMAC_ERR_REG during
periodic service. Without the read-clear it would see the settled link
change on every pass and flap carrier. This is a *software expectation*
observed on the model. Whether silicon clears either latch on a read is
*unknown*.

IRIX writes GPIO_INT_MASK as all ones or with only bit 63 clear and never
clears the LINK_UP or LINK_DOWN bits, yet its interface comes up, *software
expectation*, observed on the model. Which source carries the link
indication to IRIX is not identified.

Scheduled interrupt
-------------------

The IRIX driver resets the adapter and prints that no interrupts are
occurring after about 15 s with no interrupt of any kind, *software
expectation*, observed on the model. A link-up event alone does not
prevent it. The IRIX driver prevents it by programming SCHED_INT_CTRL and
opening PIC_INT_MASK and TXPIC_INT_MASK, which makes the adapter raise a
periodic interrupt on the TXPIC path with no traffic and no link change
(register traces on the model).

The model implements that source. A SCHED_INT_CTRL write that reaches
the high half with bit 63 set arms a virtual-time timer. Each expiry
latches TXPIC_INT_REG bit 59 and raises INTA if the masks are open, then
re-arms unless bit 62 asked for one shot. The period field is ignored and
the period is fixed at two seconds, an *approximation*. The public header
marks the scheduled-interrupt period and routing as to be defined, so the
latch bit is also a model choice. The timer is cancelled by a write that
clears bit 63, by a reset and by an ADAPTER_EN falling edge. It never
touches GPIO_INT_REG or the MAC latch, so servicing it does not change
carrier.

A read covering the high half of TXPIC_INT_REG clears the latch, which
is how Linux and OpenBSD acknowledge a polled leaf. A write reaching the
high half also clears it. IRIX acknowledges by writing: it dispatches
from GENERAL_INT_STATUS and writes TXPIC_INT_REG without reading it. With
only the read-clear in place, the latch stayed set, the interrupt handler
re-entered about 2000 times at once and ``ifconfig`` hung. This is a
*software expectation*, measured on the model. The model does not require
the write to carry the latch bit, since the scheduled interrupt is the
only source it raises on this leaf.

.. _sgi-xframe-coalescing:

Traffic-interrupt memories
--------------------------

The public drivers program a per-entry timer, frame counts and utilisation
thresholds through two indirect command interfaces. The model keeps eight
entries of each. TTI entry *n* belongs to transmit FIFO *n*. RTI entry 0
belongs to receive ring 0. Commands use a strobe in bit 48, a write-enable
in bit 56 and an entry in bits 37:32 for TTI or 34:32 for RTI. A write
strobe copies DATA1 and DATA2 into the entry and marks it valid. A read
strobe does nothing visible. A TTI entry of 8 or more logs
``LOG_GUEST_ERROR`` and is dropped.

DATA1 holds a timer value in bits 57:32 (TTI) or 60:32 (RTI), a
timer-auto-cancel flag in bit 25 and a continuous-interrupt flag in bit
24. DATA2 bits 63:48 hold the first frame count. The other three counts
and the utilisation thresholds are stored and ignored. The timer unit is
64 periods of a 133 MHz clock, about 481 ns, an *approximation*.

- By default, and for any entry that is invalid, has a zero timer, lacks
  bit 25 or has a count of 1 or less, a completion sets its status bit
  at once.
- Otherwise the model holds completions. It publishes when the count is
  reached or when the timer, started by the first held completion,
  expires. This follows the public drivers' reading of the count as a
  ceiling that forces an interrupt, not a floor, *software expectation*.
  The driver picks a count by measured link utilisation, and the model
  always uses the first one. That is right for the low utilisation of
  this machine, where the first count is 1, and wrong for a busy link,
  *approximation*.
- A status bit already set when the next completion publishes stays
  deferred and is set when the guest clears the first.
- With bit 24 set, and the entry valid, its timer nonzero and ADAPTER_EN
  set, the model sets the status bit every interval with no traffic, as
  Linux's continuous interrupt does. A missed interval publishes at most
  twice. An ADAPTER_EN edge restarts or cancels the phase.

.. _sgi-xframe-tx:

Transmit
========

BAR 2 holds eight FIFO windows 128 KiB apart. The model decodes the
FIFO from address bits 19:17 and the field from bit 3 alone, so each
FIFO's window repeats the same pair every 16 bytes, an *approximation*.
Offset 0 of a pair is the descriptor-list pointer, latched per 32-bit
half. Offset 8 is the list control. A write that reaches its high half
rings the doorbell. Bits 63:56 hold the descriptor count less one, so a
list has 1 to 256 descriptors. The window is write-only for the drivers.
A read logs ``LOG_GUEST_ERROR`` and returns zero. The doorbell does not
check ADAPTER_EN or any MAC enable bit.

A descriptor is 32 bytes. Word 0 is the first control word, word 1 the
second, word 2 the buffer address and word 3 is not read. Word 0 holds
the owner bit (56), the end-of-frame bit (40) and the buffer length in
bits 15:0. The OpenBSD and Linux drivers use these fields and bits,
*software expectation*. The first descriptor
must be card-owned, or the doorbell logs ``LOG_GUEST_ERROR`` and is
ignored. The model gathers buffers until the end-of-frame bit or the
count, capping the frame at 9600 bytes, and truncates with
``LOG_GUEST_ERROR`` past the cap. Only the first descriptor's control
words are read for offload requests. A failed descriptor or buffer read
abandons the list with ``LOG_GUEST_ERROR`` and leaves it card-owned.

After the frame is sent, the model clears the owner bit of every walked
descriptor, the first last, then sets the FIFO's TX_TRAFFIC_INT bit (or
holds it, see above) and recomputes INTA. It writes no status or error
field. A transmit completion therefore never reports a failure,
*approximation*. A reply the backend returns during the send is held
until the completion has been published, so the guest never sees a reply
before the completion that caused it.

Checksum insertion
------------------

The second control word asks for checksum insertion: bit 58 for IPv4,
57 for TCP, 56 for UDP (Linux ``TXD_TX_CKO_*``), *software expectation*.
The model computes the requested checksums over the gathered frame
before it sends it. A frame with no request goes out byte for byte. The
frame must be a well-formed IPv4 packet: Ethertype ``0x0800``, version 4,
header length of at least 20, total length at least the header length and
within the gathered bytes. Otherwise the model logs ``LOG_GUEST_ERROR``
and sends the frame unchanged. The IRIX driver asks for insertion on
every TCP and UDP segment. A model that ignores the request leaves the
IRIX guest in SYN_RCVD, because its software computes only ICMP and ARP
checksums, a *software expectation* seen on the model.

Segmentation offload
--------------------

Bit 33 of the first control word asks the card to cut a large TCP send
into segments, with the segment size in bits 29:16 (Linux
``TXD_TCP_LSO_EN`` and ``TXD_TCP_LSO_MSS``), *software expectation*. The
IRIX interface carries the L2TCPSEG flag, so IRIX uses it on bulk
transfers and hands over buffers of about 32 KB. The gathered buffer is
then one datagram, not a frame, and the model caps it at 65549 bytes,
the largest IPv4 datagram plus an Ethernet header, instead of 9600.

For an IPv4 TCP buffer larger than one segment, the model either passes
the buffer to a backend that accepts a virtio-net header for host
segmentation, or cuts it itself. Its own cut gives each segment a copy of
the headers, an IP total length for that segment, an IP identification
advanced by the segment index, a sequence number advanced by the payload
already sent, and recomputed IP and TCP checksums, whatever the checksum
bits say. FIN and PSH stay only on the last segment. A buffer that is not
IPv4 over TCP, has a zero segment size, or holds no more than one
segment's payload goes out unchanged. How the silicon rewrites headers
per segment is *unknown*. The page's rule is the standard reading of the
field, and the evidence is that IRIX sets the bit and the segment size
and that its bulk transfers complete only when segments are cut this way.

Receive
=======

The model serves one receive ring. Both public drivers use ring 0 only,
and the other seven are not decoded, *software expectation*. Writing
PRC_RXD0_0 sets the base of the ring and restarts the card's walk there.
A write to PRC_CTRL_0 that sets bit 56 puts the ring in service and also
restarts the walk. Bits 49:48 select the descriptor mode. Mode 0 is one
buffer and mode 2 is five buffers, the two modes the drivers and IRIX
program. Modes 1 and 3 log ``LOG_UNIMP`` and the model then uses the
one-buffer layout.

Descriptors sit in 4 KiB pages chained by a pointer in each page's last
eight bytes, 127 one-buffer descriptors or 63 five-buffer descriptors to
a page (OpenBSD ``if_xgereg.h``), *software expectation*. A one-buffer
descriptor is 32 bytes. Word 0 is the driver's own, word 1 holds the
owner bit (56) and the transfer code, word 2 holds the posted buffer size
in bits 63:48 and word 3 the buffer address. The model reports no buffer
when the head descriptor is not card-owned, which holds the frame in the
backend's queue. A 5 ms virtual-time poll retries while a frame is held,
because both drivers refill a live ring by setting the owner bit in
guest memory with no register write, and both acknowledge RX_TRAFFIC_INT
before they refill. The poll is a correctness backstop. Its period is a
model choice.

A frame shorter than 60 bytes is padded. A frame longer than 9600 bytes
is truncated with ``LOG_GUEST_ERROR``. A frame longer than the posted
buffer is truncated and completes with transfer code 7. A buffer write
failure completes with code 2 and the frame is dropped. The completion
writes the received length into word 2 in place of the posted size. Word
1 is written last, after the arrival deadline, with the transfer code and
classification and the owner bit clear, which is the driver's only
signal that the frame is there.

Classification
--------------

For an IPv4 frame with a valid header, word 1 carries bit 36, the IP
header checksum verdict in bits 31:16, and, for a TCP or UDP frame whose
transport header fits, bit 33 or 32 and a transport checksum verdict in
bits 15:0 (Linux ``RXD_FRAME_PROTO_*`` and ``RXD_GET_L3_CKSUM``),
*software expectation*. A verdict is ``0xffff`` only if the checksum
actually verified over the bytes received. A fragment sets bit 34 and
gets no transport bit or verdict. A UDP checksum of zero means none was
sent and gets no verdict. Any frame the model does not parse as IPv4,
including a VLAN-tagged one, reports nothing. How the silicon treats a
first fragment, which does hold a transport header, and a tagged frame is
*unknown*. The model treats every fragment alike and does not parse tags.

Five-buffer mode
----------------

IRIX arms five-buffer descriptors when it raises the MTU to a jumbo
value, ring mode 2, which the public Linux header names ``RING_MODE_5``.
A descriptor is 64 bytes. Word 2 carries three posted sizes: buffer 0 in
bits 55:48 (8 bits), buffer 1 in bits 47:32 and buffer 2 in bits 31:16.
Buffers 0 to 2 have their addresses in words 3 to 5. The IRIX driver
posts about 14, 128 and a jumbo payload, followed by two 4-byte buffers,
*software expectation*, observed on the model. The model splits a
classified frame by protocol layer, a layout that the IRIX driver
accepts and that other layouts broke:

- The 14-byte Ethernet header goes to buffer 0.
- The IP header, with its options, and any recognised TCP or UDP header
  go to buffer 1. The driver reads the transport headers there, so a
  buffer 1 left empty strands them.
- The payload goes to buffer 2.

Placing the IP header of every IPv4 frame in buffer 1, ICMP and
fragments included, is *inferred* from the layer-split design and from
IRIX dropping every echo reply when buffer 1 was left empty. No source
states it. The public Linux driver puts a layer-3 header and a layer-4
header in separate buffers and has no layer-2 buffer, which differs from
what IRIX posts, so the model does not follow it. The offload status is
the same in this mode as in one-buffer mode.

A frame that is not a recognised IPv4 frame goes to buffer 0 and buffer
2 only, with buffer 1 empty and no offload status. A parsed frame
whose headers do not fit buffer 1 is delivered the same way with
``LOG_GUEST_ERROR``, and what silicon does there is *unknown*. A frame
that does not fit buffer 2 is truncated with code 7 and
``LOG_GUEST_ERROR``. The two overflow buffers are never filled. On
completion the model writes their delivered byte counts, zero, into the
low 32 bits of word 0 and keeps the upper half, which holds the
driver's own value. Left stale, the driver adds the posted sizes of those
buffers to the frame length and rejects the frame as an input error. This
rule holds only in five-buffer mode: other drivers keep a pointer in word
0. It comes from the descriptor layout and a measured failure and is
*inferred*.

.. _sgi-xframe-dma:

DMA and byte order
==================

The model does all its bus-master access through the card's own PCI
address space: descriptors, buffers, the statistics block and completion
writes. It does not ask the host bridge for a route, so the Bridge alone
decides how bytes cross it (:doc:`bridge`).

Descriptor control words are read and written big-endian while the
path's swap-enable bit in SWAPPER_CTRL is clear, and little-endian once
it is set. TxD uses bit 44 and RxD bit 30. SWAPPER_CTRL resets to zero,
so the default is big-endian. A big-endian IRIX guest leaves these bits
clear, and a captured IRIX transmit descriptor decodes as a coherent
big-endian descriptor and as nonsense little-endian, *software
expectation*, observed on the model. Linux ``s2io_set_swapper`` sets the
swap-enable bits only on little-endian hosts, and its comment says the
device defaults to a format a big-endian driver need not change,
*software expectation*. Only the read-side bit of each pair is tested.
Both public drivers set the read and write bits together.

Frame payload goes over the bus as a byte stream unless the frame path's
swap-enable bit is set: bit 40 for frames the card reads and bit 26 for
frames it writes. When set, the model reverses the bytes of each aligned
64-bit word, with a transfer's lane taken from its bus address modulo 8,
and a write touches only the lanes it covers. The rule is an
*approximation*. The public drivers set the frame paths' flip-enable bits
on both host byte orders and their swap-enable bits on neither, and the
model does not implement the flip-enable bits. What a swap-enable bit
does to frame data at other alignments, and what a flip-enable bit does,
is *unknown*. The Xframe manual's description of the swapper would
settle it.

The IRIX driver writes SWAPPER_CTRL as ``0xffffeaff`` in the high half and
``0xabffbfff`` in the low half, so the value it intends is
``0xffffeaffabffbfff``, and reads the byte-swapper constant and the
XMSI_ADDRESS scratch back as ``0x0123456789abcdef``, *software
expectation*, observed on the model. Decoded with the OpenBSD bit names,
all the flip-enable bits are set, the PIF read and write, TxP and MSI
swap-enable bits are set, and the descriptor, frame and statistics
swap-enables are clear. On the same boot the Bridge's device register for
the slot read ``0x12091002``, bit 19 set, which puts the driver's DMA on a
byte-stream mapping that places bytes the same way at either data-phase
width. How the card itself places bytes under that value is *unknown* for
lack of a document.

Statistics block
================

STAT_ADDR names a bus address and STAT_CFG enables a periodic DMA of a
0x320-byte block of hardware counters to it. Without the block the IRIX
driver differences against memory that never advances, and its transmit
packet count runs backwards. The block's length and field offsets come
from compiling the public ``s2io`` ``stat_block`` structure, *software
expectation*.

The model fills four 64-bit words and leaves the rest of the block zero,
which is the truth for the error, pause and discard counters it never
raises. Each word holds frames in its high half and octets in its low
half.

.. list-table::
   :header-rows: 1

   * - Offset
     - High half
     - Low half
   * - ``0x000``
     - transmitted frames
     - transmitted octets
   * - ``0x060``
     - received frames
     - received octets
   * - ``0x218``
     - bits 63:32 of transmitted frames
     - bits 63:32 of transmitted octets
   * - ``0x260``
     - bits 63:32 of received frames
     - bits 63:32 of received octets

The counters are ``__le32`` pairs declared in the opposite order to the
driver's ethtool key table, which is what a little-endian declaration of
a word holding the first-listed counter in its high half looks like. The
model writes a pair as one 64-bit word and orders the whole word by the
statistics swap-enable, bit 14 of SWAPPER_CTRL: little-endian when set,
which is Linux, and big-endian when clear, which is IRIX. IRIX
takes its frame counts from the words at ``0x000`` and ``0x060``. Under an
older layout of two separate 32-bit fields, its ``netstat -i`` read the
transmit octet count as the output packet count, *software expectation*,
observed on the model. The half order of the overflow words at ``0x218`` and
``0x260`` has no evidence either way, since reaching them takes more than
4 GiB of counted traffic. Setting exactly one of bits 15 and 14 is not
modelled: the enable logs ``LOG_UNIMP`` and the block is written in
place.

Writing STAT_CFG with bit 63 set, through the high half, takes a snapshot
at once and arms a periodic DMA unless bit 62 asks for a single transfer.
A single transfer clears the enable bit when it finishes, which the Linux
one-shot path polls for. The period is the low 32 bits at 0x208d5 per
second, clamped to between 100 ms and 10 s, and a clamped value logs
``LOG_UNIMP``. The period is read when the enable is written, and a later
change to the low half takes effect one interval late. Transmit counters
advance for each frame, or each segment, put on the wire. Receive
counters advance for each frame delivered to the guest.

Reset
=====

A cold reset and a SW_RESET key write both take every register to the
values in the table above: all masks to all ones, the MAC table to zero
with entry 0 reloaded, the statistics and traffic-interrupt memories
cleared, the receive and transmit pointers cleared, RMAC_MAX_PYLD_LEN
to 1500, and any pending latch, held completion or arrival-line entry
dropped. Every timer is cancelled: link, scheduled interrupt,
statistics, receive recovery and traffic-interrupt. Reset does not touch
PCI configuration space or the BARs. On a real Xframe I, SW_RESET can
disturb configuration space, which is why OpenBSD restores its saved
config after the Xena reset, *software expectation*, and the PCI core
alone owns it here. OpenBSD runs that reset on every Xena attach, so the
model's reset has to survive the sequence of GPIO writes, reset, a 500 ms
wait and config restore, *software expectation*. DTX_CONTROL and
GPIO_CONTROL at ``0x09e8`` and ``0x09f8`` take part in that sequence and
are not modelled.

Receive arrival latency
=======================

The model has no wire, peer or bus time. Without a floor a reply's
arrival, completion and interrupt all land at the same virtual-clock
instant as the transmit doorbell that provoked it, which no real part can
do across a bus, a DMA engine and a coalescing timer. The model therefore
writes a received frame's payload and byte counts at once but releases
the descriptor's ownership, sets RX_TRAFFIC_INT and raises the line only
after a minimum delay on an arrival line shared by the four SGI Ethernet
adapters. The ``rx-arrival-latency-us`` property sets the delay, 200
microseconds by default. It is an *approximation*. Unlike the other
adapters, no register write here retries a completion the bus refused. A
stalled completion recovers only through the line's own backoff, which
doubles from the floor to a quarter of a second.

Migration
=========

The ``xframe`` VMState is version 9 and loads only at version 9.
It carries the PCI configuration, every register above, the whole address
table with its masks, the transmit list pointers, the traffic-interrupt
memories with their held counts and timers, the receive ring position,
the scheduled-interrupt state with its armed flag and pending timer, the
link timer and the arrival line. It leaves out the receive descriptor
cache, the receive recovery poll, the transmit scratch buffers and the
statistics timer.

After a load the model re-arms the scheduled interrupt when its armed
flag is set and no timer expiry was restored, so a one-shot that already
fired is not fired again. It re-arms the statistics timer when STAT_CFG is
enabled, and deasserts then recomputes INTA from the restored status and
masks. Migration works only between identical builds.

Measurements
============

No register of this part has been read on a physical machine. The only
observation of real hardware is the secondhand IRIX PCI listing in
:ref:`sgi-xframe-sources`, which gives the SGI card's vendor, device and
revision and nothing more. The register contract above rests on the
published driver sources.

Model limits
============

- Xframe II differs from Xframe I in the device ID property only. Its
  extra registers, enhanced statistics and offloads are absent.
- RMAC_MAX_PYLD_LEN is stored and never enforced. The receive limit is a
  fixed 9600 bytes.
- MAC_CFG bits other than promiscuous and accept-broadcast, ADAPTER_EN
  and the receive-MAC enables do not gate the datapath. Pause frames,
  padding and FCS control, and loopback do nothing.
- One receive ring, eight transmit FIFOs, eight traffic-interrupt entries
  of each kind. Modes 1 and 3 of the receive descriptor are
  unimplemented, and so are the two overflow buffers of mode 2.
- No MSI or MSI-X, no UDP large-send offload, no large receive, and no
  per-source error status. The model raises no error alarm and sets no
  transmit status.
- The XGXS, MDIO, I2C, GPIO, flash, RLDRAM and DTX blocks are absent.
  A read of any offset the register map does not name logs
  ``LOG_UNIMP`` and returns zero, and a write is dropped with the same
  log. The model never raises a bus error.
- Accesses to defined registers that are wrong log
  ``LOG_GUEST_ERROR``: a write to ADAPTER_STATUS or PIF_RD_SWAPPER_FB, a
  MAC_CFG write without the key, an invalid TTI entry, a read of the TxP
  window, a doorbell on a descriptor the card does not own, a transmit
  frame past its cap, and failed descriptor or buffer transfers.
- Reset values, the 5 ms link delay, the two-second scheduled period,
  the 100 ms to 10 s statistics clamp, the 481 ns traffic-interrupt
  tick and the 5 ms receive recovery poll are model choices.
- Raw GPIO latch contents at reset, whether a masked event is kept and the
  exact startup link-event timing are *unknown*. A hardware measurement
  that masks LINK_UP, cycles the link and then clears the mask bit would
  settle the retention question.
- Migration works only between identical builds.

Tests
=====

``tests/qtest/xframe-test.c`` runs the device behind a Bridge on a bare
test head with no machine. It covers PCI identity and the device-ID and
subsystem properties, the two BARs, the swapper constant and scratch
register, ADAPTER_STATUS and ADAPTER_EN, the SW_RESET key through 64-bit
and 32-bit writes, the MAC_CFG key lock across both halves, the address
table and factory-MAC read-back, the traffic-interrupt memories and their
moderation, continuous interrupts and migration, link-up delivery through
the MAC and the GPIO paths, the GENERAL, GPIO and per-source mask
contracts, the scheduled interrupt including the write acknowledgement
and carrier steadiness, transmit of a frame with byte-exact TCP and UDP
checksums, gather lists, segmentation of a send larger than a frame, the
host-offload path with a virtio-net header, completion interrupts and
their per-frame rate, receive delivery, the arrival deadline, filtering,
multicast matching, ring exhaustion and recovery, a buffer that is too
small, five-buffer header splitting for classified, truncated, fragmented
and jumbo frames, reported checksum verdicts, descriptor byte order with
SWAPPER_CTRL set, and the statistics block.

``tests/qtest/sn0-xframe-test.c`` runs the card on the ``origin200``
machine. It covers the swapper constant through the Bridge at 64-bit and
32-bit cycles, a matrix of the card's frame-path swap bits against the
Bridge's swap and width settings for transmit and receive, the payload
placement for transmit, receive and segmentation offload, the five-buffer
ICMP split, and two interrupt sources composing through the Bridge and
Hub.

``tests/functional/mips64/test_xframe_pci.py`` boots the stock OpenBSD
IP27 disk with the card at PCI device 5 and requires OpenBSD's PCI scan
to report it by vendor and product as unconfigured.
