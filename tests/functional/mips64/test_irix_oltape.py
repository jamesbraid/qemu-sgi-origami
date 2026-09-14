#!/usr/bin/env python3
#
# IRIX online diagnostic: boot the installed IRIX disk with a SCSI tape
# attached and require SGI's own oltape exerciser to report RSLT PASS
#
# SPDX-License-Identifier: GPL-2.0-or-later
"""
The tape sibling of test_irix_diag. Where that leg runs oldisk against
the disk, this one attaches an emulated `scsi-tape` on the second SCSI
adapter -- the ARCHIVE Python DDS-1 a real Origin 200 carries at ID 5 --
and runs IRIX's `oltape`, the online tape exerciser a field engineer uses,
which drives the tape through IRIX's real driver stack.

It boots the same published golden-with-diagnostics disk
(`assets/irix/6.5.30/root-diags.qcow2`), drives the maintenance menu to
Start System, logs in as root, and runs `oltape` on `/dev/rmt/tps1d5nr`.

Why the `Python 25601` identity and not the recorded `Python 02779-XXX`:
IRIX 6.5.30 classifies a tape by matching its INQUIRY strings against a
table it loads at boot. The recorded drive matches nothing, so IRIX calls
it "unknown" -- a fixed-512-byte profile whose extended-status ioctl the
driver refuses, and oltape cannot even open it. `Python 25601` is a real
ARCHIVE DDS-1 IRIX's own table recognizes as a DAT; composing one is a
faithful arbitrary composition of the device (nothing in the model changes
to make oltape pass), and it is what turns the tape into one the diagnostic
can drive. The recorded identity stays in docs/datasheets/scsi-tape.md as
the machine's inventory.

Why `--tapemark --eot`: those two subtests need the device configured down
(a second /dev/rmt node), and `eot` writes until the medium ends, which an
unbounded image never reaches. Excluding them leaves rw, thrash and block,
which is the write/read/position coverage this exercises. Its fresh empty
tape image is a scratch file per run.

test_irix_oltape_verdict.py proves the verdict logic against the real
captured transcript without booting anything.
"""

import json
import os
import sys
import tempfile

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

from lib_prom import (drive_console, resolve_asset, utc_now, file_record,
                      fork_git_identity)

try:
    from qemu_test import QemuSystemTest
    from qemu_test import skipSlowTest, skipBigDataTest, skipIfMissingEnv
except (ImportError, RuntimeError):
    QemuSystemTest = None

from test_irix_oltape_verdict import oltape_verdict, DIAG_DONE_RE


RELEASE = "6.5.30"
FIRMWARE_REL = os.path.join('assets', 'firmware', RELEASE,
                            'ip27prom.payload.bin')
DISK_REL = os.path.join('assets', 'irix', RELEASE, 'root-diags.qcow2')

# The installed IRIX single-node machine, matching test_irix_diag: one
# CPU, 64 MiB, the release's own PROM, user-mode networking so IRIX's
# network init completes without a server.
MEMORY_MIB = 64

# Standalone boot to a root login took 265s real on a quiet host; oltape's
# rw/thrash/block on a fresh tape is seconds. 1500 leaves headroom under a
# contended host and under the meson timeout registered for this test.
BOOT_TIMEOUT_SECONDS = 1500

# The tape node: no-rewind, so the diagnostic controls position itself.
TAPE_DEV = "/dev/rmt/tps1d5nr"
DIAG_CMD = f"/usr/diags/bin/oltape --tapemark --eot -d {TAPE_DEV}"

# The ARCHIVE Python DDS-1 identity IRIX recognizes as a DAT.
TAPE_VENDOR = "ARCHIVE"
TAPE_PRODUCT = "Python 25601"

# Drive the PROM menu to Start System, answer the login's TERM prompt, log
# in as root, and run the diagnostic. The steps fire once each, in order,
# on the first tail match.
DRIVER_STEPS = (
    (r"Option\?", "1\n"),
    (r"login:", "root\n"),
    (r"TERM = \(vt100\)", "\n"),
    (r"[#$] ", DIAG_CMD + "\n"),
)


if QemuSystemTest is not None:

    _FORK_ROOT = os.path.dirname(os.path.dirname(os.path.dirname(
        os.path.dirname(os.path.abspath(__file__)))))

    class IrixOltape(QemuSystemTest):
        """Boots the golden-with-diagnostics IRIX disk with a scsi-tape,
        logs in, runs oltape, and requires oltape_verdict() to be PASS."""

        def _asset_paths(self):
            assets_root = os.environ.get('ORIGAMI_ASSETS')
            prom = resolve_asset(assets_root, FIRMWARE_REL)
            disk = resolve_asset(assets_root, DISK_REL)
            if not (prom and disk):
                return None
            return prom, disk

        @skipBigDataTest()
        @skipSlowTest()
        @skipIfMissingEnv('ORIGAMI_ASSETS')
        def test_irix_oltape(self):
            paths = self._asset_paths()
            if paths is None:
                # skipIfMissingEnv above already skipped the case where
                # ORIGAMI_ASSETS is unset entirely -- getting here means
                # it IS set, so a missing PROM or disk is a provisioning
                # failure, not an absent-assets developer run, and must
                # fail loudly: a silent skip here would let a real
                # scsi-tape regression pass CI unnoticed.
                self.fail(
                    'ORIGAMI_ASSETS is set but the PROM/disk are missing '
                    f'at <ORIGAMI_ASSETS>/{FIRMWARE_REL} and '
                    f'<ORIGAMI_ASSETS>/{DISK_REL} -- provision them '
                    '(nightly assets) rather than silently skipping a '
                    'claimed capability')
            prom, disk = paths

            # A fresh empty tape image per run: oltape writes and reads it.
            tape_fd, tape_path = tempfile.mkstemp(
                prefix='oltape-', suffix='.tap', dir=self.workdir)
            os.close(tape_fd)

            vm = self.get_vm(name='irix_oltape')
            vm.set_machine(
                'origin200,synthesized-firmware=off,debug-leds=off')
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
                f'if=none,id=tape0,file={tape_path},format=raw',
                '-device',
                'scsi-tape,bus=scsi.1,scsi-id=5,lun=0,drive=tape0,'
                f'vendor={TAPE_VENDOR},product={TAPE_PRODUCT}',
                '-nic', 'user,id=net0')

            started = utc_now()
            vm.launch()
            log_path = self.log_file('irix-oltape.log')
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

            report = oltape_verdict(transcript, reached=reached)

            summary_path = self.log_file('irix-oltape-summary.json')
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
                    'tape_identity': {'vendor': TAPE_VENDOR,
                                      'product': TAPE_PRODUCT},
                    'identity': {
                        'fork': fork_git_identity(_FORK_ROOT),
                        'qemu_binary': file_record(self.qemu_bin),
                        'payload': {'prom': file_record(prom),
                                    'disk': file_record(disk)}},
                    'verdict': report['verdict'],
                    'problems': report['problems'],
                }, run_file, indent=2, sort_keys=True)
                run_file.write('\n')

            self.assertEqual(
                report['verdict'], 'PASS',
                'IRIX oltape diagnostic problems: ' +
                '; '.join(report['problems']) +
                f' (full report: {summary_path})')


if __name__ == '__main__':
    if QemuSystemTest is not None:
        QemuSystemTest.main()
