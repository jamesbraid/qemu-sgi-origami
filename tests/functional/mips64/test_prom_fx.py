#!/usr/bin/env python3
#
# fx exercise: netboot the 64-bit disk formatter BY PATH, open the SCSI
# disk at target 1 in extended mode, and require a write-compare
# sequential exercise over 100,000 blocks to finish with zero errors
#
# SPDX-License-Identifier: GPL-2.0-or-later
"""
fx.64 is the vendor's standalone disk formatter and exerciser -- the
program the install rig already netboots to label a disk. Its exercise
menu is a SCSI diagnostic in its own right: a sequential write-compare
pass writes a pattern through the qlogic controller and reads it back,
which nothing else in the nightly does before an operating system is
installed. This test drives that pass over 100,000 blocks (50 MB) of a
scratch disk at controller 0 target 1 and holds fx to its own error
ledger: TOTAL ERR BLOCKS r0(0 hard) w0(0 hard).

Extended mode ("Do you require extended mode with all options
available?") is answered yes, because only extended mode offers the
write modifiers; safe mode's exercises are read-only. fx confirms before
writing ("about to destroy data on disk ... ok?"), which the driver
answers yes, on a disk this test created seconds earlier.

Deliberately NOT driven: the butterfly exercise. On this machine it
waits forever on a completion flag its first I/O never sets (the same
disk's sequential and random exercises complete), which is a wall in the
model's SCSI path recorded in the project checkout's firmware-diags
notebook and routed to that path's owner; a test for it belongs with
the fix.

Everything structural -- PROM digest refusal, path-structured netboot
staging, the one-shot-per-step console driver, provenance records --
is shared with test_prom_mdk.py. test_prom_fx_verdict.py proves the
verdict logic against real captured transcripts.
"""

import json
import os
import re
import shutil
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

from test_prom_mdk import (
    FIRMWARE_REL,
    _drive_console,
    _file_record,
    _fork_git_identity,
    _utc_now,
    payload_identity_problems,
)

try:
    from qemu_test import QemuSystemTest
except (ImportError, RuntimeError):
    QemuSystemTest = None


NETBOOT_PATH = "stand/fx.64"
FX_REL = os.path.join('assets', 'boot', '6.5-base', 'fx.64')
# The 6.5-base disc's fx.64, as tools/extract-irix boot records it.
FX_SHA256 = (
    "c4cbacad625ee4587e24ba69f4610293383452b0c69114a4e3b3804e1e8b3100"
)

NBLOCKS = 100000
SCRATCH_DISK_BYTES = 1 << 30

ASSERTIONS = (
    ("path-structured netboot fetch",
     f"Obtaining {NETBOOT_PATH} from server"),
    ("fx banner", "SGI Version 6.5 ARCS BE64"),
    ("disk opened", "...opening dksc(0,1,0)"),
    ("drive self-test", "...drive selftest...OK"),
    ("write-compare scan started",
     f"sequential pass 1: scanning [0, {NBLOCKS}] ({NBLOCKS} blocks)"),
    ("scan completed", "100%"),
    ("zero error blocks", "TOTAL ERR BLOCKS r0(0 hard) w0(0 hard)"),
)

# fx's own error ledger (the zero-error line above) is the authority for
# miscompares and hard errors; the word "miscompare" itself appears in
# the exercise menu and its help, so it cannot be a marker.
FAILURE_MARKERS = (
    "hard error",
    "Failed to open",
    "no such device",
    "Unexpected",
)


def missing_assertions(log):
    return [name for name, evidence in ASSERTIONS if evidence not in log]


def failure_markers_present(log):
    return [marker for marker in FAILURE_MARKERS if marker in log]


def verdict(log, reached=True):
    """PASS iff fx netbooted by path, opened the disk, ran the full
    write-compare scan, reported zero error blocks, and printed no
    failure spelling; and the driver saw the error ledger before
    timeout."""
    problems = []
    for name in missing_assertions(log):
        problems.append(f"missing {name}")
    for marker in failure_markers_present(log):
        problems.append(f"failure output present: {marker!r}")
    if not reached:
        problems.append(
            "console driver did not reach fx's error ledger before "
            "timeout -- the run never proved the exercise finished")
    return {
        "verdict": "PASS" if not problems else "FAIL",
        "problems": problems,
    }


# Boot to the menu is minutes under a contended host; fx loads in
# seconds and the 100,000-block write-compare took 19s. 840 keeps the
# usual 60s margin under the 900s meson timeout.
BOOT_TIMEOUT_SECONDS = 840

# Each step fires once, in order, on the first tail match after the
# previous step -- fx reprints some prompts (>>, fx/exercise>) and the
# order below is the order they appear in a clean run.
DRIVER_STEPS = (
    (r"Option\?", "5\n"),
    (r">>", "unsetenv netaddr\n"),
    (r">>", f"boot -f bootp()/{NETBOOT_PATH}\n"),
    (r"extended mode with all options available\?", "yes\n"),
    (r"\"device-name\" = \(dksc\)", "\n"),
    (r"ctlr# =", "0\n"),
    (r"drive# =", "1\n"),
    (r"fx>", "exe\n"),
    (r"fx/exercise>", "seq\n"),
    (r"modifier =", "wr-c\n"),
    (r"starting block# =", "\n"),
    (r"nblocks =", f"{NBLOCKS}\n"),
    (r"nscans =", "\n"),
    (r"ok\?", "yes\n"),
    (r"fx/exercise>", "e\n"),
)
DRIVER_UNTIL_RE = re.compile(rb"TOTAL ERR BLOCKS r\d+\(\d+ hard\) w\d+\(\d+ hard\)")


def fx_extra_args(prom, tftp, elsc_log, capture, disk):
    """test_prom_mdk's argv shape plus the scratch disk at SCSI
    controller 0, target 1 -- the drive fx opens as dksc(0,1,0)."""
    return [
        '-cpu', 'R10000',
        '-smp', '1',
        '-m', '64',
        '-bios', str(prom),
        '-audio', 'none',
        '-chardev', f'file,id=elsc,path={elsc_log}',
        '-serial', 'none',
        '-serial', 'chardev:elsc',
        '-nic', ('user,id=net0,net=192.0.2.0/24,host=192.0.2.2,'
                 f'dhcpstart=192.0.2.15,tftp={tftp},bootfile={NETBOOT_PATH}'),
        '-object', f'filter-dump,id=netcap,netdev=net0,file={capture}',
        '-drive', f'if=none,id=disk0,file={disk},format=raw',
        '-device', 'scsi-hd,bus=scsi.0,scsi-id=1,lun=0,drive=disk0',
    ]


if QemuSystemTest is not None:

    _FORK_ROOT = os.path.dirname(os.path.dirname(os.path.dirname(
        os.path.dirname(os.path.abspath(__file__)))))

    class PromFx(QemuSystemTest):
        """Boots the real IP27 PROM, netboots fx.64, and requires a clean
        write-compare exercise on a fresh scratch disk."""

        def _asset_paths(self):
            assets_root = os.environ.get('ORIGAMI_ASSETS')
            if not assets_root:
                return None
            prom = os.path.join(assets_root, FIRMWARE_REL)
            fx = os.path.join(assets_root, FX_REL)
            if not (os.path.isfile(prom) and os.path.isfile(fx)):
                return None
            return prom, fx

        def _stage_netboot_tftp(self, fx):
            root = self.scratch_file('netboot-tftp')
            staged = os.path.join(root, *NETBOOT_PATH.split('/'))
            os.makedirs(os.path.dirname(staged), exist_ok=True)
            shutil.copyfile(fx, staged)
            return root

        def _scratch_disk(self):
            path = self.scratch_file('fx-scratch.img')
            with open(path, 'wb') as disk:
                disk.truncate(SCRATCH_DISK_BYTES)
            return path

        def test_prom_fx(self):
            paths = self._asset_paths()
            if paths is None:
                self.skipTest(
                    'ORIGAMI_ASSETS is not set, or has no firmware/fx.64 at '
                    f'<ORIGAMI_ASSETS>/{FIRMWARE_REL} and '
                    f'<ORIGAMI_ASSETS>/{FX_REL} -- tools/extract-irix boot '
                    'in the project checkout lands fx.64')
            prom, fx = paths

            identity_problems = payload_identity_problems(
                prom, fx, FX_SHA256, "fx.64")
            if identity_problems:
                self.fail('asset identity problems: ' +
                          '; '.join(identity_problems))

            netboot_tftp = self._stage_netboot_tftp(fx)
            disk = self._scratch_disk()

            vm = self.get_vm(name='prom_fx')
            vm.set_machine(
                'origin200,synthesized-firmware=off,debug-leds=off')
            vm.set_console()
            elsc_log = self.log_file('elsc-aux.log')
            capture = self.log_file('wire.pcap')
            vm.add_args('-accel', 'tcg,thread=single',
                        *fx_extra_args(prom, netboot_tftp, elsc_log,
                                       capture, disk))

            started = _utc_now()
            vm.launch()
            log_path = self.log_file('fx.log')
            try:
                transcript, reached = _drive_console(
                    vm.console_socket, log_path, BOOT_TIMEOUT_SECONDS,
                    steps=DRIVER_STEPS, until=DRIVER_UNTIL_RE)
            finally:
                vm.shutdown(hard=vm.exitcode() is not None)
            ended = _utc_now()

            if not transcript:
                self.fail(
                    'the firmware printed nothing at all -- a run that '
                    'did not start, not a netboot that failed early')

            report = verdict(transcript, reached=reached)

            summary_path = self.log_file('fx-summary.json')
            with open(summary_path, 'w', encoding='utf-8') as summary_file:
                json.dump({
                    'schema': 1,
                    'nblocks': NBLOCKS,
                    'reached_error_ledger': reached,
                    'report': report,
                }, summary_file, indent=2, sort_keys=True)
                summary_file.write('\n')

            run_path = self.log_file('run.json')
            with open(run_path, 'w', encoding='utf-8') as run_file:
                json.dump({
                    'schema': 1,
                    'started_utc': started,
                    'ended_utc': ended,
                    'console_driver_reached_until': reached,
                    'timeout_seconds': BOOT_TIMEOUT_SECONDS,
                    'identity': {
                        'fork': _fork_git_identity(_FORK_ROOT),
                        'qemu_binary': _file_record(self.qemu_bin),
                        'payload': {
                            'prom': _file_record(prom),
                            'fx': _file_record(fx),
                        },
                    },
                    'verdict': report['verdict'],
                    'problems': report['problems'],
                }, run_file, indent=2, sort_keys=True)
                run_file.write('\n')

            self.assertEqual(
                report['verdict'], 'PASS',
                'fx exercise problems: ' + '; '.join(report['problems']) +
                f' (full report: {summary_path})')


if __name__ == '__main__':
    if QemuSystemTest is not None:
        QemuSystemTest.main()
