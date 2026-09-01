#!/usr/bin/env python3
#
# Equivalence test: test_openbsd_xge's verdict logic (not yet run against a
# real boot -- see this module's own docstring) checks that xge(4) claims
# the emulated xframe adapter and that a DHCP lease plus a transfer over it
# both succeed
#
# SPDX-License-Identifier: GPL-2.0-or-later
"""
NOT YET EXERCISED AGAINST A REAL BOOT. test_openbsd_xge.py (the paired
driver, see that file's own top-of-file comment) needs an OpenBSD/IP27
kernel built with `xge* at pci?` -- OpenBSD's driver for vendor 0x17d5
product 0x5831, this fork's own emulated Xframe adapter's PCI identity
(hw/net/xframe.c) -- which does not exist as a provisioned asset yet: stock
OpenBSD 6.9's GENERIC-IP27 kernel carries no xge driver at all (proven by
test_xframe_pci.py's own real, passing boot: that kernel reports the same
device "not configured" instead of attaching a driver to it -- see
test_xframe_pci_verdict.py's own docstring). Until that kernel asset is
provisioned, test_openbsd_xge.py's own launch always skipTest()s before
this module's xge_traffic_verdict() is ever called for real.

xge_traffic_verdict() itself is written and tested here exactly as if that
boot already existed -- so that provisioning the asset and pointing
KERNEL_REL/DISK_REL at it in test_openbsd_xge.py is the only change needed
to turn this into a real test, matching test_openbsd_net_transfer_verdict
.py's own precedent (its own docstring: "this port's real boot ... was not
run to completion by the person writing this module, so there is no real
captured transcript of a passing run to copy"). Every fixture below is
therefore hand-built and clearly marked synthetic, the same way that
module's own ALL_LEGS_SUCCEED_FIXTURE is -- none of it is a claim about
what any real run has produced.

xge_traffic_verdict()'s own contract is deliberately patterned on
test_openbsd_net_transfer_verdict.py's own net_transfer_verdict(): a
transcript scanned only for TRIPWIRES (this module keeps its own copy of
the same three patterns, the same duplication every console-driving test
module in this directory already makes -- see test_xframe_pci_verdict.py's
own docstring for that convention), a dhcp_counts dict in the exact shape
summarise_pcap()'s own "dhcp" key returns
(test_openbsd_net_transfer_verdict.py; test_openbsd_xge.py's own real
launch, once it exists, is expected to call that same function on its own
captured pcap the way test_openbsd_net_transfer.py already does, rather
than this module re-deriving DHCP wire parsing), and one transfer leg dict
in the same shape net_transfer_verdict()'s own read/write/inbound leg
dicts use (attempted, host_sha256, host_bytes, guest_sha256, guest_bytes,
exit_code, reason) -- one leg, not three, because test_openbsd_xge.py's
own brief calls for "a transfer" in the singular, proving the adapter
carries a payload correctly rather than re-proving the three-leg shape
net_transfer_verdict() already covers for the onboard iec0 adapter.

What this module adds beyond net_transfer_verdict()'s own contract is the
one check specific to why this test exists at all: XGE_ATTACH_RE requires
a driver -- xge0 -- to have actually claimed the device at the same PCI
identity test_xframe_pci_verdict.py's own XFRAME_UNCLAIMED_RE proves stock
OpenBSD reports unclaimed (vendor 0x17d5 "Neterion" product 0x5831
"Xframe", pci0 dev 4 function 0 -- same slot test_xframe_pci.py's own
command line uses). Passing DHCP and a transfer over some other interface
would not prove xge(4) works; this line is what ties the traffic that
follows to this adapter specifically.
"""

import re
import unittest


# Same three patterns test_xframe_pci_verdict.py's own TRIPWIRES and
# test_openbsd_net_transfer_verdict.py's own TRIPWIRES check (see either
# module's own docstring for where they come from); duplicated here per
# this directory's own convention rather than imported.
TRIPWIRES = (
    ("the guest fell into its kernel debugger", re.compile(r"[\r\n]+ddb>")),
    ("the guest panicked", re.compile(r"panic: ")),
    ("the guest took a bus error", re.compile(r"Trap cause = 7")),
)

# xge(4) actually claiming this fork's emulated xframe adapter at the same
# PCI identity and slot test_xframe_pci_verdict.py's own XFRAME_UNCLAIMED_RE
# proves stock OpenBSD reports unclaimed. The revision byte is left as any
# two hex digits for the same reason that module's own regex does (see its
# own docstring): incidental to what is under test here.
XGE_ATTACH_RE = re.compile(
    r'xge0 at pci0 dev 4 function 0 "Neterion Xframe" rev 0x[0-9a-fA-F]{2}')

# net_transfer_verdict()'s own four-message handshake
# (test_openbsd_net_transfer_verdict.py's own LEASE_HANDSHAKE), reproduced
# here rather than imported for the same reason its own TRIPWIRES is
# reproduced rather than imported.
LEASE_HANDSHAKE = ("DHCPDISCOVER", "DHCPOFFER", "DHCPREQUEST", "DHCPACK")


def _lease_established(dhcp_counts):
    """Same check as test_openbsd_net_transfer_verdict.py's own
    lease_established(): all four DORA messages seen at least once on the
    wire, not merely a guest command's exit code (that module's own
    docstring explains why)."""
    return all(dhcp_counts.get(name, 0) >= 1 for name in LEASE_HANDSHAKE)


def xge_traffic_verdict(transcript, dhcp_counts, transfer):
    """PASS iff, in order: xge0 claims the emulated xframe adapter
    (XGE_ATTACH_RE), the DHCP lease's four-message handshake completed on
    the wire, and the transfer leg was attempted and its digest and byte
    count match. FAIL if the attach line never appears, the lease never
    completes, the leg was never attempted, or its digest/byte
    count/exit code do not match -- each its own distinct problem, never
    collapsed into one "it did not work". A TRIPWIRE anywhere in the
    transcript is also its own problem, the same unconditional scan
    net_transfer_verdict() makes (see that function's own docstring for
    why: a guest that wedged partway through must fail even if, by
    coincidence, the leg dict below still looks complete).

    transcript: the captured guest console text for the whole run.
    dhcp_counts: a dict of DHCP message name -> count, the exact shape
      summarise_pcap()'s own "dhcp" key returns
      (test_openbsd_net_transfer_verdict.py).
    transfer: falsy if never attempted, else a dict with "attempted"
      (bool) and, when attempted, "host_sha256", "host_bytes",
      "guest_sha256", "guest_bytes" -- the same host_*/guest_* naming
      net_transfer_verdict()'s own leg dicts use (named by which side
      measured them, not by which one is "expected" -- see that
      function's own docstring for why). May also carry "exit_code"
      (int or None: present and non-zero fails the leg even if its
      digest and size match) and "reason" (str, used only for the
      problem message when not attempted).

    Returns {"verdict": "PASS"|"FAIL", "problems": [...],
    "driver_attached": bool, "lease_ok": bool, "transfer_ok": bool}.
    """
    problems = [why for why, rx in TRIPWIRES if rx.search(transcript)]

    driver_attached = bool(XGE_ATTACH_RE.search(transcript))
    if not driver_attached:
        problems.append(
            'xge0 never claimed the adapter at pci0 dev 4 function 0 -- '
            'expected a line naming vendor 0x17d5 ("Neterion") product '
            '0x5831 ("Xframe") attached, not merely probed')

    lease_ok = _lease_established(dhcp_counts)
    if not lease_ok:
        seen = sorted(name for name in LEASE_HANDSHAKE if dhcp_counts.get(name))
        problems.append(
            "the DHCP lease was never acquired on xge0 -- its "
            "four-message handshake (DHCPDISCOVER, DHCPOFFER, "
            "DHCPREQUEST, DHCPACK) did not all appear on the wire (saw: "
            + (", ".join(seen) if seen else "nothing") + ")")

    if not transfer or not transfer.get("attempted"):
        reason = (transfer or {}).get("reason")
        problems.append(
            "the guest never reached the transfer leg"
            + (f" ({reason})" if reason else ""))
        transfer_ok = False
    else:
        exit_code = transfer.get("exit_code")
        exit_ok = exit_code is None or exit_code == 0
        digest_ok = transfer["guest_sha256"] == transfer["host_sha256"]
        size_ok = transfer["guest_bytes"] == transfer["host_bytes"]
        if not exit_ok:
            problems.append(
                f"transfer leg: its own command exited {exit_code}, not 0")
        if not digest_ok:
            problems.append(
                "transfer leg: digest mismatch (host measured "
                f"{transfer['host_sha256']}, guest reported "
                f"{transfer['guest_sha256']})")
        if not size_ok:
            problems.append(
                "transfer leg: byte count mismatch (host measured "
                f"{transfer['host_bytes']}, guest reported "
                f"{transfer['guest_bytes']})")
        transfer_ok = exit_ok and digest_ok and size_ok

    return {
        "verdict": "PASS" if not problems else "FAIL",
        "problems": problems,
        "driver_attached": driver_attached,
        "lease_ok": lease_ok,
        "transfer_ok": transfer_ok,
    }


# ---------------------------------------------------------------------------
# Fixtures. ALL HAND-BUILT / SYNTHETIC -- see this module's own docstring:
# no real boot of an xge-capable kernel exists yet to capture one from.

_TRANSFER_HASH = "4" * 64

PASS_TRANSCRIPT = (
    "pci0 at xbpci0 bus 0\r\n"
    'xge0 at pci0 dev 4 function 0 "Neterion Xframe" rev 0x00: '
    "irq 4, xbow irq 58, address 02:00:5d:00:00:01\r\n"
    "login: root\r\nPassword:\r\n"
    "origami# \r\n")

PASS_DHCP_COUNTS = {"DHCPDISCOVER": 1, "DHCPOFFER": 1, "DHCPREQUEST": 1,
                    "DHCPACK": 1}

PASS_TRANSFER = {
    "attempted": True,
    "host_sha256": _TRANSFER_HASH, "host_bytes": 65536,
    "guest_sha256": _TRANSFER_HASH, "guest_bytes": 65536,
    "exit_code": 0,
}

# A small hand-built probe, not a copy of test_xframe_pci_verdict.py's own
# NO_XFRAME_TRANSCRIPT fixture: that module's real-captured fixture belongs
# to it, and this module stays self-contained the same way its own
# TRIPWIRES does (see this module's own docstring) rather than importing a
# sibling's fixture across a verdict-module boundary.
NO_DRIVER_TRANSCRIPT = (
    "pci0 at xbpci0 bus 0\r\n"
    '"Neterion Xframe" rev 0x00 at pci0 dev 4 function 0 not '
    "configured\r\n")


class XgeTrafficVerdictTest(unittest.TestCase):

    def test_pass_fixture_passes(self):
        report = xge_traffic_verdict(
            PASS_TRANSCRIPT, PASS_DHCP_COUNTS, PASS_TRANSFER)
        self.assertEqual(report["problems"], [])
        self.assertEqual(report["verdict"], "PASS")
        self.assertTrue(report["driver_attached"])
        self.assertTrue(report["lease_ok"])
        self.assertTrue(report["transfer_ok"])

    def test_still_unclaimed_fails_the_attach_check(self):
        # The stock-kernel case Test 1 (test_xframe_pci.py) actually
        # proves today: unclaimed, not attached. This module's own
        # verdict must not be satisfied by DHCP/transfer data alone --
        # the whole point of this test is that xge0 claimed the device.
        report = xge_traffic_verdict(
            NO_DRIVER_TRANSCRIPT, PASS_DHCP_COUNTS, PASS_TRANSFER)
        self.assertEqual(report["verdict"], "FAIL")
        self.assertFalse(report["driver_attached"])
        self.assertIn(
            'xge0 never claimed the adapter at pci0 dev 4 function 0 -- '
            'expected a line naming vendor 0x17d5 ("Neterion") product '
            '0x5831 ("Xframe") attached, not merely probed',
            report["problems"])

    def test_incomplete_lease_fails(self):
        incomplete = dict(PASS_DHCP_COUNTS)
        del incomplete["DHCPACK"]
        report = xge_traffic_verdict(
            PASS_TRANSCRIPT, incomplete, PASS_TRANSFER)
        self.assertEqual(report["verdict"], "FAIL")
        self.assertFalse(report["lease_ok"])
        self.assertTrue(any("DHCP lease was never acquired" in p
                            for p in report["problems"]))

    def test_transfer_never_attempted_fails(self):
        report = xge_traffic_verdict(
            PASS_TRANSCRIPT, PASS_DHCP_COUNTS,
            {"attempted": False, "reason": "the guest has no nc"})
        self.assertEqual(report["verdict"], "FAIL")
        self.assertIn(
            "the guest never reached the transfer leg (the guest has "
            "no nc)", report["problems"])

    def test_transfer_digest_mismatch_fails(self):
        bad = dict(PASS_TRANSFER)
        bad["guest_sha256"] = "0" * 64
        report = xge_traffic_verdict(PASS_TRANSCRIPT, PASS_DHCP_COUNTS, bad)
        self.assertEqual(report["verdict"], "FAIL")
        self.assertFalse(report["transfer_ok"])
        self.assertTrue(any("digest mismatch" in p for p in report["problems"]))

    def test_transfer_nonzero_exit_fails_even_with_matching_digest(self):
        bad = dict(PASS_TRANSFER)
        bad["exit_code"] = 1
        report = xge_traffic_verdict(PASS_TRANSCRIPT, PASS_DHCP_COUNTS, bad)
        self.assertEqual(report["verdict"], "FAIL")
        self.assertTrue(any("exited 1, not 0" in p for p in report["problems"]))

    def test_ddb_wedge_anywhere_fails(self):
        wedged = PASS_TRANSCRIPT + "\r\nddb> "
        report = xge_traffic_verdict(wedged, PASS_DHCP_COUNTS, PASS_TRANSFER)
        self.assertEqual(report["verdict"], "FAIL")
        self.assertIn("the guest fell into its kernel debugger",
                      report["problems"])


if __name__ == "__main__":
    unittest.main()
