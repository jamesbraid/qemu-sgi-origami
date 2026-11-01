.. SPDX-License-Identifier: BSD-3-Clause

.. _sgi-running-ip32:

Running the O2
==============

The ``o2`` machine builds one IP32 board with one processor, the MACE
I/O, the GBE display and two onboard SCSI controllers (:doc:`ip32`). It
starts the boot :term:`PROM` image the user supplies. ``variant=o2plus`` selects
the O2+ with an R12000.

No test in this tree runs a stock IP32 PROM or a guest on the O2; qtests
cover its registers and its refusals (:ref:`sgi-running`).

Selections
----------

.. sgi-run:: machines o2

.. sgi-run:: facts o2

``o2,variant=o2plus`` refuses the default R10000, so it needs
``-cpu R12000`` and the R12000 properties described in
:ref:`sgi-ip32-o2plus`.

Inputs and devices
------------------

.. sgi-run:: inputs o2

The machine refuses to start without every one of its explicit inputs
and names the one that is missing:

- ``rom-image`` is the 512 KiB PROM image, with ``rom-size``,
  ``rom-offset`` and ``rom-pad-byte`` stating how it sits in the flash
  window and the three ``rom-at29c040a-`` properties stating the flash
  part's protection and block-lock state;
- ``password-clear-input`` is ``0`` or ``1``.

The identity chip is built from ``emu-id``, ``serial``, ``hostid`` and
``mac`` (:ref:`sgi-ip32-system-serial`), so it needs no input of its own.
``emu-id=42`` is an arbitrary example.

Start the PROM
--------------

With the PROM image ``ip32prom.bin`` and a SCSI disk on the first
onboard controller, ``aic0.0``::

    qemu-system-mips64 -M o2,rom-image=ip32prom.bin,rom-size=524288,\
    rom-offset=0,rom-pad-byte=255,rom-at29c040a-protection=disabled,\
    rom-at29c040a-lower-lock=unlocked,rom-at29c040a-upper-lock=unlocked,\
    emu-id=42,password-clear-input=0 \
        -m 1G -drive if=none,id=d0,file=disk.img,format=raw \
        -device scsi-hd,bus=aic0.0,scsi-id=1,drive=d0 \
        -display none -serial stdio -monitor none

The console is the first ``-serial``, MACE serial port A.

Graphics
--------

The GBE display is part of the board. It binds to a QEMU console, so a
display backend such as ``-display gtk`` shows it; there is no head to
choose (:doc:`hardware/gbe`).
