#!/usr/bin/env python3
#
# Factory diagnostics: boot the real IP27 PROM through its two-pass
# factory sequence and to sash64, and judge every diagnostic against an
# expected-failure ledger
#
# SPDX-License-Identifier: GPL-2.0-or-later
"""
Ports the check that used to live in tools/diag-gate, in the PROJECT
checkout, into this fork's own functional-test framework, the same way
test_prom_boot.py ports tools/boot-gate. diag-gate (since retired --
superseded by this functional test) drove one QEMU invocation through the
whole factory sequence: the cold first pass has no module id, so the
firmware assigns one and warm-resets itself (same process, no new
invocation); the second pass accepts that assigned id, runs the normal
power-on diagnostics, prints the System Configuration and Diagnostics
Summary, reaches the System Maintenance Menu, and is then driven through
the command monitor to netboot sash64 over QEMU's own SLIRP usermode
networking (no external TFTP server). Every `RSLT <name> PASS/FAIL` line on
both passes is parsed and judged against an explicit expected-failure
ledger, ported below from diag-gate's own expected-failures list (the file
that shipped beside it in the project checkout before retirement): an
unexpected FAIL fails the run, and an expected failure that stops happening
is reported as stale rather than silently accepted.

evaluate_run() and the pure functions it calls (split_passes,
parse_rslt_lines, parse_summary, module_ids, extract_identities,
parse_expected_failures, _judge_failures) are a direct, close-to-verbatim
port of diag-gate's own functions of the same names; they live in
lib_prom_diagnostics.py beside this file. missing_sash_assertions there
is the same port of tools/sash-gate's ASSERTIONS/missing_assertions that
diag-gate itself called (via sash_gate) as the last stage of its own
evaluate_run -- reimplemented, fixed at cpus=1 (the only population
diag-gate ever drove), because this fork test must not import origami's
tools/ (no sashlib). test_prom_diagnostics_verdict.py proves this port
against real captured transcripts and the ported ledger, without booting
anything -- see that file's docstring.

The QEMU argv lib_prom_diagnostics.py builds (diag_extra_args) was
diag-gate's own gate_qemu_command, which was tools/sash-gate's
qemu_command with cpus=1,
skip_prom_diags=False -- read directly from tools/sash-gate in the project
checkout (the "-M origin200,synthesized-firmware=off,debug-leds=off" machine
string, the "-nic user,...,tftp=...,bootfile=sash64" SLIRP usermode netboot
line, and the ELSC AUX chardev on serial line 2 that podlib.serial_args
wires so the firmware's power-on-diagnostic console output -- which the
PROM re-opens on the system controller after a failed diagnostic -- is not
lost to a dark port; console=d selects IOC3 serial A on line 0, the
primary console, not this port). tools/netinst-rig,
named in this port's brief as a second reference, turns out to drive a real
irix-netinst server over bridged tap networking, not SLIRP at all -- a
different tool for a different (real-server) netboot path -- so it is not
the source this file's argv was built from; sash_gate.qemu_command is, and
diag-gate called that function directly. A captured confirmation that this
fork's own build now needs none of sash_gate's --slirp-dir/LD_LIBRARY_PATH
plumbing is docs/boot-logs/firmware.sash-netboot-vendored-slirp -- this
fork's meson build vendors libslirp as a static subproject
(subprojects/libslirp.wrap), so the emulator built from this tree links it
in rather than loading it at runtime.
"""

import datetime
import hashlib
import json
import os
import re
import subprocess

from qemu_test import QemuSystemTest, skipIfMissingEnv

from lib_prom import drive_console, resolve_asset
from lib_prom_diagnostics import (EXPECTED_FAILURES, EXPECTED_FAILURES_TEXT,
                                  diag_extra_args, evaluate_run,
                                  extract_identities)


# ---------------------------------------------------------------------------
# The real boot: the interactive drive through the factory sequence to
# sash64, and provenance. Not covered by test_prom_diagnostics_verdict.py
# -- see this module's own docstring for why -- so nothing below this line
# needs to be pure or independently testable the way the verdict logic in
# lib_prom_diagnostics.py does.
# ---------------------------------------------------------------------------

# Where ORIGAMI_ASSETS resolves the real firmware and boot tree, matching
# test_prom_boot.py's FIRMWARE_REL and tools/sash-gate's RELEASES["6.5-base"]
# in the project checkout: ORIGAMI_ASSETS names a CHECKOUT, not assets/
# itself.
FIRMWARE_REL = os.path.join('assets', 'firmware', '6.5-base',
                            'ip27prom.payload.bin')
TFTP_REL = os.path.join('assets', 'boot', '6.5-base')

# diag-gate's own default (tools/diag-gate's DEFAULT_TIMEOUT_SECONDS,
# before that tool's retirement) covered both passes and the sash leg in
# one budget. Kept a little under the meson timeout registered for this
# test in mips64/meson.build, the same margin test_prom_boot.py keeps
# against its own 700s meson timeout.
BOOT_TIMEOUT_SECONDS = 1740

# tools/serial-console's own console driver, ported: each (pattern,
# response) fires once, the first time PATTERN matches the TAIL of
# what has been received since the last match. The five steps drive the
# PROM from the System Maintenance Menu through the command monitor to an
# interactive sash prompt, matching diag-gate's own console_driver exactly
# (tools/diag-gate's --expect list, before that tool's retirement).
DRIVER_STEPS = (
    (r"Option\?", "5\n"),
    (r">>", "unsetenv netaddr\n"),
    (r">>", "boot -f bootp()/sash64\n"),
    (r"sash:", "hinv\n"),
    (r"sash:", "printenv\n"),
)
# diag-gate's own --until was: printenv's own dlserver line, the run's
# proof that sash64 actually loaded and ran rather than merely being
# requested.
DRIVER_UNTIL_RE = re.compile(rb"dlserver=192\.0\.2\.2")


def _utc_now():
    return datetime.datetime.now(datetime.UTC).isoformat().replace(
        "+00:00", "Z")


def _file_record(path):
    resolved = os.path.realpath(path)
    digest = hashlib.sha256()
    with open(resolved, "rb") as source:
        for chunk in iter(lambda: source.read(1024 * 1024), b""):
            digest.update(chunk)
    return {
        "path": str(path),
        "resolved_path": resolved,
        "sha256": digest.hexdigest(),
        "size_bytes": os.path.getsize(resolved),
    }


def _fork_git_identity(tree):
    """This fork checkout's own commit and dirty state -- the analog of
    diag-gate's git_identity(ROOT), where ROOT there is the project tools
    checkout. This is a functional test inside the fork itself, so there
    is no separate "project tools" checkout to report on; the fork's own
    identity is the binding record instead. None on any git failure: a
    missing identity must not read as a fabricated clean one."""
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
    return {
        "path": str(tree),
        "commit": commit.stdout.strip(),
        "dirty": bool(status.stdout.strip()),
    }


# This file's own repo root:
# tests/functional/mips64/test_prom_diagnostics.py is three directories
# below it.
_FORK_ROOT = os.path.dirname(os.path.dirname(os.path.dirname(
    os.path.dirname(os.path.abspath(__file__)))))

class PromDiagnostics(QemuSystemTest):
    """Boots the real IP27 PROM firmware through diag-gate's two-pass
    factory sequence to an interactive sash prompt and requires
    evaluate_run's verdict to be PASS. See this module's docstring for
    the full contract; this class only wires it to a real boot, the
    same division test_prom_boot.py keeps between its pure logic and
    PromBoot.
    """

    def _release_paths(self):
        """The real PROM image and sash64 boot tree under
        ORIGAMI_ASSETS, or None if either is missing -- the runner may
        not have assets provisioned yet (see this method's caller)."""
        assets_root = os.environ['ORIGAMI_ASSETS']
        prom = resolve_asset(assets_root, FIRMWARE_REL)
        sash = resolve_asset(assets_root,
                             os.path.join(TFTP_REL, 'sash64'))
        if not (prom and sash):
            return None
        return prom, os.path.join(assets_root, TFTP_REL)

    @skipIfMissingEnv('ORIGAMI_ASSETS')
    def test_prom_diagnostics(self):
        paths = self._release_paths()
        if paths is None:
            self.skipTest(
                'no firmware/sash64 '
                f'at <ORIGAMI_ASSETS>/{FIRMWARE_REL} and '
                f'<ORIGAMI_ASSETS>/{TFTP_REL}/sash64 -- point it at a '
                'checkout with the real PROM and release-matched '
                'sash64 extracted (tools/extract-irix) to run this '
                'test')
        prom, tftp = paths

        vm = self.get_vm(name='prom_diagnostics')
        vm.set_machine(
            'origin200,synthesized-firmware=off,debug-leds=off')
        vm.set_console()
        elsc_log = self.log_file('elsc-aux.log')
        capture = self.log_file('wire.pcap')
        vm.add_args('-accel', 'tcg,thread=single',
                   *diag_extra_args(prom, tftp, elsc_log, capture))

        started = _utc_now()
        vm.launch()
        log_path = self.log_file('diag.log')
        try:
            transcript, reached = drive_console(
                vm.console_socket, log_path, BOOT_TIMEOUT_SECONDS,
                DRIVER_STEPS, until=DRIVER_UNTIL_RE)
        finally:
            # A VM that crashed or exited on its own has already left;
            # a graceful shutdown() against an already-dead process
            # still raises AbnormalShutdown even though qemu is in
            # fact gone. Matches PromBoot.test_prom_boot's own
            # reasoning.
            vm.shutdown(hard=vm.exitcode() is not None)
        ended = _utc_now()

        if not transcript:
            self.fail(
                'the firmware printed nothing at all -- a run that '
                'did not start, not a factory sequence that failed '
                'early. Check the emulator built, and that nothing '
                'ended the machine before it printed anything.')

        report = evaluate_run(transcript, EXPECTED_FAILURES)
        if not reached:
            report['problems'].append(
                'console driver did not reach the download-server '
                'confirmation before timeout -- the run never '
                'proved sash64 actually loaded')
            report['verdict'] = 'FAIL'

        summary_path = self.log_file('diag-summary.json')
        with open(summary_path, 'w', encoding='utf-8') as summary_file:
            json.dump({
                'schema': 1,
                'expected_failures': list(EXPECTED_FAILURES),
                'expected_failures_source_sha256': hashlib.sha256(
                    EXPECTED_FAILURES_TEXT.encode()).hexdigest(),
                'nvram_warnings': 'expected',
                'identities': extract_identities(transcript),
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
                        'origin200,synthesized-firmware=off,'
                        'debug-leds=off'),
                    'args': diag_extra_args(
                        prom, tftp, elsc_log, capture),
                },
                'identity': {
                    'fork': _fork_git_identity(_FORK_ROOT),
                    'qemu_binary': _file_record(self.qemu_bin),
                    'firmware': {
                        'release': '6.5-base',
                        'prom': _file_record(prom),
                        'sash': _file_record(
                            os.path.join(tftp, 'sash64')),
                    },
                },
                'verdict': report['verdict'],
                'problems': report['problems'],
            }, run_file, indent=2, sort_keys=True)
            run_file.write('\n')

        self.assertEqual(
            report['verdict'], 'PASS',
            'diag-gate factory sequence problems: ' +
            '; '.join(report['problems']) +
            f' (full report: {summary_path})')


if __name__ == '__main__':
    QemuSystemTest.main()
