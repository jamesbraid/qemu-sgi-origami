#!/usr/bin/env python3
#
# Linux/IP27 network multiuser: boot the Gentoo disk on the networked kernel
# and require a live ssh service answering a remote connection
#
# SPDX-License-Identifier: GPL-2.0-or-later
"""
Boots the reproducible networked disk image -- tools/gentoo-disk's Gentoo
rootfs (assets/gentoo/gentoo-rootfs.img) rooted by the
tools/linux-userspace --with-net kernel (assets/linux-ip27-net/vmlinux) --
to a networked multiuser system, and proves a live network service: a
remote TCP connection to the guest's ssh daemon over a forwarded port gets
an SSH protocol banner back. This is the network-multiuser milestone as a
functional test, distinct from the four-CPU usercheck cell
(test_linux_kernel.py's test_four_cpu), which proves the MTTCG
remote-memory contract with no disk and no network.

Both assets are provisioned into ORIGAMI_ASSETS by the nightly, published
content-addressed (generic/linux-ip27-net/<sha>/vmlinux and
generic/gentoo/<sha>/gentoo-rootfs.img). Guarded by @skipBigDataTest()
(a 2 GiB disk) and @skipSlowTest() (the boot is minutes) plus the usual
ORIGAMI_ASSETS skipTest at method entry -- the same guards
test_openbsd_login.py keeps.

The machine matches the verified milestone boot: -M origin200 -cpu R10000
-m 256, two 128 MiB memory backends as NUMA nodes 0-1, -smp
4,sockets=2,cores=2,threads=1, -accel tcg,thread=multi, one user-mode NIC
with the guest's port 22 forwarded to a host port, -snapshot so the 2 GiB
disk stays read-only.

missing_multiuser_markers() (test_linux_multiuser_verdict.py, imported
below) judges the console transcript -- ethernet link, dhcp client, sshd,
login prompt. The live ssh answer is a host-side socket probe here
(_ssh_banner): a running sshd sends its SSH-2.0 banner before any
authentication, so a banner over the forwarded port is proof the service
is up and reachable, which the console alone cannot give. No login or
password is driven: a returned banner is the pass condition for a live
network service; an authenticated round-trip is a deliberate follow-up,
not this cut.

Free of any qemu_test import on purpose below the guarded import: the pure
verdict lives entirely in test_linux_multiuser_verdict.py (imported
unconditionally, it needs no qemu_test); only the real QemuSystemTest
below needs qemu_test (MESON_BUILD_ROOT), guarded the same way
test_openbsd_login.py and test_linux_kernel.py guard it.
"""

import os
import re
import socket
import sys
import time

# This file's own directory, so a plain pytest run finds the verdict module
# the same way test_openbsd_login.py finds test_openbsd_login_verdict.py.
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

from test_linux_multiuser_verdict import missing_multiuser_markers  # noqa: E402

try:
    from qemu_test import QemuSystemTest, skipBigDataTest, skipSlowTest
except (ImportError, RuntimeError):
    QemuSystemTest = None


# Where ORIGAMI_ASSETS resolves the networked kernel and the Gentoo rootfs,
# the same idiom as test_openbsd_login.py: ORIGAMI_ASSETS names a CHECKOUT,
# not assets/ itself.
KERNEL_REL = os.path.join('assets', 'linux-ip27-net', 'vmlinux')
DISK_REL = os.path.join('assets', 'gentoo', 'gentoo-rootfs.img')

MEMORY_MIB = 256

# The verified milestone boot reached a networked login in 754-845s; keep a
# margin under the meson timeout registered for this test
# (mips64/meson.build), the same shape test_openbsd_login.py keeps.
BOOT_TIMEOUT_SECONDS = 1200

# The guest's port 22, forwarded to this host port. A fixed high port: the
# framework runs one functional guest at a time, so nothing else binds it
# (the single-instance assumption tools/mttcg-smoke documents for the
# functional cases).
SSH_HOST_PORT = 15722

LOGIN_RE = re.compile(r"[\w.-]+ login:")
CRASH_RE = re.compile(r"Kernel panic - not sync|Oops\[|Unable to handle kernel")


def _boot_to_login(sock, log_path, timeout):
    """Read the console until the multiuser login prompt (network and
    services up), a kernel crash, or the timeout. Every byte received is
    accumulated into the returned transcript and appended to log_path, so a
    run is inspectable exactly like a captured -serial file -- the same
    shape test_openbsd_login.py's _drive_login() keeps, without any device
    questions to answer (the disk carries a real init)."""
    sock.settimeout(1.0)
    deadline = time.monotonic() + timeout
    transcript = ""
    with open(log_path, "a", encoding="utf-8") as log:
        while time.monotonic() < deadline:
            try:
                chunk = sock.recv(4096)
            except socket.timeout:
                continue
            except OSError:
                break
            if not chunk:
                break
            text = chunk.decode("utf-8", errors="replace")
            transcript += text
            log.write(text)
            log.flush()
            if LOGIN_RE.search(transcript) or CRASH_RE.search(transcript):
                break
    return transcript


def _ssh_banner(port, timeout=15):
    """Connect to the forwarded ssh port and return the first bytes the
    server sends. An ssh daemon answers with its SSH-2.0 banner before any
    authentication, so this needs no credentials. Returns '' on any
    connection failure -- caller treats that as "did not answer"."""
    try:
        with socket.create_connection(("127.0.0.1", port), timeout) as conn:
            conn.settimeout(timeout)
            return conn.recv(128).decode("latin1", errors="replace").strip()
    except OSError:
        return ""


if QemuSystemTest is not None:

    class LinuxMultiuser(QemuSystemTest):
        """Boots the networked Gentoo disk and requires a networked
        multiuser system with a live ssh service. See this module's
        docstring for the contract; this class only wires it to a real
        boot, the division test_openbsd_login.py keeps between its pure
        verdict and its own QemuSystemTest."""

        def _asset_paths(self):
            """The kernel and rootfs under ORIGAMI_ASSETS, or None if either
            is missing -- the runner may not have assets provisioned yet."""
            assets_root = os.environ.get('ORIGAMI_ASSETS')
            if not assets_root:
                return None
            kernel = os.path.join(assets_root, KERNEL_REL)
            disk = os.path.join(assets_root, DISK_REL)
            if not (os.path.isfile(kernel) and os.path.isfile(disk)):
                return None
            return kernel, disk

        @skipBigDataTest()
        @skipSlowTest()
        def test_linux_multiuser(self):
            paths = self._asset_paths()
            if paths is None:
                self.skipTest(
                    'ORIGAMI_ASSETS is not set, or has no kernel/disk at '
                    f'<ORIGAMI_ASSETS>/{KERNEL_REL} and '
                    f'<ORIGAMI_ASSETS>/{DISK_REL} -- point it at a checkout '
                    'with the networked kernel and Gentoo rootfs provisioned '
                    '(nightly assets) to run this test')
            kernel, disk = paths

            vm = self.get_vm(name='linux_multiuser')
            vm.set_machine('origin200')
            vm.set_console()
            vm.add_args(
                '-cpu', 'R10000', '-m', str(MEMORY_MIB), '-snapshot',
                '-kernel', kernel, '-no-reboot',
                '-drive', f'if=none,id=disk0,file={disk},format=raw',
                '-device', 'scsi-hd,drive=disk0,bus=scsi.0,scsi-id=0,lun=0',
                '-object', 'memory-backend-ram,id=ram0,size=128M',
                '-object', 'memory-backend-ram,id=ram1,size=128M',
                '-numa', 'node,nodeid=0,cpus=0-1,memdev=ram0',
                '-numa', 'node,nodeid=1,cpus=2-3,memdev=ram1',
                '-smp', '4,sockets=2,cores=2,threads=1',
                '-accel', 'tcg,thread=multi',
                '-nic', f'user,id=n1,hostfwd=tcp::{SSH_HOST_PORT}-:22')
            vm.launch()

            log_path = self.log_file('linux-multiuser.log')
            banner = ''
            missing = None
            try:
                transcript = _boot_to_login(
                    vm.console_socket, log_path, BOOT_TIMEOUT_SECONDS)
                # Probe ssh only once the boot reached networked multiuser --
                # gated on the same verdict the markers use, not a separate
                # login match, so the banner is asserted exactly when the
                # markers say the service should be up. sshd starts just
                # before the getty prompt; give it a moment to bind, and
                # probe while the VM is still up (shutdown is below).
                missing = missing_multiuser_markers(transcript)
                if not missing:
                    time.sleep(10)
                    banner = _ssh_banner(SSH_HOST_PORT)
            finally:
                # A VM that crashed or exited on its own has already left; a
                # graceful shutdown() against an already-dead process still
                # raises AbnormalShutdown even though qemu is gone. Matches
                # test_openbsd_login.py's own reasoning.
                vm.shutdown(hard=vm.exitcode() is not None)

            if not transcript:
                self.fail(
                    'the guest printed nothing at all -- a run that did not '
                    'start, not a boot that failed early. Check the emulator '
                    'built and that nothing ended the machine before it '
                    'printed anything.')

            self.assertEqual(
                missing, [],
                'networked-multiuser markers missing: ' + ', '.join(missing) +
                f' (full transcript: {log_path})')
            # Reached only when the markers passed, so a failure here is
            # unambiguous: the console came up multiuser but the ssh service
            # did not answer.
            self.assertTrue(
                banner.startswith('SSH-2.0'),
                'the console reached networked multiuser but the ssh service '
                f'did not answer over the forwarded port (got {banner!r}; '
                f'transcript: {log_path})')


if __name__ == '__main__':
    if QemuSystemTest is not None:
        QemuSystemTest.main()
