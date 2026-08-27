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
import re
import shutil
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

from test_prom_mdk import (
    FIRMWARE_REL,
    MEMORY_MDK_SHA256,
    _drive_console,
    _file_record,
    _fork_git_identity,
    _utc_now,
    mdk_extra_args,
    payload_identity_problems,
)

try:
    from qemu_test import QemuSystemTest
except (ImportError, RuntimeError):
    QemuSystemTest = None


NETBOOT_PATH = "mdk/memory.mdk"
MDK_REL = os.path.join('assets', 'boot', 'mdk', 'memory.mdk')

# Every built-in the package's own ls lists, quick ones first so an early
# failure surfaces fast; the two long-running tests close the battery.
TESTS = (
    "scache",
    "mem.qs",
    "memum.dr",
    "memum.qs",
    "memum.nb",
    "memum.in",
    "memum.lo",
    "mem.lo",
)

COMPLETION = "has completed"
# scache reports its summary line without the "Mem test" words the seven
# memory tests share, so the two counts differ by exactly one.
MEM_PASS = "Mem test PASSED"

ASSERTIONS = (
    ("path-structured netboot fetch",
     f"Obtaining {NETBOOT_PATH} from server"),
    ("MDK 1.6 banner", "SGI MDK Version 1.6"),
    ("one-CPU configuration", "Total no. of CPUs = 1"),
    ("scache data path pass", "Scache data path test PASSED"),
    ("scache cell pass", "Scache cell test PASSED"),
    ("scache random address pass", "Scache random address test PASSED"),
)

FAILURE_MARKERS = (
    "Test Failed",
    "FAILED",
    "ERROR:",
    "Unexpected exception",
)


def missing_assertions(log):
    """Which fixed lines never appeared, plus which driven tests never
    echoed at the prompt."""
    missing = [name for name, evidence in ASSERTIONS if evidence not in log]
    for test in TESTS:
        if f"MDK> {test}" not in log:
            missing.append(f"{test} started at the prompt")
    return missing


def count_problems(log):
    """The completion and pass counts, as equalities.

    Every test ends with the same completion words, so presence proves
    nothing about the later tests; only the counts do. Equalities rather
    than at-least: a command that fired twice would inflate the count and
    must read as a broken run, not a stronger pass.
    """
    problems = []
    completions = log.count(COMPLETION)
    if completions != len(TESTS):
        problems.append(
            f"completion count {completions} != {len(TESTS)} driven tests")
    passes = log.count(MEM_PASS)
    if passes != len(TESTS) - 1:
        problems.append(
            f"memory-test pass count {passes} != {len(TESTS) - 1}")
    return problems


def failure_markers_present(log):
    return [marker for marker in FAILURE_MARKERS if marker in log]


def verdict(log, reached=True):
    """PASS iff every test echoed, every fixed line appeared, the pass
    and completion counts are exact, no failure spelling appears, and
    the driver saw the final completion before timeout."""
    problems = []
    for name in missing_assertions(log):
        problems.append(f"missing {name}")
    problems.extend(count_problems(log))
    for marker in failure_markers_present(log):
        problems.append(f"kernel failure output present: {marker!r}")
    if not reached:
        problems.append(
            "console driver did not see the final completion before "
            "timeout -- the run never proved the battery finished")
    return {
        "verdict": "PASS" if not problems else "FAIL",
        "problems": problems,
    }


def reached_all_completions(transcript):
    """The driver's end condition: the whole battery has completed."""
    return transcript.count(COMPLETION.encode()) >= len(TESTS)


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


if QemuSystemTest is not None:

    _FORK_ROOT = os.path.dirname(os.path.dirname(os.path.dirname(
        os.path.dirname(os.path.abspath(__file__)))))

    class PromMdkMemory(QemuSystemTest):
        """Boots the real IP27 PROM, netboots memory.mdk, drives the
        eight-test battery, and requires verdict() to be PASS."""

        def _asset_paths(self):
            assets_root = os.environ.get('ORIGAMI_ASSETS')
            if not assets_root:
                return None
            prom = os.path.join(assets_root, FIRMWARE_REL)
            mdk = os.path.join(assets_root, MDK_REL)
            if not (os.path.isfile(prom) and os.path.isfile(mdk)):
                return None
            return prom, mdk

        def _stage_netboot_tftp(self, mdk):
            root = self.scratch_file('netboot-tftp')
            staged = os.path.join(root, *NETBOOT_PATH.split('/'))
            os.makedirs(os.path.dirname(staged), exist_ok=True)
            shutil.copyfile(mdk, staged)
            return root

        def test_prom_mdk_memory(self):
            paths = self._asset_paths()
            if paths is None:
                self.skipTest(
                    'ORIGAMI_ASSETS is not set, or has no firmware/'
                    f'memory.mdk at <ORIGAMI_ASSETS>/{FIRMWARE_REL} and '
                    f'<ORIGAMI_ASSETS>/{MDK_REL} -- tools/extract-irix mdk '
                    'in the project checkout lands the MDK packages')
            prom, mdk = paths

            identity_problems = payload_identity_problems(
                prom, mdk, MEMORY_MDK_SHA256, "memory.mdk")
            if identity_problems:
                self.fail('asset identity problems: ' +
                          '; '.join(identity_problems))

            netboot_tftp = self._stage_netboot_tftp(mdk)

            vm = self.get_vm(name='prom_mdk_memory')
            vm.set_machine(
                'origin200,synthesized-firmware=off,debug-leds=off')
            vm.set_console()
            elsc_log = self.log_file('elsc-aux.log')
            capture = self.log_file('wire.pcap')
            vm.add_args('-accel', 'tcg,thread=single',
                        *mdk_extra_args(prom, netboot_tftp, elsc_log,
                                        capture,
                                        netboot_path=NETBOOT_PATH))

            started = _utc_now()
            vm.launch()
            log_path = self.log_file('mdk.log')
            try:
                transcript, reached = _drive_console(
                    vm.console_socket, log_path, BOOT_TIMEOUT_SECONDS,
                    steps=DRIVER_STEPS,
                    reached_when=reached_all_completions)
            finally:
                vm.shutdown(hard=vm.exitcode() is not None)
            ended = _utc_now()

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
                    'identity': {
                        'fork': _fork_git_identity(_FORK_ROOT),
                        'qemu_binary': _file_record(self.qemu_bin),
                        'payload': {
                            'prom': _file_record(prom),
                            'memory_mdk': _file_record(mdk),
                        },
                    },
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
    if QemuSystemTest is not None:
        QemuSystemTest.main()
