# Datasheet: Bedrock network interface (NI)

References rendered as citation identities name material outside this document
set. Those targets are not included or cleared for redistribution. Review and
runtime statements retain their recorded dates and scope.

**Status:** reviewed 2026-09-03 by an independent second reader
(`docs/reviews/bedrock-pi-lb-ni-datasheet-review.md`): all 142 registers cleared position for position
against the SN1 header, with the review's five findings applied in this revision,
including its ruling on the reset-enable bit. The open items stay open.
The 2026-09-24 cabled-pair delta (link-up from a cable, the firmware's
power-on programming, writes to the error and sideband registers) is pending
independent evidence review. Isolated implementation and test evidence is
recorded in the integration notebook (citation: `docs/notebooks/origin300-integration.md`);
that evidence does not establish full-machine or diagnostic acceptance.
**Fork file(s):** `hw/misc/sgi-bedrock-ni.c` with the register core in
`hw/misc/sgi-bedrock-ni-regs.c`; the Bedrock parent owns physical decode and
node-reset wiring.
**Evidence cutoff:** 2026-09-03, Origami trunk `506836b9dd`; the cabled-pair
delta 2026-09-24, Origami trunk `e09edbef40`.

**Sources used:** the SGI-generated Linux SN1 register headers at tag `v2.5.40`
(commit `7570df54ef8c`): `include/asm-ia64/sn/sn1/hubni.h` (142 registers, 128 of
them a routing table), `hubni_next.h`; the Itanium consumers
`arch/ia64/sn/io/sn1/{ip37.c,huberror.c,hubcounters.c}`, `arch/ia64/sn/io/klgraph.c`.
OpenBSD at `2df91d80c1`: `sys/arch/sgi/xbow/hub.h`, `sys/arch/sgi/sgi/ip27_machdep.c`,
`sginode.c`. Linux mainline `arch/mips/include/asm/sn/sn0/hubni.h` for the SN0
comparison; the Origin 3000 System Architecture 108-0240-002 printed page 2-14 for
the unit's role. The out-of-tree MIPS port carries no network header and touches no
network register. Reading preserved in the SN1 research notebook. The
cabled-pair delta adds the stock IP35 PROM 6.210 (payload SHA-256
`554924380189a03a512156af2faa38344902f251b49fd5c80c253c23ecaff27b`) read by
disassembly, and one instrumented one-node run of it recording every
network-interface access with its PC (run `prom-20260924T052035-55abd1`, QEMU
`sgi-origami` `ca021b99c6`), preserved in the PROM trace notebook (citation: `docs/notebooks/origin300-numalink-prom-trace-20260924.md`); and the Origin
300 User's Guide 007-4365-002, printed pages 1, 8 and 38. PROM addresses are
listing addresses without the `0xc000000000000000` prefix.

**Facts that are IRIX-derived: none.** **Reset values the header states:** none;
the port sideband register is not cleared on reset, and a companion header names a
software constant it believes to be the port-parameters reset default, with no
consumer that uses it.

## Role

The network interface is the Bedrock's NUMAlink 3 port. On a one-chassis Origin 300 the
port is cabled to nothing: the unit reports no link, reports M mode, and carries the node
reset, the contract the Origin 200's Hub network interface already fills. Two chassis
joined by a NUMAlink 3 cable is a shipping configuration: "A single system image (SSI)
of 4, 6, or 8 processors is available by interconnecting two SGI Origin 300 servers via a
NUMAlink 3 cable" (007-4365-002 printed page 1); the rear LINK connector takes that cable
at 1.6 GB/s each way (printed page 8); the operator sets the slave's system ID above the
master's with the L1, cables both LINK connectors with both servers off, and powers on,
after which "the console output should indicate that the processors and memory on both
servers have been discovered" (printed page 38). For that pair this unit is the whole
fabric: one port, one cable, one peer. No public source holds the link protocol beyond
the manual's summary (CRC-16 over 128 data bits, retry to a limit), and the register
contract below does not model it.

Four of its registers are on the boot path: the port status a guest reads for the
memory mode and the link state, and the reset and reset-enable registers a guest
writes to reboot.

## Register map

Offsets are hub-relative; the unit starts at `0x680000`. Every register is 64 bits.

| Offset | Name | Fields | Reset | Access | Consumer | Behavior |
|---|---|---|---|---|---|---|
| `0x680000` | port status | 1:0 port status; 2 remote power; 63:3 reserved | unknown | read | OpenBSD reads it at setup and takes **bit 18** as "more nodes than M mode holds" (`sginode.c` lines 100 to 103; `hub.h` line 218), and real Origin 300s print that line, one chassis and two cabled (the OpenBSD capture in `docs/library/boot-logs.md`); the Itanium port reads bit 1 as link up and skips link statistics when it is clear (`hubcounters.c` lines 146 to 149, 213) | The link-level protocol status (lines 1006, 1026 to 1031). Bit 1 is link up (`hubni_next.h` lines 24 to 25), and it is the only bit the stock IP35 PROM tests (`func_1fc34b50` returns `(status >> 1) & 1`; `func_1fc343bc` returns `status & 3` for a printed table whose value-to-string mapping was not decoded). See "Link bring-up". **Bit 18 is reserved in this generation's register file**; it is the Origin 200's more-nodes bit, and the register the Itanium port consults instead is the local block's revision register's region-size field, bits 47:46, whose N-mode value is 2 (values 0 and 1 are both M-mode compatible). A one-node machine returns 0 in both, which every consumer accepts, as the model's chosen value. The M-mode register decision packet carries it. |
| `0x680008` | port reset | 0 link reset out; 1 port reset; 2 local reset; 63:3 reserved | unknown | write | OpenBSD writes local plus link reset (`0x05`) to every other node and then its own on halt (`ip27_machdep.c` lines 706 to 729); the Itanium port defines local plus port reset (`0x06`) (`ip37.c` lines 42 to 47) | Local reset resets this Bedrock; port reset sends a warm reset over the link; link reset resets the link (lines 1042 to 1069; `hubni_next.h` lines 28 to 30). The Origin 200 used bit 0 local, bit 1 link and bit 7 port; the meanings moved. |
| `0x680010` | reset enable | 0 reset ok; 63:1 reserved | unknown | R/W | both consumers write 1 immediately before the reset register; the stock PROM writes 1 at hub initialisation (pc `0x1fc67724`), reads it at discovery, and writes 0 after node-number assignment through the remote register form `0x9200000001800000 + (n << 33) + 0x680010` for each matching discovery-table entry's node number `n` (`func_1fc5ab38`, store at `0x1fc5acf0`); on one node that entry is the node itself, and on a pair the peer's entry would be written through the cross-node register window, which no datasheet yet specifies; its reboot routine `func_1fc642b8` writes 1 here and then `0x6` to port reset | "Warm reset enable": lets a reset arriving over the link reset the Bedrock (`hubni_next.h` line 143). Whether it also controls the local-reset bit is not stated; both consumers set it first regardless. The review's ruling is under behavioral notes. |
| `0x680018` | diagnostic parameters | 0 send data error; 1 port disable; 2 send error off | unknown | R/W | the stock PROM writes 0 at hub initialisation (pc `0x1fc67730`) | Error injection and port disable (lines 1134 to 1140). |
| `0x680020` | channel control | 3:0 and 7:4 virtual-channel request and reply selects; 8 send channel select | unknown | R/W | the stock PROM writes `0x166` at hub initialisation (pc `0x1fc67748`) | Virtual-channel selection for outgoing messages (lines 1151 to 1191). |
| `0x680028` | channel test | test seed, mask, data, valid, error and flit fields | unknown | R/W | none | Link test logic (lines 1202 to 1239). |
| `0x680030` | port parameters | 9:0 maximum burst; 15:10 null timeout; 25:16 maximum retry; 27:26 data-available select; 29 first-error enable; 30 squash-error enable; 34:31 virtual-channel error enables | unknown; the companion header believes the default is virtual-channel errors `0xf`, first error 1, retries `0x3ff`, null timeout 6, burst `0x3f0` (`hubni_next.h` lines 168 to 172) | R/W | the stock PROM writes `0xffffffffa3ff1990` at hub initialisation (pc `0x1fc6779c`, a sign-extended 32-bit constant): burst `0x190`, null timeout 6, retries `0x3ff`, data-available select 0, first error 1, squash error 0, virtual-channel error enables `0xf` (from the sign extension); bit 28 and bits 63:35 are undefined and are not stored | Link-level protocol parameters (lines 1250 to 1288). |
| `0x680038` | channel age | 7:0 request inject age; 15:8 reply inject age | unknown | R/W | the stock PROM writes `0xc800` at hub initialisation (pc `0x1fc677a8`): request age 0, reply age `0xc8` | The age at which requests and replies enter the fabric so replies win under saturation (lines 1299 to 1327). No public header describes any age- or vector-related reset behavior. |
| `0x680100` | port errors | 7:0 sequence-number errors; 15:8 check-bit errors; 23:16 retries; 27:24 tail timeout; 31:28 FIFO overflow; 35:32 to 51:48 four-bit counts of external and internal short, long and bad-header events; 52 link reset in | unknown | read, non-destructive; the stock PROM writes 0 at hub initialisation (pc `0x1fc677b4`), with no public statement of what a write does | the stock PROM's link test reads it after 32 vector reads over the link and calls the link bad when check-bit errors (15:8) reach 6 (`func_1fc5f798`) | Latched errors; bits 52:24 assert the unit's interrupt, pending 1 bit 62 in the processor interface, which cannot be cleared there; the three byte counters do not wrap (lines 1338 to 1384; `hubni_next.h` lines 40 to 67). |
| `0x680108`, `0x680110` | port header A, B | the captured header of an erroring packet, fields per `hubni_next.h` lines 91 to 136 | unknown | read | none | Continually overwritten; the Itanium port sees no need to clear it. |
| `0x680118` | port sideband | 7:0 sideband; 8 to 15 error flags; 19:16 active channels | not cleared on reset (line 1397) | read; the stock PROM writes 0 at hub initialisation (pc `0x1fc677c8`), with no public statement of what a write does | none | |
| `0x680120` | port error clear | as port errors | unknown | read-to-clear | the Itanium port reads it at I/O init and at hardware-graph build, and periodically to harvest counts (`huberror.c` line 89; `hubcounters.c` lines 146 to 219) | The same information as port errors; a read clears. |
| `0x681000` plus 8n, n 0 to 127 | local routing table | 3:0 next exit port; 4 next channel bit | unknown | R/W | the stock PROM writes entry 0 to 0 before discovery (`0x1fc60b5c`), briefly again around node-number assignment (`0x1fc611ec`, restoring the old value at `0x1fc611f0`), and all 128 entries from its discovery table afterwards (`func_1fc5cff0`, stores at `0x1fc5d044` and nearby); on one node every value is `0xf` | Next-hop lookup by the 7-bit local destination (lines 1506 to 1532). |
| `0x682000` | global routing table | as above | unknown | R/W | the stock PROM writes it from its discovery table with the local entries (store at `0x1fc5d074`) | Next-hop lookup by the 1-bit meta destination (lines 1580 to 1606). |

## Behavioral contract

A one-node machine: port status reads 0 (no link, and bit 18 clear so OpenBSD prints
"Machine is in M mode", which real Origin 300s do print, two chassis cabled as well as
one, in the OpenBSD capture in `docs/library/boot-logs.md`); on a real Origin 300 with nothing cabled the PROM prints
`Local hub NUMAlink is down`, `*** Local network link down`, `Port 1 connection: Not
connected` and `Port status: NF`, then `Found 1 objects (1 hubs, 0 routers)` and `No
other nodes present; becoming global master` (the Origin 300 verbose trace in
`docs/library/boot-logs.md`); the reset-enable register retains its write; a write to the
reset register with the local bit set resets the node. The model treats that reset
as the reset the local-block and processor-interface headers describe, so afterwards
the local block's scratch 0 reads 0 and scratch 1 and 2 keep their values, and the
processor interfaces' soft-reset survivors keep theirs; no header states that
equation, and the local-block datasheet names the observation that settles it. The
Origin 200 model's "no remote link" seam is the template; the register offsets and
bit meanings differ and are listed with the SN0 comparison below.

## Link bring-up

**Nothing is written to bring the link up [established, trace and
disassembly].** On the traced boot the complete set of network-interface
accesses is: one power-on programming pass in `func_1fc676c8` (reset enable 1,
diagnostic parameters 0, channel control `0x166`, port parameters, channel age
`0xc800`, zero writes to port errors and sideband, a read of port error clear),
then port-status reads (also once through the remote register form, at
`0x1fc4d748`) and one read of reset enable at discovery (`0x1fc27370`), then the
routing-table writes and the reset-enable clear after discovery. There is no write to port reset, and no second write to
any parameter register, before or during the wait. The firmware treats link-up
as something the hardware produces on its own once the port is programmed.

**What the firmware waits for [established].** Discovery (`func_1fc587a4`,
called from `func_1fc5f2b8`) reads port status bit 1 in a loop against a
deadline of 1,000,000 us taken from the processor interface's real-time
counter. The traced one-node run made 48,117 such reads, every one 0, then
printed `Local hub NUMAlink is down.`; the local-hub entry (`func_1fc272dc`)
reads the bit once more and prints `*** Local network link down` when it is
clear. With the bit set, the firmware runs a link test and discovery through the
local block's vector engine with route 0, which reaches the peer across this
port (see the [local block's vector engine](bedrock-local-block.md#vector-engine-on-the-boot-path)).

**Inferred:** a cabled peer must show link-up within one second of the poll
starting with no software step on either side, or the node boots alone; the
manual's "If the slave server has not been discovered, use the reset L1
command" (007-4365-002 printed page 38) fits a link that can miss that window.

**Unknown:** port-status values 0, 1 and 3 and the remote-power bit (no
firmware path tested them); the hardware reset value of every register; what
writes to port errors and sideband do; how long real link training takes; and
the peer-rendezvous sequence after discovery, which only a two-node run
reaches.

**Model contract for a cabled pair.** Port status bit 1 reads 1 while a
realized cable joins this port to a peer and the cable is not marked down, and
0 otherwise; it is computed on every read, so it is visible from reset and
follows the cable's `down` property at once. Bits 0 and 2 read 0 (model
choice: their meaning is unknown). A local reset does not change it. The
power-on programming values above are stored as written through each
register's defined bits. Writes to port errors and sideband store the written
value (so the firmware's zero writes clear them) without a guest error, a
model choice while the hardware effect is unknown; port error clear then reads
0. The routing tables are stored and not consulted: a direct pair's vector
route 0 never needs them.

## Interrupts

Port-error bits 52:24 assert pending 1 bit 62 in both processor interfaces. Nothing
on a one-node boot sets them.

## DMA

None.

## Remaining implementation and qualification

Routing, channel control, port parameters, ages and error injection remain
unqualified beyond implemented register behavior. The one-chassis machine has no
peer, so no peer or link traffic may be fabricated there. On the two-chassis
pair the cable carries the vector engine's requests and a port reset: bit 1 of
the port reset register resets the peer through its local-reset path when the
peer's reset enable is set (the port-reset paragraph under [behavioral
notes](#behavioral-notes)). Whether the stock PROM's reboot write of `0x6` then
resets the peer depends on the peer's enable, which the PROM clears after
discovery (the `0x680010` row). The link-level protocol and its error counting
are still not modelled, and a link reset (bit 0) delivers nothing to the peer.
That topology does not waive local port, reset, error or diagnostic behavior.
Qualification must distinguish a physically absent peer from an unimplemented
local function.

## Behavioral notes

**The M-mode read: the port status bit is observed clear on real Origin 300s; the
register the firmware consults is still unobserved.** OpenBSD's test reads a bit the
register file reserves; the Itanium port reads the local block. Real Origin 300s print
OpenBSD's "Machine is in M mode." with one chassis and with two cabled (the openbsd-
sgi list capture in `docs/library/boot-logs.md`), so a model returning 0 there returns
what the hardware returns for every Origin 300 configuration seen; the local block's
region-size field stays the model's choice. The traced stock IP35 PROM tests only bit 1 of `0x680000`
(and prints `status & 3`), never bit 18, and writes the local block's region size
itself when it assigns the node number (see the
[local block](bedrock-local-block.md#node-number-assignment)). What an N-mode
Bedrock returns stays unobserved, a mode no Origin 300 reaches. The M-mode register decision packet is closed on the same capture.

**The reset-enable bit permits link-originated resets only; a local reset does not
need it.** The review ruled against the stricter reading: the header calls the bit a
warm-reset enable and the companion says it lets the link-level protocol reset the
Bedrock (`hubni.h` line 1079; `hubni_next.h` line 143), no public sentence ties it to
the local-reset bit, and the reviewed Origin 200 Hub datasheet found the vendor PROM
writing the local-reset bit on its own, early, on every power-on, with Linux never
arming the enable. A model that required the enable would silently swallow such a
reset with no consumer evidence to justify it. So a write with bit 2 set resets the
node regardless of the enable, recorded as the model's choice. The stock PROM does
write `0x680010` before any write to `0x680008`: at hub initialisation, long before its
reboot routine writes the enable and then local plus port reset (`func_1fc642b8`). That
ordering is consistent with both readings, so the ruling stands.

**A port reset crosses the link to the peer (2026-09-25, independently
reviewed).** Bit 1 of the port reset register sends a warm reset over the link,
and the receiving Bedrock's reset enable decides whether it takes effect (the
two header sentences above). On a cabled pair the model delivers it: with the
cable up, the Bedrock at the other end resets through the same local-reset path
a local request takes, when its reset enable bit 0 is set; with no link, or the
enable clear, nothing happens. The sending node is not reset by bit 1. What the
link-level protocol exchanges, and whether the arriving reset differs from a
local one, is unknown; equating the two is the model's choice. Test:
`sn1/bedrock-link/port-reset-crosses-the-link`.

**A local reset restarts the processors [observed on an Origin 350 with
the same PROM, inferred for the Origin 300; the model restarts the whole
machine, an approximation].** No register description says what a local
reset does to the processors, but every IP35 consumer that reboots relies
on it bringing them back through the reset vector, and none jumps to the
firmware itself. A published Origin 350 console capture running the same
PROM build ("IP35 PROM SGI Version 6.210 built 02:33:51 PM Aug 26, 2004")
shows it: after a POD `reset` it prints "Resetting the system..." and goes
straight to "Starting PROM Boot process" and the banner (the Chimera notes
in `docs/library/boot-logs.md`). The stock IP35 PROM prints
"Resetting the system...", calls its reboot routine (`func_1fc642b8`:
reset enable 1, then `0x6` to port reset) and branches back into a loop
(`0x1fc38d80`). IRIX 6.5.30's restart path, disassembled from the running
installed kernel, writes its request word to local-block scratch 2 on every
node through the remote register form, then writes reset enable 1 and port
reset `0x6` and repeats those two writes every 3 s of the real-time
counter. OpenBSD writes local plus link reset (`ip27_machdep.c` lines 706
to 729). A model that reset only the Bedrock's registers left IRIX in that
loop for good, the reboot stall of jamesb/qemu#875. The model now pulses
the Bedrock's `node-reset` line after every local reset, and the Origin 300
board answers it with a machine reset, as the Origin 200's Hub local reset
and the L1's reset command already do. Limitation: the node's I/O, its L1
and, on a cabled pair, the other node restart with it; what else a local
reset reaches on hardware is unknown. IRIX and OpenBSD write every node,
so restarting them all matches what they ask for; the PROM's own reset
writes only its node, and the PROM clears the peer's reset enable after
discovery (above), so on a cabled pair restarting the peer may over-reach. Local-block scratch 2 survives, and the PROM then prints
"Skipping diags as requested by kernel". A hardware reboot capture showing
whether a single node's local reset leaves its peer running would settle
the scope. Tests: `sn1/warm-reset/local-reset-restarts-processors/*`, and
`sn1/bedrock-ni/composed-consumer-resets` for the line.

**What moved from the Origin 200's network status register.** Beyond the more-nodes
bit, the Origin 200's node number (bits 16:8), chip identity and revision (3:0 and
7:4), region size (17) and link-up (29) all lived in its network status register
(mainline `sn0/hubni.h` lines 74 to 93); on the Bedrock the first four sit in the
local block's revision register and link-up is port status bit 1. The port-error
link-reset-in bit moved from 37 (`sn0/hubni.h` line 197) to 52.

## Open questions

- Whether widget 0 on an Origin 350 reports the PXBOW part `0xd100` that
  SGI-authored Linux headers name, or the Bedrock part `0xc110` every held
  capture shows. Every held IP53 capture shows the Bedrock's own part
  `0xc110` revision 3 at widget 0; SGI-authored Linux 2.6.5
  `include/asm-ia64/sn/xtalk/xbow.h` names `PXBOW_WIDGET_PART_NUM 0xd100`
  for a crossbar-like widget 0, which no IP53 capture confirms. This model
  reports widget 0 as the Bedrock (`0xc110` revision 3), the captured
  value, and does not model a `0xd100` crossbar identity at widget 0.
  Settled by a capture or specification reading widget 0's identity
  register on a real Origin 350.

## qtest checklist

- Port status reads 0 on a one-node machine, the model's chosen value for bits 18 and 2, and is unchanged by a write.
- Two Bedrocks joined by one cable both read port status bit 1 set and bits 0, 2 and 18 clear, with no write to any network register first; marking the cable down clears bit 1 on both, and marking it up sets it again.
- The stock PROM's power-on values (reset enable 1, diagnostic parameters 0, channel control `0x166`, port parameters `0xffffffffa3ff1990`, channel age `0xc800`) read back through each register's defined bits, and the writes of 0 to port errors and sideband are accepted, after which port error clear reads 0.
- Reset enable retains a write of 1 and of 0.
- With reset enable 0 as well as 1, a write of `0x4` to port reset resets the node (the enable does not block a local reset, the model's ruling): local block scratch 0 reads 0 afterwards while scratch 1 keeps a value written before, on the model's reset equation. The consumers' values `0x5` and `0x6` reset the node too.
- A write of `0x1` (link reset out) and of `0x2` (port reset) with no link does not reset the node.
- On the Origin 300, a processor that writes reset enable 1 and then `0x6` to port reset starts again at the reset vector.
- Port errors reads the same value twice; port error clear returns that value and zeroes port errors. A model with no link never counts anything, so no test asserts a non-zero count.
- Channel age and diagnostic parameters retain writes to their defined bits; the routing-table entries retain five bits.
