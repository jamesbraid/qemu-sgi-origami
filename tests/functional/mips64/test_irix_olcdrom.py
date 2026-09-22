#!/usr/bin/env python3
#
# IRIX online diagnostic: boot the installed IRIX disk with a SCSI CD-ROM
# holding a generated data CD and require SGI's own olcdrom to report PASS
#
# SPDX-License-Identifier: GPL-2.0-or-later
"""
The CD-ROM sibling of test_irix_diag. It attaches an emulated `scsi-cd` on
the second SCSI adapter -- the Toshiba CD-ROM a real Origin 200 carries at
ID 6 -- backed by a generated data CD, and runs IRIX's `olcdrom`, which
reads the CD through IRIX's real driver stack and checks the reads return
the data the diagnostic expects.

The medium is the full 681,050,112-byte file `olcdrom -gendata` writes:
olcdrom reads across the whole declared medium, so a partial fails with a
logical-block-out-of-range sense. `olcdrom -gendata` is byte-deterministic,
so the medium is a registered, reproducible asset
(`assets/irix/6.5.30/olcdrom-medium.raw`). Its recorded SHA-256 is checked
on load: a wrong file fails loudly rather than spending a boot on a
mysterious read mismatch.

A fresh boot with the medium attached, not a QMP media hot-swap: a hot-swap
leaves IRIX's raw SCSI node not ready, so the medium is present from
power-on. It carries diagnostic data, not a filesystem, so nothing mounts
it; olcdrom reads the raw device node directly.

test_irix_olcdrom_verdict.py proves the verdict logic against the real
captured transcript without booting anything.
"""

import json
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

from lib_prom import (FORK_ROOT, MACHINES, drive_console, require_assets,
                      run_record, utc_now)

try:
    from qemu_test import QemuSystemTest
    from qemu_test import skipSlowTest, skipBigDataTest, skipIfMissingEnv
except (ImportError, RuntimeError):
    QemuSystemTest = None

from test_irix_olcdrom_verdict import olcdrom_verdict, DIAG_DONE_RE


RELEASE = "6.5.30"
FIRMWARE_REL = os.path.join('assets', 'firmware', RELEASE,
                            'ip27prom.payload.bin')
DISK_REL = os.path.join('assets', 'irix', RELEASE, 'root-diags.qcow2')
MEDIUM_REL = os.path.join('assets', 'irix', RELEASE, 'olcdrom-medium.raw')

# The recorded SHA-256 of the olcdrom -gendata medium. require_assets
# raises AssetMismatch when a file at MEDIUM_REL is present but differs.
MEDIUM_SHA256 = \
    'ba0a1466d7125cd350ee4f047677d7a3d9f8a8ed8b13469c92042cfa0bd07829'

MEMORY_MIB = 64

# Boot to a root login plus olcdrom's pattern and random-access passes over
# the medium. 1500 leaves headroom under a contended host.
BOOT_TIMEOUT_SECONDS = 1500

# The raw SCSI node olcdrom reads: adapter 1, target 6.
CD_DEV = "/dev/scsi/sc1d6l0"
DIAG_CMD = f"/usr/diags/bin/olcdrom -device {CD_DEV}"

DRIVER_STEPS = (
    (r"Option\?", "1\n"),
    (r"login:", "root\n"),
    (r"TERM = \(vt100\)", "\n"),
    (r"[#$] ", DIAG_CMD + "\n"),
)


if QemuSystemTest is not None:

    class IrixOlcdrom(QemuSystemTest):
        """Boots the golden-with-diagnostics IRIX disk with a scsi-cd
        holding the generated data CD, logs in, runs olcdrom, and requires
        olcdrom_verdict() to be PASS."""

        @skipBigDataTest()
        @skipSlowTest()
        @skipIfMissingEnv('ORIGAMI_ASSETS')
        def test_irix_olcdrom(self):
            # A medium that is present but the wrong file raises
            # AssetMismatch rather than resolving.
            assets = require_assets(self, FIRMWARE_REL, DISK_REL,
                                    (MEDIUM_REL, MEDIUM_SHA256))
            prom, disk = assets[FIRMWARE_REL], assets[DISK_REL]
            medium = assets[MEDIUM_REL]

            vm = self.get_vm(name='irix_olcdrom')
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
                '-drive',
                f'if=none,id=cd0,file={medium},format=raw,readonly=on',
                '-device', 'scsi-cd,bus=scsi.1,scsi-id=6,lun=0,drive=cd0',
                '-nic', 'user,id=net0')

            started = utc_now()
            vm.launch()
            log_path = self.log_file('irix-olcdrom.log')
            try:
                transcript, reached = drive_console(
                    vm.console_socket, log_path, BOOT_TIMEOUT_SECONDS,
                    DRIVER_STEPS, reached_when=(
                        lambda t: bool(DIAG_DONE_RE.search(t))), vm=vm)
            finally:
                vm.shutdown(hard=vm.exitcode() is not None)
            ended = utc_now()

            if not transcript:
                self.fail(
                    'the guest printed nothing at all -- a run that did not '
                    'start, not a boot that failed early')

            report = olcdrom_verdict(transcript, reached=reached)

            summary_path = self.log_file('irix-olcdrom-summary.json')
            with open(summary_path, 'w', encoding='utf-8') as summary_file:
                json.dump({'schema': 1, 'diagnostic': DIAG_CMD,
                           'reached_summary': reached, 'report': report},
                          summary_file, indent=2, sort_keys=True)
                summary_file.write('\n')

            run_path = self.log_file('run.json')
            with open(run_path, 'w', encoding='utf-8') as run_file:
                json.dump({
                    'schema': 1, 'started_utc': started, 'ended_utc': ended,
                    'console_driver_reached_summary': reached,
                    'timeout_seconds': BOOT_TIMEOUT_SECONDS,
                    'diagnostic': DIAG_CMD,
                    'identity': run_record(FORK_ROOT, self.qemu_bin,
                                          prom=prom, disk=disk,
                                          medium=medium),
                    'verdict': report['verdict'],
                    'problems': report['problems'],
                }, run_file, indent=2, sort_keys=True)
                run_file.write('\n')

            self.assertEqual(
                report['verdict'], 'PASS',
                'IRIX olcdrom diagnostic problems: ' +
                '; '.join(report['problems']) +
                f' (full report: {summary_path})')


if __name__ == '__main__':
    if QemuSystemTest is not None:
        QemuSystemTest.main()
