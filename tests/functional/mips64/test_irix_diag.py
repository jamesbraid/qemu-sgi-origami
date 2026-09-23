#!/usr/bin/env python3
#
# IRIX online diagnostic: boot the installed IRIX disk with the vendor's
# online diagnostics, a SCSI tape and a SCSI CD-ROM attached, log in,
# and require oldisk, olmem, oltape and olcdrom to all report PASS
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
System, logs in as root, and runs the four online diagnostics that
reach a clean, definitive PASS on this machine, one after another in
one boot:

  - oldisk  -- a disk exerciser: asynchronous direct pattern read/write
    through the qlogic controller (RSLT async-direct PASS);
  - olmem   -- a memory and cache test: tagram, L1 cache, L2 cache,
    address-pattern and moving-inversion memory (RSLT ... PASS, five);
  - oltape  -- a tape exerciser against an emulated `scsi-tape` (rw,
    thrash, block: RSLT ... PASS, three);
  - olcdrom -- a CD-ROM confidence test against an emulated `scsi-cd`
    holding a generated data CD (RSLT olcdrom PASS).

Together they exercise the disk, memory/cache, tape and CD-ROM paths
through IRIX's own drivers -- the layer the firmware legs cannot reach.
Each prints the RSLT-tagged output the field manual documents; oldisk,
olmem and oltape each also print a META TOTAL <passes> 0 summary, and
olcdrom prints a LOOP Completed Loop line instead (its own field manual
does not document a META TOTAL for it).

Why these four and not the other seven installed: olperi (the online
random-instruction test) is not for IP29 and fails on it; torpedo (the
FPU stress test) runs a fixed thousand-loop workload measured in guest
time, which dilates many-fold under emulation, so a full run is
impractical and a bounded one only duplicates the firmware MDK CPU and
memory coverage; olvht, linktest and olrti need a HIPPI channel, a
router topology or a device vertex this machine does not present.
olvst, a socket-based network test, passes interactively but stalls
under the container the functional tests run in, so it is left to
manual runs. The project checkout's irix-online-diagnostics notebook
characterizes all eleven.

oltape and olcdrom were each their own boot until this merge. Neither
needed a second boot for a hardware reason: the tape is a composed
`ARCHIVE Python 25601` identity IRIX's driver recognizes as a DAT (the
recorded real drive's own identity matches nothing in IRIX's table and
the extended-status ioctl it needs refuses), and the CD-ROM's old boot
hang (jamesb/qemu#367) was fixed in jamesb/qemu#379 before either test
was written -- a `scsi-cd` attached at boot no longer stalls IRIX at
failsoftd. Attaching both to this leg's own two-chassis boot, on the
second SCSI adapter alongside the boot disk's first, needed no new
device-model work, only proving the four diagnostics still reach their
own PASS in sequence on the fuller machine -- confirmed by this test's
own run.

diag_verdict() below scores the transcript with lib_prom.verdict(),
whose own unit tests pin that scoring without booting anything.
"""

import json
import os
import re
import sys
import tempfile

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

from lib_prom import (FORK_ROOT, MACHINES, drive_console, require_assets,
                      run_record, strip_ansi, utc_now, verdict)

try:
    from qemu_test import QemuSystemTest
    from qemu_test import skipSlowTest, skipBigDataTest, skipIfMissingEnv
except (ImportError, RuntimeError):
    QemuSystemTest = None

# A pass requires the machine to have reached its IRIX release banner and
# a root shell, and each of the four diagnostics -- oldisk (disk), olmem
# (memory and cache, five subtests), oltape (tape, three subtests) and
# olcdrom (CD-ROM) -- to have printed its own RSLT PASS, the three that
# document one a META TOTAL summary of N passes and zero non-passes, and
# olcdrom its own completed-loop line.
ASSERTIONS = (
    ("IRIX release banner", re.compile(r"IRIX Release 6\.5 IP27")),
    ("root shell", re.compile(r"IRIX Release 6\.5 IP27 \w")),
    # oldisk: the disk exerciser's async-direct pattern pass and its
    # one-pass summary.
    ("oldisk async-direct pass",
     re.compile(r"RSLT\s+async-direct\s+PASS")),
    ("oldisk summary all passed",
     re.compile(r"META\s+TOTAL\s+1\s+0\b")),
    # olmem: five memory and cache subtests, all passing.
    ("olmem tagram pass", re.compile(r"RSLT\s+tagram\s+PASS")),
    ("olmem l1cache pass", re.compile(r"RSLT\s+l1cache\s+PASS")),
    ("olmem l2cache pass", re.compile(r"RSLT\s+l2cache\s+PASS")),
    ("olmem addr-pattern pass",
     re.compile(r"RSLT\s+addr-pattern\s+PASS")),
    ("olmem memaddr pass", re.compile(r"RSLT\s+memaddr\s+PASS")),
    ("olmem summary all passed",
     re.compile(r"META\s+TOTAL\s+5\s+0\b")),
    # oltape: three subtests against the composed ARCHIVE DAT identity.
    ("oltape started", re.compile(r"Online Tape Test")),
    ("oltape rw pass", re.compile(r"RSLT\s+rw\s+PASS")),
    ("oltape thrash pass", re.compile(r"RSLT\s+thrash\s+PASS")),
    ("oltape block pass", re.compile(r"RSLT\s+block\s+PASS")),
    ("oltape summary all passed",
     re.compile(r"META\s+TOTAL\s+3\s+0\b")),
    # olcdrom: reads the generated data CD across the whole medium.
    ("olcdrom started", re.compile(r"olcdrom version")),
    ("olcdrom pass", re.compile(r"RSLT\s+olcdrom\s+PASS")),
    ("olcdrom loop completed",
     re.compile(r"LOOP\s+Completed Loop\s+\d+\s+of\s+\d+")),
)

# Kernel and diagnostic failure spellings. "NON-PASSES" is a column
# header oldisk and oltape always print, so it is not a marker; a real
# non-pass shows as a nonzero count in the META TOTAL line, which the
# summary assertions catch. olcdrom aborts (rather than printing RSLT
# FAIL) when it cannot open the device or find its generated data, so
# those abort spellings are tripwires of their own.
TRIPWIRES = (
    ("oldisk async-direct failed", "RSLT async-direct   FAIL"),
    ("pattern test failed", "Patterntest Fail"),
    ("oltape rw failed", "RSLT rw             FAIL"),
    ("oltape thrash failed", "RSLT thrash         FAIL"),
    ("oltape block failed", "RSLT block          FAIL"),
    ("the tape drive refused the extended-status request",
     "ioctl(mtiocgetext) failed"),
    ("olcdrom failed", "RSLT olcdrom        FAIL"),
    ("the CD-ROM drive was not ready", "doesn't appear ready"),
    ("the CD-ROM device would not open", "can't open device"),
    ("the CD-ROM medium carried no diagnostic data",
     "can't locate SGI diagnostic data"),
    ("a CD-ROM read failed", "READ ERROR"),
    ("the guest panicked", "PANIC"),
    ("the guest took a bus error", "Bus error"),
    ("the guest took a data bus error", "Data bus error"),
)


def diag_verdict(log, reached=True):
    """PASS iff every required line appeared, no failure spelling did,
    and the console driver saw olcdrom's own result line -- the last
    diagnostic in the chain, so its line proves the other three already
    ran to completion ahead of it."""
    problems = verdict(strip_ansi(log), ASSERTIONS, TRIPWIRES)
    if not reached:
        problems.append(
            "console driver did not see olcdrom's result line before "
            "timeout -- the run never proved all four diagnostics "
            "finished")
    return {"verdict": "PASS" if not problems else "FAIL",
            "problems": problems}


RELEASE = "6.5.30"
FIRMWARE_REL = os.path.join('assets', 'firmware', RELEASE,
                            'ip27prom.payload.bin')
DISK_REL = os.path.join('assets', 'irix', RELEASE, 'root-diags.qcow2')
MEDIUM_REL = os.path.join('assets', 'irix', RELEASE, 'olcdrom-medium.raw')

# The recorded SHA-256 of the olcdrom -gendata medium. require_assets
# raises AssetMismatch when a file at MEDIUM_REL is present but differs.
MEDIUM_SHA256 = \
    'ba0a1466d7125cd350ee4f047677d7a3d9f8a8ed8b13469c92042cfa0bd07829'

# The installed IRIX two-chassis machine, the release's own PROM, and
# user-mode networking so IRIX's network init completes without a
# server (the diagnostics run locally and need none). topology= selects
# the two-chassis wiring; -m splits evenly into 64 MiB per node.
MEMORY_MIB = 128
TWO_CHASSIS_ARGS = (
    '-smp', '4',
)

# The tape node: no-rewind, so the diagnostic controls position itself.
# The ARCHIVE Python DDS-1 identity IRIX recognizes as a DAT -- see this
# module's docstring for why the recorded real drive's identity will not
# do. The raw SCSI node olcdrom reads: adapter 1, target 6.
TAPE_DEV = "/dev/rmt/tps1d5nr"
TAPE_VENDOR = "ARCHIVE"
TAPE_PRODUCT = "Python 25601"
CD_DEV = "/dev/scsi/sc1d6l0"

# The whole two-chassis run -- boot plus oldisk and olmem against four
# CPUs and 128 MiB -- took 2297s in the CI container on a first real
# run, the slowest leg here after the OpenBSD install; oltape's rw,
# thrash and block on a fresh tape and olcdrom's read passes over its
# medium are each seconds by comparison. 6900 keeps the usual 60s
# margin under the 6960s meson timeout registered in mips64/meson.build
# -- the three absorbed scripts' own budgets, 3600, 1560 and 1800,
# summed for all four diagnostics on the one boot.
BOOT_TIMEOUT_SECONDS = 6900

# The four diagnostics, run in sequence in one shell command. oldisk
# exercises an 8 MiB scratch file; olmem takes no arguments; oltape and
# olcdrom each take their own device. oltape gets --tapemark --eot as
# before: those flags tell oltape to exclude the two subtests they
# name, not to run them -- both need the tape configured down to a
# second /dev/rmt node, and eot writes until the medium ends, which an
# unbounded image never reaches.
DIAG_CMD = (
    "/usr/diags/bin/oldisk -filename /tmp/oldisk.tf -filesize 8; "
    "/usr/diags/bin/olmem; "
    f"/usr/diags/bin/oltape --tapemark --eot -d {TAPE_DEV}; "
    f"/usr/diags/bin/olcdrom -device {CD_DEV}")

# Drive the PROM menu to Start System, answer the login's TERM prompt,
# log in as root, and run the diagnostics. IRIX's default root shell is
# csh and the first login asks for TERM; a bare newline takes the
# default. The steps fire once each, in order, on the first tail match.
DRIVER_STEPS = (
    (r"Option\?", "1\n"),
    (r"login:", "root\n"),
    (r"TERM = \(vt100\)", "\n"),
    (r"[#$] ", DIAG_CMD + "\n"),
)
# olcdrom's own RSLT line, the whole chain's end condition since it is
# the last command in DIAG_CMD. This cannot be the generic "LOOP
# Completed Loop N of M" line: oldisk and oltape each print that same
# banner for themselves too, well before olcdrom ever runs, so a
# console driver watching for it stops at oldisk's own loop line and
# never lets olmem, oltape or olcdrom run at all. "RSLT olcdrom" is the
# one banner unique to this diagnostic; an abort ("ABRT ...") that
# prints no RSLT at all is also caught so a hung run there does not
# spend the whole timeout unattributed.
DIAG_DONE_RE = re.compile(
    rb"(RSLT\s+olcdrom\s+(PASS|FAIL)|ABRT\s+\w)")


if QemuSystemTest is not None:

    class IrixDiag(QemuSystemTest):
        """Boots the golden-with-diagnostics IRIX disk with a scsi-tape
        and a scsi-cd attached, logs in, runs oldisk, olmem, oltape and
        olcdrom in sequence, and requires diag_verdict() to be PASS."""

        @skipBigDataTest()
        @skipSlowTest()
        @skipIfMissingEnv('ORIGAMI_ASSETS')
        def test_irix_diag(self):
            # A medium that is present but the wrong file raises
            # AssetMismatch rather than resolving.
            assets = require_assets(self, FIRMWARE_REL, DISK_REL,
                                    (MEDIUM_REL, MEDIUM_SHA256))
            prom, disk = assets[FIRMWARE_REL], assets[DISK_REL]
            medium = assets[MEDIUM_REL]

            # A fresh empty tape image per run: oltape writes and reads
            # it.
            tape_fd, tape_path = tempfile.mkstemp(
                prefix='oltape-', suffix='.tap', dir=self.workdir)
            os.close(tape_fd)

            vm = self.get_vm(name='irix_diag')
            vm.set_machine(MACHINES['origin200_dual'])
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
                '-drive',
                f'if=none,id=tape0,file={tape_path},format=raw',
                '-device',
                'scsi-tape,bus=scsi.1,scsi-id=5,lun=0,drive=tape0,'
                f'vendor={TAPE_VENDOR},product={TAPE_PRODUCT}',
                '-drive',
                f'if=none,id=cd0,file={medium},format=raw,readonly=on',
                '-device', 'scsi-cd,bus=scsi.1,scsi-id=6,lun=0,drive=cd0',
                '-nic', 'user,id=net0')

            started = utc_now()
            vm.launch()
            log_path = self.log_file('irix-diag.log')
            try:
                transcript, reached = drive_console(
                    vm.console_socket, log_path, BOOT_TIMEOUT_SECONDS,
                    DRIVER_STEPS, reached_when=(
                        lambda t: bool(DIAG_DONE_RE.search(t))), vm=vm)
            finally:
                vm.shutdown(hard=vm.exitcode() is not None)
            ended = utc_now()

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
                    'identity': run_record(FORK_ROOT, self.qemu_bin,
                                          prom=prom, disk=disk,
                                          medium=medium),
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
