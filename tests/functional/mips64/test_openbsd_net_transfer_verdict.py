#!/usr/bin/env python3
#
# Equivalence test: test_openbsd_net_transfer's verdict logic ports
# tools/net-transfer-diag's own DHCP-lease-and-three-leg pass/fail
# contract, without booting anything
#
# SPDX-License-Identifier: GPL-2.0-or-later
"""
A plain unittest -- no QEMU, no qemu_test import, runnable with just
`python3 tests/functional/mips64/test_openbsd_net_transfer_verdict.py` (or
`uv run --with pytest pytest
tests/functional/mips64/test_openbsd_net_transfer_verdict.py -v`). It
exercises net_transfer_verdict(), the pure function
test_openbsd_net_transfer.py calls after driving a real boot, over fixture
transcripts/lease-counts/leg-records that stand in for a captured run.

Ported from tools/net-transfer-diag (project checkout, read from fj/main):
that tool boots the installed OpenBSD disk on -M origin200 WITH a network
back end, logs in, leases an address by DHCP, and moves a payload three
ways over the network -- read (host HTTP server -> guest ftp(1)), write
(guest nc(1) -> host TCP sink), inbound (host opens a forwarded port ->
guest nc -l) -- each leg checked by an independently-computed SHA-256 and
byte count on both sides, because (its own docstring) "a command exiting 0
is not evidence here": ftp(1) exits 0 having written a zero-length file if
the server answers and says nothing, nc(1) exits 0 when the connection is
accepted and the payload is dropped on the floor, and a dd whose skip runs
past its input exits 0 having transferred nothing -- the hash-and-size
comparison is the only check that catches any of those.

net_transfer_verdict() below reimplements net-transfer-diag's own two
deliverables-reported-apart contract (its main(), the
`transfer_ok = all(verdict.get(k) for k in ("read", "write", "inbound"))`
/ `lease_ok = verdict.get("lease") is not False` / `ok = transfer_ok and
lease_ok` lines): PASS requires the DHCP lease's four-message handshake to
have completed AND all three legs to have been attempted and to match
their recorded SHA-256 + byte count (and, for the read and write legs,
their own command -- ftp(1)/nc(1) -- to have exited 0, the same
`rc == 0 and ...` net-transfer-diag's own read_leg()/write_leg() check
alongside the digest and size; there is no equivalent rc for the inbound
leg, matching net-transfer-diag's own inbound_leg(), whose verdict is
digest and size alone). A leg that could not be attempted at all (a
missing guest tool, or the guest never getting far enough to try) is not
a silent skip here any more than it is a pass on nothing transferred --
it is its own named problem, distinct from a leg that ran and produced
the wrong bytes, the same distinction net-transfer-diag's own
legword()/"_skipped" bookkeeping keeps.

dhcp_kind() and summarise_pcap() are net-transfer-diag's own wire-level
DHCP counting, ported near-verbatim (summarise_pcap here takes the raw
capture bytes directly rather than a path, so it stays pure and
fixturable without a real .pcap file on disk): "dhclient printed an
address" is a weaker claim than the four-message exchange that produced
it (net-transfer-diag's own dhcp_kind() docstring), so the lease verdict
here is driven by DHCPDISCOVER/DHCPOFFER/DHCPREQUEST/DHCPACK counted off
the wire, not by a guest command's exit code.

net-transfer-diag's own --static fallback address IS carried by this
port's real boot (test_openbsd_net_transfer.py's own _lease()): when
DHCP does not produce a lease, the interface is configured with the
static GUEST_ADDR by hand instead, the same reason net-transfer-diag
keeps that path -- without it a broken lease and a broken transfer
arrive as one indistinguishable red, because the guest would have no
address at all to attempt the three legs with. With it, the lease's own
PASS/FAIL (decided here, off the wire capture, never off which address
ended up configured) stays a distinct finding from whatever the three
legs do next, which may still succeed. LEASE_NEVER_ACQUIRED_FIXTURE
below models exactly that split: a lease that never completed its
four-message handshake, alongside three legs that were still genuinely
attempted (over the static fallback) and matched -- proving that a
transfer succeeding does not redeem a failed lease, and that the two are
reported apart rather than collapsed into one verdict.

Fixture provenance: this port's real boot (test_openbsd_net_transfer.py)
was not run to completion by the person writing this module, so there is
no real captured transcript of a passing run to copy the way
test_openbsd_login_verdict.py's REAL_LOGIN_TRANSCRIPT is copied from an
actual boot. ALL_LEGS_SUCCEED_FIXTURE is therefore hand-built, clearly
marked synthetic below, and used only to pin down what this module
considers a PASS -- not as a claim about what any real run has produced.
MISMATCHED_DIGEST_FIXTURE and LEASE_NEVER_ACQUIRED_FIXTURE are likewise
hand-built, each changing exactly one thing from the PASS fixture so the
unit tests below prove the specific failure each is meant to catch.
"""

import re
import struct
import unittest

# --------------------------------------------------------------------------
# Wire-level DHCP counting, ported from tools/net-transfer-diag's own
# dhcp_kind()/summarise_pcap() (project checkout, read from fj/main).

DHCP_NAMES = {1: "DHCPDISCOVER", 2: "DHCPOFFER", 3: "DHCPREQUEST",
              5: "DHCPACK", 6: "DHCPNAK", 7: "DHCPRELEASE"}

# The lease is a deliverable of its own, and its four-message handshake is
# what net-transfer-diag counts off the wire rather than trusting a guest
# command's exit code (see this module's own docstring).
LEASE_HANDSHAKE = ("DHCPDISCOVER", "DHCPOFFER", "DHCPREQUEST", "DHCPACK")


def dhcp_kind(frame: bytes):
    """Name a single Ethernet frame's DHCP message type, if it has one.

    Byte-for-byte port of net-transfer-diag's own dhcp_kind(): requires an
    IPv4 frame (ethertype 0x0800) carrying UDP (protocol 17) between ports
    67 and 68, containing the DHCP magic cookie followed by a type-53
    option. Returns None for anything else, including a frame too short
    to hold a minimal DHCP packet.
    """
    if len(frame) < 44 or frame[12:14] != b"\x08\x00":
        return None
    ihl = (frame[14] & 0x0f) * 4
    if frame[23] != 17:                      # not UDP
        return None
    udp = 14 + ihl
    if len(frame) < udp + 8:
        return None
    sport = int.from_bytes(frame[udp:udp + 2], "big")
    dport = int.from_bytes(frame[udp + 2:udp + 4], "big")
    if {sport, dport} != {67, 68}:
        return None
    body = frame[udp + 8:]
    cookie = body.find(b"\x63\x82\x53\x63")
    if cookie < 0:
        return None
    i = cookie + 4
    while i < len(body):
        opt = body[i]
        if opt == 255:
            return None
        if opt == 0:
            i += 1
            continue
        if i + 1 >= len(body):
            return None
        length = body[i + 1]
        if opt == 53 and length >= 1:
            return DHCP_NAMES.get(body[i + 2])
        i += 2 + length
    return None


def summarise_pcap(raw: bytes):
    """Count the frames in a captured .pcap file's bytes, by who sent them
    and, for DHCP frames, by message type.

    Ported from net-transfer-diag's own summarise_pcap(), which took a
    path; this takes the raw bytes directly so it can be exercised on a
    hand-built fixture without a real capture file on disk. Returns None
    on an unrecognised magic number, {} on a file too short to hold even
    the 24-byte global pcap header, otherwise {"by_src": {...}, "dhcp":
    {...}}.
    """
    if len(raw) < 24:
        return {}
    magic = raw[:4]
    if magic == b"\xa1\xb2\xc3\xd4":
        end = ">"
    elif magic == b"\xd4\xc3\xb2\xa1":
        end = "<"
    else:
        return None
    by_src, dhcp = {}, {}
    off = 24
    while off + 16 <= len(raw):
        _, _, incl, _ = struct.unpack(end + "IIII", raw[off:off + 16])
        off += 16
        frame = raw[off:off + incl]
        off += incl
        if len(frame) >= 12:
            src = ":".join("%02x" % b for b in frame[6:12])
            by_src[src] = by_src.get(src, 0) + 1
            kind = dhcp_kind(frame)
            if kind:
                dhcp[kind] = dhcp.get(kind, 0) + 1
    return {"by_src": by_src, "dhcp": dhcp}


def lease_established(dhcp_counts):
    """Whether all four DORA messages were seen at least once.

    "dhclient printed an address" is a weaker claim than the exchange
    that produced it (net-transfer-diag's own reasoning) -- this is that
    stronger claim, checked directly against what crossed the wire.
    """
    return all(dhcp_counts.get(name, 0) >= 1 for name in LEASE_HANDSHAKE)


# --------------------------------------------------------------------------
# The three-leg + lease verdict, ported from net-transfer-diag's own
# main(): read (host -> guest over HTTP/ftp), write (guest -> host over
# nc), inbound (host -> guest again, but the host opens the connection).

LEG_NAMES = ("read", "write", "inbound")

# A run that shows any of these is a wedge, not a transfer -- ported from
# net-transfer-diag's own Console.TRIPWIRES plus the bus-error pattern
# test_openbsd_login_verdict.py's own TRIPWIRES adds for this fork's real
# OpenBSD/IP27 kernel (that file is not imported here per this port's
# SELF-CONTAINED brief; the same three patterns are reproduced
# independently).
TRIPWIRES = (
    ("the guest fell into its kernel debugger", re.compile(r"[\r\n]+ddb>")),
    ("the guest panicked", re.compile(r"panic: ")),
    ("the guest took a bus error", re.compile(r"Trap cause = 7")),
)


def net_transfer_verdict(transcript, dhcp_counts, legs):
    """PASS iff the DHCP lease's four-message handshake was seen on the
    wire AND all three legs were attempted and match their recorded
    SHA-256 + byte count. FAIL if the lease never appears, any leg's
    digest or byte count mismatches, or a leg was never attempted at all
    (a missing guest tool, or the guest never reaching that point) --
    each is reported as its own distinct problem, never collapsed into a
    single "it did not work".

    transcript: the captured guest console text for the whole run (boot
      through the three legs). Scanned only for TRIPWIRES -- a guest that
      wedged partway through is a defect this must catch even if, by
      coincidence, every leg dict below still looks complete.
    dhcp_counts: a dict of DHCP message name -> count, as
      summarise_pcap()'s own "dhcp" key reports.
    legs: a dict with keys "read", "write", "inbound". Each value is
      either falsy (the leg was never attempted) or a dict with keys
      "attempted" (bool), and when attempted, "host_sha256", "host_bytes",
      "guest_sha256", "guest_bytes" -- the digest and byte count computed
      independently by each side of the run, this Python process (host_*)
      and the guest's own shell (guest_*). Naming them by which side
      measured them, rather than by which one is "expected", is
      deliberate: for the read and inbound legs the host originates the
      payload and the guest's own report is what is being checked, but
      for the write leg it is the other way around -- the guest builds
      the payload and reports on it first, and the host's own receipt is
      what is being checked. host_*/guest_* names the same two
      independent measurements correctly in every leg; "expected" would
      not. A leg dict may also carry "exit_code" (int or None): when
      present and non-zero, the leg fails even if its digest and byte
      count happen to match -- ported from net-transfer-diag's own
      read_leg()/write_leg(), whose verdict is `rc == 0 and <digest
      match> and <size match>`, never digest-and-size alone. Absent or
      None means no exit-code check applies to that leg (net-transfer-diag
      carries none for the inbound leg either, since its nc -l runs
      backgrounded and prints no exit status this harness waits on). A
      leg dict may also carry "reason" (str), used only for the problem
      message when the leg was not attempted.

    Returns {"verdict": "PASS"|"FAIL", "problems": [...], "lease_ok":
    bool, "legs_ok": {name: bool}}.
    """
    problems = [why for why, rx in TRIPWIRES if rx.search(transcript)]

    lease_ok = lease_established(dhcp_counts)
    if not lease_ok:
        seen = sorted(name for name in LEASE_HANDSHAKE if dhcp_counts.get(name))
        problems.append(
            "the DHCP lease was never acquired -- its four-message "
            "handshake (DHCPDISCOVER, DHCPOFFER, DHCPREQUEST, DHCPACK) "
            "did not all appear on the wire (saw: "
            + (", ".join(seen) if seen else "nothing") + ")")

    legs_ok = {}
    for name in LEG_NAMES:
        leg = legs.get(name)
        if not leg or not leg.get("attempted"):
            reason = (leg or {}).get("reason")
            problems.append(
                f"the guest never reached the {name} leg"
                + (f" ({reason})" if reason else ""))
            legs_ok[name] = False
            continue
        exit_code = leg.get("exit_code")
        exit_ok = exit_code is None or exit_code == 0
        digest_ok = leg["guest_sha256"] == leg["host_sha256"]
        size_ok = leg["guest_bytes"] == leg["host_bytes"]
        if not exit_ok:
            problems.append(
                f"{name} leg: its own command exited {exit_code}, not 0")
        if not digest_ok:
            problems.append(
                f"{name} leg: digest mismatch (host measured "
                f"{leg['host_sha256']}, guest reported "
                f"{leg['guest_sha256']})")
        if not size_ok:
            problems.append(
                f"{name} leg: byte count mismatch (host measured "
                f"{leg['host_bytes']}, guest reported "
                f"{leg['guest_bytes']})")
        legs_ok[name] = exit_ok and digest_ok and size_ok

    return {
        "verdict": "PASS" if not problems else "FAIL",
        "problems": problems,
        "lease_ok": lease_ok,
        "legs_ok": legs_ok,
    }


# ---------------------------------------------------------------------------
# Fixtures. See this module's own docstring for provenance -- every one of
# these is hand-built, since no real captured transcript of a passing run
# was available to copy at the time this was written, clearly marked so,
# and each of the failure fixtures changes exactly one thing from the PASS
# fixture.

_READ_HASH = "1" * 64
_WRITE_HASH = "2" * 64
_INBOUND_HASH = "3" * 64
_PAYLOAD_BYTES = 65536

# SYNTHETIC -- hand-built, not a captured run (see this module's docstring).
# Representative of the shape a real transcript would have: root shell
# reached, tool-availability probes, dhclient, then each leg's guest-side
# command and its own MARK-tagged echo of a digest and byte count, the same
# shape net-transfer-diag's own run()/two_values() produce.
ALL_LEGS_SUCCEED_TRANSCRIPT = """\
origami# command -v ftp >/dev/null 2>&1; echo "MARK""-haveftp-$?"
MARK-haveftp-0
origami# command -v nc >/dev/null 2>&1; echo "MARK""-havenc-$?"
MARK-havenc-0
origami# command -v sha256 >/dev/null 2>&1; echo "MARK""-havesha256-$?"
MARK-havesha256-0
origami# command -v dd >/dev/null 2>&1; echo "MARK""-havedd-$?"
MARK-havedd-0
origami# dhclient iec0; echo "MARK""-dhcp-$?"
MARK-dhcp-0
origami# ifconfig iec0; echo "MARK""-leaseaddr-$?"
iec0: flags=8843<UP,BROADCAST,RUNNING,SIMPLEX,MULTICAST> mtu 1500
\tinet 10.0.2.15 netmask 0xffffff00 broadcast 10.0.2.255
MARK-leaseaddr-0
origami# ftp -V -o /root/down.bin http://10.0.2.2:8080/payload.bin; echo "MARK""-read-$?"
MARK-read-0
origami# echo D $(sha256 -q /root/down.bin) $(wc -c </root/down.bin); echo "MARK""-readsum-$?"
D %(read_hash)s 65536
MARK-readsum-0
origami# dd if=/dev/urandom of=/root/up.bin bs=1024 count=64 2>/dev/null; echo "MARK""-mkup-$?"
MARK-mkup-0
origami# echo U $(sha256 -q /root/up.bin) $(wc -c </root/up.bin); echo "MARK""-upsum-$?"
U %(write_hash)s 65536
MARK-upsum-0
origami# nc -N 10.0.2.2 9001 < /root/up.bin; echo "MARK""-write-$?"
MARK-write-0
origami# (nc -l 7000 > /root/in.bin; echo "IN $(sha256 -q /root/in.bin) $(wc -c </root/in.bin)") &
IN %(inbound_hash)s 65536
origami# halt -p
""" % {"read_hash": _READ_HASH, "write_hash": _WRITE_HASH,
       "inbound_hash": _INBOUND_HASH}

ALL_LEGS_SUCCEED_DHCP_COUNTS = {
    "DHCPDISCOVER": 1, "DHCPOFFER": 1, "DHCPREQUEST": 1, "DHCPACK": 1,
}

ALL_LEGS_SUCCEED_LEGS = {
    "read": {"attempted": True, "exit_code": 0, "host_sha256": _READ_HASH,
              "host_bytes": _PAYLOAD_BYTES, "guest_sha256": _READ_HASH,
              "guest_bytes": _PAYLOAD_BYTES},
    "write": {"attempted": True, "exit_code": 0, "host_sha256": _WRITE_HASH,
               "host_bytes": _PAYLOAD_BYTES, "guest_sha256": _WRITE_HASH,
               "guest_bytes": _PAYLOAD_BYTES},
    # No exit_code for inbound -- net-transfer-diag's own inbound_leg()
    # carries none either (see net_transfer_verdict()'s own docstring).
    "inbound": {"attempted": True, "host_sha256": _INBOUND_HASH,
                 "host_bytes": _PAYLOAD_BYTES,
                 "guest_sha256": _INBOUND_HASH,
                 "guest_bytes": _PAYLOAD_BYTES},
}

# SYNTHETIC -- the PASS fixture's own legs, with exactly one byte flipped
# in the write leg's guest-reported digest, modelling the write leg
# landing corrupted bytes the host received and hashed independently. The
# transcript and DHCP counts are unchanged from the PASS fixture: only the
# one leg's own recorded digest differs, so this proves the mismatch alone
# fails the run, not something else.
MISMATCHED_DIGEST_LEGS = dict(ALL_LEGS_SUCCEED_LEGS)
MISMATCHED_DIGEST_LEGS["write"] = dict(ALL_LEGS_SUCCEED_LEGS["write"])
MISMATCHED_DIGEST_LEGS["write"]["guest_sha256"] = "f" + _WRITE_HASH[1:]

# SYNTHETIC -- the PASS fixture's own DHCP counts with DHCPACK never
# arriving (the server offered and the guest requested, but no
# acknowledgement crossed the wire -- a lease that never completed). This
# port DOES carry net-transfer-diag's own --static fallback (see this
# module's docstring): test_openbsd_net_transfer.py's own _lease()
# configures the static GUEST_ADDR by hand when DHCP does not produce a
# lease, so the three legs still get a genuine attempt over that address.
# The legs below are therefore the PASS fixture's own legs, unchanged --
# all three attempted and matching -- to prove the point this fixture
# exists to prove: a transfer that succeeds does not redeem a lease that
# failed. The two are reported apart, and OVERALL must still be FAIL.
LEASE_NEVER_ACQUIRED_DHCP_COUNTS = {
    "DHCPDISCOVER": 1, "DHCPOFFER": 1, "DHCPREQUEST": 1,
}
LEASE_NEVER_ACQUIRED_LEGS = ALL_LEGS_SUCCEED_LEGS
LEASE_NEVER_ACQUIRED_TRANSCRIPT = """\
origami# dhclient iec0; echo "MARK""-dhcp-$?"
dhclient: no lease, giving up
MARK-dhcp-1
origami# ifconfig iec0 inet 10.0.2.15 netmask 255.255.255.0 up; echo "MARK""-cfg-$?"
MARK-cfg-0
origami# halt -p
"""

# SYNTHETIC -- spliced the same way test_openbsd_login_verdict.py's own
# SYNTHETIC_DDB_WEDGE_TRANSCRIPT is: a normal start (lease acquired, tools
# checked) followed by a real trap-dump shape (the same "Trap cause = 7"
# .. "ddb> " text that fork's real OpenBSD/IP27 kernel produces for a
# data bus error, per that file's own docstring) landing mid-run, before
# any leg's own digest line ever printed. Models the guest wedging during
# a leg rather than a leg simply printing the wrong bytes -- a different
# finding, and TRIPWIRES is what catches it here since none of the three
# leg dicts below have anything attempted to report.
GUEST_WEDGE_TRANSCRIPT = """\
origami# dhclient iec0; echo "MARK""-dhcp-$?"
MARK-dhcp-0
origami# ftp -V -o /root/down.bin http://10.0.2.2:8080/payload.bin; echo "MARK""-read-$?"
Trap cause = 7 Frame 0xffffffff82b2f9c8
\rTrap PC 0xa80000000027bf18 RA 0xa80000000048aab8 fault 0x1c533f4000
\rStopped at      0xa80000000027bf18:     sd      zero,0(a0)
\rddb> """
GUEST_WEDGE_DHCP_COUNTS = dict(ALL_LEGS_SUCCEED_DHCP_COUNTS)
GUEST_WEDGE_LEGS = {
    name: {"attempted": False, "reason": "the guest wedged before this leg ran"}
    for name in LEG_NAMES
}


class NetTransferVerdictTest(unittest.TestCase):

    # -- dhcp_kind / summarise_pcap ----------------------------------

    def test_dhcp_kind_identifies_each_message_type(self):
        for code, name in DHCP_NAMES.items():
            frame = _dhcp_frame(code)
            self.assertEqual(dhcp_kind(frame), name, f"code {code}")

    def test_dhcp_kind_ignores_a_short_frame(self):
        self.assertIsNone(dhcp_kind(b"\x00" * 20))

    def test_dhcp_kind_ignores_non_ip_ethertype(self):
        frame = bytearray(_dhcp_frame(1))
        frame[12:14] = b"\x08\x06"  # ARP, not IPv4
        self.assertIsNone(dhcp_kind(bytes(frame)))

    def test_dhcp_kind_ignores_non_udp_protocol(self):
        frame = bytearray(_dhcp_frame(1))
        frame[23] = 6  # TCP, not UDP
        self.assertIsNone(dhcp_kind(bytes(frame)))

    def test_dhcp_kind_ignores_udp_not_on_the_dhcp_ports(self):
        frame = _dhcp_frame(1, sport=12345, dport=53)
        self.assertIsNone(dhcp_kind(frame))

    def test_summarise_pcap_counts_a_full_handshake(self):
        raw = _pcap_bytes([_dhcp_frame(1), _dhcp_frame(2, sport=67, dport=68),
                           _dhcp_frame(3), _dhcp_frame(5, sport=67, dport=68)])
        summary = summarise_pcap(raw)
        self.assertEqual(summary["dhcp"], {
            "DHCPDISCOVER": 1, "DHCPOFFER": 1, "DHCPREQUEST": 1,
            "DHCPACK": 1,
        })
        self.assertTrue(lease_established(summary["dhcp"]))

    def test_summarise_pcap_missing_ack_is_not_a_lease(self):
        raw = _pcap_bytes([_dhcp_frame(1), _dhcp_frame(2, sport=67, dport=68),
                           _dhcp_frame(3)])
        summary = summarise_pcap(raw)
        self.assertFalse(lease_established(summary["dhcp"]))

    def test_summarise_pcap_bad_magic_returns_none(self):
        self.assertIsNone(summarise_pcap(b"NOTAPCAP" + b"\x00" * 16))

    def test_summarise_pcap_too_short_returns_empty(self):
        self.assertEqual(summarise_pcap(b"\x00" * 10), {})

    def test_lease_established_requires_all_four_messages(self):
        self.assertFalse(lease_established({}))
        self.assertFalse(lease_established({"DHCPDISCOVER": 1, "DHCPOFFER": 1,
                                            "DHCPREQUEST": 1}))
        self.assertTrue(lease_established({
            "DHCPDISCOVER": 1, "DHCPOFFER": 1, "DHCPREQUEST": 1,
            "DHCPACK": 1,
        }))

    # -- net_transfer_verdict -----------------------------------------

    def test_all_legs_succeed_passes(self):
        report = net_transfer_verdict(
            ALL_LEGS_SUCCEED_TRANSCRIPT, ALL_LEGS_SUCCEED_DHCP_COUNTS,
            ALL_LEGS_SUCCEED_LEGS)
        self.assertEqual(report["problems"], [])
        self.assertEqual(report["verdict"], "PASS")
        self.assertTrue(report["lease_ok"])
        self.assertEqual(report["legs_ok"],
                         {"read": True, "write": True, "inbound": True})

    def test_mismatched_digest_fails(self):
        report = net_transfer_verdict(
            ALL_LEGS_SUCCEED_TRANSCRIPT, ALL_LEGS_SUCCEED_DHCP_COUNTS,
            MISMATCHED_DIGEST_LEGS)
        self.assertEqual(report["verdict"], "FAIL")
        self.assertTrue(report["lease_ok"])
        self.assertTrue(any("write leg: digest mismatch" in p
                            for p in report["problems"]))
        self.assertEqual(report["legs_ok"]["write"], False)
        # The other two legs are untouched by this fixture and must still
        # read as fine -- a mismatch in one leg must not taint another.
        self.assertEqual(report["legs_ok"]["read"], True)
        self.assertEqual(report["legs_ok"]["inbound"], True)

    def test_mismatched_byte_count_fails_even_with_a_matching_digest(self):
        # Two empty files hash the same (net-transfer-diag's own two_values()
        # docstring: "a digest alone certifies a weaker property than it
        # appears to"). A leg whose guest-reported size differs from what
        # was sent must fail even if, hypothetically, the digest matched.
        legs = dict(ALL_LEGS_SUCCEED_LEGS)
        legs["read"] = dict(ALL_LEGS_SUCCEED_LEGS["read"])
        legs["read"]["guest_bytes"] = 0
        report = net_transfer_verdict(
            ALL_LEGS_SUCCEED_TRANSCRIPT, ALL_LEGS_SUCCEED_DHCP_COUNTS, legs)
        self.assertEqual(report["verdict"], "FAIL")
        self.assertTrue(any("read leg: byte count mismatch" in p
                            for p in report["problems"]))
        self.assertEqual(report["legs_ok"]["read"], False)

    def test_lease_never_acquired_fails_even_though_every_leg_still_matches(self):
        # The point this fixture exists to prove: this port DOES carry
        # net-transfer-diag's own --static fallback (see this module's
        # docstring), so a lease failure does not stop the three legs
        # from being genuinely attempted -- and here they all succeed.
        # OVERALL must still be FAIL, on the lease alone, and the two
        # findings must be reported apart rather than one implying the
        # other.
        report = net_transfer_verdict(
            LEASE_NEVER_ACQUIRED_TRANSCRIPT, LEASE_NEVER_ACQUIRED_DHCP_COUNTS,
            LEASE_NEVER_ACQUIRED_LEGS)
        self.assertEqual(report["verdict"], "FAIL")
        self.assertFalse(report["lease_ok"])
        # Exactly one problem, and it is the lease -- nothing about any
        # leg belongs in this list, since all three genuinely matched.
        self.assertEqual(len(report["problems"]), 1)
        self.assertTrue(
            report["problems"][0].startswith("the DHCP lease was never acquired"))
        self.assertEqual(report["legs_ok"],
                         {"read": True, "write": True, "inbound": True})

    def test_lease_never_reached_and_no_legs_attempted_fails_all_four(self):
        # The complementary case LEASE_NEVER_ACQUIRED_LEGS does not cover:
        # a lease failure where the legs genuinely never ran either (the
        # guest crashed before the static-fallback configuration, or a
        # harness timeout stopped the run there) -- each leg must still
        # be its own distinct problem, not folded into the lease finding.
        legs = {name: {"attempted": False,
                       "reason": "the run stopped before this leg"}
               for name in LEG_NAMES}
        report = net_transfer_verdict(
            LEASE_NEVER_ACQUIRED_TRANSCRIPT, LEASE_NEVER_ACQUIRED_DHCP_COUNTS,
            legs)
        self.assertEqual(report["verdict"], "FAIL")
        self.assertFalse(report["lease_ok"])
        for name in LEG_NAMES:
            self.assertTrue(
                any(f"the guest never reached the {name} leg" in p
                    for p in report["problems"]))
            self.assertEqual(report["legs_ok"][name], False)

    def test_nonzero_exit_code_fails_even_with_matching_digest_and_size(self):
        # Ports net-transfer-diag's own read_leg()/write_leg() verdict,
        # which is `rc == 0 and <digest match> and <size match>` -- never
        # digest-and-size alone. A command that exited non-zero must fail
        # the leg even though the bytes it moved happened to match.
        legs = dict(ALL_LEGS_SUCCEED_LEGS)
        legs["read"] = dict(ALL_LEGS_SUCCEED_LEGS["read"])
        legs["read"]["exit_code"] = 1
        report = net_transfer_verdict(
            ALL_LEGS_SUCCEED_TRANSCRIPT, ALL_LEGS_SUCCEED_DHCP_COUNTS, legs)
        self.assertEqual(report["verdict"], "FAIL")
        self.assertTrue(any("read leg: its own command exited 1, not 0" in p
                            for p in report["problems"]))
        self.assertEqual(report["legs_ok"]["read"], False)
        # The inbound leg carries no exit_code key at all -- None must not
        # be read as a failure.
        self.assertIsNone(ALL_LEGS_SUCCEED_LEGS["inbound"].get("exit_code"))
        self.assertEqual(report["legs_ok"]["inbound"], True)

    def test_guest_wedge_before_any_leg_fails_and_names_the_wedge(self):
        report = net_transfer_verdict(
            GUEST_WEDGE_TRANSCRIPT, GUEST_WEDGE_DHCP_COUNTS, GUEST_WEDGE_LEGS)
        self.assertEqual(report["verdict"], "FAIL")
        self.assertTrue(report["lease_ok"])
        self.assertIn("the guest fell into its kernel debugger",
                      report["problems"])
        self.assertIn("the guest took a bus error", report["problems"])
        for name in LEG_NAMES:
            self.assertTrue(
                any(f"the guest never reached the {name} leg" in p
                    for p in report["problems"]))

    def test_panic_is_reported_even_when_every_leg_otherwise_matches(self):
        # A trip wire must not be laundered by legs that otherwise look
        # complete -- a panic anywhere in the transcript is its own
        # problem regardless of what the leg dicts say.
        report = net_transfer_verdict(
            "panic: kernel diagnostic assertion failed\r\n",
            ALL_LEGS_SUCCEED_DHCP_COUNTS, ALL_LEGS_SUCCEED_LEGS)
        self.assertEqual(report["verdict"], "FAIL")
        self.assertIn("the guest panicked", report["problems"])

    def test_empty_everything_fails_naming_lease_and_all_three_legs(self):
        report = net_transfer_verdict("", {}, {})
        self.assertEqual(report["verdict"], "FAIL")
        self.assertFalse(report["lease_ok"])
        self.assertEqual(len(report["problems"]), 4)  # lease + 3 legs


# --------------------------------------------------------------------------
# Fixture-construction helpers (used only by the tests above): a minimal
# Ethernet+IPv4+UDP+DHCP frame, and a minimal .pcap file wrapping a list of
# such frames, since there was no real captured .pcap to draw one from at
# the time this was written -- see this module's own docstring.


def _dhcp_frame(msg_type: int, sport: int = 68, dport: int = 67) -> bytes:
    """Build the smallest Ethernet+IPv4+UDP+DHCP frame that dhcp_kind()
    will read a message type out of. Every field dhcp_kind() does not
    itself inspect (MAC addresses, IP addresses, checksums, TTL) is a
    fixed placeholder; only the ethertype, IP protocol, UDP ports, DHCP
    magic cookie, and the option 53 (message type) TLV are meaningful.
    """
    eth = (bytes.fromhex("080069123456") + bytes.fromhex("525400abcdef")
           + b"\x08\x00")
    ip = (bytes([0x45, 0x00]) + (0x0028).to_bytes(2, "big")
          + (0x0000).to_bytes(2, "big") + (0x4000).to_bytes(2, "big")
          + bytes([64, 17]) + (0x0000).to_bytes(2, "big")
          + bytes([10, 0, 2, 2]) + bytes([10, 0, 2, 15]))
    payload = b"\x63\x82\x53\x63" + bytes([53, 1, msg_type]) + bytes([255])
    udp = (sport.to_bytes(2, "big") + dport.to_bytes(2, "big")
           + (8 + len(payload)).to_bytes(2, "big") + b"\x00\x00")
    return eth + ip + udp + payload


def _pcap_bytes(frames) -> bytes:
    """Wrap a list of frame bytes in a minimal little-endian .pcap file:
    the 24-byte global header summarise_pcap() skips over, then one
    16-byte per-record header plus the frame itself for each frame.
    """
    header = b"\xd4\xc3\xb2\xa1" + struct.pack("<HHIIII", 2, 4, 0, 0, 65535, 1)
    body = b"".join(
        struct.pack("<IIII", 0, 0, len(frame), len(frame)) + frame
        for frame in frames)
    return header + body


if __name__ == "__main__":
    unittest.main()
