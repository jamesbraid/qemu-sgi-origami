.. SPDX-License-Identifier: BSD-3-Clause

.. _sgi-running:

Running the SGI machines
========================

These pages say how to start each SGI machine this tree emulates: which
``-M`` selections exist, what files the reader must supply, which devices
the machine fits and one command line that starts it. The platform pages
(:doc:`ip30`, :doc:`ip32`, :doc:`ip27`, :doc:`ip35`, :doc:`ip53`) say
what each machine is and how it was modelled. These pages say how to run
it.

What the reader supplies
------------------------

QEMU ships no SGI firmware and no guest software. A machine needs:

- the boot :term:`PROM` image of its node board, which the reader
  supplies. :doc:`prom-image` lists the images the machines are developed
  against and the sizes and formats each accepts. The Origin 2000 and
  Onyx2 optionally take the BaseIO I/O PROM image too;
- for a guest, a disk image on the machine's SCSI bus or a network boot
  from the PROM. The commands below use ``disk.img`` for a raw image
  whose guest the reader installed;
- for the O2 only, the flash protection states and password-clear input
  it requires (:doc:`ip32`).

File names in the commands (``ip27prom.img``, ``ip30prom.bin`` and so on)
are placeholders for the reader's own copies. They are the names the
automated checks substitute files for.

Two ways to start
-----------------

``-bios`` takes the PROM image directly and gives the machine blank
flash, NVRAM and clock. It is the shortest command, and the Origin 200,
Origin 2000 and Onyx2 start this way. Nothing the PROM writes survives
QEMU exiting.

:doc:`/tools/qemu-sgi-machine-init` creates the writable stores a machine
needs, one raw file each, from the machine catalogue. Binding them keeps
the PROM's settings across runs. The IP35 machines (Origin 300, Fuel, the
Origin 3000 family and the IP53 products) are started on them, because
the PROM writes its boot flash early in the first boot and the machine
drops those writes when no flash is bound. Each machine's
page shows the exact two commands for its default selection. They are
generated from the catalogue, so the store names and sizes are those the
machine requires.

The console is the first ``-serial``. ``-display none -serial stdio
-monitor none`` puts it on the terminal. The catalogue also lists the
other serial lines of each machine, in order. A graphics head is a
``-device``; with ``-display none`` nothing draws it on the host.

What has been shown to boot
---------------------------

A functional test boots a guest only where this table lists one. Where a
machine has no row, no test in ``tests/functional/mips64`` runs its stock
PROM or any operating system on it. That machine's register behaviour is
covered by qtests, and its platform page says what else has been seen on
the model. A model run is not evidence of what the hardware does.

Each test name is followed by the suite it is in. A quick test runs in
the quick suite. A thorough test belongs to the thorough set the nightly
run uses. The tests that boot firmware or a guest need the SGI asset
directory (``QEMU_TEST_SGI_ASSET_DIR``) holding the PROM and disk images
and skip without it, so a plain run of the quick suite exercises none of
the guest rows.

.. list-table::
   :header-rows: 1
   :widths: 18 82

   * - Machine
     - What a functional test requires
   * - Octane, Octane2
     - Nothing from the stock PROM or a guest. :test:`octane_flash_execution`
       runs a few synthetic instructions from the boot flash window.
   * - O2
     - Nothing; qtests only.
   * - Origin 200
     - The stock PROM boot in a matrix of populations
       (:test:`origin200_prom`) and with a GIGAchannel cabinet
       (:test:`origin200_gigachannel_prom`). An installed IRIX 6.5.30 disk
       reaches a root login, run level 2 and a clean shutdown
       (:test:`irix_installed_boot`), moves checked TCP and UDP payloads
       over the onboard Ethernet (:test:`irix_ef`) and powers off and
       reboots (:test:`irix_installed_power`). The miniroot kernel
       netbooted through the PROM prints its release banner, with
       nothing checked beyond it (:test:`irix_miniroot`). A mainline
       Linux kernel runs its embedded init on one, two and four
       processors (:test:`linux_kernel`), and a Gentoo root disk reaches
       a multiuser system answering ssh (:test:`linux_multiuser`).
       OpenBSD/sgi 6.9 installs (:test:`openbsd_install`) and then logs
       in and moves checked payloads over the network
       (:test:`openbsd_net_transfer`).
   * - Origin 2000, Onyx2
     - The stock PROM reports the deskside and rack hardware inventory
       (:test:`onyx2_deskside_prom`, :test:`onyx2_rack_prom`). IRIX online
       diagnostics run on an installed disk of a one-node Origin 2000
       (:test:`irix_diag_origin2000_one_node`), and an installed IRIX
       powers off on an Origin 2000 (:test:`irix_installed_power`).
   * - Origin 300
     - The stock PROM reaches its System Maintenance Menu on one chassis
       (:test:`origin300_prom`) and on two joined by :term:`NUMAlink`
       (:test:`origin300_two_chassis_prom`). An installed IRIX 6.5.30
       disk reaches a root shell, run level 2 and the IP35 release
       (:test:`irix_origin300_installed_boot`).
   * - Fuel
     - An installed IRIX 6.5.30 disk reaches a root shell and shuts down
       to the PROM menu (:test:`irix_fuel_installed_boot`).
   * - Origin 350
     - An installed IRIX disk moves checked TCP and UDP payloads over the
       BCM5701 (:test:`irix_io9_tg_perf`).
   * - Origin 3000 family, Onyx 350, Tezro, Onyx 4 host
     - No functional test. qtests build and launch every one of them.

What keeps these pages current
------------------------------

Some content is generated when the manual is built. Some is checked by
tests. The rest is written by hand and nothing checks it.

Generated from ``hw/mips/sgi/sgi-machines.json`` by the ``sgi-run``
directive (``scripts/sgi_model/running_doc.py``): the tables of
selections, the inputs and devices of each machine, the graphics heads,
the short lists of accepted ``-cpu`` parts, memory and default topology,
and the commands that create a machine's stores and start it on them.
They follow the catalogue.

Checked by tests:

- ``scripts/tests/test_sgi_running_pages.py`` reads every
  ``qemu-system-mips64`` and ``qemu-sgi-machine-init`` command written in
  these pages and checks its machine, topology, node count, board,
  processor, ``-cpu``, ``-smp``, ``-m`` and store names against the
  catalogue. It fails when a catalogue product has no running page, when
  a page omits a product's tables, and when a command names a file the
  checks cannot stand in for. It needs no build.
- A ``:test:`` reference to a test that is not in the mips64
  ``meson.build`` lists fails the manual build and the script test.
- ``tests/functional/mips64/test_sgi_manual_commands.py`` starts every
  command written in these pages, and every generated one, on the built
  emulator with placeholder files, paused at reset. It also checks the
  refusals the pages state: ``-kernel`` is refused, and a product with no
  default topology refuses a launch without ``topology=``. The
  placeholders are zero-filled, so this shows that QEMU accepts the
  options and creates the machine. It does not show that a guest boots.

Written by hand and unchecked: the descriptions of what the firmware and
guests do, the choice of example commands, the size and format of the
firmware image the reader supplies, and the wording of the table above.
A row of the table summarizes its test's documentation, and nothing
checks that the summary is right; the test is the authority.

.. toctree::
   :maxdepth: 1

   running-ip30
   running-ip32
   running-ip27
   running-ip35
   running-ip53
