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
(a 2 GiB disk) and @skipSlowTest() (the boot is minutes) plus
@skipIfMissingEnv('ORIGAMI_ASSETS') and a skipTest at method entry --
the same guards test_openbsd_net_transfer.py keeps.

The machine matches the verified milestone boot: -M
origin200,topology=origin200-dual -cpu R10000 -m 256 (128 MiB per node),
-smp 4, -accel tcg,thread=multi, -snapshot
so the 2 GiB disk stays read-only -- with the kernel now loaded through
the real PROM's own netboot path rather than -kernel directly (lib_prom's
prom_netboot_steps(), the same conversion test_linux_kernel.py makes and
for the same reason: the plan this test belongs to retires synthesized
firmware). The one user-mode NIC carries both jobs the guest's single
onboard IOC3 needs across the run: `tftp=`/`bootfile=` for the PROM's
netboot fetch, and the same `hostfwd=` port forward as before for the
ssh probe once multiuser is up -- one SLIRP backend, not two, since both
are the same interface at different points in the same boot. `-no-reboot`
is dropped: test_linux_kernel.py's own docstring covers why it does not
survive booting the real PROM (an immediate, silently-swallowed shutdown,
confirmed by running that exact command line with and without the flag)
and neither test's own pass condition ever depended on it.

missing_multiuser_markers() below judges the console transcript --
ethernet link, dhcp client, sshd, login prompt. The live ssh answer is a
host-side socket probe here (_ssh_banner): a running sshd sends its
SSH-2.0 banner before any authentication, so a banner over the forwarded
port is proof the service is up and reachable, which the console alone
cannot give. No login or password is driven: a returned banner is the
pass condition for a live network service; an authenticated round-trip is
a deliberate follow-up, not this cut.
"""

import os
import re
import socket
import time

from qemu_test import (QemuSystemTest, skipBigDataTest, skipIfMissingEnv,
                       skipSlowTest)

from lib_prom import (FIRMWARE_REL, MACHINES, drive_console,
                      prom_netboot_steps, require_assets, verdict)


# What a networked multiuser boot of the Gentoo disk should print: the
# IOC3 ethernet linking, OpenRC starting the DHCP client and sshd, and the
# multiuser login prompt. Each is a substring of a line a real boot
# prints, so the check is line-ending agnostic -- a serial capture's CRLF
# never splits one.
MARKERS = (
    ("ethernet link", "eth0: link up"),
    ("dhcp client", "Starting DHCP Client Daemon"),
    ("ssh service", "Starting sshd"),
    ("multiuser login prompt", "login:"),
)


def missing_multiuser_markers(log):
    """Which networked-multiuser markers never appeared in log, in order;
    empty means the guest reached a networked multiuser login. The
    stronger proof that ssh actually answers is not here -- that is the
    host-side banner probe below, the network-service evidence a console
    transcript cannot give."""
    return verdict(log, MARKERS)


# Where ORIGAMI_ASSETS resolves the networked kernel and the Gentoo rootfs,
# ORIGAMI_ASSETS names a checkout, not its assets/ directory.
KERNEL_REL = os.path.join('assets', 'linux-ip27-net', 'vmlinux')
DISK_REL = os.path.join('assets', 'gentoo', 'gentoo-rootfs.img')

# The boot file this test's own SLIRP TFTP server serves the kernel
# under -- see prom_netboot_steps()'s own docstring for why this need
# not match the kernel's name on disk.
NETBOOT_BOOTFILE = 'vmlinux'
NETBOOT_STEPS = prom_netboot_steps(NETBOOT_BOOTFILE)

MEMORY_MIB = 256

# The verified milestone boot reached a networked login in 754-845s under
# a direct -kernel load, before this test carried a real PROM boot and
# netboot transfer in front of it; kept unchanged as a floor pending a
# real measurement of the netbooted boot (see this module's own
# docstring for why one could not be taken here). Still a margin under
# the meson timeout registered for this test (mips64/meson.build), the
# same shape test_openbsd_net_transfer.py keeps.
BOOT_TIMEOUT_SECONDS = 1200

# The guest's port 22, forwarded to this host port. A fixed high port: the
# framework runs one functional guest at a time, so nothing else binds it
# (the single-instance assumption tools/mttcg-smoke documents for the
# functional cases).
SSH_HOST_PORT = 15722

LOGIN_RE = re.compile(r"[\w.-]+ login:")
CRASH_RE = re.compile(r"Kernel panic - not sync|Oops\[|Unable to handle kernel")


def _login_reached(transcript):
    """The netboot drive's end condition once the netboot dialogue has
    been answered: the multiuser login prompt (network and services up)
    or a kernel crash. xframe_pci's own _pci_probe_reached() decodes the
    same way, for the same reason: drive_console() accumulates bytes,
    every marker here is a str pattern."""
    text = transcript.decode('utf-8', errors='replace')
    return bool(LOGIN_RE.search(text) or CRASH_RE.search(text))


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


class LinuxMultiuser(QemuSystemTest):
    """Boots the networked Gentoo disk and requires a networked
    multiuser system with a live ssh service. See this module's
    docstring for the contract; this class only wires it to a real
    boot, the division test_openbsd_net_transfer.py keeps between its pure
    verdict and its own QemuSystemTest."""

    @skipBigDataTest()
    @skipSlowTest()
    @skipIfMissingEnv('ORIGAMI_ASSETS')
    def test_linux_multiuser(self):
        assets = require_assets(self, FIRMWARE_REL, KERNEL_REL, DISK_REL)
        firmware, kernel = assets[FIRMWARE_REL], assets[KERNEL_REL]
        disk = assets[DISK_REL]

        vm = self.get_vm(name='linux_multiuser')
        vm.set_machine(MACHINES['origin200_dual'])
        vm.set_console()
        vm.add_args(
            '-cpu', 'R10000', '-m', str(MEMORY_MIB), '-snapshot',
            '-bios', firmware,
            '-drive', f'if=none,id=disk0,file={disk},format=raw',
            '-device', 'scsi-hd,drive=disk0,bus=scsi.0,scsi-id=0,lun=0',
            '-smp', '4',
            '-accel', 'tcg,thread=multi',
            '-nic', ('user,id=n1,'
                    f'hostfwd=tcp::{SSH_HOST_PORT}-:22,'
                    f'tftp={os.path.dirname(kernel)},'
                    f'bootfile={NETBOOT_BOOTFILE}'))
        vm.launch()

        log_path = self.log_file('linux-multiuser.log')
        banner = ''
        missing = None
        try:
            transcript, _reached = drive_console(
                vm.console_socket, log_path, BOOT_TIMEOUT_SECONDS,
                NETBOOT_STEPS, reached_when=_login_reached, vm=vm)
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
            # test_openbsd_net_transfer.py's own reasoning.
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
    QemuSystemTest.main()
