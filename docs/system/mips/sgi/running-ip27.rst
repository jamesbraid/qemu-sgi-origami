.. SPDX-License-Identifier: BSD-3-Clause

.. _sgi-running-ip27:

Running the Origin 200, Origin 2000 and Onyx2
=============================================

The ``origin200``, ``origin2000`` and ``onyx2`` machines build IP27-family
node boards (:doc:`ip27`). They boot a stock IP27 :term:`PROM`, given with
``-bios`` or bound per node as ``node<N>-flash``. The machines refuse
``-kernel``, so a guest reaches its kernel through the PROM's netboot or
a disk.

These are the machines with the most functional tests. On the Origin 200,
:test:`irix_installed_boot` boots an installed IRIX, :test:`linux_kernel`
runs a Linux kernel loaded through the PROM's netboot, and
:test:`openbsd_install` installs OpenBSD/sgi. On the Origin 2000 and
Onyx2, :test:`onyx2_deskside_prom` and :test:`onyx2_rack_prom` read the
hardware inventory from the PROM, and :test:`irix_diag_origin2000_one_node`
runs IRIX diagnostics on an Origin 2000. The full list is in
:ref:`sgi-running`.

.. sgi-run:: facts origin200

.. sgi-run:: facts origin2000

.. sgi-run:: facts onyx2

Selections
----------

``-m`` is the total memory, split evenly across the nodes. Each node's
share must be one of the values in the last column. ``-smp`` is the total
processor count and must be one the row lists. ``-cpu`` takes the part of
the processor module; a module of another part is picked with
``processor=``, and then ``-cpu`` names its part.

.. sgi-run:: machines origin200

.. sgi-run:: machines origin2000

.. sgi-run:: machines onyx2

Inputs and devices
------------------

.. sgi-run:: inputs origin200

.. sgi-run:: inputs origin2000

.. sgi-run:: inputs onyx2

The Origin 2000 and Onyx2 base I/O cards carry their own I/O PROM flash.
The ``io-prom`` machine property stores an I/O PROM image in every such
part with no drive bound. With neither it nor a drive, the IP27 PROM finds
no valid image there, reports it and runs its own internal copy
(:ref:`sgi-ip27-firmware`).

Start the PROM
--------------

Each command starts the stock PROM image ``ip27prom.img`` with the console
on the terminal. :test:`origin200_prom` runs the stock PROM on the Origin
200, and the two Onyx2 tests above run it on that machine. The Origin 2000
selection in the third command comes from the table, and no test starts
it. :ref:`sgi-ip27-running` lists what the PROM prints::

    qemu-system-mips64 -M origin200 -bios ip27prom.img \
        -display none -serial stdio -monitor none

    qemu-system-mips64 -M onyx2 -bios ip27prom.img \
        -display none -serial stdio -monitor none

    qemu-system-mips64 -M origin2000,topology=origin2000-deskside,nodes=2 \
        -smp 4 -m 512M -bios ip27prom.img \
        -display none -serial stdio -monitor none

Boot a disk
-----------

The boot disk attaches to the first onboard SCSI bus, ``scsi.0``, at
target 1 or another the table lists. The installed IRIX 6.5.30 tests run
the two-processor Origin 200 with this command, on a disk the reader
installed; the onboard Ethernet sits on QEMU's user-mode network::

    qemu-system-mips64 -M origin200 -cpu R10000 -smp 2 -m 64M \
        -bios ip27prom.img \
        -drive if=none,id=d0,file=disk.img,format=raw \
        -device scsi-hd,bus=scsi.0,scsi-id=1,drive=d0 \
        -nic user -display none -serial stdio -monitor none

OpenBSD/sgi 6.9 on the Origin 200 runs with 128 MiB (``-m 128M``). The
tests use no more, because the Origin 200 model does not back an
installed OpenBSD guest's memory map correctly above that size.

A dual-module Origin 200 is ``topology=origin200-dual`` with ``-smp 4``.
The serial lines of each node follow a fixed order
(:ref:`sgi-sn-serial-order`).

Keep the flash and NVRAM
------------------------

These pairs of commands create the stores and start the machine on them,
for each machine's default selection. The Origin 2000 and Onyx2 also take
the I/O PROM, ``io6prom.img``.

.. sgi-run:: launch origin200

.. sgi-run:: launch origin2000

.. sgi-run:: launch onyx2

Graphics
--------

Each machine's heads are in the table. An Origin 2000 fits an IMPACT board
in an :term:`XIO` slot. This command adds one to the two-node deskside::

    qemu-system-mips64 -M origin2000,topology=origin2000-deskside,nodes=2 \
        -smp 4 -m 512M -bios ip27prom.img -vga none \
        -device sgi-mgras,slot=io2,product=si \
        -display none -serial stdio -monitor none

An Onyx2 fits an InfiniteReality pipe by default. A display backend such
as ``-display gtk`` replaces ``-display none`` to show a head. The
families, and what the model leaves out of each, are on
:doc:`hardware/graphics`.

.. sgi-run:: graphics origin200

.. sgi-run:: graphics origin2000

.. sgi-run:: graphics onyx2
