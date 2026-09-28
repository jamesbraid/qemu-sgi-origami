#!/usr/bin/env python3
#
# MDK memory battery: netboot memory.mdk BY PATH and require every one of
# its eight built-in tests to report its own pass and completion
#
# SPDX-License-Identifier: GPL-2.0-or-later
"""
memory.mdk is the second MDK 1.6 package: the secondary-cache test and
seven memory tests (mapped and unmapped), each started by name at the
MDK> prompt and each ending with the kernel's own "... has completed"
line. All eight pass on this machine (the project checkout's
docs/notebooks/firmware-diags.md carries the results and the capture),
so this test drives the complete battery in one boot -- MDK reruns tests
from the same package without rebooting -- and holds the machine to it.

Everything structural is shared with test_prom_mdk.py: the PROM digest
refusal, the path-structured netboot staging, the console driver (with
this file's own end condition -- see below), the CR-terminated MDK
commands, and the provenance records. What is this file's own is the
verdict: the suite's tests end with the SAME completion words, so no
single line can name the end of the run; the driver's end condition and
the verdict both count completions instead, and the counts are
equalities so a double-fired command cannot slip through as extra
passes. test_prom_mdk_memory_verdict.py proves the logic against real
captured transcripts without booting anything.

"Command not found" is deliberately NOT a failure marker here: a driven
test that does not exist never prints its completion line, so the count
equalities already fail the run, and keeping the marker out lets the
real project capture -- which probes the manual's mem.dr name and gets
exactly that answer -- serve as a fixture unedited.
"""

import json
import os
import shutil

from qemu_test import QemuSystemTest, skipIfMissingEnv
from qemu_test.asset import LocalAsset

from lib_prom import (FIRMWARE_REL, FORK_ROOT, MACHINES, drive_console,
                      run_record, utc_now)
from lib_prom_mdk import (MEMORY_MDK_SHA256, PROM_SHA256,
                          mdk_extra_args,
                          payload_identity_problems)
from lib_prom_mdk_memory import (NETBOOT_PATH, TESTS, reached_all_completions,
                                 verdict)


MDK_REL = os.path.join('boot', 'mdk', 'memory.mdk')

# The PROM leg takes minutes; the battery's two long tests dominate (the
# recorded suite ran about forty minutes under a heavily contended host,
# with mem.lo alone near twenty). 2440 keeps the usual 60s margin under
# the 2500s meson timeout.
BOOT_TIMEOUT_SECONDS = 2440

DRIVER_STEPS = (
    (r"Option\?", "5\n"),
    (r">>", "unsetenv netaddr\n"),
    (r">>", f"boot -f bootp()/{NETBOOT_PATH}\n"),
    # CR-terminated: MDK's line reader ignores LF (test_prom_mdk.py).
    *((r"MDK>", f"{test}\r") for test in TESTS),
)


class PromMdkMemory(QemuSystemTest):
    """Boots the real IP27 PROM, netboots memory.mdk, drives the
    eight-test battery, and requires verdict() to be PASS."""

    ASSET_PROM = LocalAsset(FIRMWARE_REL, PROM_SHA256)
    ASSET_MDK = LocalAsset(MDK_REL, MEMORY_MDK_SHA256)

    def _stage_netboot_tftp(self, mdk):
        root = self.scratch_file('netboot-tftp')
        staged = os.path.join(root, *NETBOOT_PATH.split('/'))
        os.makedirs(os.path.dirname(staged), exist_ok=True)
        shutil.copyfile(mdk, staged)
        return root

    @skipIfMissingEnv('QEMU_TEST_SGI_ASSET_DIR')
    def test_prom_mdk_memory(self):
        prom, mdk = self.ASSET_PROM.fetch(), self.ASSET_MDK.fetch()

        identity_problems = payload_identity_problems(
            prom, mdk, MEMORY_MDK_SHA256, "memory.mdk")
        if identity_problems:
            self.fail('asset identity problems: ' +
                      '; '.join(identity_problems))

        netboot_tftp = self._stage_netboot_tftp(mdk)

        vm = self.get_vm(name='prom_mdk_memory')
        vm.set_machine(MACHINES['origin200'])
        vm.set_console()
        elsc_log = self.log_file('elsc-aux.log')
        capture = self.log_file('wire.pcap')
        vm.add_args('-accel', 'tcg,thread=single',
                    *mdk_extra_args(prom, netboot_tftp, elsc_log,
                                    capture,
                                    netboot_path=NETBOOT_PATH))

        started = utc_now()
        vm.launch()
        log_path = self.log_file('mdk.log')
        try:
            transcript, reached = drive_console(
                vm.console_socket, log_path, BOOT_TIMEOUT_SECONDS,
                DRIVER_STEPS,
                reached_when=reached_all_completions, vm=vm)
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
                'reached_all_completions': reached,
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
                'tests': list(TESTS),
                'identity': run_record(FORK_ROOT, self.qemu_bin,
                                       prom=prom, memory_mdk=mdk),
                'verdict': report['verdict'],
                'problems': report['problems'],
            }, run_file, indent=2, sort_keys=True)
            run_file.write('\n')

        self.assertEqual(
            report['verdict'], 'PASS',
            'MDK memory battery problems: ' +
            '; '.join(report['problems']) +
            f' (full report: {summary_path})')


if __name__ == '__main__':
    QemuSystemTest.main()
