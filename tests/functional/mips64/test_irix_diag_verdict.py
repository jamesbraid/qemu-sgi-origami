#!/usr/bin/env python3
#
# Verdict logic for test_irix_diag, and its equivalence test
#
# SPDX-License-Identifier: GPL-2.0-or-later
"""
The pure PASS/FAIL judgment for the IRIX online-diagnostic leg, kept out
of the QemuSystemTest so a plain `uv run --with pytest pytest
tests/functional/mips64/test_irix_diag_verdict.py -v` proves it against
the real captured transcript without booting anything.

oldisk wraps its RSLT and summary lines in ANSI colour, so the verdict
strips escape sequences before matching. A pass requires the machine to
have reached its IRIX release banner and a root shell, oldisk to have
printed its own REV line, an `RSLT ... PASS` for the async-direct
pattern test, and a `META TOTAL` summary of one pass and zero
non-passes; and none of the kernel or diagnostic failure spellings.
"""

import re


_ANSI_RE = re.compile(r"\x1b\[[0-9;]*m")


def _strip(text):
    return _ANSI_RE.sub("", text)


ASSERTIONS = (
    ("IRIX release banner", re.compile(r"IRIX Release 6\.5 IP27")),
    ("root shell", re.compile(r"IRIX Release 6\.5 IP27 \w")),
    ("oldisk started", re.compile(r"oldisk version")),
    ("async-direct pass",
     re.compile(r"RSLT\s+async-direct\s+PASS")),
    ("summary all passed",
     re.compile(r"META\s+TOTAL\s+1\s+0\b")),
)

# Kernel and diagnostic failure spellings. "NON-PASSES" is a column
# header oldisk always prints, so it is not a marker; a real non-pass
# shows as a nonzero count in the META TOTAL line, which the summary
# assertion catches.
FAILURE_MARKERS = (
    "RSLT async-direct   FAIL",
    "Patterntest Fail",
    "PANIC",
    "Bus error",
    "Data bus error",
)


def missing_assertions(log):
    text = _strip(log)
    return [name for name, pattern in ASSERTIONS if not pattern.search(text)]


def failure_markers_present(log):
    text = _strip(log)
    return [marker for marker in FAILURE_MARKERS if marker in text]


def diag_verdict(log, reached=True):
    problems = []
    for name in missing_assertions(log):
        problems.append(f"missing {name}")
    for marker in failure_markers_present(log):
        problems.append(f"failure output present: {marker!r}")
    if not reached:
        problems.append(
            "console driver did not see oldisk's summary line before "
            "timeout -- the run never proved the diagnostic finished")
    return {"verdict": "PASS" if not problems else "FAIL",
            "problems": problems}


REAL_PASS_TRANSCRIPT = '\r\n\r\nIP27 PROM SGI Version 6.156  built 11:27:56 AM Nov 18, 2003\r\nusing BaseIO nic\r\nTesting/Initializing memory ...............\t\tDONE\r\nCopying PROM code to memory ...............\t\tDONE\r\nDiscovering local IO ......................\t\tDONE\r\nupdate_klcfg_cpuinfo: Couldn\'t find my structure.\r\nDiscovering NUMAlink connectivity .........\t\t\r\nLocal hub NUMAlink is down.\r\n*** Local network link down\r\nDONE\r\nFound 1 objects (1 hubs, 0 routers) in 4049 usec\r\nWaiting for peers to complete discovery....\t\tDONE\r\nNo other nodes present; becoming global master\r\nGlobal master is /hw/module/0/slot/MotherBoard\r\n\x08\x08\x08\x08Testing/Initializing all memory ...........\t\tDONE\r\n\x08Checking partitioning information .........\t\tDONE\r\nNo other nodes present; becoming partition master\r\n\x08Loading BASEIO prom .......................\t\tDONE\r\n\r\nBASEIO PROM Monitor SGI Version 6.156  built 11:05:48 AM Nov 18, 2003 (BE64)\r\n1 CPUs on 1 nodes found.\r\n\r\nNVRAM checksum is incorrect: reinitializing.\r\nInstalling PROM Device drivers ............             \r\nBase I/O Ethernet set to /dev/ethernet/ef0\r\n\r\nWalking SCSI Adapter 0 (/hw/module/1/slot/MotherBoard), (pci id 0)\r\n1+ 2- 3- 4- 5- 6- 7- 8- 9- 10- 11- 12- 13- 14- 15- = 1 device(s)\r\n\r\n\r\nWalking SCSI Adapter 1 (/hw/module/1/slot/MotherBoard), (pci id 1)\r\n1- 2- 3- 4- 5- 6- 7- 8- 9- 10- 11- 12- 13- 14- 15- = 0 device(s)\r\n\r\nInitializing PROM Device drivers ..........             DONE\r\nChecking hardware inventory ...............              \n\rWARNING: hardware inventory is invalid.  Reinitializing...\n\rWriting 2 records.. DONE\n\rUpdated new configuration. Wrote 2 records.\n\r\n\r**** System Configuration and Diagnostics Summary ****\n\rCONFIG:\n\r         No. of NODEs enabled    = 1\n\r         No. of NODEs disabled   = 0\n\r         No. of CPUs enabled     = 1\n\r         No. of CPUs disabled    = 0\n\r         Mem enabled             = 64 MB\n\r         Mem disabled            = 0 MB\n\r         No. of RTRs enabled     = 0\n\r         No. of RTRs disabled    = 0\n\r\n\rDIAG RESULTS:\n\r         ALL DIAGS PASSED.\n\r**** End System Configuration and Diagnostics Summary ****\n\r\r\n\r\nSystem Maintenance Menu\r\n\r\n1) Start System\r\n2) Install System Software\r\n3) Run Diagnostics\r\n4) Recover System\r\n5) Enter Command Monitor\r\n\n\rOption? 1\r\n\r\n\r\n                           Starting up the system...\r\n\r\nWARNING: Check the date to make sure it is correct\r\nWARNING: Check the date to make sure it is correct\r\nIRIX Release 6.5 IP27 Version 07202013 System V - 64 Bit\r\nCopyright 1987-2006 Silicon Graphics, Inc.\r\nAll Rights Reserved.\r\n\r\nSetting rbaud to 19200\r\nWARNING: Check the date to make sure it is correct\n\rNOTICE: Starting failsoftd\n\rThe system is coming up.\r\n\r\nWarning:  Internet Gateway web server running as root.\r\n          Use "chkconfig webface_apache off" to disable.\r\nStarting new eventmond...\r\nNo System change detected.\r\n\r\n\r\n\nwallclock-prom-5 console login: root\n\rIRIX Release 6.5 IP27 wallclock-prom-5\r\nCopyright 1987-2006 Silicon Graphics, Inc. All Rights Reserved.\r\nLast login: Thu Sep  3 16:08:24 PDT 2026 on ttyd1\r\nTERM = (vt100) \r\nwallclock-prom-5 1# /usr/diags/bin/oldisk -filename /tmp/oldisk.tf -filesize 8\r\n\r\nREV                 oldisk version 1.2 built on Jun 13 2002 at 19:32:46\r\nINFO                Start time Thu Sep  3 17:47:04 2026\r\nINFO                Running on IRIX64 6.5.30m 07202013 IP27 (wallclock-prom-5)\r\nCMDL                /usr/diags/bin/oldisk -filename /tmp/oldisk.tf -filesize 8\r\nTEST async-direct   Asynchronus Direct pattern read/write  Test(1/1), Loop(1/1)\r\nHRTB                Running Patterns.......... DONE!\r\n\x1b[30m\x1b[42mRSLT async-direct   PASS         Patterntest Pass                              \x1b[0m\r\nLOOP                Completed Loop 1   of 1, duration: 83.964 sec   \x1b[30m\x1b[42mPASS\x1b[0m\r\nMETA                ITERATION=1       PASSES         NON-PASSES                \r\n\x1b[0m\x1b[30m\x1b[42mMETA                async-direct      1              0                         \r\n\x1b[0mMETA                TOTAL             1              0                         '


if __name__ == '__main__':
    import unittest

    class DiagVerdictTests(unittest.TestCase):
        """The real transcript above is the mdk.log of the first passing
        meson run of test_irix_diag (func-mips64-irix_diag OK in 319s,
        2026-09-04): the maintenance-menu boot, a root login, oldisk, and
        RSLT async-direct PASS with a META TOTAL 1 0 summary. Byte-exact,
        including the ANSI colour oldisk wraps its result lines in."""

        def test_the_real_pass_transcript_passes(self):
            report = diag_verdict(REAL_PASS_TRANSCRIPT, reached=True)
            self.assertEqual(report['problems'], [])
            self.assertEqual(report['verdict'], 'PASS')

        def test_no_failure_marker_in_the_real_run(self):
            self.assertEqual(failure_markers_present(REAL_PASS_TRANSCRIPT), [])

        def test_an_unreached_summary_is_named(self):
            report = diag_verdict(REAL_PASS_TRANSCRIPT, reached=False)
            self.assertEqual(report['verdict'], 'FAIL')
            self.assertIn(
                "console driver did not see oldisk's summary line before "
                "timeout -- the run never proved the diagnostic finished",
                report['problems'])

        def test_a_failed_pattern_test_is_caught(self):
            injected = REAL_PASS_TRANSCRIPT.replace(
                "RSLT async-direct   PASS", "RSLT async-direct   FAIL")
            report = diag_verdict(injected, reached=True)
            self.assertEqual(report['verdict'], 'FAIL')

        def test_a_nonzero_non_pass_count_fails_the_summary(self):
            # The summary must read one pass and zero non-passes; a run
            # whose TOTAL shows a non-pass is not a clean diagnostic.
            injected = REAL_PASS_TRANSCRIPT.replace(
                "TOTAL             1              0",
                "TOTAL             0              1")
            report = diag_verdict(injected, reached=True)
            self.assertEqual(report['verdict'], 'FAIL')
            self.assertIn("missing summary all passed", report['problems'])

        def test_an_empty_log_fails_on_every_assertion(self):
            report = diag_verdict("", reached=True)
            self.assertEqual(report['verdict'], 'FAIL')
            self.assertEqual(len(report['problems']), len(ASSERTIONS))

    unittest.main()
