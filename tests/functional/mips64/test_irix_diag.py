#!/usr/bin/env python3
#
# IRIX online diagnostic: boot the installed IRIX disk with the vendor's
# online diagnostics on it, log in, and require SGI's own oldisk test to
# report RSLT PASS
#
# SPDX-License-Identifier: GPL-2.0-or-later
"""
The layer above the firmware. The MDK, POD and fx legs run diagnostics
before or instead of IRIX; this one runs IRIX's own online diagnostics
(`/usr/diags/bin`) on a booted, multi-user system -- the diagnostic
stack a field engineer uses on a live machine, and the one that
exercises the hardware through IRIX's real drivers rather than the
firmware's.

It boots the published golden-with-diagnostics disk
(`assets/irix/6.5.30/root-diags.qcow2`: the installed IRIX 6.5.30 root
with the `field_diags_irix` product added and its environment loaded by
a post-install reboot -- the project checkout's irix-online-diagnostics
notebook records how the asset is built and why the reboot matters),
drives the maintenance menu to Start System, logs in as root, and runs
`oldisk`, the disk exerciser. oldisk writes and reads a pattern through
the qlogic controller under IRIX and prints the RSLT-tagged result the
field manual documents; a clean run ends `RSLT async-direct PASS` and a
`META TOTAL 1 0` summary (one pass, zero non-passes).

Why oldisk and not a CPU or memory diagnostic: the online CPU/FPU
diagnostic (`torpedo`) runs a fixed thousand-loop stress workload
measured in guest time, which dilates many-fold under emulation, so a
full run is impractical and a bounded run duplicates the firmware
MDK `instgen`/`memory.mdk` coverage. oldisk reaches a definitive PASS
in about a minute of guest time and covers the one path the firmware
legs cannot: a real IRIX driver stack. The other online tests
(`olvst`, `oltape`, `linktest`, `pandora`, ...) are available on the
disk for manual runs; this leg asserts the one that is fast and
definitive.

test_irix_diag_verdict.py proves the verdict logic against the real
captured transcript without booting anything.
"""

import json
import os
import re
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

from lib_prom import drive_console, resolve_asset

try:
    from qemu_test import QemuSystemTest
    from qemu_test import skipSlowTest, skipBigDataTest, skipIfMissingEnv
except (ImportError, RuntimeError):
    QemuSystemTest = None

from test_irix_diag_verdict import diag_verdict


RELEASE = "6.5.30"
FIRMWARE_REL = os.path.join('assets', 'firmware', RELEASE,
                            'ip27prom.payload.bin')
DISK_REL = os.path.join('assets', 'irix', RELEASE, 'root-diags.qcow2')

# The installed IRIX single-node machine: one CPU, 64 MiB, the release's
# own PROM, user-mode networking so IRIX's network init completes without
# a server (the diagnostics run locally and need none).
MEMORY_MIB = 64

# Standalone boot to a root login took 265s real on a quiet host, and
# oldisk about 60s more. 1500 leaves headroom under a contended host and
# under the 1560s meson timeout registered for this test.
BOOT_TIMEOUT_SECONDS = 1500

# oldisk exercises a scratch file; -filesize 8 (MiB) is enough for the
# async-direct pattern pass and keeps the run near a minute.
DIAG_CMD = "/usr/diags/bin/oldisk -filename /tmp/oldisk.tf -filesize 8"

# Drive the PROM menu to Start System, answer the login's TERM prompt,
# log in as root, and run the diagnostic. IRIX's default root shell is
# csh and the first login asks for TERM; a bare newline takes the
# default. The steps fire once each, in order, on the first tail match.
DRIVER_STEPS = (
    (r"Option\?", "1\n"),
    (r"login:", "root\n"),
    (r"TERM = \(vt100\)", "\n"),
    (r"[#$] ", DIAG_CMD + "\n"),
)
# oldisk's own end-of-run summary line, WITH its pass/non-pass counts:
# waiting only for "META TOTAL" broke the capture a beat before the two
# numbers on that line arrived, so require the counts. The line is wrapped
# in ANSI colour, but the counts follow TOTAL as plain text.
DIAG_DONE_RE = re.compile(rb"META\s+TOTAL\s+\d+\s+\d+")


def _utc_now():
    import datetime
    return datetime.datetime.now(datetime.UTC).isoformat().replace(
        "+00:00", "Z")


def _file_record(path):
    import hashlib
    resolved = os.path.realpath(path)
    digest = hashlib.sha256()
    with open(resolved, "rb") as source:
        for chunk in iter(lambda: source.read(1024 * 1024), b""):
            digest.update(chunk)
    return {"path": str(path), "resolved_path": resolved,
            "sha256": digest.hexdigest(),
            "size_bytes": os.path.getsize(resolved)}


def _fork_git_identity(tree):
    import subprocess
    try:
        commit = subprocess.run(
            ["git", "-C", str(tree), "rev-parse", "HEAD"],
            capture_output=True, text=True, timeout=30)
        status = subprocess.run(
            ["git", "-C", str(tree), "status", "--porcelain=v1",
             "--untracked-files=normal"],
            capture_output=True, text=True, timeout=30)
    except (OSError, subprocess.TimeoutExpired):
        return None
    if commit.returncode != 0 or status.returncode != 0:
        return None
    return {"path": str(tree), "commit": commit.stdout.strip(),
            "dirty": bool(status.stdout.strip())}


if QemuSystemTest is not None:

    _FORK_ROOT = os.path.dirname(os.path.dirname(os.path.dirname(
        os.path.dirname(os.path.abspath(__file__)))))

    class IrixDiag(QemuSystemTest):
        """Boots the golden-with-diagnostics IRIX disk, logs in, runs
        oldisk, and requires diag_verdict() to be PASS."""

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
        def test_irix_diag(self):
            paths = self._asset_paths()
            if paths is None:
                # skipIfMissingEnv above already skipped the case where
                # ORIGAMI_ASSETS is unset entirely -- getting here means
                # it IS set, so a missing PROM or disk is a provisioning
                # failure, not an absent-assets developer run, and must
                # fail loudly: a silent skip here would let a real
                # oldisk regression pass CI unnoticed.
                self.fail(
                    'ORIGAMI_ASSETS is set but the PROM/disk are missing '
                    f'at <ORIGAMI_ASSETS>/{FIRMWARE_REL} and '
                    f'<ORIGAMI_ASSETS>/{DISK_REL} -- provision them '
                    '(nightly assets) rather than silently skipping a '
                    'claimed capability')
            prom, disk = paths

            vm = self.get_vm(name='irix_diag')
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
                '-nic', 'user,id=net0')

            started = _utc_now()
            vm.launch()
            log_path = self.log_file('irix-diag.log')
            try:
                transcript, reached = drive_console(
                    vm.console_socket, log_path, BOOT_TIMEOUT_SECONDS,
                    DRIVER_STEPS, reached_when=(
                        lambda t: bool(DIAG_DONE_RE.search(t))))
            finally:
                vm.shutdown(hard=vm.exitcode() is not None)
            ended = _utc_now()

            if not transcript:
                self.fail(
                    'the guest printed nothing at all -- a run that did not '
                    'start, not a boot that failed early')

            report = diag_verdict(transcript, reached=reached)

            summary_path = self.log_file('irix-diag-summary.json')
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
                    'identity': {
                        'fork': _fork_git_identity(_FORK_ROOT),
                        'qemu_binary': _file_record(self.qemu_bin),
                        'payload': {'prom': _file_record(prom),
                                    'disk': _file_record(disk)}},
                    'verdict': report['verdict'],
                    'problems': report['problems'],
                }, run_file, indent=2, sort_keys=True)
                run_file.write('\n')

            self.assertEqual(
                report['verdict'], 'PASS',
                'IRIX online diagnostic problems: ' +
                '; '.join(report['problems']) +
                f' (full report: {summary_path})')


if __name__ == '__main__':
    if QemuSystemTest is not None:
        QemuSystemTest.main()
