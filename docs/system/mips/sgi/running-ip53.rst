.. SPDX-License-Identifier: BSD-3-Clause

.. _sgi-running-ip53:

Running the Origin 350, Onyx 350, Tezro and Onyx 4 host
=======================================================

The ``origin350``, ``onyx350``, ``tezro`` and ``onyx4`` machines are
serial software profiles on IP53 and IP59 node boards, each fixing a
processor, memory, :term:`PIC` and IO9 population (:doc:`ip53`). They boot
the same stock IP35 :term:`PROM` as the Origin 300 and start the same way:
on stores that :doc:`/tools/qemu-sgi-machine-init` creates
(:doc:`running-ip35`). The machines refuse ``-kernel``.

The topology name picks the node board: the ``-ip53`` and ``-ip59``
topologies are the two boards. The selections table lists them.

.. sgi-run:: facts origin350

.. sgi-run:: facts onyx350

.. sgi-run:: facts tezro

.. sgi-run:: facts onyx4

The only functional test that runs a guest on this family is
:test:`irix_io9_tg_perf`, on the Origin 350 with an installed IRIX disk
(:ref:`sgi-running`). Onyx 350, Tezro and Onyx 4 host have none; qtests
launch them.

Selections
----------

.. sgi-run:: machines origin350

.. sgi-run:: machines onyx350

.. sgi-run:: machines tezro

.. sgi-run:: machines onyx4

Inputs and devices
------------------

.. sgi-run:: inputs origin350

.. sgi-run:: inputs onyx350

.. sgi-run:: inputs tezro

.. sgi-run:: inputs onyx4

The console is the first ``-serial``; the tables above list the lines that
follow it, which include the :term:`IOC4` ports.

Start the PROM
--------------

For each product's default selection, these pairs create the stores from
the PROM image ``ip35prom.img`` and start the machine on them.

.. sgi-run:: launch origin350

.. sgi-run:: launch onyx350

.. sgi-run:: launch tezro

.. sgi-run:: launch onyx4

Boot a disk
-----------

The boot disk attaches to the first onboard SCSI bus, ``scsi.0``, as on
the Origin 300::

    qemu-sgi-machine-init \
        --machine origin350,topology=origin350-ip53,nodes=1 \
        --boot-prom ip35prom.img state

    qemu-system-mips64 \
        -M origin350,topology=origin350-ip53,nodes=1,\
    node0-flash=node0-flash,nvram0=nvram0,nvram0-clock=nvram0-clock \
        -blockdev driver=file,filename=state/node0-flash.raw,node-name=node0-flash \
        -blockdev driver=file,filename=state/nvram0.raw,node-name=nvram0 \
        -blockdev driver=file,filename=state/nvram0-clock.raw,node-name=nvram0-clock \
        -drive if=none,id=d0,file=disk.img,format=raw \
        -device scsi-hd,bus=scsi.0,scsi-id=1,drive=d0 \
        -nic user -display none -serial stdio -monitor none

Graphics
--------

None of these machines fits a head by default. The Onyx 350 and Tezro
take an optional V12 at the slot the table lists.

.. sgi-run:: graphics onyx350

.. sgi-run:: graphics tezro
