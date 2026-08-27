#!/usr/bin/env python3
#
# Linux/IP27 network-multiuser check: the pure verdict over a boot transcript
#
# SPDX-License-Identifier: GPL-2.0-or-later
"""
The pure marker check for test_linux_multiuser, split out so it needs no
QEMU and no qemu_test import -- runnable with plain python3, the same
division test_linux_kernel_verdict.py and test_openbsd_login_verdict.py
keep from their boot tests.

missing_multiuser_markers(log) returns, in order, the markers a networked
multiuser boot of the Gentoo disk should print but did not: the IOC3
ethernet linking, OpenRC starting the DHCP client and sshd, and the
multiuser login prompt. An empty return means every one was seen -- the
guest reached a networked multiuser login. The stronger proof that the ssh
service actually answers a remote connection is NOT here: it is a
host-side socket probe in test_linux_multiuser.py (the SSH banner over the
forwarded port), the network-service evidence a console transcript cannot
give. This module only judges the transcript.

Each marker is a substring of a line a real boot prints, so the check is
line-ending agnostic (a real serial capture's CRLF never splits a marker);
the fixture below therefore uses plain line endings rather than the
byte-faithful CRLF the whole-capture fixtures in
test_linux_kernel_verdict.py preserve. The lines themselves are from a
4-CPU/2-node Gentoo-disk boot that reached a networked login with sshd
answering (the milestone this test ports).
"""

import unittest


MARKERS = [
    ("ethernet link", "eth0: link up"),
    ("dhcp client", "Starting DHCP Client Daemon"),
    ("ssh service", "Starting sshd"),
    ("multiuser login prompt", "login:"),
]


def missing_multiuser_markers(log):
    """Which networked-multiuser markers never appeared in log, in order;
    empty means PASS."""
    return [description for description, marker in MARKERS if marker not in log]


# Real lines from a 4-CPU/2-node Gentoo-disk boot to a networked login (the
# milestone capture): the IOC3 ethernet link, OpenRC bringing up dhcpcd and
# sshd, and the getty login prompt. Plain line endings -- see this module's
# docstring for why the CRLF the console really emits is irrelevant here.
REAL_MULTIUSER_TAIL = """\
ioc3-eth ioc3-eth.4.auto eth0: link up, 100Mbps, full-duplex, lpa 0x01E1
 * Starting DHCP Client Daemon ...
 [ ok ]
 * Starting sshd ...
 [ ok ]
 * Starting local ...
 [ ok ]
localhost login:
"""


class MissingMultiuserMarkersTest(unittest.TestCase):

    def test_real_tail_passes(self):
        self.assertEqual(missing_multiuser_markers(REAL_MULTIUSER_TAIL), [])

    def test_empty_transcript_fails_every_marker(self):
        self.assertEqual(
            missing_multiuser_markers(""),
            [description for description, _ in MARKERS])

    def test_no_ssh_fails_only_ssh(self):
        # OpenRC never started sshd: only that marker is missing.
        without = REAL_MULTIUSER_TAIL.replace("Starting sshd", "Starting xyz")
        self.assertEqual(missing_multiuser_markers(without), ["ssh service"])

    def test_no_network_fails_only_ethernet(self):
        without = REAL_MULTIUSER_TAIL.replace("eth0: link up", "eth0: link is")
        self.assertEqual(missing_multiuser_markers(without), ["ethernet link"])

    def test_network_up_but_no_login_fails_only_login(self):
        # Came up on the network but never reached the multiuser prompt.
        without = REAL_MULTIUSER_TAIL.replace("localhost login:", "")
        self.assertEqual(
            missing_multiuser_markers(without), ["multiuser login prompt"])


if __name__ == "__main__":
    unittest.main()
