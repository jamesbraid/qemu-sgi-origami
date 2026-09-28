.. SPDX-License-Identifier: GPL-2.0-or-later

SGI machine checks
==================

Build the MIPS64 system emulator with tests, HMP, and user-mode networking enabled:

.. code-block:: sh

   ./configure --target-list=mips64-softmmu --enable-tests \
       --enable-hmp --enable-slirp
   ninja -C build

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

Debian Trixie build dependencies used by the Forgejo workflows are declared in
``tests/docker/dockerfiles/debian-sgi.docker``. That image includes the MIPS64
cross compilers used to build the source ROMs. QEMU builds and tests run inside
that public image; the Forgejo runner supplies the Docker client, reachable
container service, and a named volume for its checkout. Those runner facilities
must be configured before these workflows can run. A missing facility fails
with a diagnostic; the workflow does not install tools on the runner.
GitHub Actions workflows are planned to run these same native QEMU commands;
this guide documents the Forgejo workflows and does not claim that the
provider-specific workflows have been qualified on both services.

The source-built ROM functional test is registered as
``func-mips64-sgi_roms`` in the ``qemu:thorough`` Meson setup. Its required
case list comes from ``tests/functional/mips64/roms/suite``. Build and run it with:

.. code-block:: sh

   build/pyvenv/bin/meson test -C build --setup qemu:thorough \
       --print-errorlogs func-mips64-sgi_roms

The offline source-archive build was checked with the ROM matrix's 85 expected
identities and one selected Origin 200 PROM case. Both emitted fresh JUnit
reports that passed ``scripts/check-sgi-test-results.py``; this does not
qualify the complete functional matrix.

The tests that use supplied assets read paths directly below
``QEMU_TEST_SGI_ASSET_DIR``. The directory is an asset root, without an extra
``assets/`` or repository level. The core full-qualification boots use:

.. code-block:: text

   <asset-root>/firmware/6.5.30/ip27prom.payload.bin
   <asset-root>/linux-ip27/vmlinux
   <asset-root>/boot/6.5.30/unix.IP27

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

Keep supplied files unchanged. Tests that boot QCOW2 disks use QEMU snapshot
mode or disposable overlays for guest writes. The installed-disk checks expect
an already installed IRIX image; they do not install IRIX. The full build job's
IRIX check boots the PROM and downloads the supplied miniroot kernel over QEMU
user-mode networking before direct-booting it. It does not use an external
netinst server. External installation services and package qualification remain
downstream checks.

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

The required pull-request workflow runs all MIPS64 qtests and QEMU unit tests
without proprietary files. The full workflow-dispatch check runs on the
trusted ``sgi-origami`` branch, requires the supplied asset root, and runs the
register and unit suites, the ROM matrix, and selected PROM, Linux, and IRIX
boots. The prior pull-request Linux boot is therefore covered by the full
asset-qualified check. Scheduled nightly runs rotate through the supplied-asset
MIPS64 matrix, diagnostic, install, and OpenBSD workloads. A manual
``workload=asset-free`` run exercises the ROM matrix without private files.
