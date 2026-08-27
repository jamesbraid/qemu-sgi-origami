#!/usr/bin/env python3
#
# Equivalence test: test_prom_mdk's verdict logic against real captured
# MDK console transcripts, without booting anything
#
# SPDX-License-Identifier: GPL-2.0-or-later
"""
A plain unittest -- no QEMU, no qemu_test import, runnable with just
`uv run --with pytest pytest
tests/functional/mips64/test_prom_mdk_verdict.py -v`. It exercises
missing_assertions(), failure_markers_present(), verdict() and
artifact_identity_problems() from test_prom_mdk.py against real captured
console transcripts, the same division test_prom_sash_verdict.py keeps.

Fixture provenance (every fixture is real captured guest-console text,
byte-exact including CR/LF; none is synthesized):

  REAL_MDK_PASS_TRANSCRIPT
      The mdk.log of the first real meson run of test_prom_mdk itself
      (func-mips64-prom_mdk OK in 152s, 2026-08-27, run.json beside it in
      that build tree): the PROM menu, the path-structured
      `boot -f bootp()/mdk/cpu.mdk` netboot, the MDK 1.6 banner, and a
      passing pcache. The one all-assertions-present PASS fixture.

  REAL_FLAT_NAME_TRANSCRIPT
      The transcript of the project checkout's
      docs/boot-logs/firmware.mdk-cpu-netboot.2026-08-26T23:34-07:00
      (everything below that capture's separator): the first-ever MDK
      netboot on this machine, served FLAT (`bootp()/cpu.mdk`), running
      ls, pcache (passing) and instgen (which exits before its first
      pass on one CPU -- the capture's own header and the project
      notebook explain that exit; it prints no failure marker). Dual
      duty, exactly like test_prom_sash_verdict's flat fixture: every
      assertion except the by-path fetch line is present, so it is the
      real "a flat name was used" FAIL fixture for the path check.

  REAL_NEVER_REACHED_PREFIX
      A real prefix of REAL_MDK_PASS_TRANSCRIPT, cut before the BASEIO
      PROM loads -- the point a run whose budget expired during early
      boot would stop at. Verbatim text, truncated, not edited.

  REAL_KERNEL_WRECK_SLICE
      A live MDK session slice from the same project work (the notebook
      records it): `dstack 2` against a pid that does not exist takes an
      unexpected TLB exception inside the kernel and dumps its eframe.
      The real carrier of the "Unexpected exception" failure marker.
"""

import os
import sys
import tempfile
import unittest

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

from test_prom_mdk import (
    ASSERTIONS,
    artifact_identity_problems,
    failure_markers_present,
    missing_assertions,
    verdict,
)


REAL_MDK_PASS_TRANSCRIPT = '\r\n\r\nIP27 PROM SGI Version 6.11  built 03:19:48 PM Apr 29, 1998\r\nusing BaseIO nic\r\nTesting/Initializing memory ...............\t\tDONE\r\nCopying PROM code to memory ...............\t\tDONE\r\nDiscovering local IO ......................\t\tDONE\r\nupdate_klcfg_cpuinfo: Couldn\'t find my structure.\r\nDiscovering CrayLink connectivity .........\t\t\r\nLocal hub CrayLink is down.\r\n*** Local network link down\r\nDONE\r\nFound 1 objects (1 hubs, 0 routers) in 3408 usec\r\nWaiting for peers to complete discovery....\t\tDONE\r\nNo other nodes present; becoming global master\r\nGlobal master is /hw/module/0/slot/MotherBoard\r\nTesting/Initializing all memory ...........\t\tDONE\r\nChecking partitioning information .........\t\tDONE\r\nNo other nodes present; becoming partition master\r\nLoading BASEIO prom .......................\t\tDONE\r\n\r\nBASEIO PROM Monitor SGI Version 6.11  built 03:18:44 PM Apr 29, 1998 (BE64)\r\n1 CPUs on 1 nodes found.\r\n***************************************************************\r\n*  Some module(s) did not have module id(s). New module id(s) *\r\n*               assigned by default. Resetting system.        *\r\n* You can assign moduleids using the module command at the POD*\r\n***************************************************************\r\nResetting the system...\r\n\r\n\r\nIP27 PROM SGI Version 6.11  built 03:19:48 PM Apr 29, 1998\r\nusing BaseIO nic\r\nTesting/Initializing memory ...............\t\tDONE\r\nCopying PROM code to memory ...............\t\tDONE\r\nDiscovering local IO ......................\t\tDONE\r\nupdate_klcfg_cpuinfo: Couldn\'t find my structure.\r\nDiscovering CrayLink connectivity .........\t\t\r\nLocal hub CrayLink is down.\r\n*** Local network link down\r\nDONE\r\nFound 1 objects (1 hubs, 0 routers) in 3404 usec\r\nWaiting for peers to complete discovery....\t\tDONE\r\nNo other nodes present; becoming global master\r\nGlobal master is /hw/module/1/slot/MotherBoard\r\nTesting/Initializing all memory ...........\t\tDONE\r\nChecking partitioning information .........\t\tDONE\r\nNo other nodes present; becoming partition master\r\nLoading BASEIO prom .......................\t\tDONE\r\n\r\nBASEIO PROM Monitor SGI Version 6.11  built 03:18:44 PM Apr 29, 1998 (BE64)\r\n1 CPUs on 1 nodes found.\r\n\r\nNVRAM checksum is incorrect: reinitializing.\r\nInstalling PROM Device drivers ............             \r\n\r\nWalking SCSI Adapter 0 (/hw/module/1/slot/MotherBoard), (pci id 0)\r\n1- 2- 3- 4- 5- 6- 7- 8- 9- 10- 11- 12- 13- 14- 15- = 0 device(s)\r\n\r\nWalking SCSI Adapter 1 (/hw/module/1/slot/MotherBoard), (pci id 1)\r\n1- 2- 3- 4- 5- 6- 7- 8- 9- 10- 11- 12- 13- 14- 15- = 0 device(s)\r\nInitializing PROM Device drivers ..........             DONE\r\nChecking hardware inventory ...............              \n\rWARNING: hardware inventory is invalid.  Reinitializing...\n\rUpdated new configuration. Wrote 2 records.\n\r\n\r**** System Configuration and Diagnostics Summary ****\n\rCONFIG:\n\r         No. of NODEs enabled    = 1\n\r         No. of NODEs disabled   = 0\n\r         No. of CPUs enabled     = 1\n\r         No. of CPUs disabled    = 0\n\r         Mem enabled             = 64 MB\n\r         Mem disabled            = 0 MB\n\r         No. of RTRs enabled     = 0\n\r         No. of RTRs disabled    = 0\n\r\n\rDIAG RESULTS:\n\r         ALL DIAGS PASSED!\n\r**** End System Configuration and Diagnostics Summary ****\n\r\r\n\r\nSystem Maintenance Menu\r\n\r\n1) Start System\r\n2) Install System Software\r\n3) Run Diagnostics\r\n4) Recover System\r\n5) Enter Command Monitor\r\n\n\rOption? 5\r\nCommand Monitor.  Type "exit" to return to the menu.\n\r>> unsetenv netaddr\r\n>> boot -f bootp()/mdk/cpu.mdk\r\nSetting $netaddr to 192.0.2.15 (from server )\n\rObtaining mdk/cpu.mdk from server \n\r   \x08\x08\x08   \x08\x08\x08   \x08\x08\x081048584   \x08|\x08/\x08-\x08\\\x08|\x08/\x08-\x08\\\x08|\x08/\x08-\x08\\\x08|\x08/\x08-\x08\\\x08|\x08/\x08-\x08\\\x08|\x08/\x08-\x08\\\x08|\x08/\x08-\x08\\\x08|\x08/\x08-\x08\\\x08|\x08/\x08-\x08\\\x08|\x08/\x08-\x08\\\x08|\x08/\x08-\x08\\\x08|\x08/\x08-\x08\\\x08|\x08/\x08-\x08\\\x08|\x08/\x08-\x08\\\x08|\x08/\x08-\x08\\\x08|\x08/\x08-\x08\\\x08|\x08/\x08-\x08\\\x08\x08\x08+7968 entry: 0xa80000000001df60\n\r_check_env: nmiprom clr\r\nBooting Nanos...\r\n.....\r\nMDK built with PROM version 6.32\r\nPROM Version 6.11 installed\r\n\r\n********************************************************************\r\nSGI MDK Version 1.6(256p RtrWAR) built 07:42:00 AM Jul 26, 1999\r\n********************************************************************\r\n\r\nCPU 0: Total no. of CPUs = 1  Total Kernel Memory = a800000000397000\r\nMDK> pcache\r\n.....\r\n\r\n\r\n\r\n\r\nStarting Primary Data Cache test (version 1.1.2). \r\n\r\nSystem Configuration: \r\n\r\n   Number of nodes:      1\r\n   Number of cpus:       1\r\n   Number of routers:    0\r\n\r\nNODE0 CPU0 Pid 1 Primary Data Cache Test Started. \r\nNODE0 CPU0 Pid 1    This test takes about 5 minutes.\r\nNODE0 CPU0 Pid 1 PCACHE Test Passed  \r\nMDK Pcache test run is complete'

REAL_FLAT_NAME_TRANSCRIPT = '\n\n\nIP27 PROM SGI Version 6.11  built 03:19:48 PM Apr 29, 1998\nusing BaseIO nic\nTesting/Initializing memory ...............\t\tDONE\nCopying PROM code to memory ...............\t\tDONE\nDiscovering local IO ......................\t\tDONE\nupdate_klcfg_cpuinfo: Couldn\'t find my structure.\nDiscovering CrayLink connectivity .........\t\t\nLocal hub CrayLink is down.\n*** Local network link down\nDONE\nFound 1 objects (1 hubs, 0 routers) in 1968 usec\nWaiting for peers to complete discovery....\t\tDONE\nNo other nodes present; becoming global master\nGlobal master is /hw/module/0/slot/MotherBoard\nTesting/Initializing all memory ...........\t\tDONE\nChecking partitioning information .........\t\tDONE\nNo other nodes present; becoming partition master\nLoading BASEIO prom .......................\t\tDONE\n\nBASEIO PROM Monitor SGI Version 6.11  built 03:18:44 PM Apr 29, 1998 (BE64)\n1 CPUs on 1 nodes found.\n***************************************************************\n*  Some module(s) did not have module id(s). New module id(s) *\n*               assigned by default. Resetting system.        *\n* You can assign moduleids using the module command at the POD*\n***************************************************************\nResetting the system...\n\n\nIP27 PROM SGI Version 6.11  built 03:19:48 PM Apr 29, 1998\nusing BaseIO nic\nTesting/Initializing memory ...............\t\tDONE\nCopying PROM code to memory ...............\t\tDONE\nDiscovering local IO ......................\t\tDONE\nupdate_klcfg_cpuinfo: Couldn\'t find my structure.\nDiscovering CrayLink connectivity .........\t\t\nLocal hub CrayLink is down.\n*** Local network link down\nDONE\nFound 1 objects (1 hubs, 0 routers) in 2211 usec\nWaiting for peers to complete discovery....\t\tDONE\nNo other nodes present; becoming global master\nGlobal master is /hw/module/1/slot/MotherBoard\nTesting/Initializing all memory ...........\t\tDONE\nChecking partitioning information .........\t\tDONE\nNo other nodes present; becoming partition master\nLoading BASEIO prom .......................\t\tDONE\n\nBASEIO PROM Monitor SGI Version 6.11  built 03:18:44 PM Apr 29, 1998 (BE64)\n1 CPUs on 1 nodes found.\n\nNVRAM checksum is incorrect: reinitializing.\nInstalling PROM Device drivers ............             \n\nWalking SCSI Adapter 0 (/hw/module/1/slot/MotherBoard), (pci id 0)\n1- 2- 3- 4- 5- 6- 7- 8- 9- 10- 11- 12- 13- 14- 15- = 0 device(s)\n\nWalking SCSI Adapter 1 (/hw/module/1/slot/MotherBoard), (pci id 1)\n1- 2- 3- 4- 5- 6- 7- 8- 9- 10- 11- 12- 13- 14- 15- = 0 device(s)\nInitializing PROM Device drivers ..........             DONE\nChecking hardware inventory ...............              \n\nWARNING: hardware inventory is invalid.  Reinitializing...\n\nUpdated new configuration. Wrote 2 records.\n\n\n**** System Configuration and Diagnostics Summary ****\n\nCONFIG:\n\n         No. of NODEs enabled    = 1\n\n         No. of NODEs disabled   = 0\n\n         No. of CPUs enabled     = 1\n\n         No. of CPUs disabled    = 0\n\n         Mem enabled             = 64 MB\n\n         Mem disabled            = 0 MB\n\n         No. of RTRs enabled     = 0\n\n         No. of RTRs disabled    = 0\n\n\nDIAG RESULTS:\n\n         ALL DIAGS PASSED!\n\n**** End System Configuration and Diagnostics Summary ****\n\n\n\nSystem Maintenance Menu\n\n1) Start System\n2) Install System Software\n3) Run Diagnostics\n4) Recover System\n5) Enter Command Monitor\n\n\nOption? 5\nCommand Monitor.  Type "exit" to return to the menu.\n\n>> unsetenv netaddr\n>> boot -f bootp()/cpu.mdk\nSetting $netaddr to 192.0.2.15 (from server )\n\nObtaining cpu.mdk from server \n\n   \x08\x08\x08   \x08\x08\x08   \x08\x08\x081048584   \x08|\x08/\x08-\x08\\\x08|\x08/\x08-\x08\\\x08|\x08/\x08-\x08\\\x08|\x08/\x08-\x08\\\x08|\x08/\x08-\x08\\\x08|\x08/\x08-\x08\\\x08|\x08/\x08-\x08\\\x08|\x08/\x08-\x08\\\x08|\x08/\x08-\x08\\\x08|\x08/\x08-\x08\\\x08|\x08/\x08-\x08\\\x08|\x08/\x08-\x08\\\x08|\x08/\x08-\x08\\\x08|\x08/\x08-\x08\\\x08|\x08/\x08-\x08\\\x08|\x08/\x08-\x08\\\x08|\x08/\x08-\x08\\\x08\x08\x08+7968 entry: 0xa80000000001df60\n\n_check_env: nmiprom clr\nBooting Nanos...\n.....\nMDK built with PROM version 6.32\nPROM Version 6.11 installed\n\n********************************************************************\nSGI MDK Version 1.6(256p RtrWAR) built 07:42:00 AM Jul 26, 1999\n********************************************************************\n\nCPU 0: Total no. of CPUs = 1  Total Kernel Memory = a800000000397000\nMDK> ls\n\npcache\ninstgen\n\nMDK> pcache\n.....\n\n\n\n\nStarting Primary Data Cache test (version 1.1.2). \n\nSystem Configuration: \n\n   Number of nodes:      1\n   Number of cpus:       1\n   Number of routers:    0\n\nNODE0 CPU0 Pid 1 Primary Data Cache Test Started. \nNODE0 CPU0 Pid 1    This test takes about 5 minutes.\nNODE0 CPU0 Pid 1 PCACHE Test Passed  \nMDK Pcache test run is complete. \nMDK> instgen\n.....\n\n\nStarting Random Instruction Generation Test (version 2.0.1). \n\n\nSystem Configuration: \n\n   Number of nodes:      1\n   Number of cpus:       1\n   Number of routers:    0\n\n\nTable of module, nasid, and slot numbers: \n\n     Module      Nasid      Node      Router \n                            Slot       slot  \n     ------      -----      ----      ------ \n        1          0          1          0 \n\n\nRandom Instruction Generation test parameters: \n\n   Num_Cpus_To_Run              =  1 \n   Start Pass                   =  1 \n   End Pass                     =  30000 \n   Global Table Ptr             =  f36000      a800000000413000 \n   Trace  Table Ptr             =  f3a000      a800000000417000 \n   Instr Def Ptr                =  f103a0      a8000000003db3a0 \n  mem_alloc_code_data_address[ 0]           =          17da000 \n  mem_alloc_code_data_address physical addr = a800000000cb7000 \n  mem_alloc_code_data_length[ 0]            =          1000000 \nSysRegUsrExc: pid = 1 type = 15\nMDK> '

REAL_NEVER_REACHED_PREFIX = "\r\n\r\nIP27 PROM SGI Version 6.11  built 03:19:48 PM Apr 29, 1998\r\nusing BaseIO nic\r\nTesting/Initializing memory ...............\t\tDONE\r\nCopying PROM code to memory ...............\t\tDONE\r\nDiscovering local IO ......................\t\tDONE\r\nupdate_klcfg_cpuinfo: Couldn't find my structure.\r\nDiscovering CrayLink connectivity .........\t\t\r\nLocal hub CrayLink is down.\r\n*** Local network link down\r\nDONE\r\nFound 1 objects (1 hubs, 0 routers) in 3408 usec\r\nWaiting for peers to complete discovery....\t\tDONE\r\nNo other nodes present; becoming global master\r\nGlobal master is /hw/module/0/slot/MotherBoard\r\nTesting/Initializing all memory ...........\t\tDONE\r\nChecking partitioning information .........\t\tDONE\r\nNo other nodes present; becoming partition master\r\n"

REAL_KERNEL_WRECK_SLICE = 'dstack 2\r\nPageMgrVirtToPhy: pid = 2 virtAddr = 0\r\nPageMgrVirtToPhy: Failure: No sec. lvl. ptbl or PCB\r\nPageMgrVirtToPhy: pid = 2 virtAddr = 0\r\nPageMgrVirtToPhy: Failure: No sec. lvl. ptbl or PCB\r\n   Stack Base: 0x0000000000000000 (0x0000000000000000)\r\nStack Pointer: 0x00000000000000000 (0x0000000000000000)\r\n\r\nError, Unexpected TLB Invalid general exception\r\n=======================================================\r\n Unexpected exception occured.                         \r\n         Eframe Registers on Stack                     \r\n=======================================================\r\n  PID     = 0\r\n  CPUNUM  = 0\r\n  NASID   = 0\r\n  LOC CPU = A\r\n  EPC     = 0xa80000000001a73c     BADVADDR= 0x        e0000000\r\n  CAUSE   = 0x            8008     STATUS  = 0x        b40004e3\r\n  XCTXT   = 0xfffffffffff00000     ENHI    = 0xc0000fffe0000002\r\n  ENLO0   = 0x               0     ENLO1   = 0x               0\r\n  COUNT   = 0x        91b36c7c     COMPARE = 0x         c800000\r\n  AT/r1   = 0x              18\r\n  V0/r2   = 0x               0     V1/r3   = 0x              20\r\n  A0/r4   = 0xa800000000280000     A1/r5   = 0xa800000000280000\r\n  A2/r6   = 0x               a     A3/r7   = 0x               3\r\n  A4/r8   = 0x               0     A5/r9   = 0x              30\r\n  A6/r10  = 0x               0     A7/r11  = 0x              30\r\n  T0/r12  = 0x               0     T1/r13  = 0x               1\r\n  T2/r14  = 0xa8000000003853a8     T3/r15  = 0xa800000000385400\r\n  S0/r16  = 0xa8000000000442b0     S1/r17  = 0x               0\r\n  S2/r18  = 0xffffffffffffffff     S3/r19  = 0x40000f0000000068\r\n  S4/r20  = 0xffffffffb40000e0     S5/r21  = 0x               0\r\n  S6/r22  = 0x     fffffffffff     S7/r23  = 0x              80\r\n  T8/r24  = 0x          115280     T9/r25  = 0xa800000000026e58\r\n  SP/r29  = 0xa800000000385418     GP/r28  = 0x               0\r\n  RA/r31  = 0xa80000000001a730\r\n\r\n  FPSR    = 0xc0000fffe0000002\r\n  FP0     = 0x               0     FP1     = 0x               0\r\n  FP2     = 0x               0     FP3     = 0x               0\r\n  FP4     = 0x               0     FP5     = 0x               0\r\n  FP6     = 0x               0     FP7     = 0x               0\r\n  FP8     = 0x               0     FP9     = 0x               0\r\n  FP10    = 0x               0     FP11    = 0x               0\r\n  FP12    = 0x               0     FP13    = 0x               0\r\n  FP14    = 0x               0     FP15    = 0x               0\r\n  FP16    = 0x               0     FP17    = 0x               0\r\n  FP18    = 0x               0     FP19    = 0x               0\r\n  FP20    = 0x               0     FP21    = 0x               0\r\n  FP22    = 0x               0     FP23    = 0x               0\r\n  FP24    = 0x               0     FP25    = 0x               0\r\n  FP26    = 0x               0     FP27    = 0x               0\r\n  FP28    = 0x               0     FP29    = 0x               0\r\n  FP30    = 0x               0     FP31    = 0x               0\r\n=======================================================\r\nMDK>'


class MissingAssertionTests(unittest.TestCase):
    def test_the_real_pass_transcript_misses_nothing(self):
        self.assertEqual(missing_assertions(REAL_MDK_PASS_TRANSCRIPT), [])

    def test_a_flat_netboot_misses_exactly_the_path_assertion(self):
        self.assertEqual(missing_assertions(REAL_FLAT_NAME_TRANSCRIPT),
                         ["path-structured netboot fetch"])

    def test_an_early_death_misses_every_kernel_line(self):
        missing = missing_assertions(REAL_NEVER_REACHED_PREFIX)
        for name, _ in ASSERTIONS:
            self.assertIn(name, missing)


class FailureMarkerTests(unittest.TestCase):
    def test_the_real_transcripts_carry_no_failure_marker(self):
        self.assertEqual(
            failure_markers_present(REAL_MDK_PASS_TRANSCRIPT), [])
        self.assertEqual(
            failure_markers_present(REAL_FLAT_NAME_TRANSCRIPT), [])

    def test_the_kernel_wreck_carries_its_marker(self):
        self.assertEqual(
            failure_markers_present(REAL_KERNEL_WRECK_SLICE),
            ["Unexpected exception"])


class VerdictTests(unittest.TestCase):
    def test_the_real_pass_transcript_passes(self):
        report = verdict(REAL_MDK_PASS_TRANSCRIPT, reached=True)
        self.assertEqual(report["verdict"], "PASS")
        self.assertEqual(report["problems"], [])

    def test_a_flat_netboot_fails_on_the_path_assertion_alone(self):
        report = verdict(REAL_FLAT_NAME_TRANSCRIPT, reached=True)
        self.assertEqual(report["verdict"], "FAIL")
        self.assertEqual(report["problems"],
                         ["missing path-structured netboot fetch"])

    def test_an_unreached_completion_is_named_even_on_a_pass_log(self):
        report = verdict(REAL_MDK_PASS_TRANSCRIPT, reached=False)
        self.assertEqual(report["verdict"], "FAIL")
        self.assertIn(
            "console driver did not reach the pcache completion line "
            "before timeout -- the run never proved the test finished",
            report["problems"])

    def test_a_kernel_wreck_is_a_failure_beyond_missing_lines(self):
        report = verdict(REAL_KERNEL_WRECK_SLICE, reached=False)
        self.assertEqual(report["verdict"], "FAIL")
        self.assertIn(
            "kernel failure output present: 'Unexpected exception'",
            report["problems"])


class ArtifactIdentityTests(unittest.TestCase):
    def test_wrong_bytes_name_both_mismatches(self):
        with tempfile.TemporaryDirectory() as scratch:
            prom = os.path.join(scratch, "prom")
            mdk = os.path.join(scratch, "cpu.mdk")
            for path in (prom, mdk):
                with open(path, "wb") as handle:
                    handle.write(b"not the recorded bytes")
            problems = artifact_identity_problems(prom, mdk)
        self.assertEqual(problems, [
            "PROM SHA-256 does not match the recorded release",
            "cpu.mdk SHA-256 does not match the recorded extraction",
        ])

    def test_matching_bytes_are_accepted(self):
        # The recorded digests belong to restricted assets this test must
        # not embed, so the acceptance side is proven against whatever
        # real assets are present, and skipped without them.
        assets_root = os.environ.get("ORIGAMI_ASSETS")
        if True:
            if not assets_root:
                self.skipTest("ORIGAMI_ASSETS is not set; the acceptance "
                              "side needs the real assets")
            prom = os.path.join(assets_root, "assets", "firmware",
                                "6.5-base", "ip27prom.payload.bin")
            mdk = os.path.join(assets_root, "assets", "boot", "mdk",
                               "cpu.mdk")
            if not (os.path.isfile(prom) and os.path.isfile(mdk)):
                self.skipTest("the real PROM or cpu.mdk is absent")
            self.assertEqual(artifact_identity_problems(prom, mdk), [])


if __name__ == "__main__":
    unittest.main()
