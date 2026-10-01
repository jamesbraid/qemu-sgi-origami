# Datasheet: the Origin 2000 module's node-board populations

References rendered as citation identities name material outside this document
set. Those targets are not included or cleared for redistribution. Review and
runtime statements retain their recorded dates and scope.

**Status:** draft, awaiting independent evidence review. Nothing here clears
a model change on its own.

**QEMU file(s) on `sgi-origami`:** `hw/mips/sgi/sn-machine.c` and
`hw/mips/sgi/sn-catalogue.c` build one node-population row of a chassis;
`hw/mips/sgi/sn-catalogue.json` carries the rows.

**Sources used:**

- SGI *Origin2000 Deskside Owner's Guide*, `007-3453-002`.[^desk]
- SGI *Origin2000 Rackmount Owner's Guide*, `007-3456-003`.[^rack]
- SGI *Origin and Onyx2 Theory of Operations Manual*, `007-3439-002`.[^theory]
- SGI *Origin200/2000 IP27 PROM Technical Reference Manual*,
  `108-0170-001`.[^prom]
- SGI *Origin and Onyx2 Hardware Quick-reference Booklet*,
  `HMQ-380-C`.[^hmq]
- Real-hardware captures in the project's hinv store: joint-buck, a stock
  PROM 6.156 boot of a three-node module and a later IRIX listing of it
  with four,[^joint] and true-wombat, an Onyx2 rack whose module 1 is seen
  with two node boards and later with four.[^wombat]
- Disassembly of the IP27 PROM 6.156 payload shipped with IRIX 6.5.30 and of
  the IRIX 6.5.30 `unix.IP27` kernel, with `tools/origami disasm`.[^disasm]

**Clean-room statement.** The author read no IRIX or PROM source, no
source-reader notebook and no Library document that reasons from vendor
source. Kernel routine names come from the kernel's own symbol table; PROM
routines are named by address.

**Evidence labels.** Every fact is *established* (printed in a cited manual,
shown by a real-hardware capture, or read directly from disassembly),
*inferred* (follows from established facts but is not itself printed or
observed), or *unknown*.

## Role

The Origin 2000 module is one midplane with four node slots (`n1` to `n4`),
two Router slots (`r1`, `r2`), two Crossbows and twelve XIO slots. The
deskside and each rack compute module are the same module.
*Established.*[^rack-module][^desk-midplane] SGI shipped and upgraded it
with any number of node boards from one to four, and the number decides which
Router boards are fitted and which XIO slots work. This page owns those
per-count facts. The module's own wiring is described in
the Origin 2000 reference (citation: `docs/machines/origin2000.md`), the Router chip in
[the Router datasheet](router.md) and the Onyx2 variants in
[the Onyx2 datasheet](onyx2.md#node-board-populations).

## Node-board populations

Node boards fill `n1` first, then `n2`, `n3` and `n4`. *Established.*
"Node board slots are counted from right to left", and the first node board
is always in the right-hand slot.[^desk-fill][^desk2-fill] Table 42 of the
booklet also lists a three-node placement `N1, N2, N4` for three pipes; the
model supports fill order only.[^hmq-xtown]

A node board carries one or two processors. *Established.*[^rack-node]

### Router fit

| Node boards | Deskside, as shipped | Deskside, as modelled | Rack module, as modelled | Status |
|---|---|---|---|---|
| 1 | no Router board, or the first Router board (the guide says both) | none | none | Inferred for both: the deskside guide contradicts itself,[^desk-table][^desk-fig] and the rack guide says only that none is required, with no one-node capture held[^rack-router] |
| 2 | Null Router in `r1`, a passive Hub-to-Hub link | Null Router | two Standard Routers | Established.[^desk-fig][^wombat] |
| 3 | Router in `r1`, Star Router in `r2`, IR1 jumper | two Standard Routers | two Standard Routers | Rack established by capture;[^joint] deskside inferred, the model's choice |
| 4 | Router in `r1`, Star Router in `r2`, IR1 jumper | two Standard Routers | two Standard Routers | Rack established;[^prom-star][^joint] deskside inferred, the model's choice |

"If a module has only one Node board, no Router is required. A Router board
is required only when there are two or more Node boards."[^rack-router] The
deskside guide adds that without a router board only one node board and one
bank of XIO slots work.[^desk-table] The same page also says "All Origin2000
deskside systems ship with the first router board installed", which Figure
2-9's "None" for one node board contradicts. The model follows the figure
and the table. Which a one-node deskside actually carried is *unknown*; a
fitted Router would show in PROM discovery as one Router beside the one
Hub.[^desk-table][^desk-fig]

Two Standard Routers, the rack module's fabric, are a legal fit for any
count from two in a rack module. The captures show it at two, three and
four node boards:

- joint-buck boots stock PROM 6.156 on `n1` to `n3` with both Routers:
  "Found 5 objects (3 hubs, 2 routers)", "No. of RTRs enabled = 2" and
  "ALL DIAGS PASSED". The PROM numbers the nodes in slot order, `n1` NASID
  0, `n2` 1, `n3` 2, and IRIX reports Router 1's active ports as 4, 5 and 6
  and Router 2's as 5 and 6. *Established.*[^joint] A later IRIX listing
  of the same machine shows four Hubs and both Routers, each with active
  ports 4, 5 and 6.[^joint] The thread mentions the machine's MMSC display,
  the rack's controller, so it is probably a rack module; that is
  *inferred*.
- true-wombat lists a two-node module with both Routers: `r1` active ports
  4, 5 and 6, `r2` port 6 alone. *Established.*[^wombat]

With one node board the rack model carries no Router, following the manual
rather than a capture: no capture of a one-node rack module is held.

For three or four node boards in a deskside, SGI's own fit is a Router and
a Star Router (007-3453-002 Figure 2-9), and the Theory of Operations manual
says "3- and 4-Node configurations are created using a Standard router with
a Star router".[^desk-fig][^theory-router] The model fits two Standard
Routers there instead, and grades those rows *inferred*. The chain: the
deskside is one module like a rack's,[^rack-module] 108-0170-001 section
6.2.1 gives "a typical fully-populated single-module system" two
Routers,[^prom-star] and the Standard Router joins node boards "physically
located in from 1 to 8 enclosures".[^theory-router] The next section says why
the Star is not modelled.

### The Star Router, and why the model does not fit it

The Star Router is a carrier with no Router ASIC. 108-0170-001 section 6.2.2
gives its wiring with the Router in `r1`: Router port 5 to `n1`, port 4 to
`n2`, port 6 to `n3` through the midplane path the Star carries, and port 3
to `n4` through an external jumper to the Star's one connector, where its XC
chip converts the differential cable signals to single-ended. Ports 1 and 2
stay free for cables. Every node reaches every other in one hop, with
vectors 5, 4, 6 and 3. *Established.*[^prom-star][^rack-star] The PROM
therefore sees one Router with Hubs on four of its ports; the carrier
presents no register of its own. *Inferred* from the manual; the Router
datasheet agrees.

The stock PROM does not finish discovery on this wiring in the model. PROM
6.156's discovery loop (`func_1fc4fe4c`) checks each Hub's partner on the
same Crossbow: `func_1fc46f64` reads the Crossbow and the partner's link,
and `func_1fc470a0` then reads the partner's network interface through the
fabric at a fixed vector, `0x56` or `0x46`: out Router port 6, then port 5 or
4. That is the route to the partner through two Standard Routers (108-0170-001
Table 6-1). *Established* by disassembly.[^disasm] With the Star wiring,
Router port 6 leads straight to `n3`, so the request reaches a Hub with one
route step left over. What real silicon does with it is *unknown*: the
[Hub network interface datasheet](hub-network-interface.md#vector-routing--how-firmware-reaches-a-hub-with-no-node-number)
leaves an unanswerable vector open, and the model gives no answer. The stock
PROM then prints "Peer HUB detected on Xbow but no NUMAlink connection" for
`n1`, rediscovers the network fourteen times, prints "Repeatedly
inconsistent discovery results; timing out" and stops.
*Established* by an emulator run of the model; it says what the model does,
not what hardware does.

SGI shipped Star desksides, which suggests hardware answers that probe. A
diagnostic run that let a Hub answer whatever route steps remained reached
ALL DIAGS PASSED with four Hubs and one Router, so that probe is the only
blocker found on the Star wiring. That is a model experiment, not a
hardware fact: the behaviour is an unobserved guess about silicon and is
not modelled. The deskside's three- and four-node rows use two Standard
Routers instead, the fit the PROM probe is known to work on.

### XIO slots

Node 1 activates IO1 to IO6 on Crossbow 0 and node 2 activates IO7 to IO12
on Crossbow 1. *Established.*[^rack-xio][^desk-table] The booklet's
controllers:

| Node boards | Crossbow 0 (IO1-IO6) | Crossbow 1 (IO7-IO12) | Status |
|---|---|---|---|
| 1 | `n1` controls all six | not usable | Established.[^rack-xio][^desk-table] |
| 2 | `n1` controls all six | `n2` controls all six | Established.[^hmq-xtown] |
| 3 | `n1` IO1, IO3, IO5; `n3` IO2, IO4, IO6 | `n2` controls all six | Inferred: the four-node quadrants with `n4` absent, `n2` being the only Hub on Crossbow 1 |
| 4 | `n1` IO1, IO3, IO5; `n3` IO2, IO4, IO6 | `n2` IO7, IO9, IO11; `n4` IO8, IO10, IO12 | Established.[^hmq-xtown] |

The booklet's Table 42 shows the controller within a Crossbow is a placement
choice: with three node boards it has "N1 controls IO6G and IO2 on XBOW0",
where the model's three-node rows give IO2 to `n3`. So the quadrants are
SGI's recommendation, not wiring.[^hmq-xtown] joint-buck works boards in IO2,
IO4, IO5, IO6 and IO7 with three node boards.[^joint] The controlling node the
model records for an empty slot has no guest-visible effect: QEMU uses it
only to validate a row and to name the node board a refused slot needs.

An absent node board's Crossbow port is an empty port. With one node board
Crossbow 1 has no Hub, so nothing can reach it. *Inferred.*

## What the model builds

The catalogue describes each chassis with every node slot fitted and one
node-population row per count; `nodes=` picks the row and defaults to four.
An absent node board has no Hub, processors, boot flash or module-controller
adapter, and its Crossbow port reads as empty, so the PROM finds an empty
slot. A board fitted at launch in a slot the row cannot use is refused with
the node board it needs. The rows' grades are the tables' above; a row
graded unknown would be refused.

## Unknowns and what would settle them

| Unknown | Why it matters | What would settle it |
|---|---|---|
| What a Hub does with a vector request that arrives with route steps left | Whether the stock PROM discovers a Star deskside | A Hub ASIC document, or a stock PROM boot log from a three- or four-node Star deskside |
| Whether joint-buck is a rack module or a deskside | Whether a deskside capture shows two Standard Routers | The poster's photographs or a board list naming the enclosure |
| Which controller a real three-node module gives IO8, IO10 and IO12 | The three-node rows | A three-node `hinv -v` from IRIX that prints each widget's Hub |
| A one-node rack module's Router boards | The rack's one-node row | A capture of a one-node rack module |
| Whether a one-node deskside carries its first Router board | The deskside's one-node row | A PROM boot log of a one-node deskside |

Neither of the project's physical SGI systems, an Origin 350 and an Octane2,
is an Origin 2000, so a physical test cannot answer any of these.

## References

[^desk]: SGI, *Origin2000 Deskside Owner's Guide*, `007-3453-002`, revision
    002, SHA-256
    `db75a435d8f0fe4e5591fde00a110ba25cd8803eb1c170af5131afb10cb7f3d7`.
[^desk-midplane]: The same guide, Figures 2-7 and 2-8, printed pages 19-20
    (PDF 37-38): the midplane's four node slots, two Router slots, two
    Crossbows and two banks of six XIO connectors.
[^desk-table]: The same guide, printed page 21 (PDF 39), Table 2-2 and the
    paragraph before it.
[^desk-fig]: The same guide, Figure 2-9, printed page 22 (PDF 40): one node
    board with no Router board, two with a Null Router, three and four with a
    Router board and a Star Router board joined by an IR1 jumper.
[^desk-fill]: The same guide, printed page 21 (PDF 39): node board slots are
    counted from right to left.
[^desk2-fill]: SGI, *Onyx2 Deskside Workstation Owner's Guide*,
    `007-3454-005`, printed page 21 (PDF 39): the first node board is always
    installed in the right-hand slot.
[^rack]: SGI, *Origin2000 Rackmount Owner's Guide*, `007-3456-003`, SHA-256
    `4d245d1fa1e495f4eecc5c657c18fc082eb5df37273521d740aa952461ae48c7`.
[^rack-module]: The same guide, printed page 30 (PDF 50): the deskside is
    equivalent to an individual module in a rackmount system; printed page
    44 (PDF 64): the base module is also known as the deskside chassis.
[^rack-router]: The same guide, printed page 30 (PDF 50).
[^rack-node]: The same guide, printed page 20 (PDF 40): a node board holds
    one or two R10000 CPUs.
[^rack-star]: The same guide, printed page 32 (PDF 52): the Star Router is
    paired with a Rack Router board and has one external connector,
    jumpered to a port on the companion Router board.
[^rack-xio]: The same guide, printed pages 20-21 (PDF 40-41) and Figure 2-3.
[^theory]: SGI, *Origin and Onyx2 Theory of Operations Manual*,
    `007-3439-002`, revision 002, SHA-256
    `f6f30191b662aeef6c9888f109c4997d58ac9c4a77acd24263937c6392113309`.
[^theory-router]: The same manual, printed page 57 (PDF 71): the Null, Star,
    Standard and Meta Routers; 3- and 4-node configurations are created
    using a Standard router with a Star router; the Standard Router cables
    together 2 to 32 node boards in 1 to 8 enclosures.
[^prom]: SGI, *Origin200/2000 IP27 PROM Technical Reference Manual*,
    `108-0170-001`, held as
    `docs/library/cache/prom/108-0170-001-ip27-prom-technical-reference.pdf`,
    SHA-256
    `08ad68675afffe7e00327b286c0586624f0b1a1f70878e34d3e1e18de423854e`.
[^prom-star]: The same manual, section 6.2 printed pages 6-2 to 6-6 (PDF
    96-100): Figure 6-2 and Table 6-1 for two Routers, Figure 6-3 and Table
    6-2 for the Star Router with the Router in slot 1, section 6.2.3 for the
    mirror fit, and Figure 6-5 for the Null Router.
[^hmq]: SGI booklet `HMQ-380-C`, held as
    `docs/library/cache/sgi-hw/origin-onyx2-hardware-quick-reference-booklet.pdf`,
    SHA-256
    `75f82fa4123bec59e81b2e4f1330ee03e2a714e7d5b6e9216c919648a704fb04`.
[^hmq-xtown]: The same booklet, Tables 41 and 42, printed pages 130-131
    (PDF 132-133).
[^joint]: Origin 2000 PROM 6.156 boot and `hinv -v` (record 01) and IRIX
    `hinv -v` listings with three node boards (02) and four (03), posted 2013
    by `pentium`, https://nekonomicon.irixnet.org/forum/3/16727716/1.html.
    Held as capture `joint-buck` in the project's hinv store.
[^wombat]: Onyx2 rack IRIX listings posted by `regan_russell`,
    https://nekonomicon.irixnet.org/forum/14/16285/1.html, held as capture
    `true-wombat`: record 01 (2007-12-03, command not recorded) shows
    module 1 with two node boards and both Routers; record 03 (2008-04-02,
    `hinv -v -m`) shows it with four.
[^disasm]: IP27 PROM 6.156 payload `assets/firmware/6.5.30/ip27prom.payload.bin`,
    SHA-256 `c2c69bf33c375b43ec7ac511fab41ddf595be1b6838ae717e58a9a1273437fdd`,
    and IRIX 6.5.30 kernel `assets/boot/6.5.30/unix.IP27`, SHA-256
    `cbdb02ea5d334ecac949a6524a363e6c945f94baf438d41e8d9248eda0377bdf`, read
    with `tools/origami disasm`: PROM `0x1fc4fe4c`, `0x1fc46f64`,
    `0x1fc470a0` and the message strings they print; kernel `router_init`.
