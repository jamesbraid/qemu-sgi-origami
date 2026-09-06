#!/usr/bin/env python3
#
# Verdict logic for test_irix_olcdrom, and its equivalence test
#
# SPDX-License-Identifier: GPL-2.0-or-later
"""
The pure PASS/FAIL judgment for the IRIX olcdrom leg, kept out of the
QemuSystemTest so a plain `uv run --with pytest pytest
tests/functional/mips64/test_irix_olcdrom_verdict.py -v` proves it against
the real captured transcript without booting anything.

olcdrom wraps its RSLT and LOOP lines in ANSI colour, so the verdict strips
escape sequences before matching. A pass requires the machine to have
reached its IRIX release banner and a root shell, olcdrom to have printed
its own REV line, an `RSLT olcdrom PASS`, and the `LOOP Completed ... PASS`
line; and none of the failure spellings, including the aborts olcdrom gives
when it cannot open the device or locate its data on the medium.
"""

import re


_ANSI_RE = re.compile(r"\x1b\[[0-9;]*m")


def _strip(text):
    return _ANSI_RE.sub("", text)


# The console driver waits for olcdrom's final line over the whole
# transcript -- the LOOP-completed line that follows RSLT, so RSLT is
# captured rather than ending the driver a beat before it. A failure ends
# it sooner: an RSLT FAIL, or an abort ("ABRT ...") that prints no RSLT at
# all and would otherwise run out the timeout; the verdict fails those on
# the missing RSLT PASS and the abort marker.
DIAG_DONE_RE = re.compile(
    rb"(LOOP\s+Completed Loop\s+\d+\s+of\s+\d+"
    rb"|RSLT\s+olcdrom\s+FAIL|ABRT\s+\w)")


ASSERTIONS = (
    ("IRIX release banner", re.compile(r"IRIX Release 6\.5 IP27")),
    ("root shell", re.compile(r"IRIX Release 6\.5 IP27 \w")),
    ("olcdrom started", re.compile(r"olcdrom version")),
    ("olcdrom pass", re.compile(r"RSLT\s+olcdrom\s+PASS")),
    ("loop completed", re.compile(r"LOOP\s+Completed Loop\s+\d+\s+of\s+\d+")),
)

# olcdrom aborts (rather than printing RSLT FAIL) when it cannot open the
# device or find its generated data on the medium; those spellings, plus a
# read error and the kernel faults, are the failures.
FAILURE_MARKERS = (
    "RSLT olcdrom        FAIL",
    "doesn't appear ready",
    "can't open device",
    "can't locate SGI diagnostic data",
    "READ ERROR",
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


def olcdrom_verdict(log, reached=True):
    problems = []
    for name in missing_assertions(log):
        problems.append(f"missing {name}")
    for marker in failure_markers_present(log):
        problems.append(f"failure output present: {marker!r}")
    if not reached:
        problems.append(
            "console driver did not see olcdrom's result line before "
            "timeout -- the run never proved the diagnostic finished")
    return {"verdict": "PASS" if not problems else "FAIL",
            "problems": problems}


REAL_PASS_TRANSCRIPT = '\r\n\r\nIP27 PROM SGI Version 6.156  built 11:27:56 AM Nov 18, 2003\r\nusing BaseIO nic\r\nTesting/Initializing memory ...............\t\tDONE\r\nCopying PROM code to memory ...............\t\tDONE\r\nDiscovering local IO ......................\t\tDONE\r\nupdate_klcfg_cpuinfo: Couldn\'t find my structure.\r\nDiscovering NUMAlink connectivity .........\t\t\r\nLocal hub NUMAlink is down.\r\n*** Local network link down\r\nDONE\r\nFound 1 objects (1 hubs, 0 routers) in 1907 usec\r\nWaiting for peers to complete discovery....\t\tDONE\r\nNo other nodes present; becoming global master\r\nGlobal master is /hw/module/0/slot/MotherBoard\r\n\x08\x08\x08\x08Testing/Initializing all memory ...........\t\tDONE\r\n\x08Checking partitioning information .........\t\tDONE\r\nNo other nodes present; becoming partition master\r\n\x08Loading BASEIO prom .......................\t\tDONE\r\n\r\nBASEIO PROM Monitor SGI Version 6.156  built 11:05:48 AM Nov 18, 2003 (BE64)\r\n1 CPUs on 1 nodes found.\r\n\r\nNVRAM checksum is incorrect: reinitializing.\r\nInstalling PROM Device drivers ............             \r\nBase I/O Ethernet set to /dev/ethernet/ef0\r\n\r\nWalking SCSI Adapter 0 (/hw/module/1/slot/MotherBoard), (pci id 0)\r\n1+ 2- 3- 4- 5- 6- 7- 8- 9- 10- 11- 12- 13- 14- 15- = 1 device(s)\r\n\r\n\r\nWalking SCSI Adapter 1 (/hw/module/1/slot/MotherBoard), (pci id 1)\r\n1- 2- 3- 4- 5- 6+ 7- 8- 9- 10- 11- 12- 13- 14- 15- = 1 device(s)\r\n\r\nInitializing PROM Device drivers ..........             DONE\r\nChecking hardware inventory ...............              \n\rWARNING: hardware inventory is invalid.  Reinitializing...\n\rWriting 2 records.. DONE\n\rUpdated new configuration. Wrote 2 records.\n\r\n\r**** System Configuration and Diagnostics Summary ****\n\rCONFIG:\n\r         No. of NODEs enabled    = 1\n\r         No. of NODEs disabled   = 0\n\r         No. of CPUs enabled     = 1\n\r         No. of CPUs disabled    = 0\n\r         Mem enabled             = 64 MB\n\r         Mem disabled            = 0 MB\n\r         No. of RTRs enabled     = 0\n\r         No. of RTRs disabled    = 0\n\r\n\rDIAG RESULTS:\n\r         ALL DIAGS PASSED.\n\r**** End System Configuration and Diagnostics Summary ****\n\r\r\n\r\nSystem Maintenance Menu\r\n\r\n1) Start System\r\n2) Install System Software\r\n3) Run Diagnostics\r\n4) Recover System\r\n5) Enter Command Monitor\r\n\n\rOption? 1\r\n\r\n\r\n                           Starting up the system...\r\n\r\nWARNING: Check the date to make sure it is correct\r\nWARNING: Check the date to make sure it is correct\r\nIRIX Release 6.5 IP27 Version 07202013 System V - 64 Bit\r\nCopyright 1987-2006 Silicon Graphics, Inc.\r\nAll Rights Reserved.\r\n\r\nSetting rbaud to 19200\r\nWARNING: Check the date to make sure it is correct\n\rNOTICE: Starting failsoftd\n\rThe system is coming up.\r\n\r\nWarning:  Internet Gateway web server running as root.\r\n          Use "chkconfig webface_apache off" to disable.\r\nStarting new eventmond...\r\nNo System change detected.\r\n\r\n\r\n\nwallclock-prom-5 console login: root\n\rIRIX Release 6.5 IP27 wallclock-prom-5\r\nCopyright 1987-2006 Silicon Graphics, Inc. All Rights Reserved.\r\nLast login: Thu Sep  3 16:08:24 PDT 2026 on ttyd1\r\nTERM = (vt100) \r\nwallclock-prom-5 1# /usr/diags/bin/olcdrom -device /dev/scsi/sc1d6l0\r\nREV                 olcdrom version 1.0 built on Jun 13 2002 at 19:33:13\r\nINFO                Start time Sat Sep  5 19:42:21 2026\r\nINFO                Running on IRIX64 6.5.30m 07202013 IP27 (wallclock-prom-5)\r\nINFO                Compiled on Jun 13 2002 at 19:35:28\r\nCMDL                /usr/diags/bin/olcdrom -device /dev/scsi/sc1d6l0\r\nTEST olcdrom        CDROM confidence test                             Test(1/1), Loop(1/1)\r\nINFO                CD-ROM      QEMU          QEMU CD-ROM       2.5+\r\nINFO                device block size:         512 bytes\r\nINFO                device block count:    1330176 blocks\r\nINFO                SGI diagnostic data located at block 0, offset 0\r\nINFO                testing zeros...\r\nINFO                testing ones...\r\nINFO                testing evens...\r\nINFO                testing odds...\r\nINFO                testing random data, pass 0/4\r\nINFO                testing random data, pass 1/4\r\nINFO                testing random data, pass 2/4\r\nINFO                testing random data, pass 3/4\r\nINFO                testing random access, pass 1/2...\r\nINFO                testing random access, pass 2/2...\r\n\x1b[30m\x1b[42mRSLT olcdrom        PASS         all tests passed                                         \x1b[0m\r\nLOOP                Completed Loop 1   of 1, durat'


if __name__ == '__main__':
    import unittest

    class OlcdromVerdictTests(unittest.TestCase):
        """The real transcript above is the irix-olcdrom.log of a passing
        run of test_irix_olcdrom: the maintenance-menu boot, a root login,
        olcdrom -device over the generated data CD, and RSLT olcdrom PASS
        with the LOOP-completed line. Byte-exact, including the ANSI colour
        olcdrom wraps its result lines in."""

        def test_the_real_pass_transcript_passes(self):
            report = olcdrom_verdict(REAL_PASS_TRANSCRIPT, reached=True)
            self.assertEqual(report['problems'], [])
            self.assertEqual(report['verdict'], 'PASS')

        def test_no_failure_marker_in_the_real_run(self):
            self.assertEqual(
                failure_markers_present(REAL_PASS_TRANSCRIPT), [])

        def test_an_unreached_result_is_named(self):
            report = olcdrom_verdict(REAL_PASS_TRANSCRIPT, reached=False)
            self.assertEqual(report['verdict'], 'FAIL')
            self.assertIn(
                "console driver did not see olcdrom's result line before "
                "timeout -- the run never proved the diagnostic finished",
                report['problems'])

        def test_a_failed_result_is_caught(self):
            injected = REAL_PASS_TRANSCRIPT.replace(
                "RSLT olcdrom        PASS", "RSLT olcdrom        FAIL")
            report = olcdrom_verdict(injected, reached=True)
            self.assertEqual(report['verdict'], 'FAIL')

        def test_an_abort_is_caught(self):
            # olcdrom aborts (no RSLT) when the device is not ready or its
            # data is not on the medium; that must not read as clean.
            injected = REAL_PASS_TRANSCRIPT.replace(
                "RSLT olcdrom        PASS         all tests passed",
                "ABRT                device /dev/scsi/sc1d6l0 doesn't appear "
                "ready")
            report = olcdrom_verdict(injected, reached=True)
            self.assertEqual(report['verdict'], 'FAIL')

        def test_an_empty_log_fails_on_every_assertion(self):
            report = olcdrom_verdict("", reached=True)
            self.assertEqual(report['verdict'], 'FAIL')
            self.assertEqual(len(report['problems']), len(ASSERTIONS))

    unittest.main()
