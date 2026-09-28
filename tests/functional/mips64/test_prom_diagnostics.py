#!/usr/bin/env python3
#
# Factory diagnostics: boot the real IP27 PROM through its two-pass
# factory sequence and to sash64, and judge every diagnostic against an
# expected-failure ledger
#
# SPDX-License-Identifier: GPL-2.0-or-later
"""
Boot the IP27 PROM through its factory diagnostics, then netboot sash64
over SLIRP and inspect the resulting console transcript. The verdict
accepts two passes when the first assigns a module ID and warm resets, and
the next runs diagnostics and reaches the maintenance menu. It also
accepts a complete single pass with no assignment box or warm reset. A
partial sequence fails. Every RSLT result is checked against an
expected-failure ledger: an unlisted failure fails the run, and a listed
failure that no longer occurs is reported as stale.

The verdict logic lives in lib_prom_diagnostics.py and can be exercised
without QEMU by test_prom_diagnostics_verdict.py. The real boot also
checks release-matched PROM and sash64 digests and verifies that the
netboot request names the path-structured boot/sash64 fixture. It then
drives the command monitor through sash64 and checks the complete
transcript, including diagnostic console output on the ELSC AUX port.
"""

import hashlib
import json
import os
import re
import shutil

from qemu_test import QemuSystemTest, skipIfMissingEnv
from qemu_test.asset import LocalAsset

from lib_prom import (FORK_ROOT, MACHINES, drive_console, file_record,
                      fork_git_identity, utc_now)
from lib_prom_diagnostics import (EXPECTED_FAILURES, EXPECTED_FAILURES_TEXT,
                                  diag_extra_args, evaluate_run,
                                  extract_identities)
from lib_prom_sash import (NETBOOT_PATH, RELEASES,
                           artifact_identity_problems)


# ---------------------------------------------------------------------------
# The real boot: the interactive drive through diagnostics to sash64.
# The verdict companion covers the pure logic without starting QEMU.
# ---------------------------------------------------------------------------

# The release this test's real boot drives.
# This QemuSystemTest method drives one release at a time.
RELEASE = "6.5.30"

# Resolve the firmware and boot tree beneath QEMU_TEST_SGI_ASSET_DIR.
FIRMWARE_REL = os.path.join('firmware', RELEASE,
                            'ip27prom.payload.bin')
TFTP_REL = os.path.join('boot', RELEASE)

# One timeout covers the diagnostic sequence and the sash netboot.
BOOT_TIMEOUT_SECONDS = 1740

# Each (pattern, response) fires once when PATTERN matches the received
# console tail. The steps drive the PROM from the maintenance menu through
# the command monitor to sash64. The boot command names NETBOOT_PATH so the
# transcript proves the PROM requested the path-structured fixture.
DRIVER_STEPS = (
    (r"Option\?", "5\n"),
    (r">>", "unsetenv netaddr\n"),
    (r">>", f"boot -f bootp()/{NETBOOT_PATH}\n"),
    (r"sash:", "hinv\n"),
    (r"sash:", "printenv\n"),
)
# printenv's dlserver value proves sash64 loaded and ran, rather than only
# being requested.
DRIVER_UNTIL_RE = re.compile(rb"dlserver=192\.0\.2\.2")


class PromDiagnostics(QemuSystemTest):
    """Boot the IP27 PROM through diagnostics to an interactive sash
    prompt, then require evaluate_run() to return PASS.
    """

    ASSET_PROM = LocalAsset(
        FIRMWARE_REL, RELEASES[RELEASE]['prom_sha256'])
    ASSET_SASH = LocalAsset(
        os.path.join(TFTP_REL, 'sash64'),
        RELEASES[RELEASE]['sash_sha256'])

    def _stage_netboot_tftp(self, sash):
        """A fresh tftp root, under this test's own scratch directory,
        with the real (already digest-checked) sash64 copied in at
        NETBOOT_PATH -- e.g. <root>/boot/sash64. SLIRP serves the path
        named by bootfile= relative to this root.
        """
        root = self.scratch_file('netboot-tftp')
        staged_sash = os.path.join(root, *NETBOOT_PATH.split('/'))
        os.makedirs(os.path.dirname(staged_sash), exist_ok=True)
        shutil.copyfile(sash, staged_sash)
        return root

    @skipIfMissingEnv('QEMU_TEST_SGI_ASSET_DIR')
    def test_prom_diagnostics(self):
        # The source asset tree is flat; _stage_netboot_tftp() creates the
        # path-structured root served by SLIRP.
        prom, sash = self.ASSET_PROM.fetch(), self.ASSET_SASH.fetch()

        # Check both assets before booting so a release mismatch cannot
        # produce a misleading diagnostic result.
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
