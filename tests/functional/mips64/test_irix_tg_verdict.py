#!/usr/bin/env python3
#
# Pure pass/fail logic for the tg (bcm570x) IRIX acceptance, split out so it is
# unit-testable off recorded values without a boot -- the same division
# test_irix_eg_net_verdict.py keeps from its boot test.
#
# SPDX-License-Identifier: GPL-2.0-or-later
"""
tg_acceptance_verdict() decides whether the tg Ethernet adapter (the Broadcom
BCM570x, QEMU device "bcm570x") passed traffic end-to-end on IRIX, from values
scraped off the guest's own output. PASS requires ALL of them -- a half-working
adapter cannot pass:

  - the guest reached a root shell (login_reached);
  - tg1 is present in `ifconfig -a` (tg1_present);
  - `ifconfig tg1 ... up` returned to a shell prompt (tg_up) -- it did not wedge
    the console;
  - the driver logged its link-up NOTICE (link_up) -- IRIX's tg driver announces
    "tg1: Link up: 1000 Mbps, FULL duplex" once the PHY negotiates, which is the
    driver's own statement that it will start moving frames;
  - over a burst of pings to the SLIRP gateway, tg1's own interface counters
    move the right way (traffic): received packets climb, transmitted packets
    climb, and input errors stay at zero.

Why the interface counters, not the ping's own summary: over QEMU user-mode
SLIRP the guest ping's line "N packets received" reads 0 even when the frames
did arrive, because SLIRP answers the gateway echo with an ICMP id the guest's
ping does not match back to its request. That is a SLIRP quirk, not a device
fault, so trusting the ping's received count would fail a working adapter. What
is trustworthy is the device's own accounting in `netstat -in`: Opkts climbing
means the adapter's transmit DMA put the echo requests on the wire, and Ipkts
climbing means its receive DMA took the gateway's echo replies (and the ARP
reply) back off it -- the receive path proven by the counter the driver itself
increments, not by a file that appeared. Ierrs staying at zero means those
frames came in clean, not as receive-ring errors. Both directions plus a clean
receive is a complete exercise of the adapter's data path.

This module carries the unit tests for that logic (run without any emulator);
the boot test in test_irix_tg.py imports tg_acceptance_verdict and feeds it the
values it scraped from a real IRIX boot.
"""

import unittest


def tg_acceptance_verdict(login_reached, tg1_present, tg_up, link_up, traffic):
    """Decide PASS/FAIL for one IRIX tg bring-up run.

    login_reached, tg1_present, tg_up, link_up: booleans for the four
    milestones above.

    traffic: a dict of the interface's own counters, read from `netstat -in`
    before and after a burst of pings to the SLIRP gateway --
        {"ipkts_before": int or None, "ipkts_after": int or None,
         "opkts_before": int or None, "opkts_after": int or None,
         "ierrs_after":  int or None}
    A None is a value that could not be read.

    Returns {"verdict": "PASS"|"FAIL", "problems": [str, ...]}. PASS iff every
    clause holds; problems names every clause that did not, so a single run
    reports all of what went wrong rather than only the first.
    """
    problems = []

    if not login_reached:
        problems.append("the guest never reached a root shell")
    if not tg1_present:
        problems.append("tg1 not present in ifconfig -a")
    if not tg_up:
        problems.append(
            "`ifconfig tg1 ... up` did not return to a shell prompt "
            "(console wedged)")
    if not link_up:
        problems.append(
            "the tg driver never logged its link-up notice; the adapter's "
            "link never came up")

    ib = traffic.get("ipkts_before")
    ia = traffic.get("ipkts_after")
    if ia is None or ib is None:
        problems.append(
            "tg1's received-packet counter could not be read before and "
            "after the pings")
    elif ia <= ib:
        problems.append(
            f"tg1's received-packet counter did not climb over the pings "
            f"({ib} -> {ia}): no frames arrived over the adapter's receive "
            f"ring")

    ob = traffic.get("opkts_before")
    oa = traffic.get("opkts_after")
    if oa is None or ob is None:
        problems.append(
            "tg1's transmitted-packet counter could not be read before and "
            "after the pings")
    elif oa <= ob:
        problems.append(
            f"tg1's transmitted-packet counter did not climb over the pings "
            f"({ob} -> {oa}): the adapter put no frames on the wire")

    ie = traffic.get("ierrs_after")
    if ie is None:
        problems.append("tg1's input-error counter could not be read")
    elif ie != 0:
        problems.append(
            f"tg1 reported {ie} input errors; the frames did not arrive clean")

    return {
        "verdict": "PASS" if not problems else "FAIL",
        "problems": problems,
    }


# A traffic reading that passes every clause, copied and mutated per test so a
# single wrong field is what fails a case, not a hand-built dict per test.
def _good_traffic():
    return {"ipkts_before": 4, "ipkts_after": 12,
            "opkts_before": 6, "opkts_after": 15,
            "ierrs_after": 0}


class TgAcceptanceVerdictTest(unittest.TestCase):
    """Unit tests for the pure verdict logic -- no emulator, no boot."""

    def _verdict(self, **over):
        args = dict(login_reached=True, tg1_present=True, tg_up=True,
                    link_up=True, traffic=_good_traffic())
        args.update(over)
        return tg_acceptance_verdict(**args)

    def test_all_good_passes(self):
        r = self._verdict()
        self.assertEqual(r["verdict"], "PASS")
        self.assertEqual(r["problems"], [])

    def test_no_login_fails(self):
        r = self._verdict(login_reached=False)
        self.assertEqual(r["verdict"], "FAIL")
        self.assertTrue(any("root shell" in p for p in r["problems"]))

    def test_missing_tg1_fails(self):
        r = self._verdict(tg1_present=False)
        self.assertEqual(r["verdict"], "FAIL")
        self.assertTrue(any("tg1 not present" in p for p in r["problems"]))

    def test_wedged_up_fails(self):
        r = self._verdict(tg_up=False)
        self.assertEqual(r["verdict"], "FAIL")
        self.assertTrue(any("wedged" in p for p in r["problems"]))

    def test_no_link_up_fails(self):
        r = self._verdict(link_up=False)
        self.assertEqual(r["verdict"], "FAIL")
        self.assertTrue(any("link never came up" in p for p in r["problems"]))

    def test_ipkts_no_climb_fails(self):
        t = _good_traffic()
        t["ipkts_after"] = t["ipkts_before"]
        r = self._verdict(traffic=t)
        self.assertEqual(r["verdict"], "FAIL")
        self.assertTrue(any("received-packet counter did not climb" in p
                            for p in r["problems"]))

    def test_opkts_no_climb_fails(self):
        t = _good_traffic()
        t["opkts_after"] = t["opkts_before"]
        r = self._verdict(traffic=t)
        self.assertEqual(r["verdict"], "FAIL")
        self.assertTrue(any("transmitted-packet counter did not climb" in p
                            for p in r["problems"]))

    def test_ipkts_unreadable_fails(self):
        for missing in ("ipkts_before", "ipkts_after"):
            t = _good_traffic()
            t[missing] = None
            r = self._verdict(traffic=t)
            self.assertEqual(r["verdict"], "FAIL", f"missing={missing}")
            self.assertTrue(any("received-packet counter could not be read" in p
                                for p in r["problems"]))

    def test_opkts_unreadable_fails(self):
        for missing in ("opkts_before", "opkts_after"):
            t = _good_traffic()
            t[missing] = None
            r = self._verdict(traffic=t)
            self.assertEqual(r["verdict"], "FAIL", f"missing={missing}")
            self.assertTrue(any(
                "transmitted-packet counter could not be read" in p
                for p in r["problems"]))

    def test_ierrs_nonzero_fails(self):
        t = _good_traffic()
        t["ierrs_after"] = 3
        r = self._verdict(traffic=t)
        self.assertEqual(r["verdict"], "FAIL")
        self.assertTrue(any("input errors" in p for p in r["problems"]))

    def test_ierrs_unreadable_fails(self):
        t = _good_traffic()
        t["ierrs_after"] = None
        r = self._verdict(traffic=t)
        self.assertEqual(r["verdict"], "FAIL")
        self.assertTrue(any("input-error counter could not be read" in p
                            for p in r["problems"]))

    def test_multiple_problems_all_reported(self):
        r = tg_acceptance_verdict(
            login_reached=False, tg1_present=False, tg_up=False, link_up=False,
            traffic={"ipkts_before": 5, "ipkts_after": 5,
                     "opkts_before": 5, "opkts_after": 5,
                     "ierrs_after": 2})
        self.assertEqual(r["verdict"], "FAIL")
        # login, tg1-present, tg-up, link-up, ipkts, opkts, ierrs -> seven.
        self.assertEqual(len(r["problems"]), 7)


if __name__ == '__main__':
    unittest.main()
