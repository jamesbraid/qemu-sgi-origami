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
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

from test_prom_mdk import (
    CPU_MDK_SHA256,
    FIRMWARE_REL,
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


NETBOOT_PATH = "mdk/cpu.mdk"
MDK_REL = os.path.join('assets', 'boot', 'mdk', 'cpu.mdk')

END_PASS = 1000
INSTGEN_COMMAND = f"instgen -s 1 -e {END_PASS}"

ASSERTIONS = (
    ("path-structured netboot fetch",
     f"Obtaining {NETBOOT_PATH} from server"),
    ("MDK 1.6 banner", "SGI MDK Version 1.6"),
    ("two-CPU configuration", "Total no. of CPUs = 2"),
    ("two CPUs driven", "Num_Cpus_To_Run              =  2"),
    ("first pass", "Instgen: Starting PASS  1 "),
    (f"final pass", f"Instgen: Starting PASS  {END_PASS} "),
    ("instgen completion",
     "MDK Random Instruction Generator test run is complete"),
)

# "ERROR: PASS: <n> GPR miscompare: ..." is the miscompare shape the
# manual documents; "miscompare" alone catches the FPR and other
# variants, and the wreck marker catches a kernel-side death.
FAILURE_MARKERS = (
    "ERROR",
    "miscompare",
    "FAILED",
    "Unexpected exception",
)


def missing_assertions(log):
    return [name for name, evidence in ASSERTIONS if evidence not in log]


def failure_markers_present(log):
    return [marker for marker in FAILURE_MARKERS if marker in log]


def verdict(log, reached=True):
    """PASS iff the run reached pass 1000 and its completion line with
    every fixed assertion present and no failure spelling anywhere."""
    problems = []
    for name in missing_assertions(log):
        problems.append(f"missing {name}")
    for marker in failure_markers_present(log):
        problems.append(f"kernel failure output present: {marker!r}")
    if not reached:
        problems.append(
            "console driver did not reach the instgen completion line "
            "before timeout -- the run never proved the passes finished")
    return {
        "verdict": "PASS" if not problems else "FAIL",
        "problems": problems,
    }


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


if QemuSystemTest is not None:

    _FORK_ROOT = os.path.dirname(os.path.dirname(os.path.dirname(
        os.path.dirname(os.path.abspath(__file__)))))

    class PromMdkInstgen(QemuSystemTest):
        """Boots the real IP27 PROM with two CPUs, netboots cpu.mdk, and
        requires a clean thousand-pass instgen run."""

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

        def test_prom_mdk_instgen(self):
            paths = self._asset_paths()
            if paths is None:
                self.skipTest(
                    'ORIGAMI_ASSETS is not set, or has no firmware/cpu.mdk '
                    f'at <ORIGAMI_ASSETS>/{FIRMWARE_REL} and '
                    f'<ORIGAMI_ASSETS>/{MDK_REL} -- tools/extract-irix mdk '
                    'in the project checkout lands the MDK packages')
            prom, mdk = paths

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
            vm.set_machine('origin200,synthesized-firmware=off')
            vm.set_console()
            elsc_log = self.log_file('elsc-aux.log')
            capture = self.log_file('wire.pcap')
            extra = mdk_extra_args(prom, netboot_tftp, elsc_log, capture,
                                   netboot_path=NETBOOT_PATH)
            # The one -smp departure from the shared argv: instgen needs
            # its worker CPU.
            extra[extra.index('-smp') + 1] = '2'
            vm.add_args('-accel', 'tcg,thread=single', *extra)

            started = _utc_now()
            vm.launch()
            log_path = self.log_file('mdk.log')
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
                    'identity': {
                        'fork': _fork_git_identity(_FORK_ROOT),
                        'qemu_binary': _file_record(self.qemu_bin),
                        'payload': {
                            'prom': _file_record(prom),
                            'cpu_mdk': _file_record(mdk),
                        },
                    },
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
    if QemuSystemTest is not None:
        QemuSystemTest.main()
