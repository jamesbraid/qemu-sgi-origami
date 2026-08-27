#!/usr/bin/env python3
#
# Equivalence test: test_prom_diagnostics's verdict logic implements
# diag-gate's own two-pass factory-sequence and expected-failure contract,
# without booting anything
#
# SPDX-License-Identifier: GPL-2.0-or-later
"""
A plain unittest -- no QEMU, no qemu_test import, runnable with just
`uv run --with pytest pytest
tests/functional/mips64/test_prom_diagnostics_verdict.py -v`. It exercises
evaluate_run() and the functions it is built from
against real captured console transcripts (docs/boot-logs/ in the project
checkout) plus the ported expected-failure ledger, and one clearly-labeled
synthetic transcript for the scenarios no single archived capture covers.

Fixture provenance (each is real captured guest-console text unless
labeled SYNTHETIC; see each fixture's own comment for the exact source
file and what, if anything, was changed):

  REAL_TWO_PASS_TRANSCRIPT
      docs/boot-logs/firmware.boot.2026-08-18T03:28-07:00, the only
      archived capture with the real two-pass factory-sequence shape
      (module-id assignment box, warm reset, then a second pass) --
      named in this port's brief as the fixture source to prefer. Its
      RSLT failures (serial_dma, enet_ssram, scsi_ram, scsi_dma) are
      exactly the four names in the shipped expected-failure ledger, so
      this fixture proves the ledger against real evidence, not a
      constructed match. It never drives to sash (the capture's own
      budget expired at the maintenance-menu prompt), so its verdict is
      correctly FAIL on the sash-inventory check alone -- a real,
      informative failure mode, not a fixture defect.

  REAL_SERIAL_DMA_EXCERPT, REAL_SUMMARY_EXCERPT
      Five- and eighteen-line excerpts of the two other transcripts named
      in this port's brief, docs/boot-logs/firmware.enet-diag-pass and
      firmware.enet-diag-all-pass, used for two smaller, targeted tests
      (a single real RSLT FAIL line with a diag_rc, and a real DIAG
      RESULTS block with four "Reason:" lines under one result -- which
      demonstrates parse_summary keeps only the LAST reason line per
      entry, a genuine contract detail worth pinning). Both source files
      render a captured carriage return as the literal text "^M" rather
      than a raw 0x0D byte (a documentation convention, not what the
      guest printed); this fixture module converts that back into a real
      \r so the fixture carries the same byte the real transcript did.

  SYNTHETIC_ALL_PASS_TRANSCRIPT
      SYNTHETIC. No archived capture combines the two-pass module-id
      assignment with a full sash netboot session in one run, so this
      fixture is assembled from two real captures: the early-boot and
      sash-session text is copied verbatim from
      firmware.boot.2026-08-23T01:19-07:00 (a real, all-diagnostics-pass,
      single-pass sash-gate run) and reused for both passes since the
      firmware runs the identical code path each time; the module-id
      assignment box and warm-reset banner are copied verbatim from
      firmware.boot.2026-08-18T03:28-07:00 (the same generic, fixed
      firmware text used in REAL_TWO_PASS_TRANSCRIPT above).

      Pass 2's text carries /hw/module/1/... in five places: "Global
      master is /hw/module/1/slot/MotherBoard", both "Walking SCSI
      Adapter N (/hw/module/1/slot/MotherBoard)" labels, and both
      "oldConsolePath=" / "ConsolePath=/hw/module/1/slot/MotherBoard"
      env lines. Only the FIRST of those five -- "Global master is" --
      was actually edited: the source capture (2026-08-23T01:19-07:00)
      prints it as /hw/module/0/..., because that single-pass run never
      went through a module-id reassignment. The other four are exactly
      as that same source capture printed them, unedited -- that
      specific real run already reports itself as module 1 for the SCSI
      adapter walk and its own ConsolePath, even though its earlier
      "Global master" line still said 0. The one edit (0 -> 1) brings
      the "Global master" line into agreement with those four already-
      real occurrences, because module_ids() does not read any one
      line: it scans the WHOLE pass-2 text for every /hw/module/(\\d+)/
      match and collects them as a set, and evaluate_run() requires
      that set to contain exactly one, nonzero, id. Leaving "Global
      master" at 0 would have made module_ids(pass 2) == {0, 1}, two
      ids, which fails that check on its own -- so this fixture's
      validity rests on all five agreeing, not on the one line that
      needed to change to make them agree. This is the same
      post-assignment state the OTHER real capture (2026-08-18T03:28)
      already demonstrates happens for real, where every pass-2
      /hw/module/ occurrence the firmware itself prints already agrees
      on the assigned id. Every RSLT-format line, summary field, and
      sash inventory string in this fixture is copied unmodified from a
      real capture; nothing was invented from scratch.
"""

import unittest

from test_prom_diagnostics import (
    diag_extra_args,
    evaluate_run,
    EXPECTED_FAILURES,
    EXPECTED_FAILURES_TEXT,
    extract_identities,
    missing_sash_assertions,
    module_ids,
    parse_expected_failures,
    parse_rslt_lines,
    parse_summary,
    split_passes,
)


REAL_TWO_PASS_TRANSCRIPT = """\
IP27 PROM SGI Version 6.11  built 03:19:48 PM Apr 29, 1998\r
using BaseIO nic\r
Testing/Initializing memory ...............		DONE\r
Copying PROM code to memory ...............		DONE\r
Discovering local IO ......................		serial_dma: Timeout on uartA dma ....\r
serial_dma: Timeout on uartB dma ....\r
serial_dma failed:  \r
RSLT serial_dma     FAIL                diag_rc = 63                            \r
diag_serial_dma: /hw/module/0/slot/io7:  FAILED\r
DONE\r
update_klcfg_cpuinfo: Couldn't find my structure.\r
Discovering CrayLink connectivity .........		\r
Local hub CrayLink is down.\r
*** Local network link down\r
DONE\r
Found 1 objects (1 hubs, 0 routers) in 2253 usec\r
Waiting for peers to complete discovery....		DONE\r
No other nodes present; becoming global master\r
Global master is /hw/module/0/slot/MotherBoard\r
Testing/Initializing all memory ...........		DONE\r
Checking partitioning information .........		DONE\r
No other nodes present; becoming partition master\r
Loading BASEIO prom .......................		DONE\r
\r
BASEIO PROM Monitor SGI Version 6.11  built 03:18:44 PM Apr 29, 1998 (BE64)\r
1 CPUs on 1 nodes found.\r
***************************************************************\r
*  Some module(s) did not have module id(s). New module id(s) *\r
*               assigned by default. Resetting system.        *\r
* You can assign moduleids using the module command at the POD*\r
***************************************************************\r
Resetting the system...\r
\r
\r
IP27 PROM SGI Version 6.11  built 03:19:48 PM Apr 29, 1998\r
using BaseIO nic\r
Testing/Initializing memory ...............		DONE\r
Copying PROM code to memory ...............		DONE\r
Discovering local IO ......................		serial_dma: Timeout on uartA dma ....\r
serial_dma: Timeout on uartB dma ....\r
serial_dma failed:  \r
RSLT serial_dma     FAIL                diag_rc = 63                            \r
diag_serial_dma: /hw/module/1/slot/io7:  FAILED\r
DONE\r
update_klcfg_cpuinfo: Couldn't find my structure.\r
Discovering CrayLink connectivity .........		\r
Local hub CrayLink is down.\r
*** Local network link down\r
DONE\r
Found 1 objects (1 hubs, 0 routers) in 1416 usec\r
Waiting for peers to complete discovery....		DONE\r
No other nodes present; becoming global master\r
Global master is /hw/module/1/slot/MotherBoard\r
Testing/Initializing all memory ...........		DONE\r
Checking partitioning information .........		DONE\r
No other nodes present; becoming partition master\r
Loading BASEIO prom .......................		DONE\r
\r
BASEIO PROM Monitor SGI Version 6.11  built 03:18:44 PM Apr 29, 1998 (BE64)\r
1 CPUs on 1 nodes found.\r
\r
NVRAM checksum is incorrect: reinitializing.\r
Installing PROM Device drivers ............             \r
enet_ssram failed: Failed parity test\r
RSLT enet_ssram     FAIL                diag_rc = 102 Failed parity test        \r
enet_ssram: ssram parity test error. Exp: 2bad1 Recv: 0bad1 \r
enet_ssram: ssram parity test error. Exp: 3bad0 Recv: 1bad0 \r
enet_ssram: IOC3 ssram test failed .... \r
enet_all: SSRAM test failed.\r
enet_phy_reg: Warning - unexpected Reg 0 reset value,  exp: 0x3100  mask: 0xff80  got: 0x3000\r
enet_phy_reg: Warning - unexpected Reg 1 reset value,  exp: 0x7809  mask: 0xf83f  got: 0x782d\r
enet_phy_reg: Warning - unexpected Reg 5 reset value,  exp: 0x0000  mask: 0xe3ff  got: 0x01e1\r
enet_phy_reg: Warning - unexpected Reg 6 reset value,  exp: 0x0000  mask: 0x001f  got: 0x0001\r
enet_phy_reg: Warning - unexpected Reg 23 reset value,  exp: 0x8040  mask: 0xb8d6  got: 0x0000\r
enet_phy_reg: Warning - unexpected Reg 28 reset value,  exp: 0x0039  mask: 0x003d  got: 0x0000\r
scsi_ram: SCSI mailbox test failed .... \r
scsi_ram failed:  \r
RSLT scsi_ram       FAIL                diag_rc = 127                           \r
scsi_dma: DMA test failed .... \r
scsi_dma failed:  \r
RSLT scsi_dma       FAIL                diag_rc = 69                            \r
QL SCSI controller initialization failed\r
Encountered a fatal error while initializing QL SCSI controller\r
scsi_ram: SCSI mailbox test failed .... \r
scsi_ram failed:  \r
RSLT scsi_ram       FAIL                diag_rc = 127                           \r
scsi_dma: DMA test failed .... \r
scsi_dma failed:  \r
RSLT scsi_dma       FAIL                diag_rc = 69                            \r
QL SCSI controller initialization failed\r
Encountered a fatal error while initializing QL SCSI controller\r
Initializing PROM Device drivers ..........             DONE\r
Checking hardware inventory ...............              
\rWARNING: hardware inventory is invalid.  Reinitializing...
\rUpdated new configuration. Wrote 2 records.
\r
\r**** System Configuration and Diagnostics Summary ****
\rCONFIG:
\r         No. of NODEs enabled    = 1
\r         No. of NODEs disabled   = 0
\r         No. of CPUs enabled     = 1
\r         No. of CPUs disabled    = 0
\r         Mem enabled             = 64 MB
\r         Mem disabled            = 0 MB
\r         No. of RTRs enabled     = 0
\r         No. of RTRs disabled    = 0
\r
\rDIAG RESULTS:
\r         /hw/module/1/slot/io1: IOC3 failed diag
\r                 Reason: Failed SCSI memory test.
\r**** End System Configuration and Diagnostics Summary ****
\r\r
\r
System Maintenance Menu\r
\r
1) Start System\r
2) Install System Software\r
3) Run Diagnostics\r
4) Recover System\r
5) Enter Command Monitor\r

\rOption? qemu-system-mips64: terminating on signal 15
"""

REAL_SERIAL_DMA_EXCERPT = """\
Discovering local IO ......................		serial_dma: Timeout on uartA dma ....\r
serial_dma: Timeout on uartB dma ....\r
serial_dma failed:  \r
RSLT serial_dma     FAIL                diag_rc = 79                            \r
diag_serial_dma: /hw/module/0/slot/io7: FAILED\r
"""

REAL_SUMMARY_EXCERPT = """\
\r**** System Configuration and Diagnostics Summary ****
\rCONFIG:
\r         No. of NODEs enabled    = 1
\r         No. of NODEs disabled   = 0
\r         No. of CPUs enabled     = 1
\r         No. of CPUs disabled    = 0
\r         Mem enabled             = 64 MB
\r         Mem disabled            = 0 MB
\r         No. of RTRs enabled     = 0
\r         No. of RTRs disabled    = 0
\r
\rDIAG RESULTS:
\r         /hw/module/1/slot/io1: SCSI failed diag
\r                 Reason: pci id 0 Failed SCSI memory test.
\r                 Reason: pci id 0 Failed SCSI DMA test.
\r                 Reason: pci id 1 Failed SCSI memory test.
\r                 Reason: pci id 1 Failed SCSI DMA test.
\r**** End System Configuration and Diagnostics Summary ****
"""

SYNTHETIC_ALL_PASS_TRANSCRIPT = """\
IP27 PROM SGI Version 6.156  built 11:27:56 AM Nov 18, 2003
using BaseIO nic
Testing/Initializing memory ...............		DONE
Copying PROM code to memory ...............		DONE
Discovering local IO ......................		DONE
update_klcfg_cpuinfo: Couldn't find my structure.
Discovering NUMAlink connectivity .........		
Local hub NUMAlink is down.
*** Local network link down
DONE
Found 1 objects (1 hubs, 0 routers) in 2592 usec
Waiting for peers to complete discovery....		DONE
No other nodes present; becoming global master
Global master is /hw/module/0/slot/MotherBoard
Testing/Initializing all memory ...........		DONE
Checking partitioning information .........		DONE
No other nodes present; becoming partition master
Loading BASEIO prom .......................		DONE

BASEIO PROM Monitor SGI Version 6.156  built 11:05:48 AM Nov 18, 2003 (BE64)
1 CPUs on 1 nodes found.
***************************************************************
*  Some module(s) did not have module id(s). New module id(s) *
*               assigned by default. Resetting system.        *
* You can assign moduleids using the module command at the POD*
***************************************************************
Resetting the system...


IP27 PROM SGI Version 6.156  built 11:27:56 AM Nov 18, 2003
using BaseIO nic
Testing/Initializing memory ...............		DONE
Copying PROM code to memory ...............		DONE
Discovering local IO ......................		DONE
update_klcfg_cpuinfo: Couldn't find my structure.
Discovering NUMAlink connectivity .........		
Local hub NUMAlink is down.
*** Local network link down
DONE
Found 1 objects (1 hubs, 0 routers) in 2592 usec
Waiting for peers to complete discovery....		DONE
No other nodes present; becoming global master
Global master is /hw/module/1/slot/MotherBoard
Testing/Initializing all memory ...........		DONE
Checking partitioning information .........		DONE
No other nodes present; becoming partition master
Loading BASEIO prom .......................		DONE

BASEIO PROM Monitor SGI Version 6.156  built 11:05:48 AM Nov 18, 2003 (BE64)
1 CPUs on 1 nodes found.

NVRAM checksum is incorrect: reinitializing.
Installing PROM Device drivers ............             
Base I/O Ethernet set to /dev/ethernet/ef0

Walking SCSI Adapter 0 (/hw/module/1/slot/MotherBoard), (pci id 0)
1- 2- 3- 4- 5- 6- 7- 8- 9- 10- 11- 12- 13- 14- 15- = 0 device(s)


Walking SCSI Adapter 1 (/hw/module/1/slot/MotherBoard), (pci id 1)
1- 2- 3- 4- 5- 6- 7- 8- 9- 10- 11- 12- 13- 14- 15- = 0 device(s)

Initializing PROM Device drivers ..........             DONE
Checking hardware inventory ...............              
WARNING: hardware inventory is invalid.  Reinitializing...
Writing 2 records.. DONE
Updated new configuration. Wrote 2 records.

**** System Configuration and Diagnostics Summary ****
CONFIG:
         No. of NODEs enabled    = 1
         No. of NODEs disabled   = 0
         No. of CPUs enabled     = 1
         No. of CPUs disabled    = 0
         Mem enabled             = 64 MB
         Mem disabled            = 0 MB
         No. of RTRs enabled     = 0
         No. of RTRs disabled    = 0

DIAG RESULTS:
         ALL DIAGS PASSED.
**** End System Configuration and Diagnostics Summary ****


System Maintenance Menu

1) Start System
2) Install System Software
3) Run Diagnostics
4) Recover System
5) Enter Command Monitor

Option? 5
Command Monitor.  Type "exit" to return to the menu.
>> unsetenv netaddr
>> boot -f bootp()/sash64
Setting $netaddr to 192.0.2.15 (from server )
Obtaining sash64 from server 
      896   |/-\\|/-\\|/-\\|/-\\|+111764Setting $netaddr to 192.0.2.15 (from server )
   |/-\\|/-+16853   |+3848      |/-\\Setting $netaddr to 192.0.2.15 (from server )
   |/-\\|/-\\      |/-\\      |/- entry: 0xa8000000012a6ee4
Standalone Shell SGI Version 6.5 ARCS   Jul 20, 2006 (64 Bit)
sash: hinv
System  SGI-IP27
1 180 MHz IP27 Processors
Main memory size: 64 Mbytes
Integral SCSI controller 0
Integral SCSI controller 1
Integral Fast Ethernet
IOC3 serial port
sash: printenv
AutoLoad=No
dbgtty=/dev/tty/ioc30
root=dks0d1s0
nonstop=0
rbaud=19200
SystemPartition=dksc(0,1,8)
OSLoadPartition=dksc(0,1,0)
OSLoader=sash
OSLoadFilename=unix
TimeZone=PST8PDT
console=d
gConsoleIn=default
gConsoleOut=default
diskless=0
scsihostid=00
ProbeAllScsi=n
RestorePartEnv=y
dbaud=9600
volume=80
sgilogo=y
ConsoleOut=/dev/tty/ioc30
ConsoleIn=/dev/tty/ioc30
cpufreq=180
oldConsolePath=/hw/module/1/slot/MotherBoard
ConsolePath=/hw/module/1/slot/MotherBoard
kernname=bootp()/sash64
netaddr=192.0.2.15
dlserver=192.0.2.2
"""


class ParseExpectedFailuresTest(unittest.TestCase):

    def test_ported_ledger_parses_to_the_empty_acceptance_list(self):
        # The module-level EXPECTED_FAILURES is exactly
        # parse_expected_failures(EXPECTED_FAILURES_TEXT) -- this pins
        # the result so a silent edit to either is caught here.
        # Empty since the 2026-08-27 run retired the last four entries:
        # the full-acceptance contract, pinned so a silent re-addition
        # is caught here.
        self.assertEqual(EXPECTED_FAILURES, [])
        self.assertEqual(
            parse_expected_failures(EXPECTED_FAILURES_TEXT),
            EXPECTED_FAILURES)

    def test_comments_and_blank_lines_are_ignored(self):
        text = "# a comment\n\nfoo   # trailing comment\n\nbar\n"
        self.assertEqual(parse_expected_failures(text), ["foo", "bar"])

    def test_whitespace_in_a_name_is_rejected(self):
        with self.assertRaises(ValueError):
            parse_expected_failures("foo bar\n")

    def test_duplicate_name_is_rejected(self):
        with self.assertRaises(ValueError):
            parse_expected_failures("foo\nfoo\n")


class ParseRsltLinesTest(unittest.TestCase):

    def test_real_fail_line_with_diag_rc_and_no_detail(self):
        # docs/boot-logs/firmware.enet-diag-pass: serial_dma FAILs with a
        # diag_rc and nothing else on the RSLT line itself.
        entries = parse_rslt_lines(REAL_SERIAL_DMA_EXCERPT)
        self.assertEqual(len(entries), 1)
        self.assertEqual(entries[0], {
            "name": "serial_dma", "verdict": "FAIL",
            "diag_rc": 79, "detail": "",
        })

    def test_real_fail_line_with_diag_rc_and_detail_text(self):
        # REAL_TWO_PASS_TRANSCRIPT's enet_ssram line carries a detail
        # message after the diag_rc: "diag_rc = 102 Failed parity test".
        entries = parse_rslt_lines(REAL_TWO_PASS_TRANSCRIPT)
        by_name = {e["name"]: e for e in entries}
        self.assertEqual(by_name["enet_ssram"]["diag_rc"], 102)
        self.assertEqual(by_name["enet_ssram"]["detail"], "Failed parity test")


class ParseSummaryTest(unittest.TestCase):

    def test_real_summary_block_counts(self):
        summary = parse_summary(REAL_SUMMARY_EXCERPT)
        self.assertEqual(summary["nodes_enabled"], 1)
        self.assertEqual(summary["cpus_enabled"], 1)
        self.assertEqual(summary["mem_enabled_mb"], 64)
        self.assertEqual(summary["routers_enabled"], 0)

    def test_multiple_reason_lines_keep_only_the_last(self):
        # A real contract detail, not a simplification of this port:
        # diag-gate's own parse_summary overwrites results[-1]["reason"]
        # on every "Reason:" line under one result entry, so with four
        # real Reason: lines under one /hw/module/1/slot/io1 entry, only
        # the last survives.
        summary = parse_summary(REAL_SUMMARY_EXCERPT)
        self.assertEqual(len(summary["diag_results"]), 1)
        entry = summary["diag_results"][0]
        self.assertEqual(entry["path"], "/hw/module/1/slot/io1")
        self.assertEqual(entry["finding"], "SCSI failed diag")
        self.assertEqual(entry["reason"], "pci id 1 Failed SCSI DMA test.")

    def test_no_summary_block_returns_none(self):
        self.assertIsNone(parse_summary("nothing relevant here"))


class TwoPassInvariantsTest(unittest.TestCase):
    """The two-pass module-id invariants diag-gate's evaluate_run checks,
    against REAL_TWO_PASS_TRANSCRIPT: pass 1 assigns and warm-resets using
    module id [0]; pass 2 accepts the single assigned nonzero id, with no
    repeated assignment box and no second warm reset."""

    def setUp(self):
        self.passes = split_passes(REAL_TWO_PASS_TRANSCRIPT)

    def test_exactly_two_passes(self):
        self.assertEqual(len(self.passes), 2)

    def test_pass_one_uses_module_id_zero_and_warm_resets(self):
        first = self.passes[0]
        self.assertEqual(module_ids(first), [0])
        self.assertIn("did not have module id", first)
        self.assertIn("Resetting the system...", first)
        self.assertNotIn("System Maintenance Menu", first)

    def test_pass_two_uses_the_single_assigned_nonzero_id(self):
        second = self.passes[1]
        self.assertEqual(module_ids(second), [1])
        self.assertNotIn("did not have module id", second)
        self.assertNotIn("Resetting the system...", second)
        self.assertIn("System Maintenance Menu", second)


class EvaluateRunTest(unittest.TestCase):

    # The red-baseline ledger as it stood before the 2026-08-27 run
    # retired every line: the real two-pass transcript below was
    # captured against exactly these four failures.
    RED_BASELINE_LEDGER = ("serial_dma", "enet_ssram", "scsi_ram",
                           "scsi_dma")

    def test_real_two_pass_transcript_matches_the_red_baseline_exactly(self):
        # unexpected == [] and stale == [] together prove the parser and
        # evaluate_run agree on a real red run against the ledger that
        # described it. The verdict is still FAIL, correctly: this
        # capture's own budget expired at the maintenance-menu prompt, so
        # it never drove to sash and the sash-inventory check is the only
        # failure.
        report = evaluate_run(REAL_TWO_PASS_TRANSCRIPT,
                              self.RED_BASELINE_LEDGER)
        self.assertEqual(report["failures"]["unexpected"], [])
        self.assertEqual(report["failures"]["stale"], [])
        self.assertEqual(report["verdict"], "FAIL")
        self.assertTrue(report["sash_missing"])
        self.assertTrue(
            all("did not occur" not in p and "unexpected" not in p
                for p in report["problems"]),
            report["problems"])

    def test_the_old_red_run_is_a_regression_against_the_shipped_ledger(self):
        # The shipped ledger is empty now, so the same real red run reads
        # as four unexpected failures and nothing stale -- which is what
        # a reappearance of any of them must look like.
        report = evaluate_run(REAL_TWO_PASS_TRANSCRIPT, EXPECTED_FAILURES)
        self.assertEqual(sorted(report["failures"]["unexpected"]),
                         sorted(self.RED_BASELINE_LEDGER))
        self.assertEqual(report["failures"]["stale"], [])
        self.assertEqual(report["verdict"], "FAIL")

    def test_all_pass_transcript_with_empty_ledger_passes(self):
        # diag-gate's own full-acceptance contract: an empty
        # expected-failure list, run against a transcript with no RSLT
        # FAIL lines at all, is a clean PASS.
        report = evaluate_run(SYNTHETIC_ALL_PASS_TRANSCRIPT, ())
        self.assertEqual(report["verdict"], "PASS", report["problems"])
        self.assertEqual(report["failures"]["unexpected"], [])
        self.assertEqual(report["failures"]["stale"], [])
        self.assertEqual(report["sash_missing"], [])

    def test_a_summary_without_the_positive_verdict_is_not_a_pass(self):
        # The synthetic clean transcript minus the firmware's own ALL DIAGS
        # PASSED line: no RSLT failure anywhere, empty ledger, and yet the
        # summary never judged the diagnostics -- the hole an absent line
        # would otherwise slip through.
        line = "         ALL DIAGS PASSED.\n"
        self.assertEqual(SYNTHETIC_ALL_PASS_TRANSCRIPT.count(line), 1)
        clipped = SYNTHETIC_ALL_PASS_TRANSCRIPT.replace(line, "")
        report = evaluate_run(clipped, ())
        self.assertEqual(report["verdict"], "FAIL")
        self.assertIn(
            "pass 2 summary does not report ALL DIAGS PASSED although no "
            "RSLT failure was observed", report["problems"])

    def test_expected_failure_present_is_accepted(self):
        # Real pass 1 (which genuinely FAILs serial_dma) combined with
        # the synthetic clean pass 2 -- expecting exactly that one
        # failure is accepted, not merely tolerated as a side effect of
        # an empty ledger.
        real_pass1 = split_passes(REAL_TWO_PASS_TRANSCRIPT)[0]
        clean_pass2 = split_passes(SYNTHETIC_ALL_PASS_TRANSCRIPT)[1]
        log = real_pass1 + "\n\n" + clean_pass2
        report = evaluate_run(log, ["serial_dma"])
        self.assertEqual(report["verdict"], "PASS", report["problems"])
        self.assertEqual(report["failures"]["unexpected"], [])
        self.assertEqual(report["failures"]["stale"], [])

    def test_unexpected_failure_is_rejected(self):
        # A real RSLT FAIL line (hub_bte_diag, in the same real format
        # docs/boot-logs/firmware.enet-diag-pass and -all-pass both show
        # it) injected into an otherwise clean run must fail the verdict
        # and be named, even though the ledger it is checked against is
        # otherwise satisfied.
        injected = SYNTHETIC_ALL_PASS_TRANSCRIPT.replace(
            "NVRAM checksum is incorrect: reinitializing.\n",
            "NVRAM checksum is incorrect: reinitializing.\n"
            "DIAG hub_bte_diag   HUB on Nodeboard in SLOT n1 failed test "
            "run by CPU 0.\n"
            "hub_bte_diag failed: Hub BTE test failed: hang.\n"
            "RSLT hub_bte_diag   FAIL                diag_rc = 160 Hub "
            "BTE test failed: hang.\n")
        report = evaluate_run(injected, ())
        self.assertEqual(report["verdict"], "FAIL")
        self.assertEqual(report["failures"]["unexpected"], ["hub_bte_diag"])
        self.assertEqual(report["failures"]["stale"], [])
        self.assertIn("unexpected diagnostic failure: hub_bte_diag",
                     report["problems"])

    def test_stale_expected_failure_is_reported(self):
        # Expecting a failure that did not happen must be reported as
        # stale, not silently accepted as "a failure was merely allowed".
        report = evaluate_run(SYNTHETIC_ALL_PASS_TRANSCRIPT, ["serial_dma"])
        self.assertEqual(report["verdict"], "FAIL")
        self.assertEqual(report["failures"]["stale"], ["serial_dma"])
        self.assertEqual(report["failures"]["unexpected"], [])
        self.assertIn(
            "expected diagnostic failure did not occur: serial_dma",
            report["problems"])

    def test_wrong_number_of_passes_fails_with_a_named_problem(self):
        one_pass_only = split_passes(REAL_TWO_PASS_TRANSCRIPT)[0]
        report = evaluate_run(one_pass_only, EXPECTED_FAILURES)
        self.assertEqual(report["verdict"], "FAIL")
        self.assertEqual(report["passes"], 1)
        self.assertIn(
            "expected exactly 2 PROM boot passes, saw 1",
            report["problems"])

    def test_nvram_warnings_expected_on_pass_two_only(self):
        # nvram_warnings="expected" (the default, and diag-gate's own
        # observed-behavior default) requires both warnings on pass 2 and
        # neither on pass 1 -- both real transcripts satisfy this, so
        # neither contributes an nvram-warning problem.
        report = evaluate_run(SYNTHETIC_ALL_PASS_TRANSCRIPT, ())
        self.assertTrue(
            all("warning" not in p for p in report["problems"]),
            report["problems"])

    def test_nvram_warnings_absent_flags_a_warning_that_appears(self):
        # nvram_warnings="absent" is diag-gate's other --nvram-warnings
        # mode: neither warning may appear on EITHER pass. The synthetic
        # transcript's pass 2 genuinely prints both (copied verbatim from
        # the real capture it was assembled from), so both must be named.
        report = evaluate_run(SYNTHETIC_ALL_PASS_TRANSCRIPT, (),
                              nvram_warnings="absent")
        self.assertEqual(report["verdict"], "FAIL")
        self.assertIn(
            "run printed a warning forbidden by nvram_warnings=absent: "
            "NVRAM checksum is incorrect",
            report["problems"])
        self.assertIn(
            "run printed a warning forbidden by nvram_warnings=absent: "
            "hardware inventory is invalid",
            report["problems"])

    def test_nvram_warnings_absent_passes_when_neither_warning_appears(self):
        quiet = SYNTHETIC_ALL_PASS_TRANSCRIPT.replace(
            "NVRAM checksum is incorrect: reinitializing.\n", ""
        ).replace(
            "WARNING: hardware inventory is invalid.  Reinitializing...\n", ""
        )
        self.assertNotIn("NVRAM checksum is incorrect", quiet)
        self.assertNotIn("hardware inventory is invalid", quiet)
        report = evaluate_run(quiet, (), nvram_warnings="absent")
        self.assertTrue(
            all("nvram_warnings=absent" not in p for p in report["problems"]),
            report["problems"])
        self.assertEqual(report["verdict"], "PASS", report["problems"])


class ExtractIdentitiesTest(unittest.TestCase):

    def test_prom_banner_is_stripped_of_its_trailing_cr(self):
        # REAL_TWO_PASS_TRANSCRIPT's banners carry a real trailing \r
        # (PROM_BANNER_RE's ".*" consumes it); extract_identities must
        # strip it back off.
        identities = extract_identities(REAL_TWO_PASS_TRANSCRIPT)
        self.assertEqual(
            identities["prom_banners"],
            ["IP27 PROM SGI Version 6.11  built 03:19:48 PM Apr 29, 1998"])

    def test_env_values_are_the_last_occurrence(self):
        identities = extract_identities(SYNTHETIC_ALL_PASS_TRANSCRIPT)
        self.assertEqual(identities["dlserver"], "192.0.2.2")
        self.assertEqual(identities["netaddr"], "192.0.2.15")
        self.assertEqual(identities["console_out"], "/dev/tty/ioc30")


class MissingSashAssertionsTest(unittest.TestCase):

    def test_all_pass_transcript_has_every_assertion(self):
        self.assertEqual(
            missing_sash_assertions(SYNTHETIC_ALL_PASS_TRANSCRIPT), [])

    def test_a_run_that_never_reached_sash_is_missing_all_of_them(self):
        missing = missing_sash_assertions(REAL_TWO_PASS_TRANSCRIPT)
        self.assertIn("download server address", missing)
        self.assertIn("IP27 system inventory", missing)


class DiagExtraArgsTest(unittest.TestCase):

    def test_slirp_netboot_and_elsc_aux_shape(self):
        args = diag_extra_args("/tmp/prom.bin", "/tmp/tftp", "/tmp/elsc.log",
                               "/tmp/wire.pcap")
        self.assertIn("-bios", args)
        self.assertEqual(args[args.index("-bios") + 1], "/tmp/prom.bin")
        nic = args[args.index("-nic") + 1]
        self.assertIn("tftp=/tmp/tftp", nic)
        self.assertIn("bootfile=sash64", nic)
        self.assertIn("dhcpstart=192.0.2.15", nic)
        # -serial none (IOC3 port B, unused) then -serial chardev:elsc
        # (the ELSC AUX port) -- set_console() already emits line 0
        # itself, so these two are lines 1 and 2, in that order.
        serial_positions = [i for i, a in enumerate(args) if a == "-serial"]
        self.assertEqual(len(serial_positions), 2)
        first, second = serial_positions
        self.assertEqual(args[first + 1], "none")
        self.assertEqual(args[second + 1], "chardev:elsc")


if __name__ == "__main__":
    unittest.main()
