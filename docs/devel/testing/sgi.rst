.. SPDX-License-Identifier: BSD-3-Clause

SGI machine checks
==================

Build the MIPS64 system emulator with tests, HMP, and user-mode networking enabled:

.. code-block:: sh

   ./configure --target-list=mips64-softmmu --enable-tests \
       --enable-hmp --enable-slirp
   ninja -C build

Meson suite tags select the SGI tests by scope. Run all tagged tests or one
category from the configured MIPS64 build:

.. code-block:: sh

   build/pyvenv/bin/meson test -C build --setup qemu:thorough \
       --suite qemu:sgi --print-errorlogs
   build/pyvenv/bin/meson test -C build --suite qemu:sgi-unit \
       --print-errorlogs
   build/pyvenv/bin/meson test -C build --suite qemu:sgi-qtest \
       --print-errorlogs
   build/pyvenv/bin/meson test -C build --setup qemu:thorough \
       --suite qemu:sgi-functional --print-errorlogs

The category tags select unit/helper, qtest, or functional tests; ``sgi``
selects the combined set. The unit group includes fork-maintained one-wire
and I2C checks used by SGI devices, even though those buses are general.
When registering an SGI test, include ``sgi`` and
the matching category tag, while keeping its existing architecture and speed
tags. The existing architecture and speed selections still work.
Suite selection does not supply test assets or enable slow and large-data
cases. Asset-dependent cases can skip when ``QEMU_TEST_SGI_ASSET_DIR`` is
unset. Slow and large-data cases require their opt-in variables. See the
asset requirements below. These tags provide local Meson selection; they do not
configure CI or establish that a suite has passed.

To build without network access, prepare a source archive from a clean QEMU
checkout while connected. The archive script may download the declared Meson
subprojects before adding them to the tarball; the extracted archive contains
no ``.git`` metadata:

.. code-block:: sh

   scripts/archive-source.sh qemu.tar
   mkdir qemu-src
   tar -xf qemu.tar -C qemu-src
   cd qemu-src
   ./configure --disable-download --target-list=mips64-softmmu \
       --enable-tests --enable-hmp --enable-slirp
   ninja -C build qemu-system-mips64 qemu-img sgi-roms

Run those build commands in an environment that already has QEMU's native
build dependencies and the functional-test Python dependencies. The public
Debian Trixie image described below installs ``pygdbmi`` and ``pylint`` for the
functional tests. ``--disable-download`` prevents configure from fetching
missing dependencies; it does not install those Python packages. Test assets
are provisioned separately and are not included in the source archive.

The optional Debian Trixie build image is defined in
``tests/docker/dockerfiles/debian-sgi.docker``. It supplies the MIPS64 cross
compilers used to build the source ROMs, QEMU's build dependencies, and the
functional-test Python dependencies. Run the commands above in that image or
in another environment with those dependencies installed. The source archive
and asset root remain separate inputs.

The source-built ROM functional test is registered as
``func-mips64-sgi_roms`` in the ``qemu:thorough`` Meson setup. Its required
case list comes from ``tests/functional/mips64/roms/suite``. Build and run it with:

.. code-block:: sh

   build/pyvenv/bin/meson test -C build --setup qemu:thorough \
       --print-errorlogs func-mips64-sgi_roms

The timer-stall regression uses a separate ``func-mips64-timer_stall``
functional test. Configure a host GDB path with ``--gdb=/path/to/gdb`` or
``meson configure build -Dgdb=/path/to/gdb`` before selecting it. The test
builds only its ``timerstall`` ROM, launches QEMU under host GDB, and requires
both a breakpoint hit and ``PASS timerstall``. An unset GDB path skips the
case; required selections must reject that skip in the JUnit report.

The offline source-archive build was checked with the ROM matrix's 85 expected
identities and one selected IP27 PROM case on Origin 200. Both emitted fresh JUnit
reports that passed ``scripts/check-sgi-test-results.py``; this does not
qualify the complete functional matrix.

The tests that use supplied assets read paths directly below
``QEMU_TEST_SGI_ASSET_DIR``. The directory is an asset root, without an extra
``assets/`` or repository level. The core full-qualification boots use:

.. code-block:: text

   <asset-root>/firmware/6.5.30/ip27prom.payload.bin
   <asset-root>/linux-ip27/vmlinux
   <asset-root>/boot/6.5.30/unix.IP27

These three files are the inputs for focused Origin 200 PROM, Linux kernel,
and IRIX miniroot checks. In the current local sample they occupy 28,287,976
bytes in total; this is a sum of those file sizes, not a distribution
guarantee. Run the existing targets with the local asset root set:

.. code-block:: sh

   export QEMU_TEST_SGI_ASSET_DIR=/path/to/sgi-assets
   build/pyvenv/bin/meson test -C build --setup qemu:thorough \
       --print-errorlogs func-mips64-origin200_prom func-mips64-linux_kernel \
       func-mips64-irix_miniroot

This documents local placement only. Public acquisition instructions for
these files remain unresolved, and this note makes no claim about distribution
rights.

The installed-disk tests also use preinstalled images such as
``<asset-root>/irix/6.5.30/root.qcow2``. Additional functional cases declare
their own asset paths in their test modules. The PROM checks recognize the
6.5-base PROM SHA-256
``14e0d0a77b92154e3b33573f30608f241ded249db6060fba9f97bfd00b2145b2`` and the
6.5.30 PROM SHA-256
``c2c69bf33c375b43ec7ac511fab41ddf595be1b6838ae717e58a9a1273437fdd``. The
full boot cases use the 6.5.30 PROM. The declarations and matching checks in
``tests/functional/mips64/lib_prom_sash.py`` and
``tests/functional/mips64/test_irix_miniroot.py`` own these identities; tests
that do not declare a digest still validate the path and file before launch.

Running the focused MDK PROM check locally
------------------------------------------

The ``func-mips64-prom_mdk`` functional target boots the IP27 PROM and
netboots ``cpu.mdk``. It can run in a standalone QEMU checkout with two
already lawfully obtained files. Place them under an asset root using the
paths below; the root itself must not include another ``assets/`` directory:

.. code-block:: text

   <asset-root>/firmware/6.5.30/ip27prom.payload.bin
   <asset-root>/boot/mdk/cpu.mdk

For example, create those parent directories below a local ``sgi-assets``
directory and copy the files there unchanged. Check both files against the
identities enforced by the test:

.. code-block:: sh

   sha256sum sgi-assets/firmware/6.5.30/ip27prom.payload.bin
   # c2c69bf33c375b43ec7ac511fab41ddf595be1b6838ae717e58a9a1273437fdd
   sha256sum sgi-assets/boot/mdk/cpu.mdk
   # 8a72e5578436b076bae72f6243940a523364ea961604d6070de7f40b408d5d89

Set the asset root and run the target from the QEMU checkout:

.. code-block:: sh

   export QEMU_TEST_SGI_ASSET_DIR="$PWD/sgi-assets"
   ./configure --target-list=mips64-softmmu --enable-tests \
       --enable-hmp --enable-slirp
   ninja -C build
   build/pyvenv/bin/meson test -C build --setup qemu:thorough \
       --print-errorlogs func-mips64-prom_mdk

The build needs QEMU's native build dependencies and functional-test Python
dependencies, as described above. When ``QEMU_TEST_SGI_ASSET_DIR`` is unset,
the functional case skips. When the root is set but either required file is
missing, the test fails while resolving its declared local asset. This
example documents placement and verification only: acquisition instructions
for these exact files are not established, and public distribution rights
remain unresolved.

Keep supplied files unchanged. Tests that boot QCOW2 disks use QEMU snapshot
mode or disposable overlays for guest writes. The installed-disk checks expect
an already installed IRIX image; they do not install IRIX. The IP27 PROM
miniroot check downloads the supplied kernel over QEMU
user-mode networking, then executes it through the PROM boot path. It does not use an external
netinst server. QEMU no longer checks the external ``irix-netinst`` DHCP/TFTP service.

The Origin 300's two first-line PROM cases require only
``firmware/6.5.30/ip35prom.payload.bin``. Its menu cases also require both
SPD files. The installed-guest tests pin their disk inputs:

.. list-table:: Supplied inputs declared by these functional tests
   :header-rows: 1

   * - Native target and case
     - Inputs below the asset root
   * - ``func-mips64-origin300_prom`` — ``Origin300Prom`` first-line cases
     - ``firmware/6.5.30/ip35prom.payload.bin``
   * - ``func-mips64-origin300_prom`` — ``Origin300PromMenu`` menu case
     - The IP35 PROM and
       ``eeprom/origin300-dimm2-spd.bin`` plus
       ``eeprom/origin300-dimm3-spd.bin``
   * - ``func-mips64-origin300_two_chassis_prom``
     - The IP35 PROM and both SPD files
   * - ``func-mips64-irix_installed_boot``
     - ``firmware/6.5.30/ip27prom.payload.bin`` and
       ``irix/6.5.30/root.qcow2`` (SHA-256
       ``15b4bc1992845b6242948a9e944ce39682b7decdaee1e5474579418fb3a19c12``)
   * - ``func-mips64-irix_origin300_installed_boot``
     - The IP35 PROM (SHA-256
       ``554924380189a03a512156af2faa38344902f251b49fd5c80c253c23ecaff27b``),
       both SPD files (DIMM 2 SHA-256
       ``a8bb5857941fefae8e11037fe98b01d199199bd34eb059b269a8c4b6875ca0d3``,
       DIMM 3 SHA-256
       ``9e60c3400772d227b91c5d61cea8de7f89777d41377516de072944c26cef4aa7``),
       and ``irix/6.5.30/root-diags-origin300.qcow2`` (SHA-256
       ``150cf2c125cb3bd55764acfeec00804899ff3140cc536cf63a254765faa29390``)
   * - ``func-mips64-sn0_reset`` — SN0 reset cases
     - No supplied assets
   * - ``func-mips64-sn1_l1_reset`` — SN1 L1 reset and power-down cases
     - ``firmware/6.5.30/ip35prom.payload.bin``
   * - ``func-mips64-origin200_gigachannel_prom`` — flash case
       (``Origin200GigachannelPromFlash``)
     - The IP27 PROM and ``firmware/6.5.30/io6prom.img``
   * - ``func-mips64-origin200_gigachannel_prom`` — cabinet fallback
       and loopback cases
       (``Origin200GigachannelProm``)
     - The IP27 PROM
   * - ``func-mips64-prom_diagnostics``
     - The IP27 PROM and ``boot/6.5.30/sash64``
   * - ``func-mips64-prom_pod``
     - ``firmware/6.5-base/ip27prom.payload.bin``
   * - ``func-mips64-prom_fx``
     - The 6.5.30 IP27 PROM and ``boot/6.5-base/fx.64``
   * - ``func-mips64-prom_mdk`` and ``func-mips64-prom_mdk_instgen``
     - The IP27 PROM and ``boot/mdk/cpu.mdk``
   * - ``func-mips64-prom_mdk_memory``
     - The IP27 PROM and ``boot/mdk/memory.mdk``
   * - ``func-mips64-prom_mdk_router``
     - The IP27 PROM and ``boot/mdk/MDK_router_sso``
   * - ``func-mips64-linux_kernel``
     - The IP27 PROM and ``linux-ip27/vmlinux`` (SHA-256
       ``d095c9033e2f8aeca0a75adbbdc1d0acdf764230999b63eb6261574d65c77bd4``)
   * - ``func-mips64-linux_multiuser``
     - The IP27 PROM, ``linux-ip27-net/vmlinux`` (SHA-256
       ``5309e8e14c48bfb52267588b632470d3a89ad6e34e2f18999cf540799d96f265``),
       and ``gentoo/gentoo-rootfs.img`` (SHA-256
       ``0eb821e70cd698b83d833a5d937e31aef070eb5e5604294c4e3c693a133fc5ac``)
   * - ``func-mips64-xframe_pci`` and ``func-mips64-openbsd_net_transfer``
     - The IP27 PROM, ``openbsd/6.9/bsd.IP27`` (SHA-256
       ``5c132640eb510cbc0e63bbcca1169f587a168610837687b0d1d02950a349a7a1``),
       and ``openbsd/spike/root.img`` (SHA-256
       ``55c53b0268e55682d38485d902e47e9fbeb9e9ef77caf1625d5522dac33c70fd``)
   * - ``func-mips64-openbsd_install``
     - The IP27 PROM and ``openbsd/6.9/bsd.rd.IP27`` (SHA-256
       ``4d02fc15e5c02c2f351a6e825f617c2dfae1a8dbfb51d283090e1bebc2b31d15``).
       The generated sets disk contains ``bsd.IP27`` (SHA-256
       ``5c132640eb510cbc0e63bbcca1169f587a168610837687b0d1d02950a349a7a1``),
       ``base69.tgz`` (SHA-256
       ``b71e368f9f820a3c6ca5df53fc2ba617e9f535b23504f01448fb0e2100aa9530``),
       ``comp69.tgz`` (SHA-256
       ``f65af2b2351873f9966f75a3f36b9dd3446489be0b05af060a6d5e9f97535d0b``),
       ``SHA256`` (SHA-256
       ``7ad56623376fff7f869b0b1f08920562e477efb71ada3e7a2da54e96064d5106``),
       and ``SHA256.sig`` (SHA-256
       ``9b67eac0663a17e73714b896b5d504f26bc022b43607591ee5c7014a680b7494``)

The OpenBSD tests use checksummed supplied files, but public download locations
and preparation provenance have not been established for those files. The
installer creates its blank target disk with ``qemu-img`` and builds the sets
disk from the listed kernel, sets, and signed manifest with ``makefs``; these
scratch disks are not supplied fixtures. Do not treat the scratch-disk recipe
as provenance for the source files. The full installer remains optional and
requires QEMU's slow-test and large-data opt-ins.

Both IRIX installed-guest cases use QEMU's ``skipSlowTest`` and
``skipBigDataTest`` decorators. Run them with ``QEMU_TEST_ALLOW_SLOW=1`` and
``QEMU_TEST_ALLOW_LARGE_STORAGE=1`` as well as the asset root. These checks
boot existing installations; they do not install IRIX.

The additional IRIX diagnostic and installed-guest network and graphics tests
also declare their local inputs with ``LocalAsset``. Their paths are relative
to ``QEMU_TEST_SGI_ASSET_DIR``:

.. list-table:: Additional declared IRIX test inputs
   :header-rows: 1

   * - Test module
     - Declared inputs below the asset root
   * - ``test_irix_diag.py`` and its Origin 200, Origin 2000 and Origin 300 variants
     - The IP27 PROM, or the IP35 PROM and its two SPD files for the Origin
       300; ``irix/6.5.30/root-diags.qcow2`` for IP27 or
       ``irix/6.5.30/root-diags-origin300.qcow2`` for IP35; and
       ``irix/6.5.30/olcdrom-medium.raw`` (SHA-256
       ``ba0a1466d7125cd350ee4f047677d7a3d9f8a8ed8b13469c92042cfa0bd07829``).
       The IP27 diagnostic disk SHA-256 is
       ``82e394e28cbe1bf92aa08f01216636bdff7a0b224d15166f321e1f075b253e66``.
       The IP35 diagnostic disk uses the checksum listed for the installed
       Origin 300 image above.
   * - ``test_irix_tg.py`` and ``test_irix_tg_perf.py``
     - The IP27 PROM and ``irix/6.5.30/root.qcow2``
   * - ``test_irix_eg.py`` and ``test_irix_ef.py``
     - The IP27 PROM and ``irix/6.5.30/root.qcow2``
   * - ``test_mgras_desktop.py``
     - The IP27 PROM and ``irix/6.5.30/root-si.qcow2``

The thorough graphics-matrix rows also declare checksum-pinned local assets:
the IP27 PROM and ``irix/6.5.30/root-rad4.qcow2`` (SHA-256
``f47c806f6a1234044668bbd67fd820a3f9e7dff80ff97edc4b84be8ba84887bc``).

The Origin 300 PROM tests build their writable boot-flash images and identity
EEPROM records in each test's scratch directory. Supply only the IP35 PROM and
SPD files listed above. The generated images do not belong in the asset root.

Asset requirements are marked by each functional test. An unset asset-root
variable can skip an asset-dependent case for a developer who has not supplied
the files; once configured, a missing or mismatched file fails that case. Full
qualification requires the asset root and rejects every skip. Each test keeps
its own pass markers: the Linux kernel test checks its CPU-specific progress
markers, the miniroot test checks the kernel's release output, and the ROM test
checks each generated ROM's named result. A full suite result is not inferred
from a process exit alone.

To require selected results, use a unique Meson log base and remove its earlier
report before the run:

.. code-block:: sh

   printf '%s\n' 'OriginPromMatrix.test_prom_full_boot_1cpu' > build/expected.txt
   rm -f build/meson-logs/sgi-prom*.junit.xml
   build/pyvenv/bin/meson test -C build --setup qemu:thorough \
       --logbase sgi-prom --print-errorlogs \
       --test-args OriginPromMatrix.test_prom_full_boot_1cpu \
       func-mips64-origin200_prom
   test "$(find build/meson-logs -maxdepth 1 -type f \
       -name 'sgi-prom*.junit.xml' | wc -l)" -eq 1
   junit=$(find build/meson-logs -maxdepth 1 \
       -name 'sgi-prom*.junit.xml' -print -quit)
   python3 scripts/check-sgi-test-results.py \
       --junit "$junit" --expect build/expected.txt

The expected file path is resolved from the caller's current directory. Each
nonblank line is a literal identity: an exact JUnit testcase name, an exact
``classname::name`` pair, or a testsuite name. The checker normalizes Meson's
numeric TAP case counter and QEMU's ``test_<module>.`` prefix, so a Python
identity such as ``OriginPromMatrix.test_prom_full_boot_1cpu`` matches its TAP
case. A testsuite-name entry requires at least one emitted testcase. The
checker rejects empty, malformed, incomplete, skipped, failed, or errored
reports. It does not decide whether a report is fresh; callers remove old
reports or use unique log bases and require exactly one matching result.
The log base isolates JUnit reports, not test scratch files. Concurrent runs of
the same functional test need separate build directories because Meson shares
that test's scratch directory within a build.

The commands above describe individual native checks and their evidence.
Choose the Meson targets needed for the change, and set
``QEMU_TEST_SGI_ASSET_DIR`` for tests that use supplied files. Full asset-based
qualification requires all declared files and rejects skips. The JUnit checker
can require named results, as shown above; a successful process exit alone does
not establish that the expected cases ran.

Translation-health failures
---------------------------

The MIPS64 PROM functional tests can sample HMP ``info jit`` after boot
progress. A sick result means QEMU is producing very small translation blocks
or the translation-block hash has not retained entries despite substantial
translation work. The check reports sick when both the average and maximum
target block sizes are at most 8 bytes, or when zero of the hash buckets are
used after at least 1,000,000 translated blocks. These are regression
thresholds for the observed failure pattern, not a hardware specification.

When this check fails, inspect the guest physical memory map around the address
where execution is spending time, especially the reset-fetch region. Confirm
that executable RAM or ROM is mapped as memory rather than as an I/O region.
Then inspect the reported block sizes, TB count, and hash bucket counts in the
failure output. A sick result may indicate a bad mapping or another condition
that prevents useful translation; the counters alone do not identify the
cause.

The check skips runs launched with ``-d exec``, ``-d in_asm``, ``-d nochain``,
``-singlestep``, ``-gdb`` or ``-s``, and samples reporting ``one-insn-per-tb``
or icount. These modes change translation behavior or the counters, so rerun
without them before using this diagnostic. The included Origin 300 and Origin
200 counter samples were captured from QEMU using a development harness.
Their exact QEMU revision and capture environment were not recorded.
