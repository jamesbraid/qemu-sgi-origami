#!/usr/bin/env python3
#
# IRIX online diagnostic: boot the installed IRIX disk with the vendor's
# online diagnostics on it, log in, and require SGI's own oldisk test to
# report RSLT PASS
#
# SPDX-License-Identifier: GPL-2.0-or-later
"""
The layer above the firmware. The MDK, POD and fx legs run diagnostics
before or instead of IRIX; this one runs IRIX's own online diagnostics
(`/usr/diags/bin`) on a booted, multi-user system -- the diagnostic
stack a field engineer uses on a live machine, and the one that
exercises the hardware through IRIX's real drivers rather than the
firmware's.

It boots the published golden-with-diagnostics disk
(`assets/irix/6.5.30/root-diags.qcow2`: the installed IRIX 6.5.30 root
with the `field_diags_irix` product added and its environment loaded by
a post-install reboot -- the project checkout's irix-online-diagnostics
notebook records how the asset is built and why the reboot matters) on
the TWO-CHASSIS machine (four CPUs across two nodes, 128 MiB), the
fuller and newer of the two configurations: the diagnostics then run
against a four-CPU SMP kernel and exercise both nodes' memory. The
single-node install boots on it unchanged; IRIX autoconfigures to four
processors and two nodes. It drives the maintenance menu to Start
System, logs in as root, and runs
the three online diagnostics that reach a clean, definitive PASS on this
machine, one after another in one boot:

  - oldisk -- a disk exerciser: asynchronous direct pattern read/write
    through the qlogic controller (RSLT async-direct PASS);
  - olmem  -- a memory and cache test: tagram, L1 cache, L2 cache,
    address-pattern and moving-inversion memory (RSLT ... PASS, five).

Together they exercise the disk and the memory/cache paths through
IRIX's own drivers -- the layer the firmware legs cannot reach. Each
prints the RSLT-tagged output the field manual documents and a
META TOTAL <passes> 0 summary.

Why these three and not the other eight installed: olperi (the online
random-instruction test) is not for IP29 and fails on it; torpedo (the
FPU stress test) runs a fixed thousand-loop workload measured in guest
time, which dilates many-fold under emulation, so a full run is
impractical and a bounded one only duplicates the firmware MDK CPU and
memory coverage; and olcdrom, oltape, olvht, linktest and olrti need
hardware or a target this machine does not present (a CD-ROM -- see the
boot-hang report -- a tape, a HIPPI channel, a router topology, a device
vertex). olvst, a socket-based network test, passes interactively but
stalls under the container the functional tests run in, so it is left to
manual runs. The project checkout's irix-online-diagnostics notebook
characterizes all eleven.

test_irix_diag_verdict.py proves the verdict logic against the real
captured transcript without booting anything.
"""

import json
import os
import re
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

from lib_prom import drive_console, resolve_asset

try:
    from qemu_test import QemuSystemTest
    from qemu_test import skipSlowTest, skipBigDataTest, skipIfMissingEnv
except (ImportError, RuntimeError):
    QemuSystemTest = None

from test_irix_diag_verdict import diag_verdict


RELEASE = "6.5.30"
FIRMWARE_REL = os.path.join('assets', 'firmware', RELEASE,
                            'ip27prom.payload.bin')
DISK_REL = os.path.join('assets', 'irix', RELEASE, 'root-diags.qcow2')

# The installed IRIX two-chassis machine, the release's own PROM, and
# user-mode networking so IRIX's network init completes without a
# server (the diagnostics run locally and need none). topology= selects
# the two-chassis wiring; -m splits evenly into 64 MiB per node.
MEMORY_MIB = 128
TWO_CHASSIS_ARGS = (
    '-smp', '4,sockets=2,cores=2,threads=1',
)

# The whole two-chassis run -- boot, oldisk and olmem against four CPUs
# and 128 MiB -- took 2297s in the CI container on a first real run.
# That is the slowest leg here after the OpenBSD install; a generous
# budget keeps it from flaking under contention rather than trimming its
# coverage. 3540 leaves comfortable margin under the 3600s meson
# timeout registered for this test.
BOOT_TIMEOUT_SECONDS = 3540

# The three diagnostics, run in sequence in one shell command. oldisk
# exercises an 8 MiB scratch file; olmem takes no arguments.
DIAG_CMD = (
    "/usr/diags/bin/oldisk -filename /tmp/oldisk.tf -filesize 8; "
    "/usr/diags/bin/olmem")

# Drive the PROM menu to Start System, answer the login's TERM prompt,
# log in as root, and run the diagnostic. IRIX's default root shell is
# csh and the first login asks for TERM; a bare newline takes the
# default. The steps fire once each, in order, on the first tail match.
DRIVER_STEPS = (
    (r"Option\?", "1\n"),
    (r"login:", "root\n"),
    (r"TERM = \(vt100\)", "\n"),
    (r"[#$] ", DIAG_CMD + "\n"),
)
# The end condition is the third "META TOTAL <passes> <non-passes>"
# summary line -- one per diagnostic. Counting them is self-match proof
# (the command line the console echoes contains no such line) where a
# text sentinel would fire on its own echo, and it holds the capture
# until the last diagnostic's summary has arrived.
DIAG_SUMMARY_RE = re.compile(rb"META\s+TOTAL\s+\d+\s+\d+")
DIAG_COUNT = 2


def _utc_now():
    import datetime
    return datetime.datetime.now(datetime.UTC).isoformat().replace(
        "+00:00", "Z")


def _file_record(path):
    import hashlib
    resolved = os.path.realpath(path)
    digest = hashlib.sha256()
    with open(resolved, "rb") as source:
        for chunk in iter(lambda: source.read(1024 * 1024), b""):
            digest.update(chunk)
    return {"path": str(path), "resolved_path": resolved,
            "sha256": digest.hexdigest(),
            "size_bytes": os.path.getsize(resolved)}


def _fork_git_identity(tree):
    import subprocess
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
    return {"path": str(tree), "commit": commit.stdout.strip(),
            "dirty": bool(status.stdout.strip())}


if QemuSystemTest is not None:

    _FORK_ROOT = os.path.dirname(os.path.dirname(os.path.dirname(
        os.path.dirname(os.path.abspath(__file__)))))

    class IrixDiag(QemuSystemTest):
        """Boots the golden-with-diagnostics IRIX disk, logs in, runs
        oldisk, and requires diag_verdict() to be PASS."""

        def _asset_paths(self):
            assets_root = os.environ.get('ORIGAMI_ASSETS')
            prom = resolve_asset(assets_root, FIRMWARE_REL)
            disk = resolve_asset(assets_root, DISK_REL)
            if not (prom and disk):
                return None
            return prom, disk

        @skipBigDataTest()
        @skipSlowTest()
        @skipIfMissingEnv('ORIGAMI_ASSETS')
        def test_irix_diag(self):
            paths = self._asset_paths()
            if paths is None:
                # skipIfMissingEnv above already skipped the case where
                # ORIGAMI_ASSETS is unset entirely -- getting here means
                # it IS set, so a missing PROM or disk is a provisioning
                # failure, not an absent-assets developer run, and must
                # fail loudly: a silent skip here would let a real
                # oldisk regression pass CI unnoticed.
                self.fail(
                    'ORIGAMI_ASSETS is set but the PROM/disk are missing '
                    f'at <ORIGAMI_ASSETS>/{FIRMWARE_REL} and '
                    f'<ORIGAMI_ASSETS>/{DISK_REL} -- provision them '
                    '(nightly assets) rather than silently skipping a '
                    'claimed capability')
            prom, disk = paths

            vm = self.get_vm(name='irix_diag')
            vm.set_machine(
                'origin200,topology=origin200-dual,debug-leds=off')
            vm.set_console()
            vm.add_args(
                '-accel', 'tcg,thread=single',
                '-cpu', 'R10000', '-m', str(MEMORY_MIB),
                *TWO_CHASSIS_ARGS,
                '-bios', str(prom),
                '-audio', 'none',
                '-drive',
                f'if=none,id=disk0,file={disk},format=qcow2,snapshot=on',
                '-device', 'scsi-hd,bus=scsi.0,scsi-id=1,lun=0,drive=disk0',
                '-nic', 'user,id=net0')

            started = _utc_now()
            vm.launch()
            log_path = self.log_file('irix-diag.log')
            try:
                transcript, reached = drive_console(
                    vm.console_socket, log_path, BOOT_TIMEOUT_SECONDS,
                    DRIVER_STEPS, reached_when=(
                        lambda t: len(DIAG_SUMMARY_RE.findall(t))
                        >= DIAG_COUNT), vm=vm)
            finally:
                vm.shutdown(hard=vm.exitcode() is not None)
            ended = _utc_now()

            if not transcript:
                self.fail(
                    'the guest printed nothing at all -- a run that did not '
                    'start, not a boot that failed early')

            report = diag_verdict(transcript, reached=reached)

            summary_path = self.log_file('irix-diag-summary.json')
            with open(summary_path, 'w', encoding='utf-8') as summary_file:
                json.dump({'schema': 1, 'diagnostics': DIAG_CMD,
                           'reached_sentinel': reached, 'report': report},
                          summary_file, indent=2, sort_keys=True)
                summary_file.write('\n')

            run_path = self.log_file('run.json')
            with open(run_path, 'w', encoding='utf-8') as run_file:
                json.dump({
                    'schema': 1, 'started_utc': started, 'ended_utc': ended,
                    'console_driver_reached_sentinel': reached,
                    'timeout_seconds': BOOT_TIMEOUT_SECONDS,
                    'diagnostics': DIAG_CMD,
                    'identity': {
                        'fork': _fork_git_identity(_FORK_ROOT),
                        'qemu_binary': _file_record(self.qemu_bin),
                        'payload': {'prom': _file_record(prom),
                                    'disk': _file_record(disk)}},
                    'verdict': report['verdict'],
                    'problems': report['problems'],
                }, run_file, indent=2, sort_keys=True)
                run_file.write('\n')

            self.assertEqual(
                report['verdict'], 'PASS',
                'IRIX online diagnostic problems: ' +
                '; '.join(report['problems']) +
                f' (full report: {summary_path})')


if __name__ == '__main__':
    if QemuSystemTest is not None:
        QemuSystemTest.main()
