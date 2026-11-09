.. SPDX-License-Identifier: BSD-3-Clause

.. _sgi-identity:

=====================================
Emulated machine serials and host ids
=====================================

IRIX builds ``sysinfo -s`` and FLEXlm's ``lmhostid`` from a number the
machine's hardware gives it. Node-locked licenses match that number, so an
emulated machine needs one that is valid, the same on every boot, different
from every other emulated machine, and plainly not a real SGI's unless the
user chose it. This page defines the interface the SGI machines share. The page
of each family says where its guest reads the value.

One value, the system id
========================

The observable is the 32-bit system id, the number ``lmhostid`` and
``sysinfo -s`` report. Every other value is derived from it or is a spelling of
it, through a fixed map for each family:

- On the Origin 300, Origin 350, Origin 3000, Onyx 350 and Tezro, IRIX folds
  the low nibble of each of the first 10 characters of the L1 system serial
  into the id and keeps the low 32 bits (*software expectation*). Guests on
  the model printed ``d5100042`` for ``LEMU100042`` and ``KEMU100042``,
  ``c5d51042`` for ``LEMU1042``, ``51c00000`` for ``EMU1L0000042`` and
  ``d2001347`` for ``M2001347``.
- On the Origin 2000 and Onyx2 it does the same with the module serial
  (*software expectation*). A guest on the model printed ``d5052208`` for
  ``KEMU052208``.
- On the Octane, Octane2, Origin 200 and Fuel it takes the last four bytes of
  the station address (*software expectation*). Guests on the model printed,
  for example, ``69e5514e`` for ``08:00:69:e5:51:4e``.
- On the O2 it takes the station address the :term:`PROM` reads from the identity
  chip's memory, bytes 0 to 5 in reverse order, and the last four bytes of
  that address are the id (*software expectation*, measured in model boots).
  The network adapter's own address plays no part
  (:ref:`sgi-ip32-system-serial`). A guest on the model printed ``69e102a0``
  for ``08:00:69:e1:02:a0``.

The interface
=============

The same properties exist on every SGI machine.
Which emulator built the machine, ``1`` for QEMU and ``2`` for Iris, is a
constant of the emulator (``SGI_BOARD_KIND`` in
``include/hw/mips/sgi/sgi-board-identity.h``), not a property.

.. list-table::
   :header-rows: 1

   * - Property
     - Meaning
   * - ``emu-id``
     - Optional, 0 to 4095, default 0. Mints a system id in the emulator
       layout below. 0 gives a plainly generic id such as ``d5100000``.
   * - ``serial``
     - Optional text of 1 to 10 printable characters, none of ``:`` or ``;``:
       the system serial as a real unit has it written, for example
       ``M2100887``. Sets the system id by the fold rule. The characters are
       checked when the property is set. On the Origin 2000 and Onyx2 it must
       also start with ``K``, checked when the machine starts.
   * - ``hostid``
     - Optional, 8 hex digits: the system id itself. On the Octane, Octane2,
       Origin 200 and Fuel also 6 digits, the low 24 bits with the leading
       ``69`` implied. Read back, the id of the first node or module once the
       machine is built.
   * - ``mac``
     - The address of the machine's own network port, as on every machine
       today.
   * - ``hostids``
     - Read-only: every id the guest can print, as 8 hex digits each separated
       by spaces, in node, module or chassis order. The first is what
       ``hostid`` reads back.

``emu-id``, ``serial`` and ``hostid`` cannot change once the machine is built;
an ``emu-id`` above 4095 is refused. ``qom-get`` reads ``hostid`` and
``hostids`` over QMP from a running machine, and a guest check compares
``lmhostid`` in the guest with ``hostids``.

Precedence, highest first:

1. the debug inputs: the low-level store inputs a device keeps for one store,
   such as ``system-serial`` on the ``sgi-l1`` device, ``brick-serial``,
   ``fuel-mac-eeprom=`` and the :term:`IOC3` and board addresses. When one is set it
   wins, and ``hostids`` reflects it;
2. ``serial``, ``hostid`` or ``mac``;
3. ``emu-id``;
4. the default ``emu-id`` of 0.

Two inputs at the same level must agree. If they do not, the machine refuses to
start with a message naming both and the family's rule. An id set by
``serial``, ``hostid`` or ``mac`` is the user's own and carries no emulator
marker.

What each family accepts
------------------------

.. list-table::
   :header-rows: 1

   * - Family
     - Id source
     - ``serial``
     - ``hostid``
     - ``mac``
   * - Origin 300, 350, 3000, Onyx 350, Tezro
     - the L1 text
     - any text up to 10 characters, kept as given
     - any id; the L1 text is its eight-character spelling
     - the port's address only, in the L1 record and the IOC3
   * - Origin 2000, Onyx2
     - one module serial per module
     - text starting with ``K``, the base for module 0; else refused
     - any id; the module text is ``KK`` and its spelling
     - the port's address only
   * - Octane, Octane2, Origin 200, Fuel
     - the station address
     - refused unless the text folds to an id starting ``69``
     - 8 digits starting ``69``, or 6 digits; else refused
     - sets the id; node or port ``n`` adds ``n`` to the address
   * - O2
     - the station address, held in the identity chip
     - refused unless the text folds to an id starting ``69``
     - 8 digits starting ``69``, or 6 digits; else refused
     - sets the id and the adapter's address; the ``08:00:69`` prefix is
       this project's policy, not the firmware's

On the text families ``serial`` and ``hostid`` are two spellings of one id and
must agree. On the address families ``serial``, ``hostid`` and ``mac`` are
three, and all given must agree. The firmware fixes the first address bytes,
so an address-family id starts ``69`` and its station address starts
``08:00:69``. A ``mac`` outside that prefix is refused, as is a base address
whose node or port numbers would carry out of the block (for example
``mac=08:00:69:ff:ff:ff`` on a two-chassis Origin 200, which would give the
second chassis ``6a000000``). A refusal says what to use instead and the
suggestion is accepted:

- ``mac=52:54:00:12:34:56``: "this machine's station address must start
  08:00:69; use mac=08:00:69:12:34:56 or hostid=123456".
- ``mac=08:00:69:ff:ff:ff`` on two chassis: "node 1 of mac=08:00:69:ff:ff:ff
  would leave the 08:00:69 block, which this machine's station address must
  stay in; use mac=08:00:69:xx:xx:xx with room for the nodes, or a lower
  hostid". The text families keep any port address.
- ``hostid=d2100887`` on an Octane: "this machine's host id is 69 plus 6 hex
  digits; use hostid=100887 or mac=08:00:69:10:08:87".
- ``serial=M2100887`` there: "serial folds to d2100887; this machine's host id
  comes from its station address, which always starts 69; use hostid=100887 or
  mac=08:00:69:10:08:87".
- A ``mac`` and ``hostid`` that disagree: the message names both, and says the
  machine would take the id from ``mac``.

The O2 takes all four like the Octane. It builds its identity chip from them
(:ref:`sgi-ip32-system-serial`): the address reversed in the chip's first six
bytes, the rest ``0xff``, and its seven status bytes ``0xff``, both an
*approximation* because a fitted chip's contents are unknown. ``emu-id`` also sets its real-time clock serial. ``hostids``
reads the chip image the machine built or was given. The adapter takes the
same address and a different ``-nic mac=`` is refused.

Spelling an id as a serial
--------------------------

``serial`` to id: take the first 10 characters and fold each one's low nibble
into 4 bits. ``M2100887`` gives ``d2100887``; ``LEMU100420`` gives
``d5100420``. Only the low nibble of each character counts and only the last
eight nibbles survive, so the first two characters of a 10-character text never
show in the id: ``KEMU100420`` and ``LEMU100420`` give the same id.

``hostid`` to serial on the text families: eight characters, one per hex digit.
A digit 0 to 9 stays itself. A digit ``a`` to ``f`` becomes a letter ``J`` to
``O``. The first character is always a letter: ``P`` for 0, ``A`` to ``I`` for 1
to 9 and ``J`` to ``O`` for ``a`` to ``f``. So ``d2100887`` becomes
``M2100887``, ``d21a0f87`` becomes ``M21J0O87``, ``00000001`` becomes
``P0000001`` and ``ffffffff`` becomes ``OOOOOOOO``. The id round trip holds,
and a serial the user gave need not come back as the same text. A real serial
is a letter and seven digits, so an id with a letter digit after the first has
a synthetic spelling no real unit has (*inferred* from the fold rule).

Modules
-------

The Origin 2000 and Onyx2 guest reads a module serial only when its first
character is ``K``: a guest printed 0 for ``lmhostid`` and could not retrieve
the system serial for ``M2100887``, ``00M2100887``, ``00K2100887`` and
``M210088799``, and printed ``d5100887`` for ``KEMU100887`` and ``10088799``
for ``K210088799`` (*software expectation*, from the analysis of the IRIX
6.5.30 IP27 kernel, entry ``unix.IP27-6.5.30``, at address
``0xc000000000040974``, and these guest runs on the model). So
``serial=`` on these machines must start with ``K`` and is otherwise refused
with "serial 'M2100887' must start with K on this machine: the guest ignores a
module serial that does not". An 8-character serial such as ``K0017299`` folds
to an id starting ``b``, the fold of ``K``, as the Origin 2000 ids in the
captures of real machines do (*inferred*); for a 10-character serial the first
two characters never reach the id.

Module ``i`` has the id of the base plus ``i``, and module 0 keeps the serial
as given. The module record is ten characters, so a shorter serial is padded
on the left with ``K``, so it still starts with ``K``: ``K0017299`` becomes
``KKK0017299`` and folds to ``b0017299``. The padding character is a model
choice (*approximation*). The id is the fold of the padded text, so padding
changes the id of a serial shorter than 8 characters: ``K12`` becomes
``KKKKKKKK12`` and gives ``bbbbbb12``, where a plain fold would give ``b12``
(*approximation*). With
``hostid=`` the module text is ``KK`` and the eight-character spelling, so any
32-bit id is reachable: ``d2100887`` gives ``KKM2100887``, and module ``i``
spells the id plus ``i``. A
machine with more than 10 modules is refused, because the emulator layout has
one digit for the module. The catalogue has no machine with more than 8 nodes
or more than 2 Origin 200 modules, so the 10-module and 16-node limits are
tested on the helpers the machines call (``tests/unit/test-sgi-board-mac.c``)
and not on a built machine. ``hostids`` lists one id per module.

On the Origin 200 the guest's id comes from the station address, so its module
serial follows ``emu-id`` alone: two Origin 200 machines with different
``hostid`` values and the same ``emu-id`` show the same module serial and
different ids.

The emulator layout
===================

``emu-id`` is a pure function of the kind (1 for QEMU), the ``emu-id`` and a
small sub-index (a node, module or network port). There is no hashing. The
functions are in ``include/hw/mips/sgi/sgi-board-identity.h`` and the unit test
``tests/unit/test-sgi-board-identity.c`` carries the worked vectors, so a
second emulator can port both.

.. list-table::
   :header-rows: 1

   * - Family
     - Value
     - Host id
   * - Origin 300, 350, 3000, Onyx 350, Tezro
     - ``LEMU``, the kind digit, the ``emu-id`` as four digits, ``0``
     - ``d5``, kind, ``emu-id``, ``0``
   * - Origin 2000, Onyx2
     - ``KEMU``, the kind digit, the ``emu-id`` as four digits, the module
       number 0 to 9
     - ``d5``, kind, ``emu-id``, module
   * - Octane, Octane2, Origin 200, Fuel
     - ``08:00:69:E``\ *kind*\ ``:``\ *emu-id >> 4*\ ``:``\ *(emu-id & 15) << 4 | sub-index*
     - ``69E``, kind, ``emu-id``, sub-index
   * - O2
     - the Octane's address, held in the identity chip; the clock serial is
       ``45 4D 55``, the kind, the ``emu-id`` as two bytes
     - ``69E``, kind, ``emu-id``, 0

For kind 1: ``emu-id`` 42 on an Origin 300 reads ``LEMU100420`` with host id
``d5100420``, module 1 of an Origin 2000 reads ``KEMU100421``, and an Octane's
address is ``08:00:69:E1:02:A0`` with host id ``69E102A0``. ``emu-id`` 0 gives
``LEMU100000`` and ``d5100000``. Kind 2 ``emu-id`` 42 gives ``LEMU200420`` and
``d5200420``.

Reading a host id back: ``d5`` or ``69E`` up front, then the kind digit, then
the ``emu-id``. The ``d5`` is the low nibbles of ``M`` and ``U`` from ``EMU``.
The prefixes ``d5`` and ``69E`` and the ``E?`` address block are reserved for
emulators in this project's documentation.

The sub-index
-------------

A machine with several nodes or ports gives each a station address: the
machine's plus the node or port number. The emulator layout leaves the low
nibble of its last byte for that number, so adding 0 to 15 never carries into
the ``emu-id``. A machine that needs more than 16 nodes or ports in the
emulator layout is refused. An address derived from a ``hostid``, ``serial`` or
an explicit ``mac`` is not constrained and the sum can carry: the real
two-chassis Origin 200 ids are unrelated to each other, so this is a
convenience, not a copy of a real unit.

Ranges
------

The ``emu-id`` space is split by purpose so two machines do not share a number.
The split is this project's convention and the machine does not enforce it.

.. list-table::
   :header-rows: 1

   * - ``emu-id``
     - Reserved for
   * - 0
     - the default, a generic id
   * - 1 to 2999
     - machines users create
   * - 3000 to 4095
     - harness and continuous-integration guests

Board serials
=============

The six-character board text serials, ``EMUN`` and two hex digits, are not host
ids and follow no identity input. They count the node: ``EMUN00`` for node 0.
The stock IP35 :term:`PROM` ties its NVRAM to the node board's serial and starts
the NVRAM afresh when it changes, so an id that followed the inputs would wipe
the settings a guest stored. Tests hold the board serial equal across ``emu-id``
values.

Overrides and debug inputs
==========================

A debug input wins over the properties above and is not part of the interface;
nobody needs one to choose an identity.

- ``system-serial`` on the ``sgi-l1`` device sets the text an L1 answers with,
  so the host id of the Origin 300 family.
- ``fuel-mac-eeprom=`` replaces the Fuel's address with a 512-byte record image,
  and so its host id.
- ``identity-eprom-file=`` on the O2 replaces its identity chip's 128 bytes
  with a captured part's. ``hostids`` then reads the file's bytes 3, 2, 1 and
  0, and the file wins over every identity input.

``hostids`` is computed from the values the machine holds once it is built, so
a debug input changes it and a check against the guest still agrees.

Compatibility
=============

Every host id changes once, when the interface lands. On the O2 nothing stores
the address, so a machine started on an existing disk and flash takes the new
one on its next boot (*software expectation*, measured). On the other machines
a guest whose NVRAM already holds a station address may keep printing that
address after the machine's inputs change, so its ``lmhostid`` can differ from
``hostids``; the model does not rewrite the stored address, and this was not
tested in a guest (*unknown*). A saved machine state from an earlier build does not load into this
one, because the machine's properties changed; the project does not migrate
saved states across builds. The identity inputs are machine configuration like
``mac``: they are not part of the saved state, and a migration destination must
be started with the same values.

Evidence
========

What follows was measured in IRIX 6.5.30 guests on this model. A model run
shows what the model gives a guest, not what hardware does, so the model's
results carry no rating; what they show about IRIX is a *software
expectation*.

- Setting the L1 text to ``LEMU100042`` gave ``d5100042``, ``KEMU100042`` the
  same, ``LEMU1042`` gave ``c5d51042``, ``EMU1-0042`` gave ``d51d0042`` and
  ``EMU1L0000042`` gave ``51c00000``, so IRIX ignores characters beyond the
  tenth (*software expectation*).
- It accepts letters, punctuation and 12-character text, and printed a host id
  for all of them, so IRIX imposes no format on the text (*software
  expectation*).
- Octane, Octane2, Fuel and Origin 200 guests printed the last four bytes of
  the station address as the host id, so IRIX takes the id from the address
  (*software expectation*).
- The Origin 2000 and Onyx2 guests printed an id from the module serial
  (*software expectation*). Which of several module records the kernel reads
  for ``sysinfo -s`` is *unknown*.
- O2 guests printed an id equal to the last four bytes of the identity chip's
  address, ``69e102a0`` for ``08:00:69:e1:02:a0``, and never the clock's
  serial or the Ethernet adapter's address, which differed from the chip's in
  six of nine boots. So IRIX takes the O2's id from the chip, reversed from
  bytes 0 to 5 (*software expectation*). A chip address of ``02:00:00:e1:02:a0``
  gave ``00e102a0``, so the ``08:00:69`` prefix is this project's choice.
- The letter spelling of an id is not measured: that a guest folds ``J`` to
  ``O`` and ``P`` to the nibbles ``a`` to ``f`` and 0 is *inferred* from the
  fold rule, as is the left padding of a module serial.

What real machines show, from a sample of captures:

- Real system serials are one letter and seven digits, and the digit after the
  letter is 1 or 2 in every sample. Real Origin 2000 module barcodes start
  ``K0``. Real serial-derived host ids carry decimal digits after their first
  digit. Real address-derived ids can carry ``a`` to ``f`` (*observed*).
- The byte after ``08:00:69`` in a real station address starts with ``0`` or
  ``1`` in every sample, and none is in the ``E?`` block (*observed*).

This is a sample, not proof. Whether SGI ever issued the ``E?`` block, a ``d5``
system id or a ``K`` module serial beginning ``EMU`` is *unknown*. A real L2's
serial check documents a letter and seven digits, which ``LEMU...`` does not
satisfy. The model has no L2, and the stock PROM and IRIX accepted the text in
the runs above (*software expectation*); what a real L2 does with it is *unknown*. The
text is built in one function, so a conforming fallback with the marker in the
digits is one change.

Which source the Origin 200 guest reads when it has both a module serial and a
station address is *unknown*; the measured id was the station address's.
