#!/usr/bin/env python3
#
# Sash netboot inventory check: boot the real IP27 PROM, netboot sash64
# over SLIRP usermode networking, and require the standalone shell's
# one-CPU hardware inventory
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

missing_assertions() and ASSERTIONS below are a byte-for-byte port of
tools/sashlib.py's own ASSERTIONS tuple and missing_assertions(log, cpus),
fixed at cpus=1 -- the only population this test drives. This is the same
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

The QEMU argv this file builds (sash_extra_args) is tools/sashlib.py's own
qemu_command(cpus=1, skip_prom_diags=False) -- diag-gate calls that exact
function to build the argv test_prom_diagnostics.py's diag_extra_args
ports, so the two functions are the same shape by construction (the
"-M origin200,synthesized-firmware=off,debug-leds=off" machine string, the
"-nic user,...,tftp=...,bootfile=sash64" SLIRP usermode netboot line, and
the ELSC AUX chardev on serial line 2 that podlib.serial_args wires, so
the firmware's power-on-diagnostic console output -- which the PROM
re-opens on the system controller after a failed diagnostic -- is captured
rather than lost to a dark port; console=d selects IOC3 serial A on line
0, the primary console, not this port). The dynamic-vs-static libslirp
--slirp-dir injection sash-gate's CLI carries is NOT ported:
test_prom_diagnostics.py already established it is dead code on this
fork, which vendors libslirp as a static meson subproject
(subprojects/libslirp.wrap) -- the captured confirmation is
docs/boot-logs/firmware.sash-netboot-vendored-slirp.2026-08-24T07:04+00:00,
a real sash-gate PASS run recording that its built emulator needed none of
that plumbing.
"""

import datetime
import hashlib
import json
import os
import re
import socket
import subprocess
import sys
import time

# This file's own directory, so a plain `pytest test_prom_sash_verdict.py`
# run (which imports straight from this module) finds it the same way
# test_prom_boot_verdict.py finds test_prom_boot.py.
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

# qemu_test is only importable inside meson's own test environment (see
# test_prom_boot.py's docstring for the exact reason). Guarded so that
# every pure function below -- the whole surface test_prom_sash_verdict.py
# exercises -- stays importable without it; PromSash itself is simply not
# defined in that case, which is fine because nothing outside meson ever
# tries to run the real boot.
try:
    from qemu_test import QemuSystemTest
except (ImportError, RuntimeError):
    QemuSystemTest = None


# ---------------------------------------------------------------------------
# Verdict logic, ported from tools/sashlib.py (project checkout, read from
# fj/main). Kept byte-for-byte on the fixed strings; departures are called
# out where they happen.
# ---------------------------------------------------------------------------

# tools/sashlib.py's own ASSERTIONS tuple, copied verbatim.
ASSERTIONS = (
    ("IP27 system inventory", "System  SGI-IP27"),
    ("64 MiB inventory", "Main memory size: 64 Mbytes"),
    ("IOC3 Ethernet inventory", "Integral Fast Ethernet"),
    ("IOC3 serial inventory", "IOC3 serial port"),
    ("IOC3 output console", "ConsoleOut=/dev/tty/ioc30"),
    ("leased guest address", "netaddr=192.0.2.15"),
    ("download server address", "dlserver=192.0.2.2"),
)

# The generic, non-vendor path this test's real boot serves sash64 under
# and requests by name -- see this module's docstring for why "boot/
# sash64" and not a flat name or a vendor IRIX release path. This is a
# DEPARTURE from tools/sashlib.py's own ASSERTIONS above (which stay a
# byte-for-byte port): sash-gate's own config serves a flat
# "bootfile=sash64", so it never had a path to assert. Checking this line
# is what turns this test from "sash netbooted" into "sash netbooted
# BY THE PATH THIS TEST SERVED IT UNDER" -- the guard a plain flat-name
# check cannot tell apart from a PROM that silently ignored the requested
# path and fetched something else with the same content.
NETBOOT_PATH = "boot/sash64"
NETBOOT_PATH_ASSERTION = (
    "path-structured netboot fetch",
    f"Obtaining {NETBOOT_PATH} from server",
)


def missing_assertions(log, cpus=1):
    """Which of sash's post-netboot hardware-inventory lines never appeared,
    PLUS whether the PROM's own fetch line named the path-structured file
    this test serves (NETBOOT_PATH_ASSERTION -- new to this port, not part
    of the sashlib.py port below).

    Ports tools/sashlib.py's own missing_assertions(log, cpus): for the
    given population, the two CPU-count lines and the memory-size line are
    built from `cpus`, and every other ASSERTIONS entry is checked as-is
    (sashlib's own assertions tuple rebuilds the memory-size line from
    `cpus` too and drops the fixed "64 MiB inventory" entry from
    ASSERTIONS so it is not checked twice; at cpus=1 the two are the same
    string, so that rebuild is not a behavior change here).

    This port carries ONLY the proven 1-CPU contract (see this module's
    docstring): cpus values other than 1 would build sash-smp-gate's
    two-node topology, which this fork test does not drive, so they are
    refused rather than silently mischecked.
    """
    if cpus != 1:
        raise NotImplementedError(
            "test_prom_sash ports only sash-gate's proven 1-CPU contract; "
            "--cpus 2/4 build the two-node topology this port does not "
            "carry (see this module's docstring)")
    memory = 64
    assertions = (
        (f"{cpus} CPU inventory", f"{cpus} 180 MHz IP27 Processors"),
        (f"{cpus} CPUs enabled", f"No. of CPUs enabled     = {cpus}"),
        ("no CPUs disabled", "No. of CPUs disabled    = 0"),
        (f"{memory} MiB inventory", f"Main memory size: {memory} Mbytes"),
        NETBOOT_PATH_ASSERTION,
        *(
            assertion
            for assertion in ASSERTIONS
            if assertion[0] != "64 MiB inventory"
        ),
    )
    return [name for name, evidence in assertions if evidence not in log]


# tools/sashlib.py's own RELEASES table, SHA-256 digests copied verbatim.
# The prom/tftp Path entries sashlib.py carries alongside them point at
# the project checkout's own assets/ layout; this port's real boot below
# resolves the same relative layout under ORIGAMI_ASSETS itself (see
# FIRMWARE_REL/TFTP_REL), so only the digests are reproduced here.
RELEASES = {
    "6.5-base": {
        "prom_sha256": (
            "14e0d0a77b92154e3b33573f30608f241ded249db6060fba9f97bfd00b2145b2"
        ),
        "sash_sha256": (
            "d1a00a1a592022d9939026168a8e040548c4986284dcb29310d569f0c8b7d631"
        ),
    },
    "6.5.30": {
        "prom_sha256": (
            "c2c69bf33c375b43ec7ac511fab41ddf595be1b6838ae717e58a9a1273437fdd"
        ),
        "sash_sha256": (
            "7b86941c14cb8679301decc5ebb192b1344510d16f8f7d9f986fea7ec4a17142"
        ),
    },
}


def _file_sha256(path):
    digest = hashlib.sha256()
    with open(path, "rb") as source:
        for chunk in iter(lambda: source.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def artifact_identity_problems(release, prom_path, sash_path):
    """Which of the release-matched PROM/sash64 SHA-256 digests mismatch.

    Ports tools/sashlib.py's own artifact_identity_problems(artifacts,
    prom, sash): sash-gate refuses to launch a run against a PROM or
    sash64 that does not match the selected --release's recorded digest,
    so a stale or wrong-release asset is caught before spending a boot on
    it. stdlib hashlib only -- no sashlib import.
    """
    artifacts = RELEASES[release]
    problems = []
    if _file_sha256(prom_path) != artifacts["prom_sha256"]:
        problems.append("PROM SHA-256 does not match the selected release")
    if _file_sha256(sash_path) != artifacts["sash_sha256"]:
        problems.append("sash64 SHA-256 does not match the selected release")
    return problems


def verdict(log, reached=True, cpus=1):
    """PASS iff the console driver reached sash's post-netboot state and
    every fixed hardware-inventory assertion is present in the transcript.

    This is sash-gate's own check, reimplemented: sash-gate's main() runs
    missing_assertions(log, args.cpus) and fails on anything missing, and
    separately fails if its console driver's own --until match never
    fires (a run that timed out before proving sash64 actually loaded).
    Unlike test_prom_diagnostics.py's evaluate_run, there is no RSLT
    ledger to judge here -- sash-gate drives exactly one PROM pass and
    makes exactly this one check, so this port makes exactly that one
    check too.

    Returns {"verdict": "PASS"|"FAIL", "missing": [...], "problems": [...]}.
    """
    problems = []
    missing = missing_assertions(log, cpus)
    for name in missing:
        problems.append(f"missing {name}")
    if not reached:
        problems.append(
            "console driver did not reach the download-server "
            "confirmation before timeout -- the run never proved sash64 "
            "actually loaded")
    return {
        "verdict": "PASS" if not problems else "FAIL",
        "missing": missing,
        "problems": problems,
    }


# ---------------------------------------------------------------------------
# The real boot: QEMU argv, the interactive drive through the command
# monitor to sash64, and provenance. Not covered by
# test_prom_sash_verdict.py -- see this module's own docstring for why --
# so nothing below this line needs to be pure or independently testable
# the way the verdict logic above does.
# ---------------------------------------------------------------------------

# The release this test's real boot drives by default -- matching
# test_prom_boot.py's and test_prom_diagnostics.py's own default
# (sash-gate's CLI default is also 6.5-base). RELEASES above carries
# 6.5.30 too so the verdict logic and its unit tests keep sash-gate's
# --release choice represented even though this one QemuSystemTest
# method drives one release at a time.
RELEASE = "6.5-base"

# Where ORIGAMI_ASSETS resolves the real firmware and boot tree, matching
# test_prom_diagnostics.py's FIRMWARE_REL/TFTP_REL and tools/sashlib.py's
# RELEASES["6.5-base"] in the project checkout: ORIGAMI_ASSETS names a
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
# --expect list exactly (cpus=1, the only population this port drives).
DRIVER_STEPS = (
    (r"Option\?", "5\n"),
    (r">>", "unsetenv netaddr\n"),
    (r">>", "boot -f bootp()/sash64\n"),
    (r"sash:", "hinv\n"),
    (r"sash:", "printenv\n"),
)
# sash-gate's own --until: printenv's own dlserver line, the run's proof
# that sash64 actually loaded and ran rather than merely being requested.
DRIVER_UNTIL_RE = re.compile(rb"dlserver=192\.0\.2\.2")
_TAIL_MAX = 8192


def sash_extra_args(prom, tftp, elsc_log, capture):
    """The QEMU args beyond what QEMUMachine's set_machine()/set_console()
    already provide, for sash-gate's fixed 1-CPU configuration: normal
    (not skipped) power-on diagnostics and SLIRP usermode networking
    serving sash64 with no external TFTP server. Ported from
    tools/sashlib.py's qemu_command(cpus=1, skip_prom_diags=False) -- see
    this module's docstring for exactly which call sash-gate makes.

    set_console() already emits "-serial chardev:console" for serial line
    0 (IOC3 port A, the primary console); the "-serial none" here fills
    line 1 (IOC3 port B, unused) so the "-serial chardev:elsc" that
    follows lands on line 2, the ELSC AUX port -- the same
    podlib.serial_args(ioc3_a=..., elsc=...) numbering tools/sashlib.py's
    qemu_command itself uses. console=d selects IOC3 serial A on line 0,
    captured by set_console() above; the ELSC AUX on line 2 carries the
    firmware's power-on-diagnostic console output, which the PROM re-opens
    on the system controller after a failed diagnostic, captured here
    rather than lost to a dark port.
    """
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
                 f'dhcpstart=192.0.2.15,tftp={tftp},bootfile=sash64'),
        '-object', f'filter-dump,id=netcap,netdev=net0,file={capture}',
    ]


def _drive_console(sock, log_path, timeout):
    """Answer the PROM's System Maintenance Menu prompts and drive it to
    an interactive sash prompt.

    Ported from tools/serial-console's own expect loop (see that file's
    docstring for why: commands sent before the firmware is listening are
    lost, and a rule that could refire on a prompt reappearing after being
    answered would send the wrong thing next time). Every byte received is
    both accumulated into the returned transcript and appended to
    log_path, so a run this drove is inspectable exactly like a captured
    -serial file.

    Returns (transcript, reached) -- reached is True once DRIVER_UNTIL_RE
    is seen anywhere in the stream, matching sash-gate's own --until exit.
    """
    rules = [
        {"re": re.compile(pattern.encode()), "response": response.encode(),
         "fired": False}
        for pattern, response in DRIVER_STEPS
    ]
    sock.settimeout(1.0)
    deadline = time.monotonic() + timeout
    transcript = bytearray()
    tail = b""
    reached = False
    with open(log_path, "wb") as log:
        while time.monotonic() < deadline:
            try:
                chunk = sock.recv(4096)
            except socket.timeout:
                continue
            except OSError:
                break
            if not chunk:
                break
            transcript += chunk
            log.write(chunk)
            log.flush()
            tail = (tail + chunk)[-_TAIL_MAX:]
            for rule in rules:
                if not rule["fired"] and rule["re"].search(tail):
                    sock.sendall(rule["response"])
                    rule["fired"] = True
                    tail = b""
            if DRIVER_UNTIL_RE.search(tail):
                reached = True
                break
    return transcript.decode("utf-8", errors="replace"), reached


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
    sash-gate's git_identity(ROOT), where ROOT there is the project tools
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


if QemuSystemTest is not None:

    # This file's own repo root: tests/functional/mips64/test_prom_sash.py
    # is three directories below it.
    _FORK_ROOT = os.path.dirname(os.path.dirname(os.path.dirname(
        os.path.dirname(os.path.abspath(__file__)))))

    class PromSash(QemuSystemTest):
        """Boots the real IP27 PROM firmware, netboots sash64 over SLIRP
        usermode networking, and requires verdict() to be PASS. See this
        module's docstring for the full contract; this class only wires
        it to a real boot, the same division test_prom_diagnostics.py and
        test_prom_boot.py keep between their pure logic and their own
        QemuSystemTest classes.
        """

        def _release_paths(self):
            """The real PROM image and sash64 boot tree under
            ORIGAMI_ASSETS, or None if either is missing -- the runner may
            not have assets provisioned yet (see this method's caller)."""
            assets_root = os.environ.get('ORIGAMI_ASSETS')
            if not assets_root:
                return None
            prom = os.path.join(assets_root, FIRMWARE_REL)
            tftp = os.path.join(assets_root, TFTP_REL)
            sash = os.path.join(tftp, 'sash64')
            if not (os.path.isfile(prom) and os.path.isfile(sash)):
                return None
            return prom, tftp

        def test_prom_sash(self):
            paths = self._release_paths()
            if paths is None:
                self.skipTest(
                    'ORIGAMI_ASSETS is not set, or has no firmware/sash64 '
                    f'at <ORIGAMI_ASSETS>/{FIRMWARE_REL} and '
                    f'<ORIGAMI_ASSETS>/{TFTP_REL}/sash64 -- point it at a '
                    'checkout with the real PROM and release-matched '
                    'sash64 extracted (tools/extract-irix) to run this '
                    'test')
            prom, tftp = paths
            sash = os.path.join(tftp, 'sash64')

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

            vm = self.get_vm(name='prom_sash')
            vm.set_machine(
                'origin200,synthesized-firmware=off,debug-leds=off')
            vm.set_console()
            elsc_log = self.log_file('elsc-aux.log')
            capture = self.log_file('wire.pcap')
            vm.add_args('-accel', 'tcg,thread=single',
                       *sash_extra_args(prom, tftp, elsc_log, capture))

            started = _utc_now()
            vm.launch()
            log_path = self.log_file('sash.log')
            try:
                transcript, reached = _drive_console(
                    vm.console_socket, log_path, BOOT_TIMEOUT_SECONDS)
            finally:
                # A VM that crashed or exited on its own has already left;
                # a graceful shutdown() against an already-dead process
                # still raises AbnormalShutdown even though qemu is in
                # fact gone. Matches test_prom_diagnostics.py's
                # PromDiagnostics.test_prom_diagnostics and
                # test_prom_boot.py's PromBoot.test_prom_boot.
                vm.shutdown(hard=vm.exitcode() is not None)
            ended = _utc_now()

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
                        'machine': (
                            'origin200,synthesized-firmware=off,'
                            'debug-leds=off'),
                        'args': sash_extra_args(
                            prom, tftp, elsc_log, capture),
                    },
                    'identity': {
                        'fork': _fork_git_identity(_FORK_ROOT),
                        'qemu_binary': _file_record(self.qemu_bin),
                        'firmware': {
                            'release': RELEASE,
                            'prom': _file_record(prom),
                            'sash': _file_record(sash),
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
    if QemuSystemTest is not None:
        QemuSystemTest.main()
