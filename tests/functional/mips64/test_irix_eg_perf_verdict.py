#!/usr/bin/env python3
#
# Pure PASS/FAIL logic for the IRIX eg transport-and-throughput functional
# test, split out so it can be proven without booting anything
#
# SPDX-License-Identifier: GPL-2.0-or-later
"""
eg_perf_verdict() is the success contract of test_irix_eg_perf.py, factored
into one pure function the same way test_irix_eg_net.py factors
eg_net_verdict(): running this file directly exercises the verdict on
synthetic inputs with no qemu_test import in the way, so the logic that
decides a real boot's outcome is itself tested without spending a boot.

Why each clause matters:

- login_reached / eg_up: a transfer that never got past boot or interface
  configuration must not be scored on its absent numbers.

- The TCP digest AND the byte count, both sides: two empty files share a
  digest, so the count is what catches a "successful" transfer of nothing.
  This is the clause that would have stayed red for the entire time eg
  existed, because until the transmit checksum and the general-information
  address were fixed, no TCP connection over eg ever completed.

- tcp_segments_delta: the guest's own TCP stack counting segments it
  processed. A byte-exact file proves bytes arrived; this proves they arrived
  as TCP through the guest's stack rather than by some other path.

- A TCP rate at all: throughput is the point of the bulk leg, and a missing
  rate means ttcp did not report one, which is a failed measurement rather
  than a slow one. The value is not bounded here on purpose -- what counts as
  fast depends on the host's load, and a test that fails when the machine is
  busy teaches people to ignore it.

- UDP datagrams received: enough to show the datagram path carried traffic.
  Loss is recorded but deliberately NOT failed on: UDP has no flow control
  and an emulated receiver drains slower than a host fills, so loss measures
  the sender's pacing far more than the adapter. Recording it keeps the
  number honest without turning it into a false alarm.
"""

import unittest


def eg_perf_verdict(login_reached, eg_up, results):
    """Decide PASS/FAIL for one IRIX eg transport-and-throughput run.

    login_reached, eg_up: booleans for the two upstream milestones.

    results: the dict the test records --
        {"tcp_verify": {"host_cksum": str, "host_bytes": int,
                        "guest_cksum": str or None,
                        "guest_bytes": int or None} or None,
         "tcp_mbps": float or None,
         "udp_mbps": float or None,
         "udp_loss": float or None,
         "udp_bytes": int or None,
         "tcp_segments_delta": int or None}

    Returns {"verdict": "PASS"|"FAIL", "problems": [str, ...]}. PASS iff every
    clause holds; problems names every clause that did not, so one run reports
    all of what went wrong rather than only the first.
    """
    problems = []

    if not login_reached:
        problems.append("the guest never reached a root shell")
    if not eg_up:
        problems.append("the eg interface never came up configured")

    verify = results.get("tcp_verify")
    if not verify:
        problems.append("the byte-checked TCP transfer did not run")
    else:
        gc, gb = verify.get("guest_cksum"), verify.get("guest_bytes")
        hc, hb = verify.get("host_cksum"), verify.get("host_bytes")
        if gc is None or gb is None:
            problems.append(
                "the guest never reported a checksum and size for the TCP "
                "payload it received")
        else:
            if gb != hb:
                problems.append(
                    f"TCP size mismatch: the guest received {gb} bytes, "
                    f"the host sent {hb}")
            if gc != hc:
                problems.append(
                    f"TCP checksum mismatch: the guest computed {gc}, "
                    f"the host computed {hc}")

    segs = results.get("tcp_segments_delta")
    if segs is None:
        problems.append(
            "the guest's TCP segment counter could not be read")
    elif segs <= 0:
        problems.append(
            f"the guest's TCP stack processed no segments ({segs}): the bytes "
            f"did not arrive as TCP through eg")

    if results.get("tcp_mbps") is None:
        problems.append("no TCP throughput was measured")

    udp_bytes = results.get("udp_bytes")
    if udp_bytes is None:
        problems.append("no UDP datagrams were measured")
    elif udp_bytes <= 0:
        problems.append(
            "the guest received no UDP payload at all: the datagram path "
            "carried nothing")

    # Only present when a soak was asked for. Every round carries its own
    # digest and byte count, so a run that degrades partway through -- the
    # shape a ring index or a recycled buffer produces -- names the round it
    # went wrong on rather than just failing at the end.
    soak = results.get("soak")
    if soak:
        failed = soak.get("failed") or []
        if not soak.get("rounds"):
            problems.append("a soak was asked for but no round ran")
        elif failed:
            where = ", ".join(str(r.get("round")) for r in failed)
            problems.append(
                f"{len(failed)} of {len(soak['rounds'])} soak rounds did not "
                f"match byte for byte (round {where})")

    return {
        "verdict": "PASS" if not problems else "FAIL",
        "problems": problems,
    }


def _good_results():
    """A run that passes every clause, copied and mutated per test so a single
    wrong field is what fails a case."""
    return {
        "tcp_verify": {"host_cksum": "3294665917", "host_bytes": 1048576,
                       "guest_cksum": "3294665917", "guest_bytes": 1048576},
        "tcp_mbps": 21.6,
        "udp_mbps": 13.2,
        "udp_loss": 0.79,
        "udp_bytes": 880640,
        "tcp_segments_delta": 3611,
    }


class EgPerfVerdictTest(unittest.TestCase):
    def test_all_clauses_hold_is_pass(self):
        r = eg_perf_verdict(True, True, _good_results())
        self.assertEqual(r["verdict"], "PASS")
        self.assertEqual(r["problems"], [])

    def test_no_login_fails(self):
        r = eg_perf_verdict(False, True, _good_results())
        self.assertEqual(r["verdict"], "FAIL")
        self.assertTrue(any("root shell" in p for p in r["problems"]))

    def test_eg_not_up_fails(self):
        r = eg_perf_verdict(True, False, _good_results())
        self.assertEqual(r["verdict"], "FAIL")
        self.assertTrue(any("never came up" in p for p in r["problems"]))

    def test_missing_transfer_fails(self):
        res = _good_results()
        res["tcp_verify"] = None
        r = eg_perf_verdict(True, True, res)
        self.assertEqual(r["verdict"], "FAIL")
        self.assertTrue(any("did not run" in p for p in r["problems"]))

    def test_tcp_size_mismatch_fails(self):
        res = _good_results()
        res["tcp_verify"]["guest_bytes"] = 2
        r = eg_perf_verdict(True, True, res)
        self.assertEqual(r["verdict"], "FAIL")
        self.assertTrue(any("size mismatch" in p for p in r["problems"]))

    def test_tcp_checksum_mismatch_fails(self):
        res = _good_results()
        res["tcp_verify"]["guest_cksum"] = "128"
        r = eg_perf_verdict(True, True, res)
        self.assertEqual(r["verdict"], "FAIL")
        self.assertTrue(any("checksum mismatch" in p for p in r["problems"]))

    def test_no_segments_fails(self):
        # Bytes can match while the counter says the guest's TCP stack never
        # saw them, which would mean they arrived by some other route.
        res = _good_results()
        res["tcp_segments_delta"] = 0
        r = eg_perf_verdict(True, True, res)
        self.assertEqual(r["verdict"], "FAIL")
        self.assertTrue(any("no segments" in p for p in r["problems"]))

    def test_missing_tcp_rate_fails(self):
        res = _good_results()
        res["tcp_mbps"] = None
        r = eg_perf_verdict(True, True, res)
        self.assertEqual(r["verdict"], "FAIL")
        self.assertTrue(any("throughput" in p for p in r["problems"]))

    def test_no_udp_payload_fails(self):
        res = _good_results()
        res["udp_bytes"] = 0
        r = eg_perf_verdict(True, True, res)
        self.assertEqual(r["verdict"], "FAIL")
        self.assertTrue(any("no UDP payload" in p for p in r["problems"]))

    def test_heavy_udp_loss_is_recorded_not_failed(self):
        # UDP has no flow control and the guest drains slowly, so loss says
        # more about the sender's pacing than about the adapter.
        res = _good_results()
        res["udp_loss"] = 0.98
        r = eg_perf_verdict(True, True, res)
        self.assertEqual(r["verdict"], "PASS")

    def test_soak_all_rounds_clean_is_pass(self):
        res = _good_results()
        res["soak"] = {"rounds": [{"round": i + 1, "ok": True}
                                  for i in range(20)],
                       "bytes_total": 20 * 4 * 1024 * 1024, "failed": []}
        r = eg_perf_verdict(True, True, res)
        self.assertEqual(r["verdict"], "PASS")

    def test_soak_names_the_round_that_broke(self):
        # The shape a slowly-going-wrong ring index produces: fine for a
        # while, then not.
        rounds = [{"round": i + 1, "ok": True} for i in range(20)]
        rounds[13]["ok"] = False
        res = _good_results()
        res["soak"] = {"rounds": rounds, "bytes_total": 0,
                       "failed": [rounds[13]]}
        r = eg_perf_verdict(True, True, res)
        self.assertEqual(r["verdict"], "FAIL")
        self.assertTrue(any("round 14" in p for p in r["problems"]))

    def test_soak_asked_for_but_never_ran_fails(self):
        res = _good_results()
        res["soak"] = {"rounds": [], "bytes_total": 0, "failed": []}
        r = eg_perf_verdict(True, True, res)
        self.assertEqual(r["verdict"], "FAIL")
        self.assertTrue(any("no round ran" in p for p in r["problems"]))

    def test_absent_soak_is_not_a_problem(self):
        # The ordinary run does not soak; its absence must not fail anything.
        r = eg_perf_verdict(True, True, _good_results())
        self.assertEqual(r["verdict"], "PASS")

    def test_every_broken_clause_is_reported_at_once(self):
        res = _good_results()
        res["tcp_verify"]["guest_bytes"] = 2
        res["tcp_verify"]["guest_cksum"] = "128"
        res["tcp_segments_delta"] = 0
        res["tcp_mbps"] = None
        res["udp_bytes"] = 0
        r = eg_perf_verdict(False, False, res)
        self.assertEqual(r["verdict"], "FAIL")
        # login, eg-up, size, checksum, segments, rate, udp -> seven.
        self.assertEqual(len(r["problems"]), 7)


if __name__ == "__main__":
    unittest.main()
