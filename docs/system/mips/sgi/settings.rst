.. SPDX-License-Identifier: BSD-3-Clause

.. This page is generated from hw/mips/sgi/sgi-machines.json by
   scripts/sgi_model/settings_doc.py. Edit the generator, not this
   page.

.. _sgi-settings:

Machine settings
================

A launch gives a machine input as ``NAME[.KEY]=VALUE``. Each
product's tables list the settings its offerings take: the kind of
value, the keys of a slotted setting, the QEMU property the value
sets, and the default, with the mark of how well the default is
known (see `Where the defaults come from`_). A setting with no
default and no mark is optional; one marked required must be
given. Where an offering of
the product offers a setting under other keys (the Origin 300's
chassis count, say), the keys shown are the product's first
offering's.

``fuel``
--------

.. list-table::
   :header-rows: 1

   * - Setting
     - Kind
     - Slots
     - Applied as
     - Default
   * - ``board-id-word``
     - integer
     - none
     - -M ``board-id-word``
     - ``16388`` (approximated)
   * - ``bedrock-revision``
     - integer
     - none
     - -M ``bedrock-revision``
     - none
   * - ``l1-type-code``
     - integer
     - none
     - -M ``l1-type-code``
     - none
   * - ``l1-revision``
     - text
     - none
     - -M ``l1-revision``
     - none
   * - ``ioc3-subsystem-id``
     - integer
     - none
     - -M ``ioc3-subsystem-id``
     - none
   * - ``prid``
     - integer
     - none
     - -cpu ``prid``
     - ``0xf23`` (inferred)
   * - ``chassis-eeprom``
     - file
     - board: 0
     - -M ``chassis-eeprom``
     - none
   * - ``board-eeprom``
     - file
     - board: 0
     - -M ``board-eeprom``
     - none
   * - ``fuel-mac-eeprom``
     - file
     - none
     - -M ``fuel-mac-eeprom``
     - none

Group ``fuel-node-records``: all-or-none of ``chassis-eeprom``, ``board-eeprom``.

``o2``
------

.. list-table::
   :header-rows: 1

   * - Setting
     - Kind
     - Slots
     - Applied as
     - Default
   * - ``password-clear-input``
     - enum (0, 1)
     - none
     - -M ``password-clear-input``
     - required
   * - ``rom-at29c040a-protection``
     - enum (disabled, enabled)
     - none
     - -M ``rom-at29c040a-protection``
     - required
   * - ``rom-at29c040a-lower-lock``
     - enum (unlocked, locked)
     - none
     - -M ``rom-at29c040a-lower-lock``
     - required
   * - ``rom-at29c040a-upper-lock``
     - enum (unlocked, locked)
     - none
     - -M ``rom-at29c040a-upper-lock``
     - required
   * - ``identity-eprom-file``
     - file
     - none
     - -M ``identity-eprom-file``
     - none

``octane``
----------

.. list-table::
   :header-rows: 1

   * - Setting
     - Kind
     - Slots
     - Applied as
     - Default
   * - ``flash-select``
     - boolean
     - none
     - -M ``flash-select``
     - ``off`` (approximated)
   * - ``flash-strap``
     - boolean
     - none
     - -M ``flash-strap``
     - ``off`` (approximated)
   * - ``prid``
     - integer
     - none
     - -cpu ``prid``
     - ``0x927`` (inferred)

``octane2``
-----------

.. list-table::
   :header-rows: 1

   * - Setting
     - Kind
     - Slots
     - Applied as
     - Default
   * - ``flash-select``
     - boolean
     - none
     - -M ``flash-select``
     - ``on`` (approximated)
   * - ``flash-strap``
     - boolean
     - none
     - -M ``flash-strap``
     - ``on`` (approximated)
   * - ``prid``
     - integer
     - none
     - -cpu ``prid``
     - ``0xe35`` (inferred)
   * - ``fpu-id``
     - integer
     - none
     - -cpu ``fpu-id``
     - ``0x900`` (inferred)

``onyx2``
---------

.. list-table::
   :header-rows: 1

   * - Setting
     - Kind
     - Slots
     - Applied as
     - Default
   * - ``prid``
     - integer
     - none
     - -cpu ``prid``
     - ``0x926`` (inferred)
   * - ``dip-switches``
     - integer
     - none
     - -M ``elsc-dip-switches``
     - none

``onyx350``
-----------

.. list-table::
   :header-rows: 1

   * - Setting
     - Kind
     - Slots
     - Applied as
     - Default
   * - ``board-id-word``
     - integer
     - none
     - -M ``board-id-word``
     - ``8193`` (approximated)
   * - ``bedrock-revision``
     - integer
     - none
     - -M ``bedrock-revision``
     - none
   * - ``l1-type-code``
     - integer
     - none
     - -M ``l1-type-code``
     - none
   * - ``l1-revision``
     - text
     - none
     - -M ``l1-revision``
     - none
   * - ``prid``
     - integer
     - none
     - -cpu ``prid``
     - none
   * - ``chassis-eeprom``
     - file
     - board: 0
     - -M ``chassis-eeprom``
     - none
   * - ``board-eeprom``
     - file
     - board: 0
     - -M ``board-eeprom``
     - none

``onyx4``
---------

.. list-table::
   :header-rows: 1

   * - Setting
     - Kind
     - Slots
     - Applied as
     - Default
   * - ``board-id-word``
     - integer
     - none
     - -M ``board-id-word``
     - ``8193`` (approximated)
   * - ``bedrock-revision``
     - integer
     - none
     - -M ``bedrock-revision``
     - none
   * - ``l1-type-code``
     - integer
     - none
     - -M ``l1-type-code``
     - none
   * - ``l1-revision``
     - text
     - none
     - -M ``l1-revision``
     - none
   * - ``prid``
     - integer
     - none
     - -cpu ``prid``
     - none
   * - ``chassis-eeprom``
     - file
     - board: 0
     - -M ``chassis-eeprom``
     - none
   * - ``board-eeprom``
     - file
     - board: 0
     - -M ``board-eeprom``
     - none

``origin200``
-------------

.. list-table::
   :header-rows: 1

   * - Setting
     - Kind
     - Slots
     - Applied as
     - Default
   * - ``prid``
     - integer
     - none
     - -cpu ``prid``
     - ``0x926`` (inferred)
   * - ``dip-switches``
     - integer
     - none
     - -M ``elsc-dip-switches``
     - none
   * - ``junk-uart``
     - boolean
     - none
     - -M ``junk-uart``
     - none

``origin2000``
--------------

.. list-table::
   :header-rows: 1

   * - Setting
     - Kind
     - Slots
     - Applied as
     - Default
   * - ``prid``
     - integer
     - none
     - -cpu ``prid``
     - ``0x926`` (inferred)
   * - ``dip-switches``
     - integer
     - none
     - -M ``elsc-dip-switches``
     - none

``origin300``
-------------

.. list-table::
   :header-rows: 1

   * - Setting
     - Kind
     - Slots
     - Applied as
     - Default
   * - ``board-id-word``
     - integer
     - none
     - -M ``board-id-word``
     - ``32769`` (approximated)
   * - ``bedrock-revision``
     - integer
     - none
     - -M ``bedrock-revision``
     - none
   * - ``l1-type-code``
     - integer
     - none
     - -M ``l1-type-code``
     - none
   * - ``l1-revision``
     - text
     - none
     - -M ``l1-revision``
     - none
   * - ``ioc3-subsystem-id``
     - integer
     - none
     - -M ``ioc3-subsystem-id``
     - none
   * - ``prid``
     - integer
     - none
     - -cpu ``prid``
     - ``0xf24`` (inferred)
   * - ``chassis-eeprom``
     - file
     - board: 0, 1, 2, ... 7
     - -M ``chassis-eeprom``
     - none
   * - ``board-eeprom``
     - file
     - board: 0, 1, 2, ... 7
     - -M ``board-eeprom``
     - none
   * - ``spd-eeprom``
     - file
     - dimm: 0, 1, 2, 3
     - -M ``spd-eeprom``
     - none
   * - ``brick-serial``
     - text
     - none
     - -global ``sgi-l1.brick-serial``
     - none
   * - ``debug-switches``
     - integer
     - none
     - -global ``sgi-l1.debug-switches``
     - ``0`` (approximated)
   * - ``origin300-l1cmd-compat``
     - boolean
     - none
     - -M ``origin300-l1cmd-compat``
     - ``off`` (approximated)
   * - ``controller-brick-serial``
     - text
     - controller: 0
     - -M ``controller-brick-serial``
     - none
   * - ``controller-board-eeprom``
     - file
     - controller-component: 0/0, 0/1, 0/2, 0/3
     - -M ``controller-board-eeprom``
     - none

``origin3000``
--------------

.. list-table::
   :header-rows: 1

   * - Setting
     - Kind
     - Slots
     - Applied as
     - Default
   * - ``board-id-word``
     - integer
     - none
     - -M ``board-id-word``
     - ``2`` (approximated)
   * - ``bedrock-revision``
     - integer
     - none
     - -M ``bedrock-revision``
     - none
   * - ``l1-type-code``
     - integer
     - none
     - -M ``l1-type-code``
     - none
   * - ``l1-revision``
     - text
     - none
     - -M ``l1-revision``
     - none
   * - ``prid``
     - integer
     - none
     - -cpu ``prid``
     - ``0xe35`` (inferred)
   * - ``fpu-id``
     - integer
     - none
     - -cpu ``fpu-id``
     - ``0x935`` (inferred)
   * - ``l2-controller``
     - boolean
     - none
     - the serial lines that name it
     - ``off`` (approximated)

``origin350``
-------------

.. list-table::
   :header-rows: 1

   * - Setting
     - Kind
     - Slots
     - Applied as
     - Default
   * - ``board-id-word``
     - integer
     - none
     - -M ``board-id-word``
     - ``8193`` (approximated)
   * - ``bedrock-revision``
     - integer
     - none
     - -M ``bedrock-revision``
     - none
   * - ``l1-type-code``
     - integer
     - none
     - -M ``l1-type-code``
     - none
   * - ``l1-revision``
     - text
     - none
     - -M ``l1-revision``
     - none
   * - ``prid``
     - integer
     - none
     - -cpu ``prid``
     - ``0xf41`` (inferred)
   * - ``chassis-eeprom``
     - file
     - board: 0
     - -M ``chassis-eeprom``
     - none
   * - ``board-eeprom``
     - file
     - board: 0
     - -M ``board-eeprom``
     - none

``tezro``
---------

.. list-table::
   :header-rows: 1

   * - Setting
     - Kind
     - Slots
     - Applied as
     - Default
   * - ``board-id-word``
     - integer
     - none
     - -M ``board-id-word``
     - ``8193`` (approximated)
   * - ``bedrock-revision``
     - integer
     - none
     - -M ``bedrock-revision``
     - none
   * - ``l1-type-code``
     - integer
     - none
     - -M ``l1-type-code``
     - none
   * - ``l1-revision``
     - text
     - none
     - -M ``l1-revision``
     - none
   * - ``prid``
     - integer
     - none
     - -cpu ``prid``
     - ``0xf41`` (inferred)
   * - ``chassis-eeprom``
     - file
     - board: 0
     - -M ``chassis-eeprom``
     - none
   * - ``board-eeprom``
     - file
     - board: 0
     - -M ``board-eeprom``
     - none

Where the defaults come from
----------------------------

Every default carries the mark the manual gives the fact:
``established``, ``inferred`` or ``approximated``. The generator
refuses a default whose basis is not written down, so a launch
tool never shows a value with nothing behind it.

.. list-table::
   :header-rows: 1

   * - Setting
     - Marks
     - Where the value is read from
   * - ``board-id-word``
     - approximated, inferred
     - The topology's :term:`Bedrock` record in the model file, or for an Origin 3000 offering the board-id-word of the processor option it selects. The C-brick words are marked in hardware/bricks.rst; the other products' in the board identity inputs of ip35.rst and ip53.rst, whose product-code bits are a software expectation and whose low bits are a model choice.
   * - ``debug-switches``
     - approximated
     - The sgi-l1 device's own default; the stock :term:`PROM`'s read of the word is marked in hardware/l1.rst.
   * - ``flash-select``
     - approximated
     - The Octane board's flash record in the model file, a model choice; see ip30.rst.
   * - ``flash-strap``
     - approximated
     - The Octane board's flash record in the model file, a model choice; see ip30.rst.
   * - ``fpu-id``
     - inferred
     - The processor assembly in the model file, for the FPU revision the product's inventory prints; not a raw read.
   * - ``l2-controller``
     - approximated
     - A launch choice, off: both a 3200 with and without an L2 shipped (hardware/l1-l2-link.rst); with it off the L1s' links stay unbacked, which hardware/l1-l2-link.rst describes as the model running without an L2 peer.
   * - ``origin300-l1cmd-compat``
     - approximated
     - The machine property's own default, off; the commands it enables are an approximation (hardware/l1.rst).
   * - ``prid``
     - established, inferred
     - The processor assembly in the model file, for the part revision the product's inventory prints. The Origin 3000's R14000 values are marked in hardware/bricks.rst; no product's raw PRId was read.
