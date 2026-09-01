#!/usr/bin/env python3
#
# Equivalence test: test_xframe_pci's verdict logic checks that stock
# OpenBSD's PCI autoconfiguration reports the emulated xframe adapter as an
# unclaimed device at the PCI slot it was attached to
#
# SPDX-License-Identifier: GPL-2.0-or-later
"""
A plain unittest -- no QEMU, no qemu_test import, runnable with just
`python3 tests/functional/mips64/test_xframe_pci_verdict.py` (or
`uv run --with pytest pytest tests/functional/mips64/test_xframe_pci_verdict.py
-v`). It exercises xframe_pci_verdict(), the pure function test_xframe_pci.py
calls against a captured console transcript, over fixture transcripts that
stand in for a captured guest console log.

test_xframe_pci.py boots the same stock OpenBSD 6.9 GENERIC-IP27 kernel and
installed disk test_openbsd_login.py boots (see that file's own docstring),
with one addition: `-device xframe,addr=4.0,...` puts this fork's emulated
Xframe 10G Ethernet adapter (hw/net/xframe.c) at PCI slot 4 on the machine's
one PCI bus. Stock GENERIC-IP27 carries no driver matching that device (no
`xge` in its config -- OpenBSD's xge(4) is what would claim vendor 0x17d5
product 0x5831; see test_openbsd_xge.py for the future test that boots a
kernel which has it), so OpenBSD's own PCI autoconfiguration announces it
unclaimed instead of attaching a driver to it. xframe_pci_verdict() checks
for exactly that announcement -- proof that the device is really composed
onto the guest's PCI bus through the machine's Bridge, under a real OS boot,
not merely that the emulator accepted the command line.

XFRAME_UNCLAIMED_RE's exact text was derived, not guessed, from three
independent sources that all agree:

  1. OpenBSD's own autoconfiguration path for an unclaimed device
     (refs/openbsd-sgi, project checkout, pinned branch
     origin/sgi-is-alive-at-7.9 -- the earliest SGI-tracking branch that
     checkout carries; no 6.9-specific branch exists there, but this code
     has not changed shape across releases -- see point 2):
     sys/kern/subr_autoconf.c's config_found_sm() calls
     `print(aux, parent->dv_xname)` (pnp = "pci0", never NULL, for a device
     hung directly off the pci0 bus); sys/dev/pci/pci.c's pciprint() then
     prints `pci_devinfo(..., showclass=1)` + `" at pci0"`, then
     `" dev %d function %d"`, and returns UNCONF; config_found_sm() appends
     the literal string " not configured\n". sys/dev/pci/pci_subr.c's
     pci_devinfo(): vendor 0x17d5 and product 0x5831 are both in the
     pcidevs table (see point 3), so pci_devinfo takes the
     `product_namep != NULL` branch (`"\"%s %s\""`, i.e. `"Neterion
     Xframe"`), and since that makes `showclass && product_namep == NULL`
     false regardless of showclass, appends the revision with the plain
     `" rev 0x%02x"` suffix -- no comma, no trailing paren. The composed
     line is `"Neterion Xframe" rev 0x00 at pci0 dev 4 function 0 not
     configured`.
  2. The real captured boot this module's own PASS_TRANSCRIPT below splices
     from (REAL_LOGIN_TRANSCRIPT, test_openbsd_login_verdict.py) already
     shows this exact bare "%s rev 0x%02x" shape, with no comma or paren,
     for two other PCI devices whose product name is likewise known:
     `qlw0 at pci0 dev 0 function 0 "QLogic ISP1020" rev 0x05: ...` and
     `ioc0 at pci0 dev 2 function 0 "SGI IOC3" rev 0x00`. Those lines are
     real bytes from an actual boot of the exact kernel test_xframe_pci.py
     boots, not the 7.9-pinned source read in point 1 -- and they agree
     with it.
  3. sys/dev/pci/pcidevs in that same tree names vendor 0x17d5 NETERION
     "Neterion" and product 0x5831 (under NETERION) XFRAME "Xframe". Cross-
     checked directly against the actual compiled kernel these tests boot
     (assets/openbsd/6.9/bsd.IP27, read with `strings`/a raw byte search,
     not merely the source): that binary contains the literal strings
     "Neterion", "Xframe", "Xframe II", and the pciprint()/pci_devinfo()
     format strings named in point 1 ("%s at %s", " dev %d function %d",
     " not configured", "\"%s %s\""), so this kernel really does resolve
     those names rather than falling back to raw hex IDs.

hw/net/xframe.c (this fork's own device model, read here for its public PCI
identity only -- not touched, and no vendor/IRIX material was read to write
this module) sets vendor_id = XFRAME_PCI_VENDOR_ID (0x17d5) and device_id =
XFRAME_PCI_DEVICE_ID_XENA (0x5831) in xframe_class_init(), matching the
pcidevs entries above, and never sets PCIDeviceClass.revision, which
defaults to 0 -- hw/pci/pci.c's pci_qdev_realize() writes that default
through pci_config_set_revision(), so the revision byte OpenBSD reads is
0x00. The regex below still accepts any two hex digits there rather than
pinning 0x00: the revision is incidental to what this test proves (the
device is composed onto the bus and correctly identified), not part of the
contract, and pinning it would make this test fail on a future device-model
change that has nothing to do with PCI composition.

Fixture provenance:

  PASS_TRANSCRIPT
      REAL_LOGIN_TRANSCRIPT's own real captured prefix (copied byte for
      byte from test_openbsd_login_verdict.py, from the very start of that
      transcript through its own "dsrtc0 at ioc0: DS1742W" line -- a real
      boot of the stock kernel and disk, WITHOUT xframe on its command
      line), with one line spliced in right after it: XFRAME_UNCLAIMED_LINE,
      which is NOT captured -- no real boot with the emulated xframe
      attached was available when this was written (this module's own
      docstring above is the derivation) -- followed by that same real
      transcript's own real continuation (through its "swap device
      (default sd0b): " line), unmodified. Only the one spliced line is
      constructed; everything around it is real bytes from an actual boot,
      the same splicing test_openbsd_login_verdict.py's own
      SYNTHETIC_DDB_WEDGE_TRANSCRIPT already does (see that module's own
      docstring) -- there from two real captures, here from one real
      capture plus one derived line.

  NO_XFRAME_TRANSCRIPT
      The same real prefix and continuation, concatenated with nothing
      spliced in between -- real, unmodified bytes, standing in for a boot
      where the device genuinely never showed up on the bus (wrong command
      line, or the Bridge never composed it).

  WRONG_SLOT_TRANSCRIPT
      PASS_TRANSCRIPT with XFRAME_UNCLAIMED_LINE's own "dev 4" changed to
      "dev 5" -- a hand-modified variant, not a second real capture --
      proving the verdict is pinned to the requested slot rather than
      matching an xframe device announced anywhere on the bus.
"""

import re
import unittest


# A run that shows any of these before the xframe line is reached is a
# wedge, not a clean PCI probe. Same three patterns
# test_openbsd_login_verdict.py's own TRIPWIRES checks (see that module's
# own docstring for where each comes from); reproduced here rather than
# imported -- the same duplication test_openbsd_net_transfer_verdict.py's
# own TRIPWIRES already makes of the identical three patterns, each
# console-driving test module in this directory keeping its own copy
# rather than depending on a sibling's.
TRIPWIRES = (
    ("the guest fell into its kernel debugger", re.compile(r"[\r\n]+ddb>")),
    ("the guest panicked", re.compile(r"panic: ")),
    ("the guest took a bus error", re.compile(r"Trap cause = 7")),
)

# The machine's one PCI bus coming up -- proof PCI autoconfiguration ran at
# all, before this module looks for the xframe line specifically.
PCI_BUS_RE = re.compile(r"pci0 at \S+ bus \d+")

# See this module's own docstring for exactly how this text was derived and
# cross-checked. The revision byte is left as any two hex digits
# deliberately (see the docstring); everything else is pinned exactly.
XFRAME_UNCLAIMED_RE = re.compile(
    r'"Neterion Xframe" rev 0x[0-9a-fA-F]{2} at pci0 dev 4 function 0 '
    r'not configured')


def xframe_pci_verdict(transcript):
    """PASS iff transcript shows, in order: the guest's PCI bus
    enumerated (PCI_BUS_RE) and then the xframe adapter reported
    unclaimed at that bus, PCI slot 4 function 0 (XFRAME_UNCLAIMED_RE) --
    and no TRIPWIRE fires before that unclaimed-device line is reached
    (or anywhere in the transcript, if it is never reached).

    Returns {"verdict": "PASS"|"FAIL", "problems": [...]}.
    """
    bus_m = PCI_BUS_RE.search(transcript)
    xframe_m = (XFRAME_UNCLAIMED_RE.search(transcript, bus_m.end())
                if bus_m else None)

    trip_end = xframe_m.start() if xframe_m else len(transcript)
    problems = [why for why, rx in TRIPWIRES
               if rx.search(transcript, 0, trip_end)]

    if bus_m is None:
        problems.append(
            'never reached PCI bus enumeration (no "pci0 at ... bus ..." '
            'line)')
    elif xframe_m is None:
        problems.append(
            'never saw the xframe adapter reported unclaimed at pci0 dev '
            '4 function 0 -- expected a line naming vendor 0x17d5 '
            '("Neterion") product 0x5831 ("Xframe") as "not configured"')

    return {
        "verdict": "PASS" if not problems else "FAIL",
        "problems": problems,
    }


# ---------------------------------------------------------------------------
# Fixtures. See this module's own docstring for provenance.

PASS_TRANSCRIPT = """\
qemu-system-mips64: warning: nic sgi-ioc3-eth.0 has no peer
ARCS64 Firmware\r
Found SGI-IP27, setting up.\r
Machine is in M mode.\r
Initial setup done, switching console.
\rCopyright (c) 1982, 1986, 1989, 1991, 1993
\r\x09The Regents of the University of California.  All rights reserved.
\rCopyright (c) 1995-2021 OpenBSD. All rights reserved.  https://www.OpenBSD.org
\r
\rOpenBSD 6.9 (GENERIC-IP27) #0: Thu Apr 22 03:53:59 UTC 2021
\r    visa@sgi.invalid:/usr/src/sys/arch/sgi/compile/GENERIC-IP27
\rreal mem = 134217728 (128MB)
\rrsvd mem = 14221312 (14MB)
\ravail mem = 118177792 (112MB)
\rwarning: no entropy supplied by boot loader
\rrandom: boothowto does not indicate good seed
\rmainbus0 at root: Origin 200
\rcpu0 at mainbus0 nasid 0: MIPS R10000 CPU rev 0.0 180 MHz, R10000 FPU rev 0.0
\rcpu0: cache L1-I 32KB D 32KB 2 way, L2 1024KB 2 way
\rclock0 at mainbus0 nasid 0: int 5
\rsgi_hub_led CPU 0: 0x01 (progress: initializing the CPU)
sgi_hub_led CPU 0: 0x01 (progress: initializing the CPU)
xbow0 at mainbus0 nasid 0: Bridge revision 3
\rxbridge0 at xbow0 widget 8: Bridge revision 3
\rxbpci0 at xbridge0 bus 0: 33 MHz PCI bus
\rpci0 at xbpci0 bus 0
\rqlw0 at pci0 dev 0 function 0 \"QLogic ISP1020\" rev 0x05: irq 0, xbow irq 62
\rqlw0: nvram corrupt
\rqlw0: firmware rev 1.0.0, attrs 0x0
\rscsibus0 at qlw0: 16 targets, initiator 0
\rsd0 at scsibus0 targ 1 lun 0: <QEMU, QEMU HARDDISK, 2.5+>
\rsd0: 2048MB, 512 bytes/sector, 4194304 sectors
\rqlw1 at pci0 dev 1 function 0 \"QLogic ISP1020\" rev 0x05: irq 1, xbow irq 61
\rqlw1: nvram corrupt
\rqlw1: firmware rev 1.0.0, attrs 0x0
\rscsibus1 at qlw1: 16 targets, initiator 0
\rioc0 at pci0 dev 2 function 0 \"SGI IOC3\" rev 0x00
\ronewire0 at ioc0
\rowmac0 at onewire0 family 0x11 sn 5e7000000001
\rowmac0: Ethernet Address 08:00:69:12:34:56
\rioc0: ethernet irq 2, xbow irq 60
\rioc0: superio irq 3, xbow irq 59
\rcom0 at ioc0 base 0x20178: ns16550a, 16 byte fifo
\rcom0: console
\rcom1 at ioc0 base 0x20170: ns16550a, 16 byte fifo
\riockbc0 at ioc0
\riec0 at ioc0: 128KB SSRAM, address 08:00:69:12:34:56
\rnsphy0 at iec0 phy 31: DP83840 10/100 PHY, rev. 1
\rlpt at ioc0 not configured
\rdsrtc0 at ioc0: DS1742W
\r\"Neterion Xframe\" rev 0x00 at pci0 dev 4 function 0 not configured
\r/dev/ksyms: Symbol table not valid.
\rvscsi0 at root
\rscsibus2 at vscsi0: 256 targets
\rsoftraid0 at root
\rscsibus3 at softraid0: 256 targets
\rboot device: 'unknown' unrecognized.
\rroot device: sd0a
\rswap device (default sd0b): 
"""

NO_XFRAME_TRANSCRIPT = """\
qemu-system-mips64: warning: nic sgi-ioc3-eth.0 has no peer
ARCS64 Firmware\r
Found SGI-IP27, setting up.\r
Machine is in M mode.\r
Initial setup done, switching console.
\rCopyright (c) 1982, 1986, 1989, 1991, 1993
\r\x09The Regents of the University of California.  All rights reserved.
\rCopyright (c) 1995-2021 OpenBSD. All rights reserved.  https://www.OpenBSD.org
\r
\rOpenBSD 6.9 (GENERIC-IP27) #0: Thu Apr 22 03:53:59 UTC 2021
\r    visa@sgi.invalid:/usr/src/sys/arch/sgi/compile/GENERIC-IP27
\rreal mem = 134217728 (128MB)
\rrsvd mem = 14221312 (14MB)
\ravail mem = 118177792 (112MB)
\rwarning: no entropy supplied by boot loader
\rrandom: boothowto does not indicate good seed
\rmainbus0 at root: Origin 200
\rcpu0 at mainbus0 nasid 0: MIPS R10000 CPU rev 0.0 180 MHz, R10000 FPU rev 0.0
\rcpu0: cache L1-I 32KB D 32KB 2 way, L2 1024KB 2 way
\rclock0 at mainbus0 nasid 0: int 5
\rsgi_hub_led CPU 0: 0x01 (progress: initializing the CPU)
sgi_hub_led CPU 0: 0x01 (progress: initializing the CPU)
xbow0 at mainbus0 nasid 0: Bridge revision 3
\rxbridge0 at xbow0 widget 8: Bridge revision 3
\rxbpci0 at xbridge0 bus 0: 33 MHz PCI bus
\rpci0 at xbpci0 bus 0
\rqlw0 at pci0 dev 0 function 0 \"QLogic ISP1020\" rev 0x05: irq 0, xbow irq 62
\rqlw0: nvram corrupt
\rqlw0: firmware rev 1.0.0, attrs 0x0
\rscsibus0 at qlw0: 16 targets, initiator 0
\rsd0 at scsibus0 targ 1 lun 0: <QEMU, QEMU HARDDISK, 2.5+>
\rsd0: 2048MB, 512 bytes/sector, 4194304 sectors
\rqlw1 at pci0 dev 1 function 0 \"QLogic ISP1020\" rev 0x05: irq 1, xbow irq 61
\rqlw1: nvram corrupt
\rqlw1: firmware rev 1.0.0, attrs 0x0
\rscsibus1 at qlw1: 16 targets, initiator 0
\rioc0 at pci0 dev 2 function 0 \"SGI IOC3\" rev 0x00
\ronewire0 at ioc0
\rowmac0 at onewire0 family 0x11 sn 5e7000000001
\rowmac0: Ethernet Address 08:00:69:12:34:56
\rioc0: ethernet irq 2, xbow irq 60
\rioc0: superio irq 3, xbow irq 59
\rcom0 at ioc0 base 0x20178: ns16550a, 16 byte fifo
\rcom0: console
\rcom1 at ioc0 base 0x20170: ns16550a, 16 byte fifo
\riockbc0 at ioc0
\riec0 at ioc0: 128KB SSRAM, address 08:00:69:12:34:56
\rnsphy0 at iec0 phy 31: DP83840 10/100 PHY, rev. 1
\rlpt at ioc0 not configured
\rdsrtc0 at ioc0: DS1742W
\r/dev/ksyms: Symbol table not valid.
\rvscsi0 at root
\rscsibus2 at vscsi0: 256 targets
\rsoftraid0 at root
\rscsibus3 at softraid0: 256 targets
\rboot device: 'unknown' unrecognized.
\rroot device: sd0a
\rswap device (default sd0b): 
"""

WRONG_SLOT_TRANSCRIPT = """\
qemu-system-mips64: warning: nic sgi-ioc3-eth.0 has no peer
ARCS64 Firmware\r
Found SGI-IP27, setting up.\r
Machine is in M mode.\r
Initial setup done, switching console.
\rCopyright (c) 1982, 1986, 1989, 1991, 1993
\r\x09The Regents of the University of California.  All rights reserved.
\rCopyright (c) 1995-2021 OpenBSD. All rights reserved.  https://www.OpenBSD.org
\r
\rOpenBSD 6.9 (GENERIC-IP27) #0: Thu Apr 22 03:53:59 UTC 2021
\r    visa@sgi.invalid:/usr/src/sys/arch/sgi/compile/GENERIC-IP27
\rreal mem = 134217728 (128MB)
\rrsvd mem = 14221312 (14MB)
\ravail mem = 118177792 (112MB)
\rwarning: no entropy supplied by boot loader
\rrandom: boothowto does not indicate good seed
\rmainbus0 at root: Origin 200
\rcpu0 at mainbus0 nasid 0: MIPS R10000 CPU rev 0.0 180 MHz, R10000 FPU rev 0.0
\rcpu0: cache L1-I 32KB D 32KB 2 way, L2 1024KB 2 way
\rclock0 at mainbus0 nasid 0: int 5
\rsgi_hub_led CPU 0: 0x01 (progress: initializing the CPU)
sgi_hub_led CPU 0: 0x01 (progress: initializing the CPU)
xbow0 at mainbus0 nasid 0: Bridge revision 3
\rxbridge0 at xbow0 widget 8: Bridge revision 3
\rxbpci0 at xbridge0 bus 0: 33 MHz PCI bus
\rpci0 at xbpci0 bus 0
\rqlw0 at pci0 dev 0 function 0 \"QLogic ISP1020\" rev 0x05: irq 0, xbow irq 62
\rqlw0: nvram corrupt
\rqlw0: firmware rev 1.0.0, attrs 0x0
\rscsibus0 at qlw0: 16 targets, initiator 0
\rsd0 at scsibus0 targ 1 lun 0: <QEMU, QEMU HARDDISK, 2.5+>
\rsd0: 2048MB, 512 bytes/sector, 4194304 sectors
\rqlw1 at pci0 dev 1 function 0 \"QLogic ISP1020\" rev 0x05: irq 1, xbow irq 61
\rqlw1: nvram corrupt
\rqlw1: firmware rev 1.0.0, attrs 0x0
\rscsibus1 at qlw1: 16 targets, initiator 0
\rioc0 at pci0 dev 2 function 0 \"SGI IOC3\" rev 0x00
\ronewire0 at ioc0
\rowmac0 at onewire0 family 0x11 sn 5e7000000001
\rowmac0: Ethernet Address 08:00:69:12:34:56
\rioc0: ethernet irq 2, xbow irq 60
\rioc0: superio irq 3, xbow irq 59
\rcom0 at ioc0 base 0x20178: ns16550a, 16 byte fifo
\rcom0: console
\rcom1 at ioc0 base 0x20170: ns16550a, 16 byte fifo
\riockbc0 at ioc0
\riec0 at ioc0: 128KB SSRAM, address 08:00:69:12:34:56
\rnsphy0 at iec0 phy 31: DP83840 10/100 PHY, rev. 1
\rlpt at ioc0 not configured
\rdsrtc0 at ioc0: DS1742W
\r\"Neterion Xframe\" rev 0x00 at pci0 dev 5 function 0 not configured
\r/dev/ksyms: Symbol table not valid.
\rvscsi0 at root
\rscsibus2 at vscsi0: 256 targets
\rsoftraid0 at root
\rscsibus3 at softraid0: 256 targets
\rboot device: 'unknown' unrecognized.
\rroot device: sd0a
\rswap device (default sd0b): 
"""


class XframePciVerdictTest(unittest.TestCase):

    def test_pass_transcript_passes(self):
        report = xframe_pci_verdict(PASS_TRANSCRIPT)
        self.assertEqual(report["problems"], [])
        self.assertEqual(report["verdict"], "PASS")

    def test_device_absent_fails(self):
        # The real prefix and continuation with nothing spliced in --
        # proof this does not pass just because the rest of a normal PCI
        # probe looks fine.
        self.assertNotIn("Neterion", NO_XFRAME_TRANSCRIPT)
        report = xframe_pci_verdict(NO_XFRAME_TRANSCRIPT)
        self.assertEqual(report["verdict"], "FAIL")
        self.assertIn(
            'never saw the xframe adapter reported unclaimed at pci0 dev '
            '4 function 0 -- expected a line naming vendor 0x17d5 '
            '("Neterion") product 0x5831 ("Xframe") as "not configured"',
            report["problems"])

    def test_wrong_slot_does_not_satisfy_dev_4(self):
        # An xframe reported unclaimed at a DIFFERENT slot must not
        # count -- this test asked for addr=4.0 and must see it there,
        # not merely somewhere on the bus.
        self.assertIn("dev 5 function 0", WRONG_SLOT_TRANSCRIPT)
        self.assertNotIn("dev 4 function 0", WRONG_SLOT_TRANSCRIPT)
        report = xframe_pci_verdict(WRONG_SLOT_TRANSCRIPT)
        self.assertEqual(report["verdict"], "FAIL")
        self.assertIn(
            'never saw the xframe adapter reported unclaimed at pci0 dev '
            '4 function 0 -- expected a line naming vendor 0x17d5 '
            '("Neterion") product 0x5831 ("Xframe") as "not configured"',
            report["problems"])

    def test_never_reaches_pci_bus_fails(self):
        report = xframe_pci_verdict("ARCS64 Firmware\r\nFound SGI-IP27.\r\n")
        self.assertEqual(report["verdict"], "FAIL")
        self.assertEqual(
            report["problems"],
            ['never reached PCI bus enumeration (no "pci0 at ... bus '
             '..." line)'])

    def test_ddb_wedge_before_pci_line_fails(self):
        # Small targeted probe, not a full captured transcript (the same
        # shape test_openbsd_login_verdict.py's own small probes use): a
        # wedge that shows up before the xframe line must fail even
        # though the bus itself did come up.
        transcript = (
            "pci0 at xbpci0 bus 0\r\n"
            "Trap cause = 7 Frame 0xffffffff82b2f9c8\r\nddb> ")
        report = xframe_pci_verdict(transcript)
        self.assertEqual(report["verdict"], "FAIL")
        self.assertIn("the guest took a bus error", report["problems"])
        self.assertIn("the guest fell into its kernel debugger",
                      report["problems"])
        self.assertIn(
            'never saw the xframe adapter reported unclaimed at pci0 dev '
            '4 function 0 -- expected a line naming vendor 0x17d5 '
            '("Neterion") product 0x5831 ("Xframe") as "not configured"',
            report["problems"])

    def test_panic_after_xframe_line_does_not_fail_the_probe(self):
        # A wedge that happens AFTER the xframe line was already reported
        # unclaimed is a real defect worth catching elsewhere, but it
        # must not retroactively un-prove PCI composition that already
        # printed -- mirrors login_verdict's own trip_end reasoning
        # (test_openbsd_login_verdict.py), scoped to before the marker
        # this module cares about instead of before a shell prompt.
        transcript = (
            "pci0 at xbpci0 bus 0\r\n"
            '"Neterion Xframe" rev 0x00 at pci0 dev 4 function 0 not '
            "configured\r\npanic: kernel diagnostic assertion failed\r\n")
        report = xframe_pci_verdict(transcript)
        self.assertEqual(report["verdict"], "PASS")

    def test_bus_line_required_before_xframe_line_counts(self):
        # Ordering matters, the same way login_verdict()'s own root
        # device / login / shell ordering does: an xframe unclaimed line
        # that appears without the pci0 bus line ever having been seen
        # first must not be read as having reached that stage. (Not
        # reachable from a real boot -- the bus line always precedes any
        # per-device line -- but the check exists on its own merits, the
        # same reason login_verdict() keeps its own ordering tests.)
        transcript = (
            '"Neterion Xframe" rev 0x00 at pci0 dev 4 function 0 not '
            "configured\r\npci0 at xbpci0 bus 0\r\n")
        report = xframe_pci_verdict(transcript)
        self.assertEqual(report["verdict"], "FAIL")
        self.assertIn(
            'never saw the xframe adapter reported unclaimed at pci0 dev '
            '4 function 0 -- expected a line naming vendor 0x17d5 '
            '("Neterion") product 0x5831 ("Xframe") as "not configured"',
            report["problems"])


if __name__ == "__main__":
    unittest.main()
