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
installed, and a butterfly pass reads single blocks alternating from
both ends of the range, the seek pattern the sequential pass never
makes. This test drives both over 100,000 blocks (50 MB) of a scratch
disk at controller 0 target 1 -- the write-compare first, then the
butterfly read -- and holds fx to its own error ledger after each:
TOTAL ERR BLOCKS r0(0 hard) w0(0 hard), twice.

Extended mode ("Do you require extended mode with all options
available?") is answered yes, because only extended mode offers the
write modifiers; safe mode's exercises are read-only. fx confirms before
writing ("about to destroy data on disk ... ok?"), which the driver
answers yes, on a disk this test created seconds earlier.

Every fx prompt is answered by a fixed step; a stray "?" at any of
them makes fx print help and re-prompt, which shifts every later answer
one prompt short and leaves fx waiting for input at the last one. That
is exactly what happened in the first by-hand butterfly session, and it
read as a wedge in the SCSI path until the polled address was decoded
(the console UART's line-status register, a getchar loop); the project
checkout's firmware-diags notebook records it.

Everything structural -- PROM digest refusal, path-structured netboot
staging, the one-shot-per-step console driver, provenance records --
is shared with test_prom_mdk.py. test_prom_fx_verdict.py proves the
verdict logic against real captured transcripts.
"""

import json
import os
import re
import shutil

from qemu_test import QemuSystemTest, skipIfMissingEnv
from qemu_test.asset import LocalAsset

from lib_prom import (FIRMWARE_REL, FORK_ROOT, MACHINES, drive_console,
                      run_record, utc_now)
from lib_prom_fx import LEDGERS_EXPECTED, NBLOCKS, NETBOOT_PATH, verdict
from lib_prom_mdk import PROM_SHA256, payload_identity_problems


FX_REL = os.path.join('boot', '6.5-base', 'fx.64')
# The 6.5-base disc's fx.64, as tools/extract-irix boot records it.
FX_SHA256 = (
    "c4cbacad625ee4587e24ba69f4610293383452b0c69114a4e3b3804e1e8b3100"
)

SCRATCH_DISK_BYTES = 1 << 30

# Boot to the menu is minutes under a contended host; fx loads in
# seconds, the 100,000-block write-compare took 19s and the butterfly
# read 4s. 840 keeps the usual 60s margin under the 900s meson timeout.
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
    (r"fx/exercise>", "b\n"),
    (r"modifier =", "\n"),
    (r"starting block# =", "\n"),
    (r"nblocks =", f"{NBLOCKS}\n"),
    (r"nscans =", "\n"),
    (r"fx/exercise>", "e\n"),
)
LEDGER_RE = re.compile(rb"TOTAL ERR BLOCKS r\d+\(\d+ hard\) w\d+\(\d+ hard\)")


def reached_both_ledgers(transcript):
    """The driver's end condition: fx printed its error ledger once
    after each exercise."""
    return len(LEDGER_RE.findall(transcript)) >= LEDGERS_EXPECTED


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


class PromFx(QemuSystemTest):
    """Boots the real IP27 PROM, netboots fx.64, and requires a clean
    write-compare exercise on a fresh scratch disk."""

    ASSET_PROM = LocalAsset(FIRMWARE_REL, PROM_SHA256)
    ASSET_FX = LocalAsset(FX_REL, FX_SHA256)

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

    @skipIfMissingEnv('QEMU_TEST_SGI_ASSET_DIR')
    def test_prom_fx(self):
        prom, fx = self.ASSET_PROM.fetch(), self.ASSET_FX.fetch()

        identity_problems = payload_identity_problems(
            prom, fx, FX_SHA256, "fx.64")
        if identity_problems:
            self.fail('asset identity problems: ' +
                      '; '.join(identity_problems))

        netboot_tftp = self._stage_netboot_tftp(fx)
        disk = self._scratch_disk()

        vm = self.get_vm(name='prom_fx')
        vm.set_machine(MACHINES['origin200'])
        vm.set_console()
        elsc_log = self.log_file('elsc-aux.log')
        capture = self.log_file('wire.pcap')
        vm.add_args('-accel', 'tcg,thread=single',
                    *fx_extra_args(prom, netboot_tftp, elsc_log,
                                   capture, disk))

        started = utc_now()
        vm.launch()
        log_path = self.log_file('fx.log')
        try:
            transcript, reached = drive_console(
                vm.console_socket, log_path, BOOT_TIMEOUT_SECONDS,
                DRIVER_STEPS, reached_when=reached_both_ledgers, vm=vm)
        finally:
            vm.shutdown(hard=vm.exitcode() is not None)
        ended = utc_now()

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
                'identity': run_record(FORK_ROOT, self.qemu_bin,
                                       prom=prom, fx=fx),
                'verdict': report['verdict'],
                'problems': report['problems'],
            }, run_file, indent=2, sort_keys=True)
            run_file.write('\n')

        self.assertEqual(
            report['verdict'], 'PASS',
            'fx exercise problems: ' + '; '.join(report['problems']) +
            f' (full report: {summary_path})')


if __name__ == '__main__':
    QemuSystemTest.main()
