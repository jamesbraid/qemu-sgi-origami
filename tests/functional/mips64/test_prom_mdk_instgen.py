#!/usr/bin/env python3
#
# MDK random-instruction generator: netboot cpu.mdk on TWO CPUs and
# require a thousand self-checking instgen passes with no miscompare
#
# SPDX-License-Identifier: GPL-2.0-or-later
"""
instgen is the vendor's own CPU fuzzer: a random instruction generator
that exercises the R10000 pipeline, ALU, FPU, branches, register
renaming and caches, checking every register against its own model and
reporting per-register miscompares (the class of defect the project's
R10000 audit chased by hand). This test drives the manual's own
thousand-pass example -- `instgen -s 1 -e 1000` -- and requires the
kernel's completion line with no error output.

TWO CPUS, deliberately: on one CPU instgen exits before its first pass
-- its master polls a per-CPU worker state that never appears -- and the
project checkout's docs/notebooks/firmware-diags.md traces that exit.
This is the one MDK leg that needs -smp 2, so it is also the nightly's
standing check that a two-CPU boot of the real PROM still reaches a
working multiprocessing kernel.

Everything structural is shared with test_prom_mdk.py, including the
CR-terminated MDK command. The passes are the kernel's own pseudo-random
sequence, not seedable from the command line, so runs vary in content
but not in contract: every pass self-checks, and any miscompare prints
ERROR with the register name. test_prom_mdk_instgen_verdict.py proves
the verdict logic against real captured transcripts.
"""

import json
import os
import re
import shutil

from qemu_test import QemuSystemTest, skipIfMissingEnv

from lib_prom import (FIRMWARE_REL, FORK_ROOT, drive_console,
                      require_assets, run_record, utc_now)
from lib_prom_mdk import (CPU_MDK_SHA256, mdk_extra_args,
                          payload_identity_problems)
from lib_prom_mdk_instgen import (END_PASS, INSTGEN_COMMAND, NETBOOT_PATH,
                                  verdict)


MDK_REL = os.path.join('boot', 'mdk', 'cpu.mdk')

# A thousand passes took about forty minutes under a host contended to
# nearly twice its cores, and a fraction of that quiet. 3540 keeps the
# usual 60s margin under the 3600s meson timeout.
BOOT_TIMEOUT_SECONDS = 3540

DRIVER_STEPS = (
    (r"Option\?", "5\n"),
    (r">>", "unsetenv netaddr\n"),
    (r">>", f"boot -f bootp()/{NETBOOT_PATH}\n"),
    # CR-terminated: MDK's line reader ignores LF (test_prom_mdk.py).
    (r"MDK>", f"{INSTGEN_COMMAND}\r"),
)
DRIVER_UNTIL_RE = re.compile(
    rb"Random Instruction Generator test run is complete")


class PromMdkInstgen(QemuSystemTest):
    """Boots the real IP27 PROM with two CPUs, netboots cpu.mdk, and
    requires a clean thousand-pass instgen run."""

    def _stage_netboot_tftp(self, mdk):
        root = self.scratch_file('netboot-tftp')
        staged = os.path.join(root, *NETBOOT_PATH.split('/'))
        os.makedirs(os.path.dirname(staged), exist_ok=True)
        shutil.copyfile(mdk, staged)
        return root

    @skipIfMissingEnv('QEMU_TEST_SGI_ASSET_DIR')
    def test_prom_mdk_instgen(self):
        assets = require_assets(self, FIRMWARE_REL, MDK_REL)
        prom, mdk = assets[FIRMWARE_REL], assets[MDK_REL]

        identity_problems = payload_identity_problems(
            prom, mdk, CPU_MDK_SHA256, "cpu.mdk")
        if identity_problems:
            self.fail('asset identity problems: ' +
                      '; '.join(identity_problems))

        netboot_tftp = self._stage_netboot_tftp(mdk)

        vm = self.get_vm(name='prom_mdk_instgen')
        # No debug-leds=off here, alone in this directory: with two
        # CPUs that option wedged the PROM boot twice (once before
        # CrayLink discovery, once at a headless-HUB wreck with a
        # General Exception loop on the ELSC), while the same binary
        # boots two CPUs cleanly with the flight recorder left on.
        # Recorded in the project checkout's firmware-diags notebook.
        vm.set_machine('origin200')
        vm.set_console()
        elsc_log = self.log_file('elsc-aux.log')
        capture = self.log_file('wire.pcap')
        extra = mdk_extra_args(prom, netboot_tftp, elsc_log, capture,
                               netboot_path=NETBOOT_PATH)
        # The one -smp departure from the shared argv: instgen needs
        # its worker CPU.
        extra[extra.index('-smp') + 1] = '2'
        vm.add_args('-accel', 'tcg,thread=single', *extra)

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
                'reached_completion': reached,
                'end_pass': END_PASS,
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
                'command_line': INSTGEN_COMMAND,
                'identity': run_record(FORK_ROOT, self.qemu_bin,
                                       prom=prom, cpu_mdk=mdk),
                'verdict': report['verdict'],
                'problems': report['problems'],
            }, run_file, indent=2, sort_keys=True)
            run_file.write('\n')

        self.assertEqual(
            report['verdict'], 'PASS',
            'MDK instgen problems: ' +
            '; '.join(report['problems']) +
            f' (full report: {summary_path})')


if __name__ == '__main__':
    QemuSystemTest.main()
