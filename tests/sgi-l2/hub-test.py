#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-3-Clause
"""Tests for qemu-sgi-l2-hub: slot assignment, the slot map, the device
record, EBUSY and ENODEV, segment relay in both directions, queueing before
a node is open, length stamping, and detach.

usage: hub-test.py PATH-TO-qemu-sgi-l2-hub
"""

import os
import socket
import sys
import time
import unittest

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import l2_hub_lib as lib  # noqa: E402

HUB = None


class HubTests(unittest.TestCase):
    def setUp(self):
        self.hub = lib.Hub(HUB)

    def tearDown(self):
        self.hub.close()

    def test_slot_map_follows_attach_and_detach(self):
        cs, err, _ = self.hub.request("C")
        self.assertEqual(err, 0)
        self.assertEqual(cs.recv(64), bytes(lib.SLOTS))
        first = self.hub.l1()
        second = self.hub.l1()
        self.assertEqual(cs.recv(64), bytes([1]) + bytes(lib.SLOTS - 1))
        self.assertEqual(cs.recv(64), bytes([1, 1]) + bytes(lib.SLOTS - 2))
        first.close()
        self.assertEqual(cs.recv(64), bytes([0, 1]) + bytes(lib.SLOTS - 2))
        third = self.hub.l1()
        # The lowest free slot is reused.
        self.assertEqual(cs.recv(64), bytes([1, 1]) + bytes(lib.SLOTS - 2))
        second.close()
        third.close()

    def test_open_errors_and_records(self):
        l1 = self.hub.l1()
        self.hub.wait_log("attach slot 0")
        s, err, rec = self.hub.request("O", 0)
        self.assertEqual(err, 0)
        self.assertEqual(rec[0], 1)           # bus
        self.assertEqual(rec[1], 2)           # hub ports in the path
        q, err, qrec = self.hub.request("Q", 0)
        self.assertEqual((err, qrec), (0, rec))
        _, err, _ = self.hub.request("O", 0)
        self.assertEqual(err, 16)             # EBUSY
        _, err, _ = self.hub.request("O", 1)
        self.assertEqual(err, 19)             # ENODEV
        _, err, _ = self.hub.request("O", 40)
        self.assertEqual(err, 22)             # EINVAL
        _, err, _ = self.hub.request("Q", 1)
        self.assertEqual(err, 19)
        s.close()
        self.hub.wait_log("node close slot 0")
        s2, err, _ = self.hub.request("O", 0)
        self.assertEqual(err, 0)
        s2.close()
        l1.close()

    def test_segments_both_ways_and_stamping(self):
        l1 = self.hub.l1()
        self.hub.wait_log("attach slot 0")
        s, err, _ = self.hub.request("O", 0)
        self.assertEqual(err, 0)
        seg = lib.segment(28)
        l1.send_segment(seg)
        self.assertEqual(s.recv(4096), seg)
        # A write whose length field is wrong is stamped, as the driver does.
        bad = b"\xde\xad" + lib.segment(28)[2:]
        s.send(bad)
        got = l1.recv_segment()
        self.assertEqual(got, seg[:2] + bad[2:])
        self.assertIn("stamped to 28", self.hub.log())
        s.close()
        l1.close()

    def test_a_write_shorter_than_a_segment_is_dropped(self):
        l1 = self.hub.l1()
        self.hub.wait_log("attach slot 0")
        s, err, _ = self.hub.request("O", 0)
        self.assertEqual(err, 0)
        # One byte has no length field to stamp; nothing may reach the link.
        s.send(b"\x07")
        self.hub.wait_log("short write")
        self.assertNotIn("stamped", self.hub.log())
        seg = lib.segment(28)
        s.send(seg)
        self.assertEqual(l1.recv_segment(), seg)
        s.close()
        l1.close()

    def test_segment_before_open_is_queued_in_order(self):
        l1 = self.hub.l1()
        self.hub.wait_log("attach slot 0")
        first, second = lib.segment(28, code=1), lib.segment(36, code=2)
        l1.send_segment(first)
        l1.send_segment(second)
        self.hub.wait_log("segment queued")
        time.sleep(0.2)
        s, err, _ = self.hub.request("O", 0)
        self.assertEqual(err, 0)
        self.assertEqual(s.recv(4096), first)
        self.assertEqual(s.recv(4096), second)
        s.close()
        l1.close()

    def test_detach_closes_an_open_node(self):
        l1 = self.hub.l1()
        self.hub.wait_log("attach slot 0")
        s, err, _ = self.hub.request("O", 0)
        self.assertEqual(err, 0)
        l1.close()
        self.assertEqual(s.recv(64), b"")
        s.close()

    def test_reset_is_logged(self):
        l1 = self.hub.l1()
        self.hub.wait_log("attach slot 0")
        s, err, _ = self.hub.request("R", 0, 0x05)
        self.assertEqual(err, 0)
        self.hub.wait_log("reset slot 0 ioctl 0x5305")
        _, err, _ = self.hub.request("R", 3, 0x05)
        self.assertEqual(err, 19)
        l1.close()

    def test_two_links_run_at_once_without_serialising(self):
        """The two-partition model links both C-brick L1s; their traffic
        interleaves and each must reach its own node and come back to its
        own link."""
        a, b = self.hub.l1(), self.hub.l1()
        self.hub.wait_log("attach slot 1")
        na, err, _ = self.hub.request("O", 0)
        self.assertEqual(err, 0)
        nb, err, _ = self.hub.request("O", 1)
        self.assertEqual(err, 0)
        for n in range(20):
            sa = lib.segment(28, code=100 + n)
            sb = lib.segment(36, code=200 + n)
            # Alternate the order of arrival and of reply.
            if n % 2:
                a.send_segment(sa)
                b.send_segment(sb)
            else:
                b.send_segment(sb)
                a.send_segment(sa)
            self.assertEqual(nb.recv(4096), sb)
            self.assertEqual(na.recv(4096), sa)
            if n % 3:
                nb.send(sb)
                na.send(sa)
            else:
                na.send(sa)
                nb.send(sb)
            self.assertEqual(a.recv_segment(), sa)
            self.assertEqual(b.recv_segment(), sb)
        na.close()
        nb.close()
        a.close()
        b.close()

    def test_a_node_that_does_not_read_blocks_only_its_slot(self):
        a, b = self.hub.l1(), self.hub.l1()
        self.hub.wait_log("attach slot 1")
        na, err, _ = self.hub.request("O", 0)
        self.assertEqual(err, 0)
        nb, err, _ = self.hub.request("O", 1)
        self.assertEqual(err, 0)
        # Slot 0's program never reads: flood it past the socket buffer.
        big = lib.segment(1000)
        for _ in range(2000):
            a.send_segment(big)
        self.hub.wait_log("queue full")
        seg = lib.segment(28, code=7)
        b.send_segment(seg)
        self.assertEqual(nb.recv(4096), seg)
        nb.send(seg)
        self.assertEqual(b.recv_segment(), seg)
        # Slot 0 still delivers, in order, once its program reads.
        self.assertEqual(na.recv(4096), big)
        for s in (na, nb, a, b):
            s.close()

    def test_a_link_that_does_not_read_blocks_only_its_slot(self):
        a, b = self.hub.l1(), self.hub.l1()
        self.hub.wait_log("attach slot 1")
        na, err, _ = self.hub.request("O", 0)
        nb, err2, _ = self.hub.request("O", 1)
        self.assertEqual((err, err2), (0, 0))
        # The link on slot 0 never reads; its program writes until the
        # hub's buffer for that link is full.
        big = lib.segment(1000)
        for _ in range(3000):
            na.send(big)
        self.hub.wait_log("the L1 is not reading")
        seg = lib.segment(28, code=9)
        nb.send(seg)
        self.assertEqual(b.recv_segment(), seg)
        for s in (na, nb, a, b):
            s.close()

    def test_bad_link_message_closes_the_link(self):
        l1 = self.hub.l1()
        self.hub.wait_log("attach slot 0")
        l1.sock.sendall(bytes([9, 0, 8]) + bytes(8))
        self.hub.wait_log("bad link message kind 9")
        self.hub.wait_log("detach slot 0")
        l1.close()


if __name__ == "__main__":
    HUB = sys.argv.pop(1)
    unittest.main()
