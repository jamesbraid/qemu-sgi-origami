.. SPDX-License-Identifier: BSD-3-Clause

.. _sgi-running-ip35:

Running the Origin 300, Fuel and Origin 3000
============================================

The ``origin300``, ``fuel`` and ``origin3000`` machines build
:term:`Bedrock` node boards and boot a stock IP35 :term:`PROM`
(:doc:`ip35`). ``-bios`` takes the firmware image the reader supplies, and
the machine refuses an image of the wrong size; :doc:`prom-image` says
what it accepts. The machines refuse ``-kernel``.

Given only ``-bios``, the PROM's writes to its boot flash do not persist
and it cannot get past its first reset (:ref:`sgi-ip35-firmware`). Start
these machines on stores that :doc:`/tools/qemu-sgi-machine-init`
creates, as the commands below do. The IP53 products are on
:doc:`running-ip53`.

On the Origin 300, :test:`origin300_prom` runs the stock PROM and
:test:`irix_origin300_installed_boot` boots an installed IRIX disk. On the
Fuel, :test:`irix_fuel_installed_boot` does. The Origin 3000 family has no
functional test (:ref:`sgi-running`).

Selections
----------

``-m`` is the total memory, split evenly across the nodes, and each node's
share must be one the table lists. ``-smp`` is the total processor count.

.. sgi-run:: machines origin300

.. sgi-run:: machines fuel

.. sgi-run:: machines origin3000

Inputs and devices
------------------

.. sgi-run:: facts origin300

.. sgi-run:: facts fuel

.. sgi-run:: facts origin3000

.. sgi-run:: inputs origin300

.. sgi-run:: inputs fuel

.. sgi-run:: inputs origin3000

A topology with more than one node adds ``node1-flash``, ``nvram1`` and
``nvram1-clock`` and so on; the generated commands below include every
store of the selection they show.

Start the PROM
--------------

For each machine's default selection, these pairs create the stores from
the PROM image ``ip35prom.img`` and start the machine on them with the
console on the terminal. The console is the first ``-serial``; the
inputs tables above list the serial lines that follow it.

.. sgi-run:: launch origin300

.. sgi-run:: launch fuel

.. sgi-run:: launch origin3000

Boot a disk
-----------

The boot disk attaches to the first onboard SCSI bus, ``scsi.0``. The
installed-IRIX tests above attach their disk there, at target 1, with the
boot flash bound as a pflash drive instead of the stores below. This
command starts an Origin 300 on the stores with such a disk, which the
reader installed::

    qemu-sgi-machine-init --machine origin300,topology=origin300,nodes=1 \
        --boot-prom ip35prom.img state

    qemu-system-mips64 \
        -M origin300,topology=origin300,nodes=1,node0-flash=node0-flash,\
    nvram0=nvram0,nvram0-clock=nvram0-clock \
        -cpu R14000 -smp 4 -m 512M \
        -blockdev driver=file,filename=state/node0-flash.raw,node-name=node0-flash \
        -blockdev driver=file,filename=state/nvram0.raw,node-name=nvram0 \
        -blockdev driver=file,filename=state/nvram0-clock.raw,node-name=nvram0-clock \
        -drive if=none,id=d0,file=disk.img,format=raw \
        -device scsi-hd,bus=scsi.0,scsi-id=1,drive=d0 \
        -nic user -display none -serial stdio -monitor none

The catalogue lists no graphics head for the Origin 300. The tables
below list the heads the Fuel and the Origin 3000 topologies fit.

Graphics
--------

.. sgi-run:: graphics fuel

.. sgi-run:: graphics origin3000
