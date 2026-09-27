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
lib_prom_diagnostics.py beside this file. That evaluate_run()'s last
check calls lib_prom_sash.missing_assertions(log, cpus=1) directly,
tools/sash-gate's own ASSERTIONS/missing_assertions port that diag-gate
itself called (via sash_gate) as the last stage of its own evaluate_run,
fixed at cpus=1 -- the only population diag-gate ever drove.

This test also carries the coverage test_prom_sash.py used to check on
its own separate 1-CPU netboot boot, since folded in here and deleted:
lib_prom_sash.missing_assertions() folds in NETBOOT_PATH_ASSERTION,
proof the PROM's netboot fetch named the path-structured boot/sash64
this test serves rather than merely a netboot of some name, and
test_prom_diagnostics() below runs lib_prom_sash.artifact_identity_
problems() before launching, the same release-matched SHA-256 refusal
test_prom_sash.py made. Neither check needed a second port:
lib_prom_sash.py (test_prom_pod.py's own RELEASES import keeps it in
the tree) already carries both, pure and importable without qemu_test,
the same way lib_prom_diagnostics.py's own functions are.
test_prom_diagnostics_verdict.py proves the port in that file against
real captured transcripts and the ported ledger, without booting
anything -- see that file's docstring.

The QEMU argv lib_prom_diagnostics.py builds (diag_extra_args) was
diag-gate's own gate_qemu_command, which was tools/sash-gate's
qemu_command with cpus=1,
skip_prom_diags=False -- read directly from tools/sash-gate in the project
checkout (the "-M origin200,debug-leds=off" machine
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

import hashlib
import json
import os
import re
import shutil

from qemu_test import QemuSystemTest, skipIfMissingEnv

from lib_prom import (FORK_ROOT, MACHINES, drive_console, file_record,
                      fork_git_identity, require_assets, utc_now)
from lib_prom_diagnostics import (EXPECTED_FAILURES, EXPECTED_FAILURES_TEXT,
                                  diag_extra_args, evaluate_run,
                                  extract_identities)
from lib_prom_sash import NETBOOT_PATH, artifact_identity_problems


# ---------------------------------------------------------------------------
# The real boot: the interactive drive through the factory sequence to
# sash64, and provenance. Not covered by test_prom_diagnostics_verdict.py
# -- see this module's own docstring for why -- so nothing below this line
# needs to be pure or independently testable the way the verdict logic in
# lib_prom_diagnostics.py does.
# ---------------------------------------------------------------------------

# The release this test's real boot drives -- matching test_prom_boot.py's
# own default (tools/sash-gate's CLI default is also 6.5.30).
# lib_prom_sash.RELEASES carries 6.5-base too, for the verdict logic and
# its own unit tests, but this one QemuSystemTest method drives one
# release at a time, the same way test_prom_sash.py's own RELEASE did.
RELEASE = "6.5.30"

# Resolve the firmware and boot tree beneath QEMU_TEST_SGI_ASSET_DIR,
# matching test_prom_boot.py's FIRMWARE_REL and tools/sash-gate's RELEASES["6.5.30"]
# in the project checkout: QEMU_TEST_SGI_ASSET_DIR points directly at the
# asset directory.
FIRMWARE_REL = os.path.join('firmware', RELEASE,
                            'ip27prom.payload.bin')
TFTP_REL = os.path.join('boot', RELEASE)

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
# interactive sash prompt, matching diag-gate's own console_driver
# (tools/diag-gate's --expect list, before that tool's retirement) with
# one departure absorbed from test_prom_sash.py: the boot command names
# NETBOOT_PATH (`boot -f bootp()/boot/sash64`), not diag-gate's own flat
# `boot -f bootp()/sash64` -- proof the PROM fetches the path-structured
# file this test serves (see NETBOOT_PATH_ASSERTION's own comment in
# lib_prom_sash.py).
DRIVER_STEPS = (
    (r"Option\?", "5\n"),
    (r">>", "unsetenv netaddr\n"),
    (r">>", f"boot -f bootp()/{NETBOOT_PATH}\n"),
    (r"sash:", "hinv\n"),
    (r"sash:", "printenv\n"),
)
# diag-gate's own --until was: printenv's own dlserver line, the run's
# proof that sash64 actually loaded and ran rather than merely being
# requested.
DRIVER_UNTIL_RE = re.compile(rb"dlserver=192\.0\.2\.2")


class PromDiagnostics(QemuSystemTest):
    """Boots the real IP27 PROM firmware through diag-gate's two-pass
    factory sequence to an interactive sash prompt and requires
    evaluate_run's verdict to be PASS. See this module's docstring for
    the full contract; this class only wires it to a real boot, the
    same division test_prom_boot.py keeps between its pure logic and
    PromBoot.
    """

    def _stage_netboot_tftp(self, sash):
        """A fresh tftp root, under this test's own scratch directory,
        with the real (already digest-checked) sash64 copied in at
        NETBOOT_PATH -- e.g. <root>/boot/sash64. Ported from
        test_prom_sash.py's own PromSash._stage_netboot_tftp: this is
        what proves the netboot is PATH-STRUCTURED, since SLIRP's tftp=
        serves whatever bootfile= names relative to this root.
        """
        root = self.scratch_file('netboot-tftp')
        staged_sash = os.path.join(root, *NETBOOT_PATH.split('/'))
        os.makedirs(os.path.dirname(staged_sash), exist_ok=True)
        shutil.copyfile(sash, staged_sash)
        return root

    @skipIfMissingEnv('QEMU_TEST_SGI_ASSET_DIR')
    def test_prom_diagnostics(self):
        # sash64 comes from tools/sash-gate's own FLAT assets/boot/<release>
        # layout (sash64 alongside sashARCS, unix.IP27, ...), shared
        # read-only project state this test must not restructure -- see
        # _stage_netboot_tftp() for the PATH-STRUCTURED copy this test's
        # real boot actually serves.
        sash_rel = os.path.join(TFTP_REL, 'sash64')
        assets = require_assets(self, FIRMWARE_REL, sash_rel)
        prom, sash = assets[FIRMWARE_REL], assets[sash_rel]

        # tools/sash-gate itself refuses to launch against a PROM or
        # sash64 that does not match the selected release's recorded
        # digest -- the same refusal here, absorbed from
        # test_prom_sash.py, before spending a boot on assets that would
        # silently produce a meaningless pass or a confusing failure.
        identity_problems = artifact_identity_problems(RELEASE, prom, sash)
        if identity_problems:
            self.fail(
                f'release {RELEASE} asset identity problems: ' +
                '; '.join(identity_problems))

        tftp = self._stage_netboot_tftp(sash)

        vm = self.get_vm(name='prom_diagnostics')
        vm.set_machine(MACHINES['origin200'])
        vm.set_console()
        elsc_log = self.log_file('elsc-aux.log')
        capture = self.log_file('wire.pcap')
        vm.add_args('-accel', 'tcg,thread=single',
                   *diag_extra_args(prom, tftp, elsc_log, capture))

        started = utc_now()
        vm.launch()
        log_path = self.log_file('diag.log')
        try:
            transcript, reached = drive_console(
                vm.console_socket, log_path, BOOT_TIMEOUT_SECONDS,
                DRIVER_STEPS, until=DRIVER_UNTIL_RE, vm=vm)
        finally:
            # A VM that crashed or exited on its own has already left;
            # a graceful shutdown() against an already-dead process
            # still raises AbnormalShutdown even though qemu is in
            # fact gone. Matches PromBoot.test_prom_boot's own
            # reasoning.
            vm.shutdown(hard=vm.exitcode() is not None)
        ended = utc_now()

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
                    'machine': MACHINES['origin200'],
                    'args': diag_extra_args(
                        prom, tftp, elsc_log, capture),
                },
                'identity': {
                    'fork': fork_git_identity(FORK_ROOT),
                    'qemu_binary': file_record(self.qemu_bin),
                    'firmware': {
                        'release': RELEASE,
                        'prom': file_record(prom),
                        'sash': file_record(sash),
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
