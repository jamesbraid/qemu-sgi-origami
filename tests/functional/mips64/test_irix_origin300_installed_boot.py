#!/usr/bin/env python3
#
# Boot installed IRIX 6.5.30 on the two-processor Origin 300
#
# SPDX-License-Identifier: GPL-2.0-or-later
"""Check the installed IP35 guest beyond the stock PROM menu.

The captured installed-disk run in the Origami checkout
(docs/boot-logs/irix.origin300-2cpu-no-scache-sbe.
2026-09-24T23:08:06-07:00) reaches the IRIX console login. This test
uses that disk with the same boot-flash and identity inputs as the PROM
test, then requires a root shell, run level 2 and the IP35 release.
"""

import os
import re

import lib_origin300
from lib_prom import drive_console, require_assets
from qemu_test import QemuSystemTest, skipBigDataTest, skipIfMissingEnv, skipSlowTest


DISK_REL = os.path.join('assets', 'irix', '6.5.30',
                        'root-diags-origin300.qcow2')
DISK_SHA256 = (
    '150cf2c125cb3bd55764acfeec00804899ff3140cc536cf63a254765faa29390')
BOOT_TIMEOUT_SECONDS = 900

DRIVER_STEPS = (
    (r'Option\?', '1\n'),
    (r'login:', 'root\n'),
    (r'TERM = \(vt100\)', '\n'),
    (r'[#$] ', 'who -r\n'),
    (r'[#$] ', 'uname -aR\n'),
    (r'[#$] ', 'sync; /etc/shutdown -y -g0 -i0\n'),
)
UNAME_RE = re.compile(r'IRIX64.*6\.5\.30m.*IP35')
SHUTDOWN_STARTED_RE = re.compile(rb'Shutdown started')


class IrixOrigin300InstalledBoot(QemuSystemTest):

    @skipBigDataTest()
    @skipSlowTest()
    @skipIfMissingEnv('ORIGAMI_ASSETS')
    def test_irix_origin300_installed_boot(self):
        assets_root = os.environ['ORIGAMI_ASSETS']
        assets = require_assets(
            self, *lib_origin300.ASSET_SPECS, (DISK_REL, DISK_SHA256))
        disk = assets[DISK_REL]
        machine, args = lib_origin300.launch_args(
            assets_root, 2, self.workdir)

        self.set_machine('origin300')
        self.vm.set_console()
        self.vm.set_machine(machine)
        self.vm.add_args(
            *args,
            '-drive',
            f'if=none,id=disk0,file={disk},format=qcow2,snapshot=on',
            '-device', 'scsi-hd,bus=scsi.0,scsi-id=1,lun=0,drive=disk0',
            '-nic', 'user,id=net0',
            '-S')

        self.vm.launch()
        self.vm.cmd('cont')
        log_path = self.log_file('irix-origin300-installed-boot.log')
        try:
            transcript, reached = drive_console(
                self.vm.console_socket, log_path, BOOT_TIMEOUT_SECONDS,
                DRIVER_STEPS,
                reached_when=lambda t: bool(SHUTDOWN_STARTED_RE.search(t)),
                vm=self.vm)
        finally:
            self.vm.shutdown(hard=self.vm.exitcode() is not None)
        self.assertIn('ALL DIAGS PASSED', transcript,
                      f'PROM diagnostics did not pass: {log_path}')
        self.assertIn('System Maintenance Menu', transcript,
                      f'PROM did not reach the maintenance menu: {log_path}')
        self.assertIn('run-level 2', transcript,
                      f'who -r did not report run level 2: {log_path}')
        self.assertRegex(transcript, UNAME_RE,
                         f'uname did not report IRIX 6.5.30 IP35: {log_path}')
        self.assertTrue(reached,
                        f'the guest did not begin a clean shutdown: {log_path}')


if __name__ == '__main__':
    QemuSystemTest.main()
