#!/usr/bin/env python3
#
# IRIX installed-disk boot: boot the plain installed IRIX disk on a
# single-CPU Origin 200, log in as root, and require the system to have
# reached run level 2
#
# SPDX-License-Identifier: GPL-2.0-or-later
"""Boot the installed IRIX 6.5.30 disk on a single-CPU Origin 200.

The test starts from `irix/6.5.30/root.qcow2`; it does not run the
installer. It requires a root login, run level 2, the expected release
and platform, and a clean shutdown.

An unset QEMU_TEST_SGI_ASSET_DIR skips the test. If the directory is set
but the PROM or disk is missing, asset fetching fails the test.
"""

import json
import os
import re
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

from lib_prom import FORK_ROOT, MACHINES, drive_console, run_record, utc_now
from qemu_test import (QemuSystemTest, skipBigDataTest, skipIfMissingEnv,
                       skipSlowTest)
from qemu_test.asset import LocalAsset


RELEASE = "6.5.30"
FIRMWARE_REL = os.path.join('firmware', RELEASE,
                            'ip27prom.payload.bin')
DISK_REL = os.path.join('irix', RELEASE, 'root.qcow2')

# The installed IRIX single-node machine: one CPU, 64 MiB, the release's
# own PROM, user-mode networking so IRIX's network init completes without
# a server -- the same profile test_irix_diag.py boots (root.qcow2 is the
# base root-diags.qcow2 is built from; nothing between power-on and login
# differs between the two disks).
MEMORY_MIB = 64

# The plain boot to a root login measured about 265s real on a quiet
# host, the same figure test_irix_diag.py's own BOOT_TIMEOUT_SECONDS
# comment records for the golden-with-diagnostics disk built from the
# same base image. 900 leaves comfortable headroom under a loaded host
# and a 60s margin under the 960s meson timeout registered for this test.
BOOT_TIMEOUT_SECONDS = 900

# Drive the PROM menu to Start System, answer the login's TERM prompt,
# log in as root, and run the two commands that prove the boot reached a
# working run-level-2 console, then shut down cleanly. IRIX's default
# root shell is csh and the first login asks for TERM; a bare newline
# takes the default. Each step fires once, in order, on the first tail
# match -- the last three share the "[#$] " prompt pattern and fire on
# successive prompts in list order: lib_prom.drive_console resets its
# match window to empty on every fire, and the next prompt cannot exist
# in the guest's output before this test has actually sent the command
# that produces it, so the three cannot race each other out of order.
DRIVER_STEPS = (
    (r"Option\?", "1\n"),
    (r"login:", "root\n"),
    (r"TERM = \(vt100\)", "\n"),
    (r"[#$] ", "who -r\n"),
    (r"[#$] ", "uname -aR\n"),
    (r"[#$] ", "sync; /etc/shutdown -y -g0 -i0\n"),
)

# Evidence the shutdown command was accepted -- ends the capture promptly
# rather than waiting out the PROM's own power-on cycle on the reboot
# that follows a completed shutdown.
SHUTDOWN_STARTED_RE = re.compile(rb"Shutdown started")

# Expected guest output fields checked by this test:
#   who -r:    "   .       run-level 2  Aug 31 10:27    2            0  S"
#   uname -aR: "IRIX64 irix4p-prom 6.5 6.5.30m 07202013 IP27"
RUN_LEVEL_TEXT = "run-level 2"
UNAME_RE = re.compile(r"IRIX64.*6\.5\.30m.*IP27")
PROM_SHA256 = (
    'c2c69bf33c375b43ec7ac511fab41ddf595be1b6838ae717e58a9a1273437fdd')
DISK_SHA256 = (
    '15b4bc1992845b6242948a9e944ce39682b7decdaee1e5474579418fb3a19c12')


class IrixInstalledBoot(QemuSystemTest):
    """Boots the plain installed IRIX disk, logs in, and requires the
    system to have reached run level 2 under its own release and
    platform identity, then shuts down cleanly."""

    # Pin the stock installed disk and matching release PROM documented by
    # the captured test inputs; guest assertions describe its expected state.
    ASSET_PROM = LocalAsset(FIRMWARE_REL, PROM_SHA256)
    ASSET_DISK = LocalAsset(DISK_REL, DISK_SHA256)

    @skipBigDataTest()
    @skipSlowTest()
    @skipIfMissingEnv('QEMU_TEST_SGI_ASSET_DIR')
    def test_irix_installed_boot(self):
        prom = self.ASSET_PROM.fetch()
        disk = self.ASSET_DISK.fetch()

        vm = self.get_vm(name='irix_installed_boot')
        vm.set_machine(MACHINES['origin200'])
        vm.set_console()
        vm.add_args(
            '-accel', 'tcg,thread=single',
            '-cpu', 'R10000', '-smp', '1', '-m', str(MEMORY_MIB),
            '-bios', str(prom),
            '-audio', 'none',
            '-drive',
            f'if=none,id=disk0,file={disk},format=qcow2,snapshot=on',
            '-device', 'scsi-hd,bus=scsi.0,scsi-id=1,lun=0,drive=disk0',
            '-nic', 'user,id=net0')

        started = utc_now()
        vm.launch()
        log_path = self.log_file('irix-installed-boot.log')
        try:
            transcript, reached = drive_console(
                vm.console_socket, log_path, BOOT_TIMEOUT_SECONDS,
                DRIVER_STEPS, reached_when=(
                    lambda t: bool(SHUTDOWN_STARTED_RE.search(t))),
                vm=vm)
        finally:
            vm.shutdown(hard=vm.exitcode() is not None)
        ended = utc_now()

        if not transcript:
            self.fail(
                'the guest printed nothing at all -- a run that did '
                'not start, not a boot that failed early')

        run_level_seen = RUN_LEVEL_TEXT in transcript
        uname_match = UNAME_RE.search(transcript)

        run_path = self.log_file('run.json')
        with open(run_path, 'w', encoding='utf-8') as run_file:
            json.dump({
                'schema': 1, 'started_utc': started, 'ended_utc': ended,
                'console_driver_reached_shutdown': reached,
                'timeout_seconds': BOOT_TIMEOUT_SECONDS,
                'run_level_seen': run_level_seen,
                'uname_line': (uname_match.group(0)
                              if uname_match else None),
                'identity': run_record(FORK_ROOT, self.qemu_bin,
                                      prom=prom, disk=disk),
            }, run_file, indent=2, sort_keys=True)
            run_file.write('\n')

        self.assertIn(
            RUN_LEVEL_TEXT, transcript,
            "who -r never reported run-level 2 -- console log: "
            f"{log_path}, run record: {run_path}")
        self.assertRegex(
            transcript, UNAME_RE,
            "uname -aR never reported IRIX64 ... 6.5.30m ... IP27 -- "
            f"console log: {log_path}, run record: {run_path}")
        self.assertTrue(
            reached,
            "the guest never confirmed a clean shutdown after login "
            f"-- console log: {log_path}, run record: {run_path}")


if __name__ == '__main__':
    QemuSystemTest.main()
