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

The diagnostics wrap their RSLT and summary lines in ANSI colour, so the
verdict strips escape sequences before matching. A pass requires the
machine to have reached its IRIX release banner and a root shell, and
each of the two diagnostics -- oldisk (disk) and olmem (memory and
cache, five subtests) -- to have printed its own RSLT PASS and a
META TOTAL summary of N passes and zero non-passes; and
none of the kernel or diagnostic failure spellings.
"""

import re


_ANSI_RE = re.compile(r"\x1b\[[0-9;]*m")


def _strip(text):
    return _ANSI_RE.sub("", text)


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
            "console driver did not see both diagnostics' summary lines "
            "before timeout -- the run never proved they finished")
    return {"verdict": "PASS" if not problems else "FAIL",
            "problems": problems}


REAL_PASS_TRANSCRIPT = '\r\n\r\nIP27 PROM SGI Version 6.156  built 11:27:56 AM Nov 18, 2003\r\nusing BaseIO nic\r\nTesting/Initializing memory ...............\t\tDONE\r\nCopying PROM code to memory ...............\t\tDONE\r\nDiscovering local IO ......................\t\tDONE\r\nDiscovering NUMAlink connectivity .........\t\tDONE\r\nFound 2 objects (2 hubs, 0 routers) in 35772 usec\r\nWaiting for peers to complete discovery....\t\tDONE\r\nGlobal master is /hw/module/0/slot/MotherBoard\r\n\x08\x08\x08\x08Testing/Initializing all memory ...........\t\tDONE\r\n\x08Checking partitioning information .........\t\tDONE\r\n\x08Loading BASEIO prom .......................\t\tDONE\r\n\r\nBASEIO PROM Monitor SGI Version 6.156  built 11:05:48 AM Nov 18, 2003 (BE64)\r\n4 CPUs on 2 nodes found.\r\n***************************************************************\r\n*  Some module(s) did not have module id(s). New module id(s) *\r\n*               assigned by default. Resetting system.        *\r\n* You can assign moduleids using the module command at the POD*\r\n***************************************************************\r\nResetting the system...\r\n\r\n\r\nIP27 PROM SGI Version 6.156  built 11:27:56 AM Nov 18, 2003\r\nusing BaseIO nic\r\nTesting/Initializing memory ...............\t\tDONE\r\nCopying PROM code to memory ...............\t\tDONE\r\nDiscovering local IO ......................\t\tDONE\r\nDiscovering NUMAlink connectivity .........\t\tDONE\r\nFound 2 objects (2 hubs, 0 routers) in 19594 usec\r\nWaiting for peers to complete discovery....\t\tDONE\r\nGlobal master is /hw/module/1/slot/MotherBoard\r\n\x08\x08\x08\x08Testing/Initializing all memory ...........\t\tDONE\r\n\x08Checking partitioning information .........\t\tDONE\r\n\x08Loading BASEIO prom .......................\t\tDONE\r\n\r\nBASEIO PROM Monitor SGI Version 6.156  built 11:05:48 AM Nov 18, 2003 (BE64)\r\n4 CPUs on 2 nodes found.\r\n\r\nNVRAM checksum is incorrect: reinitializing.\r\nInstalling PROM Device drivers ............             \r\nBase I/O Ethernet set to /dev/ethernet/ef0\r\n\r\nWalking SCSI Adapter 0 (/hw/module/1/slot/MotherBoard), (pci id 0)\r\n1+ 2- 3- 4- 5- 6- 7- 8- 9- 10- 11- 12- 13- 14- 15- = 1 device(s)\r\n\r\n\r\nWalking SCSI Adapter 1 (/hw/module/1/slot/MotherBoard), (pci id 1)\r\n1- 2- 3- 4- 5- 6- 7- 8- 9- 10- 11- 12- 13- 14- 15- = 0 device(s)\r\n\r\nInitializing PROM Device drivers ..........             DONE\r\nChecking hardware inventory ...............              \n\rWARNING: hardware inventory is invalid.  Reinitializing...\n\rWriting 4 records.... DONE\n\rUpdated new configuration. Wrote 4 records.\n\r\n\r**** System Configuration and Diagnostics Summary ****\n\rCONFIG:\n\r         No. of NODEs enabled    = 2\n\r         No. of NODEs disabled   = 0\n\r         No. of CPUs enabled     = 4\n\r         No. of CPUs disabled    = 0\n\r         Mem enabled             = 128 MB\n\r         Mem disabled            = 0 MB\n\r         No. of RTRs enabled     = 0\n\r         No. of RTRs disabled    = 0\n\r\n\rDIAG RESULTS:\n\r         ALL DIAGS PASSED.\n\r**** End System Configuration and Diagnostics Summary ****\n\r\r\n\r\nSystem Maintenance Menu\r\n\r\n1) Start System\r\n2) Install System Software\r\n3) Run Diagnostics\r\n4) Recover System\r\n5) Enter Command Monitor\r\n\n\rOption? 1\r\n\r\n\r\n                           Starting up the system...\r\n\r\nWARNING: Check the date to make sure it is correct\r\nWARNING: Check the date to make sure it is correct\r\nIRIX Release 6.5 IP27 Version 07202013 System V - 64 Bit\r\nCopyright 1987-2006 Silicon Graphics, Inc.\r\nAll Rights Reserved.\r\n\r\nSetting rbaud to 19200\r\nWARNING: Check the date to make sure it is correct\n\rNOTICE: Starting failsoftd\n\rThe system is coming up.\r\n\r\nnetwork: WARNING: Failed to configure ef1 as gate-wallclock-prom-5.\r\nWarning:  Internet Gateway web server running as root.\r\n          Use "chkconfig webface_apache off" to disable.\r\nStarting new eventmond...\r\nNo System change detected.\r\n\r\n\r\n\nwallclock-prom-5 console login: root\n\rIRIX Release 6.5 IP27 wallclock-prom-5\r\nCopyright 1987-2006 Silicon Graphics, Inc. All Rights Reserved.\r\nLast login: Thu Sep  3 16:08:24 PDT 2026 on ttyd1\r\nTERM = (vt100) \r\nwallclock-prom-5 1# /usr/diags/bin/oldisk -filename /tmp/oldisk.tf -filesize 8; /usr/diags/bin/olmem\r\n\r\nREV                 oldisk version 1.2 built on Jun 13 2002 at 19:32:46\r\nINFO                Start time Thu Sep  3 20:34:29 2026\r\nINFO                Running on IRIX64 6.5.30m 07202013 IP27 (wallclock-prom-5)\r\nCMDL                /usr/diags/bin/oldisk -filename /tmp/oldisk.tf -filesize 8\r\nTEST async-direct   Asynchronus Direct pattern read/write  Test(1/1), Loop(1/1)\r\nHRTB                Running Patterns.......... DONE!\r\n\x1b[30m\x1b[42mRSLT async-direct   PASS         Patterntest Pass                              \x1b[0m\r\nLOOP                Completed Loop 1   of 1, duration: 109.277 sec   \x1b[30m\x1b[42mPASS\x1b[0m\r\nMETA                ITERATION=1       PASSES         NON-PASSES                \r\n\x1b[0m\x1b[30m\x1b[42mMETA                async-direct      1              0                         \r\n\x1b[0mMETA                TOTAL             1              0                         \r\n\x1b[0m\r\nREV                 olmem version 1.3 built on Jun 13 2002 at 19:33:13\r\nINFO                Start time Thu Sep  3 20:36:23 2026\r\nINFO                Running on IRIX64 6.5.30m 07202013 IP27 (wallclock-prom-5)\r\nINFO                 \r\nINFO                NOTE: Most memory errors are seen by the kernel instead of\r\nINFO                olmem. Check the console and /var/adm/SYSLOG for memory\r\nINFO                errors after olmem is done.\r\nINFO                 \r\nCMDL                /usr/diags/bin/olmem\r\nTEST tagram         Exercise cache tagram bits             Test(1/5), Loop(1/1)\r\nINFO                Trying to test 48 MB out of 60 MB free with 4 cpu\'s\r\nHRTB                Starting threads to lock memory into place.... DONE!\r\nHRTB                Running memory test. DONE!\r\n\x1b[30m\x1b[42mRSLT tagram         PASS                                                       \x1b[0m\r\nTEST l1cache        L1 Cache test                          Test(2/5), Loop(1/1)\r\nHRTB                Running memory test........ DONE!\r\n\x1b[30m\x1b[42mRSLT l1cache        PASS                                                       \x1b[0m\r\nTEST l2cache        L2 Cache test                          Test(3/5), Loop(1/1)\r\nHRTB                Running memory test. DONE!\r\n\x1b[30m\x1b[42mRSLT l2cache        PASS                                                       \x1b[0m\r\nTEST addr-pattern   Address pattern test (with inversion)  Test(4/5), Loop(1/1)\r\nHRTB                Running memory test. DONE!\r\n\x1b[30m\x1b[42mRSLT addr-pattern   PASS                                                       \x1b[0m\r\nTEST memaddr        Moving inversion memory test           Test(5/5), Loop(1/1)\r\nHRTB                Running memory test...................... DONE!\r\n\x1b[30m\x1b[42mRSLT memaddr        PASS                                                       \x1b[0m\r\nLOOP                Completed Loop 1   of 1, duration: 459.349 sec   \x1b[30m\x1b[42mPASS\x1b[0m\r\nMETA                ITERATION=1       PASSES         NON-PASSES                \r\n\x1b[0m\x1b[30m\x1b[42mMETA                tagram            1              0                         \r\n\x1b[0m\x1b[30m\x1b[42mMETA                l1cache           1              0                         \r\n\x1b[0m\x1b[30m\x1b[42mMETA                l2cache           1              0                         \r\n\x1b[0m\x1b[30m\x1b[42mMETA                addr-pattern      1              0                         \r\n\x1b[0m\x1b[30m\x1b[42mMETA                memaddr           1              0                         \r\n\x1b[0mMETA                TOTAL             5              0                         \r\n\x1b[0m'


if __name__ == '__main__':
    import unittest

    class DiagVerdictTests(unittest.TestCase):
        """The real transcript above is the mdk.log of the first passing
        meson run of test_irix_diag (func-mips64-irix_diag OK in 319s,
        2026-09-04): the two-chassis boot (4 CPUs on 2 nodes), a root
        login, oldisk and olmem, each to its RSLT PASS and META TOTAL
        summary. Byte-exact,
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
                "console driver did not see both diagnostics' summary "
                "lines before timeout -- the run never proved they "
                "finished",
                report['problems'])

        def test_a_failed_pattern_test_is_caught(self):
            injected = REAL_PASS_TRANSCRIPT.replace(
                "RSLT async-direct   PASS", "RSLT async-direct   FAIL")
            report = diag_verdict(injected, reached=True)
            self.assertEqual(report['verdict'], 'FAIL')

        def test_a_nonzero_non_pass_count_fails_the_summary(self):
            # Each summary must read all passes and zero non-passes; a run
            # whose memory-test TOTAL shows a non-pass is not clean.
            injected = REAL_PASS_TRANSCRIPT.replace(
                "TOTAL             5              0",
                "TOTAL             4              1")
            report = diag_verdict(injected, reached=True)
            self.assertEqual(report['verdict'], 'FAIL')
            self.assertIn("missing olmem summary all passed",
                          report['problems'])

        def test_an_empty_log_fails_on_every_assertion(self):
            report = diag_verdict("", reached=True)
            self.assertEqual(report['verdict'], 'FAIL')
            self.assertEqual(len(report['problems']), len(ASSERTIONS))

    unittest.main()
