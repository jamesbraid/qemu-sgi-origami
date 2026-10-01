#!/usr/bin/env python3
#
# xframe PCI presence: boot the stock OpenBSD/IP27 disk WITH an emulated
# Xframe 10G Ethernet adapter attached, and require OpenBSD's own PCI
# autoconfiguration to report it -- unclaimed, since stock GENERIC-IP27
# carries no driver for it, but genuinely composed onto the guest's PCI
# bus through the machine's Bridge
#
# SPDX-License-Identifier: BSD-3-Clause
"""
Boots the same kernel and installed disk as the network transfer test
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

xframe_pci_verdict() below is the check this file wires to a real boot.
Its expected text was derived and cross-checked against OpenBSD's own
source, the real kernel binary these tests boot, and this fork's own
xframe device model: the device is composed at PCI slot 4 function 0 with
vendor 0x17d5 and product 0x5831, and stock GENERIC-IP27 has no driver
matching that pair, so autoconfiguration reports it by name and declares
it not configured.

_pci_probe_reached() below, past the netboot dialogue itself, never sends
the guest anything: OpenBSD's PCI bus scan finishes well before any
interactive question is asked -- in a real captured boot of this exact
kernel and disk, every PCI device line appears before "boot device:
'unknown' unrecognized." and "root device: sd0a", the first point that
transcript ever needs an answer. This test only needs to read until
XFRAME_UNCLAIMED_RE matches, a trip wire fires, or the timeout runs out;
unlike test_openbsd_net_transfer.py's own _drive_login(), it never needs a login
sequence at all.

Boots through the real stock IP27 PROM's own netboot path rather than a
direct `-kernel` load, the same conversion test_openbsd_net_transfer.py and
test_linux_kernel.py make and for the same reason (the plan this test
belongs to retires synthesized firmware): lib_prom's prom_netboot_steps()
drives the System Maintenance Menu and command monitor to `boot -f
bootp()/bsd.IP27` over a second SLIRP usermode network on the machine's
onboard IOC3, separate from the xframe adapter's own -netdev n1. The
onboard controller already exists in the machine model whether or not a
backend is attached (test_openbsd_net_transfer.py's own reference boot with
`-nic none` already shows OpenBSD attaching it), so giving it a real
netboot backend here changes nothing about the PCI topology OpenBSD's
autoconfiguration reports -- the exact concern this test's own pass
condition depends on.

BOOT_TIMEOUT_SECONDS below is a real measurement, not a guess: a run of
this exact test against real provisioned assets (podman, this worktree
mounted read-write and the asset directory mounted read-only as
QEMU_TEST_SGI_ASSET_DIR) reached XFRAME_UNCLAIMED_RE and returned PASS in about
41s wall-clock end to end -- including podman container startup and the
real PROM boot and netboot this port added -- on a host already busy
running other work. 300 keeps roughly the same seven-times margin over
that as it kept over the direct-`-kernel` measurement this timeout
predates; the meson timeout entry for this test keeps the usual 60s
margin over this budget.

Guarded the same way test_openbsd_net_transfer.py is: @skipBigDataTest() (the
disk is 2 GiB), @skipSlowTest() (booting to firmware-plus-early-kernel is
not instant even though this test stops well short of a full boot), and
@skipIfMissingEnv('QEMU_TEST_SGI_ASSET_DIR') for a developer with no assets at
all. QEMU_TEST_SGI_ASSET_DIR being set with either asset missing underneath it
is
a provisioning failure, not an absent-assets run, and fails the test
loudly rather than skipping it, the same as test_openbsd_net_transfer.py.
"""

import os
import re

from qemu_test import (QemuSystemTest, skipBigDataTest, skipIfMissingEnv,
                       skipSlowTest)
from qemu_test.asset import LocalAsset

from lib_net import OPENBSD_TRIPWIRES, ordered_stages
from lib_prom import (FIRMWARE_REL, MACHINES, drive_console,
                      prom_netboot_steps)
from lib_prom_sash import RELEASES


# See this module's own docstring for exactly how the unclaimed-device
# text was derived and cross-checked. The revision byte is left as any two
# hex digits deliberately; everything else is pinned exactly. The bus line
# comes first: it is proof PCI autoconfiguration ran at all, before this
# looks for the xframe line specifically.
XFRAME_UNCLAIMED_RE = re.compile(
    r'"Neterion Xframe" rev 0x[0-9a-fA-F]{2} at pci0 dev 4 function 0 '
    r'not configured')
STAGES = (
    ('never reached PCI bus enumeration (no "pci0 at ... bus ..." line)',
     re.compile(r"pci0 at \S+ bus \d+")),
    ('never saw the xframe adapter reported unclaimed at pci0 dev 4 '
     'function 0 -- expected a line naming vendor 0x17d5 ("Neterion") '
     'product 0x5831 ("Xframe") as "not configured"',
     XFRAME_UNCLAIMED_RE),
)


def xframe_pci_verdict(transcript):
    """PASS iff transcript shows, in order: the guest's PCI bus
    enumerated and then the xframe adapter reported unclaimed at that
    bus, PCI slot 4 function 0 -- and no trip wire fires before that
    unclaimed-device line is reached (or anywhere in the transcript, if
    it is never reached).

    Returns {"verdict": "PASS"|"FAIL", "problems": [...]}.
    """
    problems = ordered_stages(transcript, STAGES, OPENBSD_TRIPWIRES)
    return {
        "verdict": "PASS" if not problems else "FAIL",
        "problems": problems,
    }


# The same kernel and installed disk test_openbsd_net_transfer.py resolves --
# this test boots the identical stock 6.9 GENERIC-IP27 image, only with
# the xframe device added to the command line.
KERNEL_REL = os.path.join('openbsd', '6.9', 'bsd.IP27')
DISK_REL = os.path.join('openbsd', 'spike', 'root.img')

# The boot file this test's own SLIRP TFTP server serves the kernel
# under -- see prom_netboot_steps()'s own docstring for why this need
# not match the kernel's name on disk.
NETBOOT_BOOTFILE = 'bsd.IP27'
NETBOOT_STEPS = prom_netboot_steps(NETBOOT_BOOTFILE)

# Same reason test_openbsd_net_transfer.py uses it (see that file's own
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


def _pci_probe_reached(transcript):
    """The drive's end condition once the netboot dialogue has been
    answered: XFRAME_UNCLAIMED_RE has matched, or a TRIPWIRE fired --
    never sending anything beyond the netboot commands themselves (see
    this module's own docstring for why no interactive drive is needed
    past that point). xframe_pci_verdict() is what actually decides
    PASS/FAIL from the transcript captured either way; this only stops
    the read early once one of those two things is already visible.
    """
    text = transcript.decode("utf-8", errors="replace")
    return bool(XFRAME_UNCLAIMED_RE.search(text) or
               any(rx.search(text) for _why, rx in OPENBSD_TRIPWIRES))


class XframePci(QemuSystemTest):
    """Boots the stock OpenBSD 6.9 disk with an xframe adapter attached
    and requires xframe_pci_verdict() to be PASS. See this module's own
    docstring for the full contract; this class only wires it to a real
    boot, the same division test_openbsd_net_transfer.py keeps between its pure
    verdict logic and its own QemuSystemTest.
    """

    ASSET_FIRMWARE = LocalAsset(
        FIRMWARE_REL, RELEASES['6.5.30']['prom_sha256'])
    ASSET_KERNEL = LocalAsset(
        KERNEL_REL,
        '5c132640eb510cbc0e63bbcca1169f587a168610837687b0d1d02950a349a7a1')
    ASSET_DISK = LocalAsset(
        DISK_REL,
        '55c53b0268e55682d38485d902e47e9fbeb9e9ef77caf1625d5522dac33c70fd')

    @skipBigDataTest()
    @skipSlowTest()
    @skipIfMissingEnv('QEMU_TEST_SGI_ASSET_DIR')
    def test_xframe_pci(self):
        firmware = self.ASSET_FIRMWARE.fetch()
        kernel = self.ASSET_KERNEL.fetch()
        disk = self.ASSET_DISK.fetch()

        self.require_netdev('user')

        vm = self.get_vm(name='xframe_pci')
        vm.set_machine(MACHINES['origin200'])
        vm.set_console()
        vm.add_args(
            '-m', str(MEMORY_MIB), '-snapshot',
            '-bios', firmware,
            '-nic', ('user,id=net0,'
                    f'tftp={os.path.dirname(kernel)},'
                    f'bootfile={NETBOOT_BOOTFILE}'),
            '-drive', f'if=none,id=hd0,file={disk},format=raw',
            '-device', 'scsi-hd,bus=scsi.0,scsi-id=1,drive=hd0',
            *XFRAME_ARGS)
        vm.launch()

        log_path = self.log_file('xframe-pci.log')
        try:
            transcript, _reached = drive_console(
                vm.console_socket, log_path, BOOT_TIMEOUT_SECONDS,
                NETBOOT_STEPS, reached_when=_pci_probe_reached, vm=vm)
        finally:
            # A VM that crashed or exited on its own has already left; a
            # graceful shutdown() against an already-dead process still
            # raises AbnormalShutdown even though qemu is in fact gone.
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
