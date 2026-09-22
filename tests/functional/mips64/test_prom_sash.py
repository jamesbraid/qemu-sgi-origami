#!/usr/bin/env python3
#
# Sash netboot inventory check: boot the real IP27 PROM, netboot sash64
# BY PATH over SLIRP usermode networking, and require the standalone
# shell's one-CPU hardware inventory
#
# SPDX-License-Identifier: GPL-2.0-or-later
"""
Ports tools/sash-gate's proven 1-CPU, single-node contract, in the PROJECT
checkout, into this fork's own functional-test framework, the same way
test_prom_boot.py ports tools/boot-gate and test_prom_diagnostics.py
ports tools/diag-gate. sash-gate boots the real IP27 PROM, netboots
sash64 over QEMU's own SLIRP usermode networking (no external TFTP
server), and drives `hinv`/`printenv` at the standalone shell to require
the guest report the expected one-CPU hardware inventory. Its 1-CPU,
single-node mode is the only mode this port carries: sash-gate's own
--cpus 2/4 modes build sash-smp-gate's two-node topology, which has never
completed a four-CPU boot to sash (an open defect) -- this port's brief
scopes it out, so no code here serves those modes. tools/sash-gate itself
stays in place; this is additive.

missing_assertions() and ASSERTIONS (lib_prom_sash.py, beside this file,
which holds every name the verdict companion pins) are a byte-for-byte
port of tools/sashlib.py's own ASSERTIONS tuple and
missing_assertions(log, cpus), fixed at cpus=1 -- the only population
this test drives. This is the same
port test_prom_diagnostics.py's missing_sash_assertions already made
(diag-gate calls sash_gate.missing_assertions(log, cpus=1) as the last
stage of its own evaluate_run), reproduced here independently so this
file carries its own copy rather than importing test_prom_diagnostics.py.
missing_assertions() folds in one check beyond that port:
NETBOOT_PATH_ASSERTION, which requires the PROM's own "Obtaining ..."
fetch line name NETBOOT_PATH exactly -- proof the netboot fetch was for
the path-structured file this test serves (`boot/sash64`, a generic
subdirectory, not a vendor IRIX release path), not merely a netboot of
some name. sash-gate's own flat "bootfile=sash64" config never had a
path to assert, so this one check is new to this port, not a port of
sashlib.py's own logic; see NETBOOT_PATH's own comment for why.
artifact_identity_problems() and RELEASES are the same kind of port of
tools/sashlib.py's artifact_identity_problems()/RELEASES: the two
SHA-256 comparisons a real run makes against the release-matched PROM
and sash64. Both the 6.5-base and 6.5.30 entries are reproduced, not
just the one PromSash.test_prom_sash boots by default, so sash-gate's
own --release choice stays represented here and is exercised by
test_prom_sash_verdict.py -- see RELEASE below for which one this test's
real boot drives.

Every name in this module is reimplemented inline from what
`git -C <project checkout> show fj/main:tools/sash-gate` and
`...:tools/sashlib.py` read (the project checkout's working tree lags
main; both were read from main): nothing here imports origami's tools/
(no sashlib, no podlib, no ReleaseArtifacts dataclass) -- this fork test
must stand on its own. test_prom_sash_verdict.py proves this port against
real captured console transcripts (docs/boot-logs/ in the project
checkout), without booting anything -- see that file's docstring.

The QEMU argv lib_prom_sash.py builds (sash_extra_args) is close to tools/
sashlib.py's own qemu_command(cpus=1, skip_prom_diags=False) -- diag-gate
calls that exact function to build the argv test_prom_diagnostics.py's
diag_extra_args ports, so the two functions share the same shape (the
"-M origin200,debug-leds=off" machine string and
the ELSC AUX chardev on serial line 2 that podlib.serial_args wires, so
the firmware's power-on-diagnostic console output -- which the PROM
re-opens on the system controller after a failed diagnostic -- is captured
rather than lost to a dark port; console=d selects IOC3 serial A on line
0, the primary console, not this port), with one deliberate departure:
the SLIRP usermode netboot line reads "-nic user,...,tftp=...,
bootfile=boot/sash64", not sash-gate's own flat "bootfile=sash64" -- see
NETBOOT_PATH's own comment for why, and PromSash._stage_netboot_tftp()
for how `tftp=` ends up pointing at a directory that actually has
`boot/sash64` under it. The dynamic-vs-static libslirp
--slirp-dir injection sash-gate's CLI carries is NOT ported:
test_prom_diagnostics.py already established it is dead code on this
fork, which vendors libslirp as a static meson subproject
(subprojects/libslirp.wrap) -- the captured confirmation is
docs/boot-logs/firmware.sash-netboot-vendored-slirp.2026-08-24T07:04+00:00,
a real sash-gate PASS run recording that its built emulator needed none of
that plumbing.
"""

import json
import os
import re
import shutil

from qemu_test import QemuSystemTest, skipIfMissingEnv

from lib_prom import (FORK_ROOT, MACHINES, drive_console, file_record,
                      fork_git_identity, require_assets, utc_now)
from lib_prom_sash import (NETBOOT_PATH, artifact_identity_problems,
                           sash_extra_args, verdict)


# ---------------------------------------------------------------------------
# The real boot: the interactive drive through the command monitor to
# sash64, and provenance. Not covered by test_prom_sash_verdict.py -- see
# this module's own docstring for why -- so nothing below this line needs
# to be pure or independently testable the way the verdict logic in
# lib_prom_sash.py does.
# ---------------------------------------------------------------------------

# The release this test's real boot drives by default -- matching
# test_prom_boot.py's and test_prom_diagnostics.py's own default
# (sash-gate's CLI default is also 6.5.30). lib_prom_sash.RELEASES
# carries 6.5-base too so the verdict logic and its unit tests keep
# sash-gate's --release choice represented even though this one
# QemuSystemTest method drives one release at a time.
RELEASE = "6.5.30"

# Where ORIGAMI_ASSETS resolves the real firmware and boot tree, matching
# test_prom_diagnostics.py's FIRMWARE_REL/TFTP_REL and tools/sashlib.py's
# RELEASES["6.5.30"] in the project checkout: ORIGAMI_ASSETS names a
# CHECKOUT, not assets/ itself.
FIRMWARE_REL = os.path.join('assets', 'firmware', RELEASE,
                            'ip27prom.payload.bin')
TFTP_REL = os.path.join('assets', 'boot', RELEASE)

# sash-gate's own single-CPU default (tools/sashlib.py,
# DEFAULT_TIMEOUT_SECONDS[1]) is 1200s for the whole run including its
# identity bookkeeping. This test drives only the boot-and-netboot leg, so
# a smaller budget is kept, a little under the meson timeout registered
# for this test in mips64/meson.build (900s) -- the same margin
# test_prom_boot.py and test_prom_diagnostics.py keep against their own meson
# timeouts.
BOOT_TIMEOUT_SECONDS = 840

# tools/serial-console's own console driver, ported: each (pattern,
# response) fires once, the first time PATTERN matches the TAIL of what
# has been received since the last match. These five steps drive the PROM
# from the System Maintenance Menu through the command monitor to an
# interactive sash prompt, matching tools/sash-gate's own console_driver
# --expect list (cpus=1, the only population this port drives) with one
# departure: the boot command names NETBOOT_PATH
# (`boot -f bootp()/boot/sash64`), not sash-gate's own flat
# `boot -f bootp()/sash64` -- this test's whole point is proving the PROM
# fetches the path-structured file it was asked for (see
# NETBOOT_PATH_ASSERTION's own comment).
DRIVER_STEPS = (
    (r"Option\?", "5\n"),
    (r">>", "unsetenv netaddr\n"),
    (r">>", f"boot -f bootp()/{NETBOOT_PATH}\n"),
    (r"sash:", "hinv\n"),
    (r"sash:", "printenv\n"),
)
# sash-gate's own --until: printenv's own dlserver line, the run's proof
# that sash64 actually loaded and ran rather than merely being requested.
DRIVER_UNTIL_RE = re.compile(rb"dlserver=192\.0\.2\.2")


class PromSash(QemuSystemTest):
    """Boots the real IP27 PROM firmware, netboots sash64 over SLIRP
    usermode networking, and requires verdict() to be PASS. See this
    module's docstring for the full contract; this class only wires
    it to a real boot, the same division test_prom_diagnostics.py and
    test_prom_boot.py keep between their pure logic and their own
    QemuSystemTest classes.
    """

    def _stage_netboot_tftp(self, sash):
        """A fresh tftp root, under this test's own scratch directory,
        with the real (already digest-checked) sash64 copied in at
        NETBOOT_PATH -- e.g. <root>/boot/sash64. This is what proves
        the netboot is PATH-STRUCTURED: SLIRP's tftp= serves whatever
        bootfile= names relative to this root, so a flat "bootfile=
        sash64" and this test's own "bootfile=boot/sash64" (see
        sash_extra_args) are indistinguishable to the PROM unless the
        file is actually laid out under the requested subdirectory.
        Returns the staged root's path.
        """
        root = self.scratch_file('netboot-tftp')
        staged_sash = os.path.join(root, *NETBOOT_PATH.split('/'))
        os.makedirs(os.path.dirname(staged_sash), exist_ok=True)
        shutil.copyfile(sash, staged_sash)
        return root

    @skipIfMissingEnv('ORIGAMI_ASSETS')
    def test_prom_sash(self):
        # sash64 comes from sash-gate's own FLAT assets/boot/<release>
        # layout (sash64 alongside sashARCS, unix.IP27, ...), shared
        # read-only project state this test must not restructure -- see
        # _stage_netboot_tftp() for the PATH-STRUCTURED copy this test's
        # real boot actually serves.
        sash_rel = os.path.join(TFTP_REL, 'sash64')
        assets = require_assets(self, FIRMWARE_REL, sash_rel)
        prom, sash = assets[FIRMWARE_REL], assets[sash_rel]

        # sash-gate itself refuses to launch against a PROM or sash64
        # that does not match the selected release's recorded digest
        # -- the same refusal here, before spending a boot on assets
        # that would silently produce a meaningless pass or a
        # confusing failure.
        identity_problems = artifact_identity_problems(
            RELEASE, prom, sash)
        if identity_problems:
            self.fail(
                f'release {RELEASE} asset identity problems: ' +
                '; '.join(identity_problems))

        netboot_tftp = self._stage_netboot_tftp(sash)

        vm = self.get_vm(name='prom_sash')
        vm.set_machine(MACHINES['origin200'])
        vm.set_console()
        elsc_log = self.log_file('elsc-aux.log')
        capture = self.log_file('wire.pcap')
        vm.add_args('-accel', 'tcg,thread=single',
                   *sash_extra_args(prom, netboot_tftp, elsc_log,
                                     capture))

        started = utc_now()
        vm.launch()
        log_path = self.log_file('sash.log')
        try:
            transcript, reached = drive_console(
                vm.console_socket, log_path, BOOT_TIMEOUT_SECONDS,
                DRIVER_STEPS, until=DRIVER_UNTIL_RE, vm=vm)
        finally:
            # A VM that crashed or exited on its own has already left;
            # a graceful shutdown() against an already-dead process
            # still raises AbnormalShutdown even though qemu is in
            # fact gone. Matches test_prom_diagnostics.py's
            # PromDiagnostics.test_prom_diagnostics and
            # test_prom_boot.py's PromBoot.test_prom_boot.
            vm.shutdown(hard=vm.exitcode() is not None)
        ended = utc_now()

        if not transcript:
            self.fail(
                'the firmware printed nothing at all -- a run that '
                'did not start, not a netboot that failed early. '
                'Check the emulator built, and that nothing ended '
                'the machine before it printed anything.')

        report = verdict(transcript, reached=reached, cpus=1)

        summary_path = self.log_file('sash-summary.json')
        with open(summary_path, 'w', encoding='utf-8') as summary_file:
            json.dump({
                'schema': 1,
                'release': RELEASE,
                'reached_dlserver_confirmation': reached,
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
                    'args': sash_extra_args(
                        prom, netboot_tftp, elsc_log, capture),
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
            'sash-gate 1-CPU netboot problems: ' +
            '; '.join(report['problems']) +
            f' (full report: {summary_path})')


if __name__ == '__main__':
    QemuSystemTest.main()
