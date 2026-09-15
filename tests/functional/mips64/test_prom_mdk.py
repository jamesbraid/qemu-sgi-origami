#!/usr/bin/env python3
#
# MDK netboot check: boot the real IP27 PROM, netboot SGI's
# micro-diagnostic kernel (cpu.mdk) BY PATH over SLIRP usermode
# networking, and require its primary-data-cache test to pass
#
# SPDX-License-Identifier: GPL-2.0-or-later
"""
MDK 1.6 is SGI's own PROM-bootable field diagnostic for the Origin
200/2000/Onyx2: a standalone kernel with built-in hardware tests, booted
from the maintenance menu's command monitor exactly as an operating
system is. It never shipped on IRIX media -- the binaries come off the
Internal Support Tools 2.7 disc; `tools/extract-irix mdk` in the project
checkout lands them under <ORIGAMI_ASSETS>/assets/boot/mdk, and the find,
the extraction, and the full per-test results on this machine are in the
project checkout's docs/notebooks/firmware-diags.md.

This test netboots cpu.mdk the way test_prom_sash.py netboots sash64 --
same PROM, same SLIRP usermode TFTP, same path-structured `bootfile=`
departure (mdk/cpu.mdk, staged under this test's own scratch root, so
the fetch line proves the PROM fetched the file by the path it was
asked for) -- then drives the kernel's own `pcache` test at the MDK>
prompt and requires the kernel's own pass and completion lines. pcache
is the one cpu.mdk test a one-CPU machine can run: instgen exits before
its first pass without a second CPU for its worker (the notebook traces
that exit), so this test does not drive it.

One departure from every other console driver in this directory, and it
is load-bearing: THE MDK COMMAND IS SENT CR-TERMINATED ("pcache\\r").
MDK's line reader finishes a line on CR and ignores LF; an LF-terminated
command echoes and then waits forever -- observed live before this test
existed (a lone CR sent minutes later started the echoed test
instantly), and the project harness's own MDK driver
(tools/origami_lib/drivers/mdk.py) carries the same terminator for the
same reason. The PROM menu and command monitor accept either, so the
three PROM-phase steps keep this directory's usual "\\n".

test_prom_mdk_verdict.py proves missing_assertions(), verdict() and
artifact_identity_problems() (lib_prom_mdk.py, beside this file) against
real captured transcripts without booting anything, the same division
test_prom_sash.py keeps with lib_prom_sash.py and its own _verdict
companion.
"""

import json
import os
import re
import shutil

from qemu_test import QemuSystemTest, skipIfMissingEnv

from lib_prom import (FIRMWARE_REL, drive_console, file_record,
                      fork_git_identity, resolve_asset, utc_now)
from lib_prom_mdk import (NETBOOT_PATH, artifact_identity_problems,
                          mdk_extra_args, verdict)


# ---------------------------------------------------------------------------
# The real boot. The driver and provenance records mirror
# test_prom_sash.py's own; departures are the payload and the CR-terminated
# MDK command (see this module's docstring).
# ---------------------------------------------------------------------------

MDK_REL = os.path.join('assets', 'boot', 'mdk', 'cpu.mdk')

# Boot to the menu is a few minutes under a contended host; the kernel
# netboot leg is seconds; pcache announces "about 5 minutes" and took
# about two emulated. 1140 keeps the usual 60s margin under the 1200s
# meson timeout registered in mips64/meson.build.
BOOT_TIMEOUT_SECONDS = 1140

DRIVER_STEPS = (
    (r"Option\?", "5\n"),
    (r">>", "unsetenv netaddr\n"),
    (r">>", f"boot -f bootp()/{NETBOOT_PATH}\n"),
    # CR, not LF: MDK's line reader ignores LF (this module's docstring).
    (r"MDK>", "pcache\r"),
)
DRIVER_UNTIL_RE = re.compile(rb"Pcache test run is complete")


_FORK_ROOT = os.path.dirname(os.path.dirname(os.path.dirname(
    os.path.dirname(os.path.abspath(__file__)))))


class PromMdk(QemuSystemTest):
    """Boots the real IP27 PROM, netboots cpu.mdk over SLIRP usermode
    networking, drives pcache at the MDK> prompt, and requires
    verdict() to be PASS."""

    def _asset_paths(self):
        assets_root = os.environ['ORIGAMI_ASSETS']
        prom = resolve_asset(assets_root, FIRMWARE_REL)
        mdk = resolve_asset(assets_root, MDK_REL)
        if not (prom and mdk):
            return None
        return prom, mdk

    def _stage_netboot_tftp(self, mdk):
        root = self.scratch_file('netboot-tftp')
        staged = os.path.join(root, *NETBOOT_PATH.split('/'))
        os.makedirs(os.path.dirname(staged), exist_ok=True)
        shutil.copyfile(mdk, staged)
        return root

    @skipIfMissingEnv('ORIGAMI_ASSETS')
    def test_prom_mdk(self):
        paths = self._asset_paths()
        if paths is None:
            # @skipIfMissingEnv('ORIGAMI_ASSETS') above already skipped
            # the case where it is unset entirely -- getting here means
            # it IS set, so a missing PROM or cpu.mdk is a provisioning
            # failure, not an absent-assets developer run, and must
            # fail loudly: a silent skip here would let a real pcache
            # regression pass CI unnoticed.
            self.fail(
                'ORIGAMI_ASSETS is set but the firmware/cpu.mdk are '
                f'missing at <ORIGAMI_ASSETS>/{FIRMWARE_REL} and '
                f'<ORIGAMI_ASSETS>/{MDK_REL} -- provision them '
                '(nightly assets) rather than silently skipping a '
                'claimed capability')
        prom, mdk = paths

        identity_problems = artifact_identity_problems(prom, mdk)
        if identity_problems:
            self.fail('asset identity problems: ' +
                      '; '.join(identity_problems))

        netboot_tftp = self._stage_netboot_tftp(mdk)

        vm = self.get_vm(name='prom_mdk')
        vm.set_machine(
            'origin200,debug-leds=off')
        vm.set_console()
        elsc_log = self.log_file('elsc-aux.log')
        capture = self.log_file('wire.pcap')
        vm.add_args('-accel', 'tcg,thread=single',
                    *mdk_extra_args(prom, netboot_tftp, elsc_log,
                                    capture))

        started = utc_now()
        vm.launch()
        log_path = self.log_file('mdk.log')
        try:
            transcript, reached = drive_console(
                vm.console_socket, log_path, BOOT_TIMEOUT_SECONDS,
                DRIVER_STEPS, until=DRIVER_UNTIL_RE, vm=vm)
        finally:
            vm.shutdown(hard=vm.exitcode() is not None)
        ended = utc_now()

        if not transcript:
            self.fail(
                'the firmware printed nothing at all -- a run that '
                'did not start, not a netboot that failed early')

        report = verdict(transcript, reached=reached)

        summary_path = self.log_file('mdk-summary.json')
        with open(summary_path, 'w', encoding='utf-8') as summary_file:
            json.dump({
                'schema': 1,
                'reached_pcache_completion': reached,
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
                'command': {
                    'binary': self.qemu_bin,
                    'machine': (
                        'origin200,'
                        'debug-leds=off'),
                    'args': mdk_extra_args(
                        prom, netboot_tftp, elsc_log, capture),
                },
                'identity': {
                    'fork': fork_git_identity(_FORK_ROOT),
                    'qemu_binary': file_record(self.qemu_bin),
                    'payload': {
                        'prom': file_record(prom),
                        'cpu_mdk': file_record(mdk),
                    },
                },
                'verdict': report['verdict'],
                'problems': report['problems'],
            }, run_file, indent=2, sort_keys=True)
            run_file.write('\n')

        self.assertEqual(
            report['verdict'], 'PASS',
            'MDK netboot-and-pcache problems: ' +
            '; '.join(report['problems']) +
            f' (full report: {summary_path})')


if __name__ == '__main__':
    QemuSystemTest.main()
