.. SPDX-License-Identifier: BSD-3-Clause

.. _sgi-running-ip30:

Running the Octane and Octane2
==============================

The ``octane`` and ``octane2`` machines build one IP30 board with one
processor (:doc:`ip30`). They boot a stock IP30 :term:`PROM`, given with
``-bios`` or bound as ``node0-flash``. A guest reaches its kernel through
the PROM, because the machine refuses ``-kernel``.

No functional test runs the stock PROM or a guest on either machine; see
:ref:`sgi-running`. What has been seen on the model is under
:ref:`sgi-ip30-launch`.

Selections
----------

.. sgi-run:: machines octane

.. sgi-run:: machines octane2

``-m`` takes any memory size from the table.

.. sgi-run:: facts octane

.. sgi-run:: facts octane2

Inputs and devices
------------------

.. sgi-run:: inputs octane

.. sgi-run:: inputs octane2

Start the PROM
--------------

With the PROM image ``ip30prom.bin``, the console on the terminal and the
default graphics head fitted but not displayed::

    qemu-system-mips64 -M octane2 -m 256M -bios ip30prom.bin \
        -display none -serial stdio -monitor none

``-M octane`` starts the Octane the same way, with its own default
processor and memory.

Add a disk and a network
------------------------

The boot disk attaches to the first onboard SCSI bus, ``scsi.0``, at a
target the table above lists. ``-nic user`` attaches the onboard Ethernet
to QEMU's user-mode network::

    qemu-system-mips64 -M octane2 -m 256M -bios ip30prom.bin \
        -drive if=none,id=d0,file=disk.img,format=raw \
        -device scsi-hd,bus=scsi.0,scsi-id=1,drive=d0 \
        -nic user -display none -serial stdio -monitor none

Choose the graphics head
------------------------

Each machine fits one default head. ``-vga none`` removes it, and a
``-device`` fits another into a slot the table lists. The command below
replaces the Octane's default IMPACT board with one that has a texture
engine, in the slot the default occupied::

    qemu-system-mips64 -M octane -m 128M -bios ip30prom.bin -vga none \
        -device sgi-mgras,slot=widget-11,product=si-t \
        -display none -serial stdio -monitor none

A QEMU display backend such as ``-display gtk`` in place of
``-display none`` shows the head. The products each family offers, and
what the model leaves out of each, are on :doc:`hardware/graphics`.

.. sgi-run:: graphics octane

.. sgi-run:: graphics octane2

Keep the flash and NVRAM
------------------------

These two commands create the stores and start the machine on them. For
the Octane2; the Octane differs only in its selection:

.. sgi-run:: launch octane2
