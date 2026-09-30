.. SPDX-License-Identifier: GPL-2.0-or-later

Bare-metal MIPS regression ROMs
==============================

These GPL-2.0-or-later programs test CPU and machine behavior without
proprietary firmware. The ``suite`` manifest lists each required ROM, CPU and
machine. R10000 cases run on ``origin200``; R4000 and R14000 cases run on
``malta``. Four-CPU cases use ``origin200-dual``. Instruction-counting cases
specify their ``-icount`` configuration in the manifest.

Build and run
-------------

Configure QEMU for ``mips64-softmmu`` with tests, TCG and HMP enabled. Install
``mips64-linux-gnuabi64-gcc`` and ``mips64-linux-gnuabi64-objcopy`` in the build
environment. Meson omits the ROM functional test when either cross tool is
absent; required CI must require this test to appear and pass.

From the configured build directory::

  ninja sgi-roms
  meson test --setup qemu:thorough func-mips64-sgi_roms --print-errorlogs

Meson builds the ROM dependencies in the build tree. To select one case::

  meson test --setup qemu:thorough func-mips64-sgi_roms --test-args SGIRoms.test_tlb_R10000_default

For a required run, check registration before executing the suite and require
every manifest case in the fresh JUnit result. From the source root::

  python3 scripts/check-sgi-test-results.py \
      --list-rom-identities tests/functional/mips64/roms/suite \
      --require-rom-target build > build/sgi-rom-expected.txt
  build/pyvenv/bin/meson test -C build --setup qemu:thorough \
      --logbase sgi-rom-required --print-errorlogs func-mips64-sgi_roms
  python3 scripts/check-sgi-test-results.py \
      --junit build/meson-logs/sgi-rom-required-qemu_thorough.junit.xml \
      --expect build/sgi-rom-expected.txt

Each case requires both process exit status zero and an exact ``PASS <rom>``
line from the MIPS semihosting output. A timed manifest row runs on both one
and four CPUs; durations are recorded by the native framework, with no host
wall-clock ratio used as a pass criterion.

The linker places code at the reset vector and writable data in RAM. The
shared startup and exception code lives in ``lib/``. Comments distinguish
documented behavior from emulator compatibility expectations.

Some source files are intentionally outside the suite. ``timerstall`` runs
through ``func-mips64-timer_stall``, which launches QEMU under host GDB and
requires a breakpoint hit before accepting the ROM's PASS result.
