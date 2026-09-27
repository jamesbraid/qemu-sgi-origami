Bare-metal MIPS regression ROMs
==============================

These GPL-2.0-or-later programs check CPU and machine behavior without a
proprietary firmware input. The ``suite`` manifest lists the ROM, CPU models
and machine configuration for every required case. R10000 cases run on
``origin200``; R4000 and R14000 cases run on ``malta``. The four-CPU cases
use ``origin200-dual``. Instruction-counting cases retain their explicit
``-icount`` configuration.

Build and run
-------------

Configure QEMU for ``mips64-softmmu`` with tests, TCG and HMP enabled. Install
``mips64-linux-gnuabi64-gcc`` and ``mips64-linux-gnuabi64-objcopy`` in the build
environment. The SGI container recipe includes both. Meson omits the ROM
functional test when either cross tool is absent; required CI must require
this test to appear and pass.

From the configured build directory::

  ninja sgi-roms
  meson test --setup qemu:thorough func-mips64-sgi_roms --print-errorlogs

Meson builds the ROM dependencies before testing. Outputs remain under
``tests/functional/mips64/roms`` in the build directory. No prebuilt image,
source hash stamp or source-tree build directory is used. To select one case::

  meson test --setup qemu:thorough func-mips64-sgi_roms --test-args SGIRoms.test_tlb_R10000_default

Each case requires both process exit status zero and an exact ``PASS <rom>``
line from the MIPS semihosting output. A timed manifest row runs on both one
and four CPUs; durations are recorded by the native framework, with no host
wall-clock ratio used as a pass criterion.

The linker places code at the reset vector and writable data in RAM. The
shared startup and exception code lives in ``lib/``. Assertion comments cite
the specifications and reviewed contracts used when the tests were written.
References to Origami identify historical review evidence, not runtime inputs.

Some source files are intentionally outside the suite. ``timerstall`` needs
a debugger to stall the emulator during a timer update; an ordinary run would
not exercise that regression. Building that payload does not qualify it.
