#!/usr/bin/env python3
#
# xframe PCI presence: boot the stock OpenBSD/IP27 disk WITH an emulated
# Xframe 10G Ethernet adapter attached, and require OpenBSD's own PCI
# autoconfiguration to report it -- unclaimed, since stock GENERIC-IP27
# carries no driver for it, but genuinely composed onto the guest's PCI
# bus through the machine's Bridge
#
# SPDX-License-Identifier: GPL-2.0-or-later
"""
Boots the exact same kernel and installed disk test_openbsd_login.py boots
(see that file's own docstring for where they come from and why 128 MiB),
with one addition on the command line: `-device
xframe,addr=4.0,subsystem-vendor-id=0x10a9,netdev=n1,mac=02:00:5d:00:00:01`
plus a matching `-netdev user,id=n1`, putting this fork's emulated Xframe
10G Ethernet adapter (hw/net/xframe.c) at PCI slot 4 on the machine's one
PCI bus.

This is a REAL boot, not a scaffold: stock OpenBSD 6.9's GENERIC-IP27
kernel already exists and already carries no driver matching vendor 0x17d5
product 0x5831 (no xge(4) in that build -- see test_openbsd_xge.py for the
future test that needs a kernel which does), so this test's own pass
condition -- the device reported "not configured" at exactly that PCI
slot -- is something this asset can satisfy today, unlike test_openbsd_xge.py.

xframe_pci_verdict() (test_xframe_pci_verdict.py, imported below) is the
pure check this file only wires to a real boot; see that module's own
docstring for exactly how its expected text was derived and cross-checked
against OpenBSD's own source, the real kernel binary these tests boot, and
this fork's own xframe device model.

_drive_pci_probe() below never sends the guest anything: OpenBSD's PCI bus
scan finishes well before any interactive question is asked -- in
REAL_LOGIN_TRANSCRIPT (test_openbsd_login_verdict.py's own real captured
boot of this exact kernel and disk), every PCI device line appears before
"boot device: 'unknown' unrecognized." and "root device: sd0a", the first
point that transcript ever needs an answer. This test only needs to read
until XFRAME_UNCLAIMED_RE matches, a TRIPWIRE fires, or the timeout runs
out -- unlike test_openbsd_login.py's own _drive_login(), it never needs
LOGIN_STEPS at all.

BOOT_TIMEOUT_SECONDS below is a real measurement, not a guess: a run of
this exact test against real provisioned assets (podman, this worktree
mounted read-write and a checkout carrying assets/ mounted read-only as
ORIGAMI_ASSETS) reached XFRAME_UNCLAIMED_RE and returned PASS in about 43s
wall-clock end to end -- including podman container startup -- on a quiet
host. 300 gives roughly seven times that headroom for a contended host,
the same shape of margin this directory's own meson.build keeps for
sn0_smp ("four times" its own measured figure); the meson timeout entry
for this test keeps the usual 60s margin over this budget.

Guarded the same way test_openbsd_login.py is: @skipBigDataTest() (the
disk is 2 GiB), @skipSlowTest() (booting to firmware-plus-early-kernel is
not instant even though this test stops well short of a full boot), and
@skipIfMissingEnv('ORIGAMI_ASSETS') plus a skipTest() at method entry if
either asset is missing.
"""

import os
import socket
import time

from qemu_test import (QemuSystemTest, skipBigDataTest, skipIfMissingEnv,
                       skipSlowTest)

from lib_prom import resolve_asset

from test_xframe_pci_verdict import (TRIPWIRES, XFRAME_UNCLAIMED_RE,
                                     xframe_pci_verdict)


# The same kernel and installed disk test_openbsd_login.py resolves --
# this test boots the identical stock 6.9 GENERIC-IP27 image, only with
# the xframe device added to the command line.
KERNEL_REL = os.path.join('assets', 'openbsd', '6.9', 'bsd.IP27')
DISK_REL = os.path.join('assets', 'openbsd', 'spike', 'root.img')

# Same reason test_openbsd_login.py uses it (see that file's own
# docstring): at 128 MiB or below the guest's whole memory bank fits in
# the one slot the machine backs.
MEMORY_MIB = 128

# See this module's own docstring for the real measurement this is based
# on.
BOOT_TIMEOUT_SECONDS = 300

# The xframe device under test: PCI slot 4 function 0, a subsystem vendor
# ID distinct from the card's own vendor ID (0x10a9 is Winbond, not
# 0x17d5 -- exercising that the two are read from different config-space
# fields, not asserted by this test but harmless to carry), and a
# SLIRP-backed netdev matching test_openbsd_net_transfer.py's own use of
# `-nic user` for its network back end.
XFRAME_ARGS = (
    '-netdev', 'user,id=n1',
    '-device', ('xframe,addr=4.0,subsystem-vendor-id=0x10a9,'
               'netdev=n1,mac=02:00:5d:00:00:01'),
)

_TAIL_MAX = 4096


def _drive_pci_probe(sock, log_path, timeout):
    """Read the console until XFRAME_UNCLAIMED_RE matches, a TRIPWIRE
    fires, or timeout -- never sending anything (see this module's own
    docstring for why no interactive drive is needed). Every byte
    received is accumulated into the returned transcript and appended to
    log_path, the same shape test_openbsd_login.py's own _drive_login()
    keeps. Returns the transcript captured up to whichever comes first;
    xframe_pci_verdict() is what actually decides PASS/FAIL from what was
    captured either way.
    """
    sock.settimeout(1.0)
    deadline = time.monotonic() + timeout
    transcript = ""
    tail = ""
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
            tail = (tail + text)[-_TAIL_MAX:]
            if XFRAME_UNCLAIMED_RE.search(tail):
                break
            if any(rx.search(tail) for _why, rx in TRIPWIRES):
                break
    return transcript


class XframePci(QemuSystemTest):
    """Boots the stock OpenBSD 6.9 disk with an xframe adapter attached
    and requires xframe_pci_verdict() to be PASS. See this module's own
    docstring for the full contract; this class only wires it to a real
    boot, the same division test_openbsd_login.py keeps between its pure
    verdict logic and its own QemuSystemTest.
    """

    def _asset_paths(self):
        assets_root = os.environ['ORIGAMI_ASSETS']
        kernel = resolve_asset(assets_root, KERNEL_REL)
        disk = resolve_asset(assets_root, DISK_REL)
        if not (kernel and disk):
            return None
        return kernel, disk

    @skipBigDataTest()
    @skipSlowTest()
    @skipIfMissingEnv('ORIGAMI_ASSETS')
    def test_xframe_pci(self):
        paths = self._asset_paths()
        if paths is None:
            self.skipTest(
                'no kernel/disk at '
                f'<ORIGAMI_ASSETS>/{KERNEL_REL} and '
                f'<ORIGAMI_ASSETS>/{DISK_REL} -- point it at a '
                'checkout with the OpenBSD 6.9 kernel and installed '
                'disk provisioned (nightly assets) to run this test')
        kernel, disk = paths

        self.require_netdev('user')

        vm = self.get_vm(name='xframe_pci')
        vm.set_machine('origin200')
        vm.set_console()
        vm.add_args(
            '-m', str(MEMORY_MIB), '-snapshot',
            '-kernel', kernel,
            '-drive', f'if=none,id=hd0,file={disk},format=raw',
            '-device', 'scsi-hd,bus=scsi.0,scsi-id=1,drive=hd0',
            *XFRAME_ARGS)
        vm.launch()

        log_path = self.log_file('xframe-pci.log')
        try:
            transcript = _drive_pci_probe(
                vm.console_socket, log_path, BOOT_TIMEOUT_SECONDS)
        finally:
            # A VM that crashed or exited on its own has already left; a
            # graceful shutdown() against an already-dead process still
            # raises AbnormalShutdown even though qemu is in fact gone.
            # Matches test_openbsd_login.py's own OpenbsdLogin.test_openbsd_login.
            vm.shutdown(hard=vm.exitcode() is not None)

        if not transcript:
            self.fail(
                'the guest printed nothing at all -- a run that did '
                'not start, not a boot that failed early. Check the '
                'emulator built, and that nothing ended the machine '
                'before it printed anything.')

        report = xframe_pci_verdict(transcript)
        self.assertEqual(
            report['verdict'], 'PASS',
            'xframe PCI presence problems: ' + '; '.join(report['problems']) +
            f' (full transcript: {log_path})')


if __name__ == '__main__':
    QemuSystemTest.main()
