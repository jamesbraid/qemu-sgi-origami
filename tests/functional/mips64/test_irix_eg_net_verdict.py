#!/usr/bin/env python3
#
# Pure PASS/FAIL logic for the IRIX eg (Alteon Tigon) network-transfer
# functional test, split out so it can be proven without booting anything
#
# SPDX-License-Identifier: GPL-2.0-or-later
"""
eg_net_verdict() is the whole success contract of test_irix_eg_net.py,
factored into one pure function the same way test_openbsd_net_transfer.py
factors net_transfer_verdict(): running this file directly
(`python test_irix_eg_net_verdict.py`, as mips64/meson.build's
tests_mips64_unit group does, or under pytest) exercises the verdict on
synthetic inputs with no qemu_test import in the way, so the logic that
decides a real boot's outcome is itself tested without spending a boot.

Why each clause matters:

- login_reached / eg_up: a transfer that never got past boot or interface
  configuration must not be scored on its (absent) digest -- it failed for
  a reason upstream of the wire, and saying so names the right thing to
  look at.

- The digest AND the byte count, both sides: two empty files share a
  digest, so a "successful" transfer of nothing matches on the hash alone;
  the byte count is what catches it (the same reasoning
  test_openbsd_net_transfer's two_values() keeps). The host computes its
  own digest over the bytes it served before the guest ever fetched them,
  so a guest digest that matches cannot have matched by accident.

- rx_ipkts_after > rx_ipkts_before: the point of this whole test. The eg
  receive path is exactly what the fork's receive-return end-of-frame fix
  repairs -- IRIX's eg silently frees any received frame whose return
  descriptor lacks the end-of-frame flag, incrementing no counter, which
  reads downstream as a working transmit and a dead receive. So the
  interface's own received-packet counter climbing over the transfer is
  the device-level proof the frames actually traversed eg's receive ring,
  not merely that a file with the right bytes appeared by some other path.
  A byte-exact download with eg's Ipkts still at zero would mean the bytes
  came in over a different interface, and is failed here on purpose.
"""

import unittest


def eg_net_verdict(login_reached, eg_up, transfer, rx):
    """Decide PASS/FAIL for one IRIX eg transfer run.

    login_reached, eg_up: booleans for the two upstream milestones.

    transfer: a dict describing the TFTP download leg --
        {"attempted": bool,
         "reason": str or None,          # why it was not attempted/finished
         "host_cksum": str,              # POSIX cksum CRC of the served file
         "host_bytes": int,              # size of the served file
         "guest_cksum": str or None,     # cksum the guest reported on its copy
         "guest_bytes": int or None}     # size the guest reported on its copy

    rx: {"ipkts_before": int or None, "ipkts_after": int or None} -- the eg
        interface's received-packet counter before and after the transfer.

    Returns {"verdict": "PASS"|"FAIL", "problems": [str, ...]}. PASS iff
    every clause holds; problems names every clause that did not, so a
    single run reports all of what went wrong rather than only the first.
    """
    problems = []

    if not login_reached:
        problems.append("the guest never reached a root shell")
    if not eg_up:
        problems.append("the eg interface never came up configured")

    if not transfer.get("attempted"):
        problems.append(
            "the transfer was not attempted: "
            + (transfer.get("reason") or "no reason recorded"))
    else:
        if transfer.get("reason") is not None:
            problems.append(
                "the completed transfer still records a failure reason: "
                + str(transfer["reason"]))
        gc = transfer.get("guest_cksum")
        gb = transfer.get("guest_bytes")
        hc = transfer.get("host_cksum")
        hb = transfer.get("host_bytes")
        if gc is None or gb is None:
            problems.append(
                "the guest never reported a checksum and size for its copy")
        else:
            if gb != hb:
                problems.append(
                    f"size mismatch: the guest received {gb} bytes, "
                    f"the host served {hb}")
            if gc != hc:
                problems.append(
                    f"checksum mismatch: the guest computed {gc}, "
                    f"the host computed {hc}")

    before = rx.get("ipkts_before")
    after = rx.get("ipkts_after")
    if after is None:
        problems.append(
            "the eg received-packet counter could not be read after the "
            "transfer")
    elif before is not None and after <= before:
        problems.append(
            f"the eg received-packet counter did not climb over the "
            f"transfer ({before} -> {after}): the bytes did not arrive over "
            f"eg's receive ring")
    elif before is None and after <= 0:
        problems.append(
            "the eg received no packets at all (Ipkts is zero after the "
            "transfer)")

    return {
        "verdict": "PASS" if not problems else "FAIL",
        "problems": problems,
    }


# A transfer leg that passes every clause, copied and mutated per test so a
# single wrong field is what fails a case, not a hand-built dict per test.
def _good_transfer():
    return {"attempted": True, "reason": None,
            "host_cksum": "855463634", "host_bytes": 131072,
            "guest_cksum": "855463634", "guest_bytes": 131072}


class EgNetVerdictTest(unittest.TestCase):
    def test_all_clauses_hold_is_pass(self):
        r = eg_net_verdict(True, True, _good_transfer(),
                           {"ipkts_before": 2, "ipkts_after": 260})
        self.assertEqual(r["verdict"], "PASS")
        self.assertEqual(r["problems"], [])

    def test_no_login_fails(self):
        r = eg_net_verdict(False, True, _good_transfer(),
                           {"ipkts_before": 2, "ipkts_after": 260})
        self.assertEqual(r["verdict"], "FAIL")
        self.assertTrue(any("root shell" in p for p in r["problems"]))

    def test_eg_not_up_fails(self):
        r = eg_net_verdict(True, False, _good_transfer(),
                           {"ipkts_before": 2, "ipkts_after": 260})
        self.assertEqual(r["verdict"], "FAIL")
        self.assertTrue(any("never came up" in p for p in r["problems"]))

    def test_transfer_not_attempted_reports_reason(self):
        leg = {"attempted": False, "reason": "the guest has no tftp",
               "host_cksum": "1", "host_bytes": 10}
        r = eg_net_verdict(True, True, leg,
                           {"ipkts_before": 0, "ipkts_after": 5})
        self.assertEqual(r["verdict"], "FAIL")
        self.assertTrue(any("no tftp" in p for p in r["problems"]))

    def test_completed_transfer_with_stale_reason_fails(self):
        leg = _good_transfer()
        leg["reason"] = "not reached"
        r = eg_net_verdict(True, True, leg,
                           {"ipkts_before": 0, "ipkts_after": 5})
        self.assertEqual(r["verdict"], "FAIL")
        self.assertTrue(any("failure reason" in p for p in r["problems"]))

    def test_size_mismatch_fails(self):
        leg = _good_transfer()
        leg["guest_bytes"] = 0
        r = eg_net_verdict(True, True, leg,
                           {"ipkts_before": 2, "ipkts_after": 260})
        self.assertEqual(r["verdict"], "FAIL")
        self.assertTrue(any("size mismatch" in p for p in r["problems"]))

    def test_checksum_mismatch_fails(self):
        leg = _good_transfer()
        leg["guest_cksum"] = "999"
        r = eg_net_verdict(True, True, leg,
                           {"ipkts_before": 2, "ipkts_after": 260})
        self.assertEqual(r["verdict"], "FAIL")
        self.assertTrue(any("checksum mismatch" in p for p in r["problems"]))

    def test_ipkts_no_climb_fails(self):
        # bytes matched but the counter never moved: they crossed some other
        # interface, not eg's receive ring.
        r = eg_net_verdict(True, True, _good_transfer(),
                           {"ipkts_before": 5, "ipkts_after": 5})
        self.assertEqual(r["verdict"], "FAIL")
        self.assertTrue(any("did not climb" in p for p in r["problems"]))

    def test_ipkts_zero_with_unknown_baseline_fails(self):
        r = eg_net_verdict(True, True, _good_transfer(),
                           {"ipkts_before": None, "ipkts_after": 0})
        self.assertEqual(r["verdict"], "FAIL")
        self.assertTrue(any("no packets at all" in p for p in r["problems"]))

    def test_ipkts_unreadable_fails(self):
        r = eg_net_verdict(True, True, _good_transfer(),
                           {"ipkts_before": 2, "ipkts_after": None})
        self.assertEqual(r["verdict"], "FAIL")
        self.assertTrue(any("could not be read" in p for p in r["problems"]))

    def test_every_broken_clause_is_reported_at_once(self):
        leg = _good_transfer()
        leg["guest_bytes"] = 0
        leg["guest_cksum"] = "999"
        r = eg_net_verdict(False, False, leg,
                           {"ipkts_before": 5, "ipkts_after": 5})
        self.assertEqual(r["verdict"], "FAIL")
        # login, eg-up, size, checksum, ipkts -> five distinct problems.
        self.assertEqual(len(r["problems"]), 5)


if __name__ == "__main__":
    unittest.main()
