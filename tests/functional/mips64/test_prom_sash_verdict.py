#!/usr/bin/env python3
#
# Equivalence test: test_prom_sash's verdict logic implements
# tools/sash-gate's own 1-CPU netboot-inventory contract, without booting
# anything
#
# SPDX-License-Identifier: GPL-2.0-or-later
"""
A plain unittest -- no QEMU, no qemu_test import, runnable with just
`uv run --with pytest pytest
tests/functional/mips64/test_prom_sash_verdict.py -v`. It exercises
missing_assertions(), artifact_identity_problems(), and verdict() -- the
three functions test_prom_sash.py ports from tools/sashlib.py -- against
real captured console transcripts (docs/boot-logs/ in the project
checkout).

Fixture provenance (both fixtures are real captured guest-console text,
byte-exact including the original CR/LF sequences; neither is
synthesized):

  REAL_SASH_PASS_TRANSCRIPT
      docs/boot-logs/firmware.sash-netboot-vendored-slirp.2026-08-24T07:04+00:00,
      named in this port's brief as the fixture to prefer: a real
      tools/sash-gate --release 6.5.30 --skip-prom-diags run, verdict
      PASS, captured to record the first boot after the slirp
      compatibility series moved from an environment-supplied library to
      a static meson subproject. Its captured console (everything from
      that file's own separator line onward) is copied verbatim -- the
      fixture that proves missing_assertions() and verdict() accept a
      real all-inventory-present PASS run.

  REAL_NEVER_REACHED_SASH_TRANSCRIPT
      docs/boot-logs/firmware.boot.2026-08-23T01:19-07:00, also named in
      this port's brief. That capture's own full console text actually
      reaches sash with a complete inventory too (a second real PASS, not
      a failure by itself) -- confirmed by direct search of the file
      before this fixture was built, so this module does not need a
      second all-pass fixture to make that same point twice. What this
      fixture needs is a run that never reached sash: it is a REAL PREFIX
      of that same capture, sliced right after "Updated new configuration.
      Wrote 2 records." and before the PROM's own System Configuration
      and Diagnostics Summary -- the point a run whose budget expired
      during early boot would stop at. Nothing here is invented: this is
      verbatim text from a real capture, truncated, not assembled or
      edited. Every one of the ten ASSERTIONS/cpu/memory strings
      missing_assertions() checks was confirmed absent from this exact
      slice by direct search of the file, not by inspection, before it
      was adopted as the "never reached sash" fixture. The cut had to
      land before the PROM's own diagnostics-summary block, not merely
      before the sash session: that block prints two of the ten strings
      verbatim ("No. of CPUs enabled     = 1" and "No. of CPUs disabled
      = 0"), so a cut anywhere after it would leave this fixture missing
      only eight of the ten assertions instead of all ten.
"""

import hashlib
import os
import tempfile
import unittest

from test_prom_sash import (
    ASSERTIONS,
    RELEASES,
    artifact_identity_problems,
    missing_assertions,
    sash_extra_args,
    verdict,
)


REAL_SASH_PASS_TRANSCRIPT = """\
IP27 PROM SGI Version 6.156  built 11:27:56 AM Nov 18, 2003
*** Diag level set to None (2)
using BaseIO nic
Testing/Initializing memory ...............		DONE
Copying PROM code to memory ...............		DONE
Discovering local IO ......................		DONE
update_klcfg_cpuinfo: Couldn't find my structure.
Discovering NUMAlink connectivity .........		
Local hub NUMAlink is down.
*** Local network link down
DONE
Found 1 objects (1 hubs, 0 routers) in 3619 usec
Waiting for peers to complete discovery....		DONE
No other nodes present; becoming global master
Global master is /hw/module/0/slot/MotherBoard
Testing/Initializing all memory ...........		DONE
Checking partitioning information .........		DONE
No other nodes present; becoming partition master
Loading BASEIO prom .......................		DONE

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
         NO DIAGS WERE RUN!
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
      896   |/-\|/-\|/-\|/-\|+111764Setting $netaddr to 192.0.2.15 (from server )
   |/-\|/-+16853   |+3848      |/-\Setting $netaddr to 192.0.2.15 (from server )
   |/-\|/-\      |/-\      |/- entry: 0xa8000000012a6ee4
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
dlserver=192.0.2.2"""


REAL_NEVER_REACHED_SASH_TRANSCRIPT = """\
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
Testing/Initializing all memory ...........		DONE
Checking partitioning information .........		DONE
No other nodes present; becoming partition master
Loading BASEIO prom .......................		DONE

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
"""


# sash-gate's own missing_assertions(log, cpus=1) assertion names, in the
# exact order test_prom_sash.py's ported function builds them: the three
# cpu/memory lines it rebuilds from `cpus`, then every ASSERTIONS entry
# except the fixed "64 MiB inventory" (superseded by the rebuilt memory
# line just before it). Pinning this order and content here means a
# silent edit to either the ported function or ASSERTIONS is caught by
# this test, not only by a real boot's pass/fail.
EXPECTED_ASSERTION_NAMES = (
    "1 CPU inventory",
    "1 CPUs enabled",
    "no CPUs disabled",
    "64 MiB inventory",
    "IP27 system inventory",
    "IOC3 Ethernet inventory",
    "IOC3 serial inventory",
    "IOC3 output console",
    "leased guest address",
    "download server address",
)


class AssertionsTest(unittest.TestCase):

    def test_assertions_is_sashlib_s_own_seven_entries(self):
        # tools/sashlib.py's own ASSERTIONS tuple, copied verbatim (see
        # this module's docstring) -- pin its size and names so a silent
        # edit is caught here.
        self.assertEqual(len(ASSERTIONS), 7)
        self.assertEqual(
            [name for name, _ in ASSERTIONS],
            ["IP27 system inventory", "64 MiB inventory",
             "IOC3 Ethernet inventory", "IOC3 serial inventory",
             "IOC3 output console", "leased guest address",
             "download server address"])


class MissingAssertionsTest(unittest.TestCase):

    def test_real_pass_transcript_has_every_assertion(self):
        self.assertEqual(missing_assertions(REAL_SASH_PASS_TRANSCRIPT), [])

    def test_a_run_that_never_reached_sash_is_missing_all_ten_in_order(self):
        self.assertEqual(
            missing_assertions(REAL_NEVER_REACHED_SASH_TRANSCRIPT),
            list(EXPECTED_ASSERTION_NAMES))

    def test_a_single_missing_line_is_named_precisely(self):
        # A byte-exact deletion of one real assertion's evidence from the
        # real pass transcript -- not a rewrite or a synthesized line.
        without_ethernet = REAL_SASH_PASS_TRANSCRIPT.replace(
            "Integral Fast Ethernet", "")
        self.assertNotIn("Integral Fast Ethernet", without_ethernet)
        self.assertEqual(
            missing_assertions(without_ethernet),
            ["IOC3 Ethernet inventory"])

    def test_cpus_other_than_one_is_refused(self):
        # Scope guard: this port carries only sash-gate's proven 1-CPU
        # contract (see test_prom_sash.py's docstring) -- --cpus 2/4 must
        # not be silently mischecked as if they were 1.
        with self.assertRaises(NotImplementedError):
            missing_assertions(REAL_SASH_PASS_TRANSCRIPT, cpus=2)
        with self.assertRaises(NotImplementedError):
            missing_assertions(REAL_SASH_PASS_TRANSCRIPT, cpus=4)


class ArtifactIdentityProblemsTest(unittest.TestCase):
    """Exercises the two SHA-256 comparisons against real temporary files
    with known digests -- never against the actual vendor PROM/sash64
    binaries, which this fork test must not read (see this port's
    constraints) and are not present in this checkout in any case."""

    def setUp(self):
        self.tmpdir = tempfile.TemporaryDirectory()
        self.addCleanup(self.tmpdir.cleanup)

    def _write(self, name, content):
        path = os.path.join(self.tmpdir.name, name)
        with open(path, "wb") as handle:
            handle.write(content)
        return path

    def test_releases_carries_both_entries_sash_gate_exposes(self):
        # tools/sash-gate's --release choice is ("6.5-base", "6.5.30");
        # both must stay represented here so it stays selectable, exactly
        # as this port's brief requires.
        self.assertEqual(set(RELEASES), {"6.5-base", "6.5.30"})
        for release, artifacts in RELEASES.items():
            self.assertEqual(len(artifacts["prom_sha256"]), 64)
            self.assertEqual(len(artifacts["sash_sha256"]), 64)

    def test_matching_digests_report_no_problems(self):
        prom_content = b"stand-in prom content for this unit test only"
        sash_content = b"stand-in sash64 content for this unit test only"
        prom_path = self._write("prom.bin", prom_content)
        sash_path = self._write("sash64", sash_content)
        release = {
            "prom_sha256": hashlib.sha256(prom_content).hexdigest(),
            "sash_sha256": hashlib.sha256(sash_content).hexdigest(),
        }
        RELEASES["_unit_test_release"] = release
        try:
            self.assertEqual(
                artifact_identity_problems(
                    "_unit_test_release", prom_path, sash_path),
                [])
        finally:
            del RELEASES["_unit_test_release"]

    def test_prom_mismatch_is_named(self):
        prom_content = b"wrong prom bytes"
        sash_content = b"correct sash bytes"
        prom_path = self._write("prom.bin", prom_content)
        sash_path = self._write("sash64", sash_content)
        release = {
            "prom_sha256": "0" * 64,
            "sash_sha256": hashlib.sha256(sash_content).hexdigest(),
        }
        RELEASES["_unit_test_release"] = release
        try:
            problems = artifact_identity_problems(
                "_unit_test_release", prom_path, sash_path)
            self.assertEqual(
                problems,
                ["PROM SHA-256 does not match the selected release"])
        finally:
            del RELEASES["_unit_test_release"]

    def test_sash_mismatch_is_named(self):
        prom_content = b"correct prom bytes"
        sash_content = b"wrong sash bytes"
        prom_path = self._write("prom.bin", prom_content)
        sash_path = self._write("sash64", sash_content)
        release = {
            "prom_sha256": hashlib.sha256(prom_content).hexdigest(),
            "sash_sha256": "0" * 64,
        }
        RELEASES["_unit_test_release"] = release
        try:
            problems = artifact_identity_problems(
                "_unit_test_release", prom_path, sash_path)
            self.assertEqual(
                problems,
                ["sash64 SHA-256 does not match the selected release"])
        finally:
            del RELEASES["_unit_test_release"]


class VerdictTest(unittest.TestCase):

    def test_real_pass_transcript_reached_is_pass(self):
        report = verdict(REAL_SASH_PASS_TRANSCRIPT, reached=True)
        self.assertEqual(report["verdict"], "PASS", report["problems"])
        self.assertEqual(report["missing"], [])
        self.assertEqual(report["problems"], [])

    def test_never_reached_sash_transcript_is_fail_with_all_ten_named(self):
        report = verdict(REAL_NEVER_REACHED_SASH_TRANSCRIPT, reached=False)
        self.assertEqual(report["verdict"], "FAIL")
        self.assertEqual(len(report["missing"]), 10)
        self.assertTrue(
            any("did not reach the download-server" in p
                for p in report["problems"]))

    def test_full_inventory_but_driver_never_confirmed_is_still_fail(self):
        # A transcript with every inventory line present is still FAIL if
        # the console driver's own --until match never fired -- the
        # driver's confirmation is a separate, required check, not
        # implied by the inventory text alone.
        report = verdict(REAL_SASH_PASS_TRANSCRIPT, reached=False)
        self.assertEqual(report["verdict"], "FAIL")
        self.assertEqual(report["missing"], [])
        self.assertEqual(
            report["problems"],
            ["console driver did not reach the download-server "
             "confirmation before timeout -- the run never proved sash64 "
             "actually loaded"])


class SashExtraArgsTest(unittest.TestCase):

    def test_slirp_netboot_and_elsc_aux_shape(self):
        args = sash_extra_args("/tmp/prom.bin", "/tmp/tftp",
                               "/tmp/elsc.log", "/tmp/wire.pcap")
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
        self.assertIn("-smp", args)
        self.assertEqual(args[args.index("-smp") + 1], "1")
        self.assertIn("-m", args)
        self.assertEqual(args[args.index("-m") + 1], "64")


if __name__ == "__main__":
    unittest.main()
