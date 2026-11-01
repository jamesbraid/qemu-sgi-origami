.. SPDX-License-Identifier: BSD-3-Clause

.. _sgi-model-vocabulary:

Machine model vocabulary
========================

The SGI machine catalogue, ``query-sgi-machines`` and the ``-M`` options
use these words with one meaning each.

Product
  The ``-M`` machine name, such as ``origin350`` or ``tezro``. A product
  builds one of its topologies.

Topology
  One selectable wiring of a product, chosen with ``topology=``: its
  chassis or bricks, node boards, cabling and I/O. ``tezro-rack`` is a
  topology of the ``tezro`` product, and ``origin3900`` one of
  ``origin3000``.

Record
  One catalogue ``SGIModelTopology`` entry: one buildable machine. A
  topology name can have several records, told apart by board and by
  interface board.

Board
  The node board every node of a record carries, named by its IP number
  in lower case (``ip53``, ``ip59``) and chosen with ``board=``. A record
  builds one board on every node; a topology that takes more than one
  names its default. The board is not the platform generation
  (``generation``: SN0, SN1, IP30 or IP32) and not the node-board
  identity record below.

Interface board
  A board the processor count chooses, not the caller: the Tezro's
  one-processor and two/four-processor interface boards. ``-smp`` picks
  among a topology's records for one board by the CPU totals each
  accepts, so those totals never overlap.

Variant
  In a model composition, one of the records a composition builds: a
  board, an interface board, or both. It is authoring structure; the
  catalogue shows the record's board and CPU totals instead. The O2's
  ``variant=`` machine option is unrelated.

Node-board identity
  The name, part number and revision an L1 reports for a node, chosen by
  the number of processors fitted (``IP59_4CPU``, ``IP59_2CPU``). It is
  catalogue data of a board, keyed by processor count; the catalogue
  member is ``node-boards``.

Processor
  One fitted processor option: part, clock, secondary cache and the
  reset-mode word the board supplies. Its id names part, clock and cache
  (``r16000-1000mhz-16mb``). The generator holds every R16000 id to one
  processor; the other parts' ids carry no such rule, and the Fuel's and
  the Origin 300's 500 MHz R14000 share ``r14000-500mhz-2mb`` with
  different PRId revisions. ``-cpu`` names only the part.

Runtime profile
  The IP53-family settings a record carries in ``ip53-runtime``: its PIC
  compatibility profile, the L1 compatibility mode and its node-board
  identities. It applies to IP53 and IP59 boards alike.

Offering
  One launchable configuration: product, topology, board, node count and
  processor population, with the consoles, firmware and inputs a
  launcher must supply.

Machine options
  An offering's ``-M`` string. It names the product, ``topology=`` and
  ``nodes=``, ``board=`` wherever the topology takes more than one board,
  and ``processor=`` where the processor is not the topology's default.
