#!/usr/bin/env python3
#
# IRIX installed-disk boot: boot the plain installed IRIX disk on a
# single-CPU Origin 200, log in as root, and require the system to have
# reached run level 2
#
# SPDX-License-Identifier: GPL-2.0-or-later
"""
The current-state record (docs/NOW.md, project checkout) claims a blank
disk can be partitioned, IRIX 6.5.30 installed over the network, and the
machine rebooted from its own disk into run level 2 with a root console
login, on a single-CPU Origin 200 -- one such boot is captured at
docs/boot-logs/irix.boot.2026-08-31T10:41:29-07:00 (project checkout).
Nothing in this fork's functional-test suite guarded that claim:
test_irix_diag.py boots the installed disk too, but only to run SGI's
own oldisk exerciser on top of it, and would keep passing even on a disk
that no longer reached run level 2, as long as oldisk still ran once
logged in. This test asserts the boot itself, nothing more: root login,
`who -r` reporting run-level 2, and `uname -aR` reporting the expected
release and platform, then a clean shutdown.

Ports test_irix_diag.py's own real-PROM boot and login scaffolding
(single CPU, 64 MiB, user-mode networking, the release's own PROM) onto
the plain installed disk (`assets/irix/6.5.30/root.qcow2`) instead of the
golden-with-diagnostics one -- root.qcow2 is the base image
root-diags.qcow2 is built from by adding the field_diags_irix product
(see that file's own docstring), so the same boot profile applies up to
and past login.

Where this file differs from test_irix_diag.py on purpose: a developer
with ORIGAMI_ASSETS unset gets the usual skip (there is nothing to run
against at all), but a run where ORIGAMI_ASSETS IS set and the PROM or
disk is simply missing FAILS rather than silently skipping.
test_irix_diag.py's skipTest() on a missing asset is exactly the bug
this test does not repeat: a provisioning failure has to show up as a
red check, not a quietly-skipped one that lets a real regression pass
CI unnoticed.
"""

import json
import os
import re
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

from lib_prom import (FORK_ROOT, MACHINES, drive_console, require_assets,
                      run_record, utc_now)

try:
    from qemu_test import QemuSystemTest
    from qemu_test import skipSlowTest, skipBigDataTest, skipIfMissingEnv
except (ImportError, RuntimeError):
    QemuSystemTest = None


RELEASE = "6.5.30"
FIRMWARE_REL = os.path.join('assets', 'firmware', RELEASE,
                            'ip27prom.payload.bin')
DISK_REL = os.path.join('assets', 'irix', RELEASE, 'root.qcow2')

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

# who -r's run-level field and uname -aR's release/version/platform
# fields, exactly as a real capture printed them (docs/boot-logs/
# irix.boot.2026-08-31T10:41:29-07:00, project checkout):
#   who -r:    "   .       run-level 2  Aug 31 10:27    2            0  S"
#   uname -aR: "IRIX64 irix4p-prom 6.5 6.5.30m 07202013 IP27"
RUN_LEVEL_TEXT = "run-level 2"
UNAME_RE = re.compile(r"IRIX64.*6\.5\.30m.*IP27")


if QemuSystemTest is not None:

    class IrixInstalledBoot(QemuSystemTest):
        """Boots the plain installed IRIX disk, logs in, and requires the
        system to have reached run level 2 under its own release and
        platform identity, then shuts down cleanly."""

        @skipBigDataTest()
        @skipSlowTest()
        @skipIfMissingEnv('ORIGAMI_ASSETS')
        def test_irix_installed_boot(self):
            assets = require_assets(self, FIRMWARE_REL, DISK_REL)
            prom, disk = assets[FIRMWARE_REL], assets[DISK_REL]

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
    if QemuSystemTest is not None:
        QemuSystemTest.main()
