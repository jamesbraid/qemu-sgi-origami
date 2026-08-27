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
artifact_identity_problems() against real captured transcripts without
booting anything, the same division test_prom_sash.py keeps with its own
_verdict companion.
"""

import datetime
import hashlib
import json
import os
import re
import shutil
import socket
import subprocess
import sys
import time

# This file's own directory, so a plain `pytest test_prom_mdk_verdict.py`
# run (which imports straight from this module) finds it the same way
# test_prom_sash_verdict.py finds test_prom_sash.py.
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

# qemu_test is only importable inside meson's own test environment (see
# test_prom_boot.py's docstring). Guarded so every pure function below --
# the whole surface test_prom_mdk_verdict.py exercises -- stays
# importable without it.
try:
    from qemu_test import QemuSystemTest
except (ImportError, RuntimeError):
    QemuSystemTest = None


# ---------------------------------------------------------------------------
# Verdict logic. No project-tools port exists for MDK the way sashlib.py
# existed for sash; the fixed strings below are the kernel's own output,
# read from the real captured transcripts named in
# test_prom_mdk_verdict.py's fixture provenance.
# ---------------------------------------------------------------------------

# The path-structured file this test serves and asks for, staged the same
# way test_prom_sash.py stages boot/sash64 -- see that module's
# NETBOOT_PATH comment for why by-path, not flat.
NETBOOT_PATH = "mdk/cpu.mdk"

ASSERTIONS = (
    ("path-structured netboot fetch",
     f"Obtaining {NETBOOT_PATH} from server"),
    ("Nanos boot banner", "Booting Nanos..."),
    ("MDK 1.6 banner", "SGI MDK Version 1.6"),
    ("one-CPU configuration", "Total no. of CPUs = 1"),
    ("pcache start", "Primary Data Cache Test Started"),
    ("pcache pass", "PCACHE Test Passed"),
    ("pcache completion", "Pcache test run is complete"),
)

# The kernel names failures explicitly: the manual's pcache failure
# output is "ERROR: Test Failed", and a kernel-side wreck prints
# "Unexpected exception occured" (the kernel's own spelling). Any of
# these in the transcript fails the run even with every ASSERTIONS line
# present.
FAILURE_MARKERS = (
    "Test Failed",
    "ERROR:",
    "Unexpected exception",
)


def missing_assertions(log):
    """Which of the fixed netboot-and-pcache lines never appeared."""
    return [name for name, evidence in ASSERTIONS if evidence not in log]


def failure_markers_present(log):
    """Which of the kernel's own failure spellings appear in the log."""
    return [marker for marker in FAILURE_MARKERS if marker in log]


# The real PROM image and the cpu.mdk this test refuses to run without.
# The PROM digest is test_prom_sash.py's own 6.5-base entry; the cpu.mdk
# digest is the extraction record in the project checkout
# (docs/notebooks/firmware-diags.md and the installed manifest beside the
# mirror's extract), reproduced by tools/extract-irix mdk.
PROM_SHA256 = (
    "14e0d0a77b92154e3b33573f30608f241ded249db6060fba9f97bfd00b2145b2"
)
CPU_MDK_SHA256 = (
    "8a72e5578436b076bae72f6243940a523364ea961604d6070de7f40b408d5d89"
)
# The memory package, recorded by the same extraction; test_prom_mdk_memory
# boots it through this module's shared plumbing.
MEMORY_MDK_SHA256 = (
    "ee12d271bd84f2b3991724d40a211b9d9f511e1a355b58eee71c123c5b3f90bf"
)


def _file_sha256(path):
    digest = hashlib.sha256()
    with open(path, "rb") as source:
        for chunk in iter(lambda: source.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def payload_identity_problems(prom_path, payload_path, payload_sha256,
                              payload_label):
    """Which of the PROM/payload SHA-256 digests mismatch -- the same
    refusal test_prom_sash.py makes, so a stale or wrong asset is caught
    before spending a boot on it. The MDK test files share this with
    their own package digest and label."""
    problems = []
    if _file_sha256(prom_path) != PROM_SHA256:
        problems.append("PROM SHA-256 does not match the recorded release")
    if _file_sha256(payload_path) != payload_sha256:
        problems.append(
            f"{payload_label} SHA-256 does not match the recorded "
            "extraction")
    return problems


def artifact_identity_problems(prom_path, mdk_path):
    return payload_identity_problems(prom_path, mdk_path, CPU_MDK_SHA256,
                                     "cpu.mdk")


def verdict(log, reached=True):
    """PASS iff the driver reached pcache's completion line, every fixed
    assertion is present, and none of the kernel's failure spellings is.

    Returns {"verdict": "PASS"|"FAIL", "missing": [...], "problems": [...]}.
    """
    problems = []
    missing = missing_assertions(log)
    for name in missing:
        problems.append(f"missing {name}")
    for marker in failure_markers_present(log):
        problems.append(f"kernel failure output present: {marker!r}")
    if not reached:
        problems.append(
            "console driver did not reach the pcache completion line "
            "before timeout -- the run never proved the test finished")
    return {
        "verdict": "PASS" if not problems else "FAIL",
        "missing": missing,
        "problems": problems,
    }


# ---------------------------------------------------------------------------
# The real boot. The argv, driver and provenance records mirror
# test_prom_sash.py's own; departures are the payload and the CR-terminated
# MDK command (see this module's docstring).
# ---------------------------------------------------------------------------

FIRMWARE_REL = os.path.join('assets', 'firmware', '6.5-base',
                            'ip27prom.payload.bin')
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
_TAIL_MAX = 8192


def mdk_extra_args(prom, tftp, elsc_log, capture,
                   netboot_path=NETBOOT_PATH):
    """The QEMU args beyond set_machine()/set_console(), matching
    test_prom_sash.py's sash_extra_args line for line: one CPU, 64 MB,
    normal power-on diagnostics, the ELSC AUX chardev on serial line 2,
    SLIRP usermode networking serving this test's staged tftp root, and a
    wire capture. Only bootfile= differs."""
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
                 f'dhcpstart=192.0.2.15,tftp={tftp},'
                 f'bootfile={netboot_path}'),
        '-object', f'filter-dump,id=netcap,netdev=net0,file={capture}',
    ]


def _drive_console(sock, log_path, timeout, steps=None, until=None,
                   reached_when=None):
    """test_prom_sash.py's console driver, with the steps and the
    end-of-run condition injectable so the memory and instgen tests can
    reuse it: each (pattern, response) fires once on a tail match; every
    byte lands in log_path; returns (transcript, reached). `until` is a
    byte regex matched against the tail; `reached_when` is a predicate
    over the whole accumulated transcript, for runs whose completion
    line is not unique (the memory suite ends every test with the same
    words, so only a count over the whole run can name the last one).
    """
    if steps is None:
        steps = DRIVER_STEPS
    if until is None and reached_when is None:
        until = DRIVER_UNTIL_RE
    rules = [
        {"re": re.compile(pattern.encode()), "response": response.encode(),
         "fired": False}
        for pattern, response in steps
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
            if until is not None and until.search(tail):
                reached = True
                break
            if reached_when is not None and reached_when(bytes(transcript)):
                reached = True
                break
    return transcript.decode("utf-8", errors="replace"), reached


def _utc_now():
    return datetime.datetime.now(datetime.UTC).isoformat().replace(
        "+00:00", "Z")


def _file_record(path):
    resolved = os.path.realpath(path)
    return {
        "path": str(path),
        "resolved_path": resolved,
        "sha256": _file_sha256(resolved),
        "size_bytes": os.path.getsize(resolved),
    }


def _fork_git_identity(tree):
    """This fork checkout's own commit and dirty state; None on any git
    failure -- a missing identity must not read as a fabricated clean
    one. Matches test_prom_sash.py."""
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

    _FORK_ROOT = os.path.dirname(os.path.dirname(os.path.dirname(
        os.path.dirname(os.path.abspath(__file__)))))

    class PromMdk(QemuSystemTest):
        """Boots the real IP27 PROM, netboots cpu.mdk over SLIRP usermode
        networking, drives pcache at the MDK> prompt, and requires
        verdict() to be PASS."""

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

        def test_prom_mdk(self):
            paths = self._asset_paths()
            if paths is None:
                self.skipTest(
                    'ORIGAMI_ASSETS is not set, or has no firmware/cpu.mdk '
                    f'at <ORIGAMI_ASSETS>/{FIRMWARE_REL} and '
                    f'<ORIGAMI_ASSETS>/{MDK_REL} -- point it at a checkout '
                    'with the real PROM extracted and the MDK packages '
                    'landed (tools/extract-irix mdk) to run this test')
            prom, mdk = paths

            identity_problems = artifact_identity_problems(prom, mdk)
            if identity_problems:
                self.fail('asset identity problems: ' +
                          '; '.join(identity_problems))

            netboot_tftp = self._stage_netboot_tftp(mdk)

            vm = self.get_vm(name='prom_mdk')
            vm.set_machine(
                'origin200,synthesized-firmware=off,debug-leds=off')
            vm.set_console()
            elsc_log = self.log_file('elsc-aux.log')
            capture = self.log_file('wire.pcap')
            vm.add_args('-accel', 'tcg,thread=single',
                        *mdk_extra_args(prom, netboot_tftp, elsc_log,
                                        capture))

            started = _utc_now()
            vm.launch()
            log_path = self.log_file('mdk.log')
            try:
                transcript, reached = _drive_console(
                    vm.console_socket, log_path, BOOT_TIMEOUT_SECONDS)
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
                            'origin200,synthesized-firmware=off,'
                            'debug-leds=off'),
                        'args': mdk_extra_args(
                            prom, netboot_tftp, elsc_log, capture),
                    },
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
                'MDK netboot-and-pcache problems: ' +
                '; '.join(report['problems']) +
                f' (full report: {summary_path})')


if __name__ == '__main__':
    if QemuSystemTest is not None:
        QemuSystemTest.main()
