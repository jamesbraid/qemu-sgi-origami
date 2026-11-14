.. SPDX-License-Identifier: BSD-3-Clause

.. _sgi-l1-brick-profiles:

========================================================
L1 brick profiles: Origin 3000 and Onyx 3000 controllers
========================================================

Used in :ref:`sgi-ip35` (Origin 300 and Fuel), :ref:`sgi-ip53` (Origin
350, Onyx 350, Tezro and the Onyx4 host), the Origin 300 PCI expansion
module (:doc:`pci-expansion`) and :term:`NUMAlink` module
(:ref:`sgi-bricks-numalink-module`), and the Origin 3000 and Onyx 3000
C-, I-, N-, V-, R- and CX-bricks (:ref:`sgi-bricks`), which the
``origin3000`` machine's topologies build.

.. seealso::

   Platform pages: :ref:`sgi-ip35`, :ref:`sgi-ip53`.

.. seealso::

   - :doc:`The L1 page <l1>`: the request encoding, framing
     (:ref:`sgi-l1-framing`) these profiles reuse.
   - :doc:`l1-l2-link`: the L2 link the C-brick profiles use.
   - :doc:`bricks`: the bricks the controllers sit in.

This page is the L1 of the Origin 3000 and Onyx 3000 bricks: the C-brick,
I-brick, N-brick and V-brick profiles and the Origin 3900 Cx-brick profile.
Each brick has its own controller with its own rack, slot, type and records,
and the profiles reuse the request encoding, framing and task-6 request
codes of :doc:`the L1 page <l1>`.

.. _sgi-l1-origin3000:

Origin 3000 C-brick, I-brick, N-brick and V-brick profiles
==========================================================

``brick-type=cbrick`` and ``brick-type=ibrick`` model the L1 of the Origin
3000 compute brick and system I/O brick. ``brick-type=nbrick`` models the
Onyx 3000 N-brick's and ``brick-type=vbrick`` the Onyx 3000 V-brick's,
described after the table. Each brick has its own controller
with its own rack, slot, type and records (:src:`007-4240-002 p. 69, 72, 118, 128`). The profiles reuse this page's request encoding, framing
and task-6 request codes, which the public Linux and OpenBSD headers name
for the Origin 3000 products; that a fitted C-brick frames bytes the same
way is *unknown*. ``brick-type-code`` is refused. Request ``0x0005``
answers the firmware revision ``revision-*`` configures, and minus 104
without one.

Facts are rated as described in :ref:`sgi-ratings`; the choices below are approximations, not
captured controller output.

.. list-table::
   :header-rows: 1
   :widths: 30 12 58

   * - Fact
     - Rating
     - Basis
   * - A brick is named rack, type letter and slot (``003c01``); a brick
       with neither set shows ``---c--`` or ``---i--`` in ``config``.
     - established
     - :src:`007-4240-002 p. 112-113`.
   * - ``brick rack`` and ``brick slot`` take effect at the next L1 restart.
       Page numbers in this section are printed page numbers.
     - established
     - :src:`007-3938-006 p. 39` (PDF page 63), which says the controller
       must be rebooted for a new rack and slot to take effect. The
       examples at :src:`007-3938-001 p. 39` (PDF page 55) show the same
       two commands without saying so.
   * - The manuals list ``brick partition none`` and ``brick partition
       <partition number>``, with the output ``brick partition cleared.``
       and ``brick partition set to 1.``. They do not say when the number
       takes effect: no page names a restart for the partition. The later
       manual also says ``part`` or ``p`` may stand for ``partition``; that
       edition covers the Altix 3000 series, Altix 450 and Altix 4700
       (:src:`007-3938-006 p. 1`, printed page), and the 1.44 image accepts ``p``
       for ``none`` only, so ``p <number>`` is a parse error there. Whether
       the note ever held for an Origin 3000 L1 is *unknown*.
     - established for the two output lines and the list of forms;
       unknown for when the number takes effect; the ``p`` spelling of the
       later manual disagrees with the image
     - :src:`007-3938-001 p. 38-39` (PDF pages 54-55) and
       :src:`007-3938-006 p. 56-57` (PDF pages 80-81).
   * - When the command runs the image stores the typed number in the
       settings record, records it in the brick's own module record and
       publishes a partition notice; further configuration messages follow,
       whose contents were not read. The rack, slot and type setters print a
       text that says the change takes effect on the next L1 reboot or power
       cycle, and the partition setter prints none.
     - software expectation
     - :src:`l1-firmware-1.44.0-base brick partition and partition apply`
   * - The Origin 300 profile runs the Fuel, Origin 300 and PE package that
       the partition reading was made on, so applying the number at once with
       no restart note, and clearing a stored 255 or more at start-up, is a
       *software expectation* there. The test
       ``sn1/l1/o300/console-brick-partition`` pins it. The handler
       refuses only routers, the I/O brick types and type ``A``, so the
       Chimera types ``7``, ``8``, ``9`` and ``Q`` behave as a C-brick does
       on that package, and the model gives them the same behaviour; the
       rating is for those types read on the Fuel, Origin 300 and PE
       package.
     - software expectation for the Origin 300 and the Chimera types
     - :src:`l1-firmware-1.44.0 brick partition console case` (the Fuel,
       Origin 300 and PE package). Tests: ``sn1/l1/chimera/brick-partition`` and
       ``sn1/l1/irouter/identity-overrides``.
   * - The model gives every profile that runs this console one behaviour
       for the partition: applied at once, no restart note, while rack and
       slot still wait for a restart. A profile whose image behaves
       otherwise would differ, and a test that told the two apart would be a
       hardware observation of the setting at restart.
     - approximation
     - The model's single behaviour is limited by what no source says of the
       other profiles. Tests: ``sgi-l1-o3k/partition-applies-at-once`` and
       ``sgi-l1-o3k/partition-texts``.
   * - The command zeroes the stored setting 14 before it looks at the brick
       type. A router or an I/O brick refuses with the setting left at 0,
       while the module record keeps its old value until the next start.
       With setting 18 nonzero the image prints ``WARNING: brick appears to
       get partition from L2.`` and still writes the value. That the warning
       takes the place of the ``cleared`` line for ``none`` is the console
       reading's; the model prints the warning alone then, which is
       *inferred* for the display. A bare ``brick partition`` is a parse
       error that changes nothing, as is ``brick p <number>``. The image
       refuses type ``A`` before zeroing; the model has no such brick.
       Start-up replaces a stored 255 or more with 0 and forces a router's
       to 0.
     - software expectation for the order, the warning, the forms and the
       start-up rules; inferred for the suppressed ``cleared`` line
     - :src:`l1-firmware-1.44.0-base brick partition and start-up clamp`
   * - The model stores any number up to 16 bits at run time and rejects a
       larger one after zeroing the setting. The image has no such limit; the
       width is the model's stored value, and the rejection is the model's.
       A run-time value of 255 or more prints ``?`` in the brick line.
     - approximation
     - The image compares no value on the console path. Test:
       ``sgi-l1-o3k/partition-limit``.
   * - A record's partition word is at ``+0x3c`` of the 0x70-byte module
       record and is what the L2 receives. A partition notice stores the
       announced word in the peer's record found by the sender's rack and
       slot, and does nothing when the peer holds none. Entries behind the
       sender's own, its I-brick's included, are not changed by it.
     - software expectation for the record and the notice; inferred that the
       I-brick's entry is left as it was
     - :src:`l1-firmware-1.44.0-base partition notice handler`. The notice is an
       in-process store, an *approximation* of the serial frame. Test:
       ``sgi-l1-o3k/partition-notice``.
   * - ``0x0003`` answers the packed rack and bay and the letter ``C`` or
       ``I``.
     - inferred
     - Linux v2.6.0 ``ksys/l1.h`` ``L1_BRICKTYPE_C`` and
       ``L1_BRICKTYPE_I``. The C-brick letter is a candidate for the
       module name only: which controller answers the I/O side's
       identity request on a C-brick is unknown. The case of the I-brick
       letter is the model's choice.
   * - The machine configures firmware revision 1.44.0 on every C-brick,
       Cx-brick, I-brick, N-brick and V-brick L1 for request ``0x0005``,
       and the machine's ``l1-revision`` replaces it. ``version`` prints
       ``L1 1.44.0 (Image EMU-UNKNOWN)``, naming no image bank or product
       image.
     - *software expectation* for the firmware; inferred for a fitted brick
     - Every brick image in the L1 firmware IRIX 6.5.30 ships is version
       1.44.0 and answers ``0x0005`` with 1, 44 and 0 whatever the brick.
       Which of its two Origin 3000 images a brick runs depends on its L1
       board's flash, so the profiles name neither. No fitted brick's
       revision is captured. IRIX 6.5.30 labels each module's and I/O
       brick's hardware graph entry with the reply and logs an error at
       each start without one, and on a node with R14000 processors it
       wants at least 1.3.20. The stock IP35 :term:`PROM` warns below 1.12.5.
       1.44.0 passes both, *software expectation*.
   * - ``0x000a`` answers two integers. The first is 0 for a 400 MHz I/O
       link and 1 for 600 MHz (``port-speed-first``, default 0).
     - *software expectation* for the meaning
     - The stock IP35 PROM 6.210 compares the first integer with the speed
       it expects for the attached brick's letter, 400 MHz for ``I``,
       ``N`` and ``X`` and 600 MHz for ``P`` and ``V``, and warns on a
       mismatch. An Origin 300 board skips the comparison. Every C-brick
       answers 0, which matches its I-brick, N-brick or V-brick. That
       agrees with the V-brick's 800 MB/s port rate
       (:src:`007-4264-002 p. 54`), which reads as a 400 MHz link on 16
       bits (inferred). Which a real C-brick answers is unknown.
   * - A C-brick's records: node board at component 0, two PIMMs at 1 and
       2, DIMM ``n`` at ``3 + (n >> 1) + 4 * (n & 1)``.
     - approximation
     - OpenBSD ``l1.h``. Linux uses ``3 + n``; the only published
       C-brick dump (:src:`007-3938-001 p. 47`) lists DIMMs in the
       OpenBSD order, which is a print order, not a component index.
   * - An I-brick's records: power board at 0, logic board at 1 with the
       base I/O Ethernet address.
     - inferred
     - Linux ``ksys/l1.h`` and OpenBSD ``l1.h``.
   * - Which attached-brick port each cable appears at in ``0x000c``,
       ``0x000d`` and ``config``.
     - unknown
     - The guides number it differently by edition (:src:`007-3938-001 p. 41`,
       :src:`007-4240-002 p. 105-106`), so ``io-port`` and ``c-port``
       configure it. An I/O brick's own answer is the next row.
   * - What an I-brick, V-brick or N-brick lists in ``0x000c``, and its
       type letters.
     - approximation
     - The L1 firmware answers ``0x000c`` from its link table whatever the
       brick's letter. The stock IP35 PROM 6.210 sends it to the attached
       I/O address only during crossbar arbitration, which it enters only
       when the widget at the node's widget-0 window reports a crossbow
       part, ``0xd000`` or ``0xd100`` (OpenBSD ``xbowdevs`` names them
       XXBow and PXBow), and there an error reply abandons the
       arbitration. An I-brick's and a V-brick's widget 0 is an :term:`XBridge`,
       so that PROM never asks either one (software expectation). An
       I-brick or V-brick answers with the C-brick that asked at port 1
       and, when that C-brick's chip has a G-brick on its other port, the
       G-brick at port 2, in the C-brick's own reply shape, with letters
       ``C`` and ``G``. The other ports are empty. An N-brick answers so
       only with a G-brick, and the unknown-request code otherwise.
       Without a G-brick, ``0x000d`` keeps the unknown-request code on any
       of them. Which ports and letters a real brick uses are unknown.
       The N-brick reports ``N``, a candidate from the command set's brick
       type list (:src:`007-4240-002 p. 235`). The stock PROM accepts the
       letter, which does not show it is the hardware's. One N-brick
       controller serves all its C-bricks (N-brick Overview, revision
       10/24/01, printed page 5) and answers each about its own chip.

The V-brick profile (``brick-type=vbrick``) is reached from the C-brick
its :term:`XIO` cable joins, as an I-brick's is, and knows only that C-brick. The
V-brick has its own L1 (:src:`007-4264-002 p. 52, 128-129`),
*established*. It is the X-brick's chassis and midplane XBridge with V12
boards in the slots. The Origin 3000 L1 firmware has no personality of
its own for it: it picks a brick's type letter from the name field of the
power and logic board EEPROMs and installs that letter's personality, and
a logic board named ``XBRICK`` gives ``X``. Its only ``V`` belongs to a
later PIC-based graphics brick, and no firmware command can store ``V``,
*software expectation*. So the profile answers as the X-brick
personality. No V-brick L1 reply has been captured, and its address and
ID PROM contents are *unknown*:

.. list-table::
   :header-rows: 1
   :widths: 30 12 58

   * - Fact
     - Rating
     - Basis
   * - ``0x0003`` answers the packed rack and bay and the letter ``X``.
       ``brick`` prints ``type: X`` and ``config`` names the brick
       ``001x10``.
     - inferred
     - That the V-brick's logic board carries the X-brick's name is
       inferred from the guides' description of the shared chassis. Of
       the letters IRIX 6.5.30 and the stock PROM handle, ``X`` alone
       names a brick of this chassis an X-brick, draws no unknown brick
       type warning at the XBridge attach and matches the 400 MHz link the
       guide's port rate implies. A V-brick's ``brick`` or ``eeprom``
       output would settle it.
   * - Two records: the power board at component 0 and the logic board,
       named ``XBRICK``, at component 1, both read through both areas.
       Other components answer minus 104.
     - inferred for the components; approximation for the contents
     - The X-brick personality registers these two records, and the stock
       PROM and IRIX read the logic board of a brick that answers ``X`` as
       component 1. The parts are unobserved and the serials synthetic,
       lettered as the I-brick's. The builder sets the V-brick's
       ``identity-index`` to its module index so its serials differ from
       the I-brick's.
   * - ``0x000a`` answers 0 and 0, port A and port B at 400 MHz.
     - approximation
     - The X-brick personality answers with its two port speed settings,
       which default to 400 MHz when no C-brick is attached. What a cabled
       V-brick stores is unknown. 400 MHz agrees with the guide's rate.
   * - Operator commands, power and reset are refused as on any brick
       with no compute services.
     - approximation
     - No command output of a V-brick is published.

What the model chooses, and the limit of each choice:

- A brick with no rack and slot answers an identity request with the
  router's error reply, because the integer a real one returns is unknown.
  Rack and slot are supplied with ``named=on``, ``rack`` and ``bay``.
- All records are synthetic and marked as such; the SPD records set only
  the length, size and checksum bytes and a position marker. An unfitted
  PIMM or DIMM (``pimm-mask``, ``dimm-mask``) has no record and answers
  minus 104, where a real controller's wire form is unknown.
- The C-to-C and C-to-I links are in-process calls and carry no wire
  format, baud rate or detection. A C-brick has two links: ``peer`` for the
  other C-brick and ``io-peer`` for its I-brick. Both are checked against the
  two profiles: a C-brick pairs only with a C-brick, and ``io-peer`` joins a
  C-brick to an I-brick, N-brick or V-brick. The check holds whether the
  link is given with the device (it runs when the device realizes) or set
  afterwards (it runs when the link is set). An I-brick takes one C-brick:
  the fitted system's cabling of a second is unknown, so a second is
  refused with an error, and the first link stays. An N-brick takes
  several. The model does not count its connectors. After ``reboot_l1`` a brick answers no peer for one second of
  virtual time.
- An I-brick at mains is off with its bridges in reset, and has no
  power-up task of its own (*software expectation*, firmware reading). It powers
  on its own L1's ``power up`` or the front On/Off switch (the
  ``power-button`` input, a rising edge), unless setting 0x77 disables the
  switch, which the ``power-button-disabled`` property stands for
  (*software expectation* for the code path; that input bit 0 is the switch is
  *inferred*; the disabled switch's message is not modelled). In a
  machine its C-brick's auto power-up ends in ``* power up``,
  *inferred*, which reaches the I-brick, so it starts powered, logging
  ``power up (COMMAND)``. With ``auto-power`` off it starts unpowered.
  Its ``autopower`` command stays refused.
- An I-brick drives one ``reset-hold`` line, high while its bridges are
  held in reset (also the ``reset-hold`` property). Power-down raises
  it first; ``power up`` releases it after the supplies are on, ``power
  up hold`` leaves it raised; ``reset`` raises and releases it, ``reset
  hold`` leaves it raised, and ``reset`` is refused with ``power appears
  off`` while the brick is off (*software expectation*). The three bits are the
  bridges' resets by the expander's own label; which chip pins they reach
  is *unknown*, so in a machine the one line is the XBridge chip's
  ``reset-hold`` input and stands for all three (*approximation*). The
  L1's ``node-power`` output is the chip's ``main-power`` input, so a brick
  that is off or held leaves the bridge answering nothing and its C-brick's
  II link reading down, while the C-brick's processors and L1 run on. A
  ``reset`` raises the hold, which restarts the bridge cold, then releases
  it, so the ``node-reset`` pulse that follows reaches a held chip and does
  nothing more. The chip takes the L1's levels when the machine is built, so
  a brick the L1 starts off starts with the bridge held, and a restored
  machine takes them from the L1's saved state. The supplies'
  sequencing delays are not modelled.
- An R-brick L1 (``brick-type=router`` with ``router-board=rbrick``) is
  off at mains with its router in reset and accepts ``power up``,
  ``power up hold``, ``power down``, ``reset`` and ``reset hold`` from any
  request source, drives the same ``reset-hold`` line and takes the same
  front switch (*software expectation*, firmware reading). Its ``reset`` is not
  refused while off: it holds the router in reset and releases it only if
  a supply is on. It opens no RS-422 peer port, so ``peer`` and
  ``io-peer`` refuse it, and ``*``, ``ctc`` and ``cti`` are refused on it
  (*software expectation*: it cannot forward). Nothing in the stock protocol
  powers a router without an L2, so ``auto-power``, on by default,
  starts it powered as a declared *approximation* of a machine that has
  powered it; with ``auto-power`` off it starts off and in reset.
  In a machine its ``node-power`` and ``reset-hold`` outputs are the
  router's ``power`` and ``reset-hold`` inputs, so the router is offline
  whenever its brick is off or held: the pin-level join is *inferred* from
  the R-brick sheet, which records the order of the supply and the hold but
  not the pins.
- A machine reset, which a guest restart is on this machine, does not restart an I-brick, N-brick or V-brick L1: the L1
  runs on standby power, independent of brick power
  :src:`108-0240-002 p. 2-36`, and keeps its log in NVRAM
  :src:`007-3938-001 p. 3`, *established*; that a node reset leaves its
  other state alone is *inferred*. Only realize and ``reboot_l1`` start
  it.
- ``power`` and ``reset`` on an I-brick act on that brick only; a C-brick
  keeps the single-node power and reset behaviour above. ``*`` runs a
  command on this brick, the other C-brick and the I-brick in turn, each
  result headed by that brick's name. Which of two C-bricks is master is
  not modelled.
- ``select``, ``nmi``, ``softreset``, ``cpu``, ``pimm``, ``autopower``,
  ``ctc``, ``cti``, ``debug``, ``leds``, ``junkbus`` and ``*`` are refused
  on an I-brick, ``pci`` on a C-brick and ``router`` on both, with
  ``INFO: command not supported on this brick type``; other commands the
  guide lists but this model does not run are reported as unimplemented.
  The refusal text and the ``config`` line format are the model's choices.

The exact text and layout of each reply below are the model's own, whether
or not a manual prints a similar form. All are approximations. A console
ends each line with CR LF. A reply to a command interpreter request (task 3)
carries each line as its own field with no line ending; a line the model
ends with LF, as the Origin 300 profile's text does, keeps that LF inside
its field. The Origin 3000 refusals and the ``no response`` line below are
written CR LF-ended, so on a console, or relayed, they do not run into the
next brick's name line, and on task 3 they arrive as separate fields.

.. list-table::
   :header-rows: 1
   :widths: 34 36 30

   * - Choice
     - Limit
     - Printed forms
   * - ``config`` prints ``N: name`` per brick, ``N`` being 0 for this brick
       and then the cable's port, in ascending order. It lists only bricks
       on this brick's own links.
     - A C-brick with no I-brick of its own omits the other C-brick's I/O
       brick, which the guides imply is listed. The port set is the
       configured one.
     - ``:0 - 003c01`` style lines (:src:`007-3938-006 p. 25`); the
       model's spacing, punctuation and order match none of them.
   * - ``*``, ``ctc`` and ``cti`` print the target's name and a colon on its
       own line, then its output. ``*`` runs on this brick, the other
       C-brick and the I-brick, in that order, and prints a block only for
       a brick that is cabled.
     - The order is the model's: the guides print two different orders
       (:src:`007-3938-001 pp. 17-18`; :src:`007-3938-006 p. 27`).
       The typed-at brick's own command runs last and the others first, as
       the firmware hands its remote requests to the router before it runs
       its own (*software expectation*); the replies still print own answer first.
       ``*`` does not reach the other C-brick's I-brick, which the guides
       imply it should. ``ctci`` is not implemented: it is reported as an
       unknown command and nothing runs. The other C-brick runs the plain
       command itself, so a ``*`` ``power`` or ``reset`` acts through that
       brick's own handling and may request the machine action again; the
       model does not propagate between bricks.
     - Name, then output (:src:`007-3938-006 p. 27`).
   * - An attached brick with no rack and slot is absent in ``0x000c`` and
       ``0x000d`` and shown as ``---i--`` or ``---c--`` in ``config``.
     - What a controller returns for an unnamed attached brick is not
       published.
     - ``config`` shows the unnamed brick (:src:`007-4240-002 p. 112-113`).
   * - The partition is limited to 16 bits, as a property, by ``brick
       partition`` and by the request that sets it (*approximation*).
     - The manuals name no width; this is the width of the stored value.
     - None.
   * - ``cti`` with no I-brick prints ``INFO: no I/O brick is attached.``
       and fails.
     - The text is the model's.
     - None.
   * - A ``cti``, ``ctc`` or ``*`` run on a brick that was itself reached by
       a relay is refused: ``INFO: nested cti is unsupported.``, ``INFO:
       nested ctc is unsupported.`` or ``INFO: nested * is unsupported.``.
       A ``*`` that names another ``*`` is refused the same way. Nothing
       runs, and the relay depth is one.
     - Whether a real L1 relays a command back through its peer is not
       published. Without the bound, each extra ``*`` doubled the work. The
       refusals carry no brick-name prefix, where the guide prints one for
       a remote reply (:src:`007-3938-001 p. 18`).
     - None.
   * - A relay to a restarting brick prints its name, then ``no response``.
     - The interval is one second of virtual time, chosen by the model;
       the layout and words differ from the printed form.
     - ``003c01 ERROR: no response from ---i--`` answers the restart
       command itself (:src:`007-3938-001 p. 24`).

.. _sgi-l1-cxbrick:

Origin 3900 Cx-brick profile
============================

``brick-type=cxbrick`` models the one L1 of a Cx-brick, the Origin 3900
and Onyx 3000 brick that holds one to four IP53 node boards behind a single
controller on its router board (*SGI Origin 3900 Server User's Guide*
:src:`007-4653-001 p. 52-60`; *Cx-brick Addendum*
:src:`007-4572-001 p. 2-18`). ``boards`` names the fitted node boards
and ``compute-boards`` those with processors, one bit per board 0 to 3. Each fitted board reaches
the controller through its own UART window, ``sgi-l1-uart`` with
``transport-index`` the board number. Identity, naming and restart follow
the C-brick profile above: a brick starts unnamed and prints ``---c--``
(:src:`007-4653-001 p. 144`).

Request codes, the request layout and the framing follow the C-brick rule.
The profile serves the codes the Linux v2.6.0 ``ksys/l1.h`` header names
with the Origin 300 meaning (``0x0001`` to ``0x0009``, ``0x1002`` to
``0x1007``), the I/O link speed ``0x000a`` that the stock PROM reads from
its node's own L1, and the Origin 300 codes ``0x000e``, ``0x2001``,
``0x2005`` and ``0x2006``. Every other code answers the unknown-request code and is logged
as unimplemented, the attached-brick requests ``0x000c`` and ``0x000d``
among them. ``0x2001`` resets, as on the Origin 300; the public header gives
``0x2001`` to PCI power-up and ``0x2000`` to the full reset, so which a
fitted Cx-brick uses is *unknown*, and each ``0x2001`` is logged as such.

.. list-table::
   :header-rows: 1
   :widths: 30 12 58

   * - Fact
     - Rating
     - Basis
   * - One L1 per Cx-brick, holding the brick's module and partition
       identifiers, one display and one log.
     - established
     - :src:`007-4426-001 p. 12`; :src:`007-4653-001 p. 108, 112`.
   * - The console names twenty subchannels: ``0A`` to ``3D`` and
       ``console0`` to ``console3``, the number being the node board.
     - established for the names; inferred for ``console2``,
       ``console3`` and the board numbering
     - 007-3938-004 p. 16-18; :src:`007-4653-001 p. 114-115`.
   * - ``select`` at power-on prints ``console input: 001c07 console0``.
     - established
     - :src:`007-4653-001 p. 114-115`.
   * - ``select filter on`` passes only the selected target's output.
     - established at brick level; inferred for a board inside a brick
     - :src:`007-4653-001 p. 115`.
   * - ``power`` acts on the whole brick; ``reset``, ``softreset`` and ``nmi``
       act on the whole brick or on one node, as described in
       :ref:`sgi-l1-node-control`.
     - inferred for the manual's units; software expectation for the node
       commands
     - Every printed unit is the brick (:src:`007-4653-001 p. 58, 108, 113`).
   * - How the four Bedrocks reach the L1, the integers a Cx-brick uses on
       the wire for the twenty names, its type character and its record
       layout.
     - unknown
     - No Cx-brick source describes them.

What the model chooses, and the limit of each choice:

- Each board's window is the Origin 300 UART and packet path. A reply goes
  back only on the window that asked, and on each window subchannels 0 to 3
  are that board's processors A to D and 4 its console. A shared line or a
  path through the router could interleave traffic in ways this does not
  show.
- Each window keeps its own port field: ``0x000e`` answers the board number
  after L1 power-on and ``0x1002`` rewrites only the asking window's field,
  by the Origin 300 rule. A real L1 may keep one value per brick.
- The console selection is the brick's: a board and a subchannel, changed
  by ``select`` and by ``0x1003``, which moves it to the volunteer's console
  subchannel. Console output on a window does not move it, as it does on the
  Origin 300. It starts on board 0's console, or on the lowest board with
  processors when board 0 has none; a real L1 may keep ``console0`` and time
  out. The filter is cleared at L1 power-on.
- ``select`` refuses a board that is not fitted with ``ERROR: bad subchanel
  selection.`` and accepts one without processors. Console mode toward a
  board without processors prints the guide's ``no response from 001c07
  junk bus console UART:UART_TIMEOUT``, or for a processor subchannel
  ``no response from 001c07 Junk bus CPU2B system not responding``
  (:src:`007-4653-001 p. 115, 141`), and stays in L1 mode.
- Power requests from any window, and from the prompt, act on every board
  at once and log one entry; per-board power is not offered. A reset, soft
  reset or NMI reaches every present, enabled node, or the one node a
  command names (:ref:`sgi-l1-node-control`). The machine's own reset
  carries out a brick-wide ``reset``, so the router is reset with the rest,
  where the profile's contract keeps the router's state. ``nmi`` pulses one
  ``nmi`` line per node, and only boards with processors are wired to
  them.
- Request ``0x0006`` follows the 38-entry component table of the L1
  firmware's Cx-brick personality: component 0 the power entry module's
  inlet board, 1 the router board, then a node board at 2, 11, 20 and 29,
  each followed by its eight DIMM SPDs in the order 0, 2, 4, 6, 1, 3, 5, 7.
  The firmware numbers a request by the port it arrived on. On board ``n``'s
  window component 0 is table entry 2 + 9n (its node board), 1 is 3 + 9n (its
  DIMM 0 SPD, not the router), and ``c`` of 2 or more is ``c`` + 9n, so 3 to
  10 are its own DIMMs and larger numbers reach other boards; 38 and up
  answer minus 104, as do the entries of an unfitted board. With
  ``cpu-context-present`` each node board's record carries the CPU context
  the Origin 350's carries, which the stock PROM compares with its flash.
  Approximations:

  - Link ``n`` is node board ``n``'s port. The numbering is a *software expectation* from
    the firmware; the port to board mapping is *inferred*.
  - The router's own port is the router window: a router-profile L1 behind
    the window answers component 1 with the ``ROUTER`` record (name
    inferred, part ``NOTOBSERVED0`` and revision ``00`` not observed,
    synthetic serial) and components 0 and 2 up with minus 104. The brick's
    own L1 has no router port, so it serves neither the router record nor
    the power inlet.
  - The power inlet is absent: its record's contents are unknown and the
    stock PROM never asks for it.
  - SPDs exist only for the DIMMs the catalogue's 2048 MiB per node needs,
    the two 1 GiB DIMMs in slots 0 and 1. Each has a valid checksum, byte 13
    zero (the PROM's memory self-test decides the width) and byte 31 the
    1 GiB rank density, which the PROM uses only when its memory probe finds
    nothing. Type and timing bytes stay zero and are not interpreted. The
    population of a real Cx-brick is unknown. Byte 99 marks the board.
- Board ``n``'s window answers the identity requests with the brick's slot
  plus ``n``, because the stock PROM refuses two modules at one slot. Request
  ``0x0003`` returns the personality letter ``6`` (``brick-type-code``
  overrides it), and ``brick`` prints it. Request ``0x0004`` returns the
  serial of the board record the port names: node ``n``'s synthetic serial
  on link ``n`` and the ``ROUTER`` record's on the router window, which is
  also the brick's serial; the ``brick-serial`` property does not reach the
  guest. The serials are the model's synthetic ones.
- Request ``0x0005`` answers the firmware revision ``revision-*``
  configures, 1.44.0 in the Origin 3900 catalogue entries as on the other
  brick profiles, and minus 104 without one. ``version`` prints it as
  ``L1 1.44.0 (Image EMU-UNKNOWN)``. That a fitted Cx-brick runs 1.44.0
  is *inferred*, as for the other bricks.
- Request ``0x000a`` answers as on the C-brick profile, 400 MHz unless
  ``port-speed-first`` says otherwise, which matches the I-brick the
  Cx-brick's L1 reaches. What a fitted Cx-brick answers is *unknown*.
- A neighbouring Cx-brick link is refused. The L2, the neighbouring
  Cx-brick and the ``iia`` to ``iid``, ``nia`` and ``nib`` commands are not
  modelled. One I-brick may be named as ``io-peer``: the attached I/O
  brick request then goes to it in the C-brick's request form, because the
  addressing of a Cx-brick's four I/O ports is unknown. Without a peer an
  attached-brick destination answers the router's error reply.

.. _sgi-l1-router-role:

Router role
===========

The R-brick and the Cx-brick keep a router role in the controller's settings
(setting ``0x3c``, bits 7:4 of byte ``+0x17`` of the first settings record).
The model keeps the four-bit value as ``router-role``, which any writer can
set to any value, and shows it with the ``router`` command (also ``rtr``).

.. list-table::
   :header-rows: 1
   :widths: 40 20 40

   * - Fact
     - Rating
     - Basis
   * - The legal values are ``0xf`` ordinary, ``0xe`` meta and ``0xc``
       repeater. The console offers the three words ``ordinary`` or
       ``ord``, ``meta`` and ``repeater`` or ``rep`` and cannot store
       anything else. A bare ``router`` prints ``<series>: router type is
       <role>``, or ``router type is unknown (0x..)`` for an illegal value,
       and a change prints ``router type changed, effective on power up or
       reset``. The series word comes from persistent setting ``0x3d``,
       bits 2:1 of byte ``+0x17`` of the first settings record, so only
       ``NEW``, ``UNDECIDED``, ``3400 Series`` and ``3800 Series`` (0 to 3)
       can be read back, each padded to eleven characters; the image's
       ``INVALID`` is unreachable through it. The model keeps the setting as
       the ``router-series`` property, 0 by default, which any writer can set
       and which migrates; no command sets it, and who writes it in the
       image is *unknown*. Two service mode lines come first in the image
       while the service flag (setting ``0x3e``) and an expiry time are set;
       the model prints neither. The model offers the command on the
       R-brick and the Cx-brick; the image also offers it on other brick
       types the model does not have.
     - software expectation for the values, the command and its texts;
       approximation for the missing service lines
     - :src:`l1-firmware-1.44.0-mips router console command`
   * - A role never written reads as ordinary.
     - software expectation
     - :src:`l1-firmware-1.44.0-mips router role start-up check`
   * - Any other stored value is replaced by ``0xf``, and stored back, both
       when the R-brick's router is released from reset and in the start-up
       record initialiser, which runs on the R-brick and on the Cx-brick
       (personality ``6``). The model's ``router-role`` is repaired at the
       same two points.
     - software expectation
     - :src:`l1-firmware-1.44.0-mips router role start-up check`
   * - The role reaches bits 7:4 of the ROUTER CONTROL expander only when the
       R-brick releases its router, whose cold-start byte is ``0xf1``. A
       ``router`` change reaches the stored value alone: it takes effect at
       the next release and never writes the pins. A hold writes nothing.
       The model keeps only bits 7:4 of that byte, as ``router-control``,
       with the low nibble held at ``0x1`` where the image's other bits
       move during the release sequence; it is written at every release the
       model performs, among them ``reset``, ``power up`` and the power-up
       at start with ``auto-power``. Which router input each bit reaches,
       and the polarity, is *unknown*, so the byte drives nothing.
     - software expectation for the release; unknown for the pins
     - :src:`l1-firmware-1.44.0-base R-brick release routine`; the same routine is in :src:`l1-firmware-1.44.0-mips R-brick release routine`
   * - The start-up initialiser copies the validated nibble into word
       ``+0x2c`` of the controller's own table entry, which the L2 receives
       whole in the configuration reply. That word is the start-up copy: a
       later ``router`` change reaches it at the next controller start. The
       model's R-brick has no rack and slot to name itself with, so it
       offers no table entry; the Cx-brick's entry carries the word.
     - software expectation for the copy; inferred that nothing else reads it
     - :src:`l1-firmware-1.44.0-mips router role start-up check`
   * - Metarouter levels (the values ``0xa``, ``0x6`` and ``0x2`` that
       another L1 package stores) are illegal here and become ``0xf``.
     - software expectation
     - :src:`l1-firmware-1.44.0-mips router role start-up check`

The role, the table-entry word and the byte last written to the router
control expander migrate in the ``sgi-l1/controls`` section, with the
Cx-brick's node-disable mask once that exists. The section is sent only when
one of them differs from a cold start, so the stream of a default controller
of any profile carries none and still loads into a binary without it.

.. _sgi-l1-node-control:

Cx-brick node control
=====================

The Cx-brick L1 (personality ``6``) holds one reset, soft reset and NMI line
for each node, where the C-brick's one control block serves both of its
nodes. The commands below are those of the personality's per-node hooks
(*software expectation* throughout, from a reading of the L1 firmware 1.44.0
package that holds both personalities: :src:`l1-firmware-1.44.0-mips
compute-brick node reset and disable`). Which expander bit each hook drives
and which :term:`Bedrock` input it reaches is *unknown*, so the model keeps
only the pulses.

.. list-table::
   :header-rows: 1
   :widths: 30 70

   * - Command
     - Model
   * - ``reset``, ``reset hold``, ``0x2001``
     - Brick-wide: the node-reset pulse that reaches every node of the brick
       when none is disabled. With a node disabled, each present, enabled
       node gets its own ``board-reset`` pulse and the disabled one is
       skipped. The machine joins ``board-reset`` to that node's Bedrock
       alone; that join has no machine-level test.
   * - ``reset <n>``
     - The node's own ``board-reset`` pulse, a disabled node's too: the
       image's handler bypasses the iterator that skips disabled nodes. The
       image holds the node and leaves it held, where the model's hold is
       the pulse, an *approximation*. The handler takes 0 to 4 although the
       node count is 4. Index 4 addresses a fifth node record, which is
       record 0 of the next variant of a static table (identical to record 0
       of any variant except for one byte that differs between variants 7
       and 8) and so names node 0's expanders and type setting, and a fifth type byte one past the
       four-byte type table, in RAM that nothing was found to write. The
       hold compares that byte with setting ``0x56`` and, only when a
       margin reset then succeeds, stores it there; it then drives node 0's
       reset lines (on the last variant, lines chosen by index 0). Which
       variant a brick selects comes from a character code whose source was
       not traced. The reading is a *software expectation* for the table layout
       and the hold's reads; the effect, a second hold of node 0 that may
       overwrite its stored type with 0, is *inferred*. The model does
       nothing for index 4 and logs it, an *approximation*. Above 4 it
       prints ``ERROR: invalid NODE index N.`` An absent node does nothing.
   * - ``softreset``, ``softreset <n>``, ``nmi``, ``nmi <n>``
     - Brick-wide: every present, enabled node in turn. With a number: that
       node alone. A number of 4 or more prints ``ERROR: Node:invalid node
       number`` (code -4201), an absent node ``ERROR: Node:node not
       present`` (-4204) and a disabled one ``ERROR: Node:node disabled``
       (-4203). The texts and codes are a *software expectation* of the
       image's error table; which text a node that is both absent and
       marked disabled gets was not followed, so the model reports it absent
       (*approximation*). A soft reset is a pulse on that
       node's ``board-soft-reset`` line, whose ten-tick width the model does
       not keep, and an NMI is a pulse on its ``nmi`` line, cleared then set
       in the image.
   * - ``node d``, ``node e``, ``node <n> d``, ``node <n> e``
     - Set or clear bit ``n`` of the persistent node-disable setting, or the
       bit of every present node when no number is given. Refused while the
       brick is powered with ``ERROR: Node:power must be down to
       enable/disable NODEs`` (-4202), checked before the index, then with
       the two node errors above. No line is driven. A disabled node is
       skipped by a brick-wide ``reset``, ``softreset`` and ``nmi``. Only
       this argument order parses in the image: ``node d <n>`` matches no
       row and prints ``INFO: command not supported on this brick type``,
       as the model does. Bare ``node`` and ``node status`` print a status
       table the model does not offer; it logs and prints nothing. The
       model keeps the bits across a controller restart and migrates them.
   * - Exit status
     - The -1 that each refused command returns is the model's choice, an
       *approximation*: the image's codes are the ones above and its
       console prints the texts.

The model keeps the node-disable mask as the ``node-disable`` property, a
writable 8-bit value that any writer can set; it accepts bits above the four
nodes. Every use, the brick-wide reset path and the test of whether the
``sgi-l1/controls`` migration section is sent included, reads it through the
fitted boards, so those bits do nothing: a brick with only such bits set
takes the brick-wide reset and sends no section. No source says what a
stray bit means, so this is the model's choice, an *approximation* (the tests
``sgi-l1-cxbrick/node/disable-mask`` and ``sgi-l1-o3k/migration/stray-disable-bits``
check it).
``sn1/cross-node/cx-node-reset/*`` sets the property on an Origin 3900
machine, where ``reset <n>`` and a brick-wide ``reset`` are checked to
restart only the intended boards.

The firmware's node-disable hook drives no line, so a disabled node's power-up
is not changed by it: the model leaves power alone. What the setting does to
the scan list the firmware builds at release, which shows a disabled node as
state 0, and what state a skipped node's reset lines are left in, are
*unknown*. The firmware's ``Node Disabled`` progress line belongs to a
different brick type (``7``), which the model does not have, and to no
Cx-brick.

On a C-brick (types ``C`` in packages 0 and 2) no hook reads a node number:
``softreset <n>`` is ``softreset`` and ``nmi <n>`` is ``nmi``, one pulse of
one control line for the brick, and ``reset <n>`` and the ``node`` family are
refused with ``INFO: command not supported on this brick type``. The C-brick
has no node disable. The model's peer link joins C-bricks only:
``l1_peer_fault()`` in ``hw/misc/sgi-l1.c`` refuses a Cx-brick at either end
with ``L1 cxbrick profile models no peer link``, and
``sgi-l1-cxbrick/no-peer`` checks that refusal. The two callers of
``l1_peer_reset()`` are the ``* reset`` broadcast, which acts through that
link, and the I-brick and R-brick ``reset``, so the function never pulses a Cx
peer's node-reset line and needs no disabled-node skip.

The C-brick's one per-part disable is ``cpu d`` and ``cpu e``, for every
processor or one letter ``a`` to ``d`` (*software expectation*). The command is
refused with ``ERROR: CPU:power must be down to enable/disable CPUs``
(code -2402) unless every supply is off, and success prints nothing. The
image checks, in order: the brick class, the processor count, the parse of
the word, the supply state, the index against the count (-2401, ``invalid cpu
number``), the presence of the disable hook (-2400, ``cpu commands are not
supported on this brick type``), the hook, and only then the stored bit; with
no word, each present processor goes through the last three. That order was
read instruction by instruction in the Origin 300 package, and the helpers
carry the same constants in packages 0 and 2 without a line-by-line
comparison, so it is a *software expectation* with the equality for packages
0 and 2 *inferred*. The model's order is the word, then the supply, and it
has the four processors a to d, so the bound and the hook never fail. A bad word prints the
image's ``ERROR: invalid CPU (valid format is ...)`` line; the digit form is
refused as a bad word, *inferred*. The choice is one bit per processor of the
persistent settings, which the board initialisation applies again to each
processor; the model keeps the mask across a controller restart and migrates
it, and that the image runs the initialisation at every controller start is
*inferred*. The processor-module power bits it drives are not modelled, so the
mask drives nothing. The ``cpu`` status table is not modelled.

None of these commands, nor ``node d`` and ``node e``, reaches the routine
that sends a power-state notice or any other event, so the L2 learns of none
of them (*software expectation*, from the call structure; whether an L2
learns of a disabled node some other way is *unknown*). A brick-wide ``reset``
can send other events in the image, the console subchannel notice when a
nonzero subchannel was selected and the margin event when a module type
changed; the model sends neither, an *approximation*.

The Origin 3900 catalogue seats node board ``n`` on router port ``n`` + 2,
a model choice (*inferred*). The L1's table of router port records is
ordered ``PORT A``, ``INT N1``, ``INT N2``, ``INT N0``, ``INT N3``,
``PORT F``, ``PORT G``, ``PORT H``, with one-bit masks that fit "position
``k`` is router port ``k``"; the external names then land on ports 1, 6, 7
and 8, which agree with the connector labels for NUMAlink ports 1, 8, 7 and
6 at A, H, G and F (:src:`007-4572-001 p. 11`, printed page). That puts node
1 on port 2, node 2 on 3, node 0 on 4 and node 3 on 5. The order is
*inferred* from the mask pattern and the labels and is not stated by any
source; it agrees with the catalogue for node 3 only. The L1 model uses
neither.
