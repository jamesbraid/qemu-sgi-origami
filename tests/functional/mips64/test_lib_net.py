#!/usr/bin/env python3
#
# Unit tests for lib_net: the console driver's command/marker idiom, the
# ordered-progression scorer the OpenBSD verdicts are built from, and the
# wire-level DHCP counting that decides whether a lease really happened
#
# SPDX-License-Identifier: GPL-2.0-or-later
"""
A plain unittest -- no QEMU, no qemu_test import, runnable with just
`python3 tests/functional/mips64/test_lib_net.py`.

The DHCP and pcap cases came from the transfer test's own verdict module,
and the ordering and trip-wire cases from the login and install ones,
when the three modules' shared halves moved here. ConsoleTests plays a
scripted guest over a socketpair, the same way test_lib_prom.py's console
tests do.
"""

import os
import re
import socket
import struct
import tempfile
import threading
import unittest

from lib_net import (DHCP_NAMES, OPENBSD_LOGIN_STAGES, OPENBSD_TRIPWIRES,
                     Console, GuestFault,
                     HarnessError, IrixConsole, arp_neighbor_line,
                     arp_neighbor_mac,
                     arp_resolution, dhcp_kind, icmp_echo_exchange,
                     icmp_ipv4_exchange,
                     drive_irix_to_shell, lease_established, login_steps,
                     ordered_stages, summarise_pcap,
                     tftp_transfer, tftp_transfer_problems,
                     tftp_session_report,
                     transfer_result_pattern,
                     arp_request_frame, parse_arp_frame,
                     build_udp_fragments, parse_ipv4_udp_fragment,
                     reassemble_udp_fragments)
from test_irix_tg import tg_acceptance_verdict


# ---------------------------------------------------------------------------
# Fixture builders: the smallest Ethernet+IPv4+UDP+DHCP frame dhcp_kind()
# will read a message type out of, and a minimal .pcap file wrapping a
# list of them.

def _dhcp_frame(msg_type: int, sport: int = 68, dport: int = 67) -> bytes:
    """Every field dhcp_kind() does not itself inspect (MAC addresses, IP
    addresses, checksums, TTL) is a fixed placeholder; only the ethertype,
    IP protocol, UDP ports, DHCP magic cookie, and the option 53 (message
    type) TLV are meaningful."""
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
    """The 24-byte global header summarise_pcap() skips over, then one
    16-byte per-record header plus the frame itself for each frame."""
    header = (b"\xd4\xc3\xb2\xa1"
              + struct.pack("<HHIIII", 2, 4, 0, 0, 65535, 1))
    body = b"".join(
        struct.pack("<IIII", 0, 0, len(frame), len(frame)) + frame
        for frame in frames)
    return header + body


def _arp_frame(opcode, sender_mac, sender_ip, target_mac, target_ip,
               ethernet_dst):
    eth = ethernet_dst + sender_mac + b"\x08\x06"
    arp = (b"\x00\x01\x08\x00\x06\x04" + opcode.to_bytes(2, "big")
           + sender_mac + bytes(map(int, sender_ip.split(".")))
           + target_mac + bytes(map(int, target_ip.split("."))))
    return eth + arp


def _tftp_frame(opcode, src_port, dst_port, body=b''):
    payload = opcode.to_bytes(2, 'big') + body
    udp = (src_port.to_bytes(2, 'big') + dst_port.to_bytes(2, 'big')
           + (8 + len(payload)).to_bytes(2, 'big') + b'\x00\x00')
    src_ip, dst_ip = ((b'\x0a\x00\x02\x02', b'\x0a\x00\x02\x0f')
                      if src_port == 69 else
                      (b'\x0a\x00\x02\x0f', b'\x0a\x00\x02\x02'))
    ip = (b'\x45\x00' + (20 + len(udp) + len(payload)).to_bytes(2, 'big')
          + b'\x00\x00\x40\x00\x40\x11\x00\x00'
          + src_ip + dst_ip)
    return (b'\x52\x54\x00\xab\xcd\xef\x08\x00\x69\x12\x34\x56'
            + b'\x08\x00' + ip + udp + payload)


def _tftp_capture(data=b'file contents', filename=b'boot/sash64', error=False,
                  malformed_rrq=False, wrong_direction=False,
                  skip_data_block=False):
    mode = b'' if malformed_rrq else b'octet'
    frames = [_tftp_frame(1, 49152, 69,
                          filename + b'\x00' + mode + b'\x00padding')]
    blocks = [data[i:i + 512] for i in range(0, len(data), 512)]
    if not blocks or len(blocks[-1]) == 512:
        blocks.append(b'')
    for block, chunk in enumerate(blocks, 1):
        data_block = block + 1 if skip_data_block and block == 2 else block
        frames.append(_tftp_frame(3, 69, 49152,
                                  data_block.to_bytes(2, 'big') + chunk))
        frames.append(_tftp_frame(4, 49152, 69,
                                  block.to_bytes(2, 'big')))
    if error:
        frames.append(_tftp_frame(5, 69, 49152,
                                  b'\x00\x01missing\x00'))
    if wrong_direction:
        frames.extend((_tftp_frame(3, 49152, 69,
                                   b'\x00\x01unexpected'),
                       _tftp_frame(4, 69, 49152, b'\x00\x01')))
    return _pcap_bytes(frames)


def _icmp_ipv4_frame(src_ip, dst_ip, valid_checksum=True, icmp_type=8,
                     identifier=0, sequence=0, payload=b'\x00' * 8):
    eth = (b"\x52\x54\x00\x12\x34\x56\x52\x54\x00\xab\xcd\xef"
           + b"\x08\x00")
    header = bytearray(
        b"\x45\x00" + (28 + len(payload)).to_bytes(2, "big")
        + b"\x00\x00\x40\x00\x40\x01\x00\x00"
        + bytes(map(int, src_ip.split(".")))
        + bytes(map(int, dst_ip.split("."))))
    total = sum((header[i] << 8) | header[i + 1]
                for i in range(0, len(header), 2))
    while total >> 16:
        total = (total & 0xffff) + (total >> 16)
    checksum = (~total) & 0xffff
    header[10:12] = checksum.to_bytes(2, "big")
    if not valid_checksum:
        header[10] ^= 1
    icmp = (bytes([icmp_type, 0]) + b"\x00\x00"
            + identifier.to_bytes(2, "big")
            + sequence.to_bytes(2, "big") + payload)
    return eth + header + icmp


class DhcpKindTests(unittest.TestCase):

    def test_identifies_each_message_type(self):
        for code, name in DHCP_NAMES.items():
            self.assertEqual(dhcp_kind(_dhcp_frame(code)), name,
                             f"code {code}")

    def test_ignores_a_short_frame(self):
        self.assertIsNone(dhcp_kind(b"\x00" * 20))

    def test_ignores_non_ip_ethertype(self):
        frame = bytearray(_dhcp_frame(1))
        frame[12:14] = b"\x08\x06"  # ARP, not IPv4
        self.assertIsNone(dhcp_kind(bytes(frame)))

    def test_ignores_non_udp_protocol(self):
        frame = bytearray(_dhcp_frame(1))
        frame[23] = 6  # TCP, not UDP
        self.assertIsNone(dhcp_kind(bytes(frame)))

    def test_ignores_udp_not_on_the_dhcp_ports(self):
        self.assertIsNone(dhcp_kind(_dhcp_frame(1, sport=12345, dport=53)))


class SummarisePcapTests(unittest.TestCase):

    def test_counts_a_full_handshake(self):
        raw = _pcap_bytes([
            _dhcp_frame(1), _dhcp_frame(2, sport=67, dport=68),
            _dhcp_frame(3), _dhcp_frame(5, sport=67, dport=68)])

        summary = summarise_pcap(raw)

        self.assertEqual(summary["dhcp"], {
            "DHCPDISCOVER": 1, "DHCPOFFER": 1, "DHCPREQUEST": 1,
            "DHCPACK": 1,
        })
        self.assertTrue(lease_established(summary["dhcp"]))

    def test_counts_frames_by_who_sent_them(self):
        raw = _pcap_bytes([_dhcp_frame(1), _dhcp_frame(3)])

        self.assertEqual(summarise_pcap(raw)["by_src"],
                         {"52:54:00:ab:cd:ef": 2})

    def test_a_missing_ack_is_not_a_lease(self):
        raw = _pcap_bytes([
            _dhcp_frame(1), _dhcp_frame(2, sport=67, dport=68),
            _dhcp_frame(3)])

        self.assertFalse(lease_established(summarise_pcap(raw)["dhcp"]))

    def test_bad_magic_returns_none(self):
        self.assertIsNone(summarise_pcap(b"NOTAPCAP" + b"\x00" * 16))

    def test_truncated_packet_record_returns_none(self):
        self.assertIsNone(summarise_pcap(_pcap_bytes([_dhcp_frame(1)])[:-1]))

    def test_too_short_returns_empty(self):
        self.assertEqual(summarise_pcap(b"\x00" * 10), {})

    def test_lease_requires_all_four_messages(self):
        self.assertFalse(lease_established({}))
        self.assertFalse(lease_established({"DHCPDISCOVER": 1,
                                            "DHCPOFFER": 1,
                                            "DHCPREQUEST": 1}))
        self.assertTrue(lease_established({
            "DHCPDISCOVER": 1, "DHCPOFFER": 1, "DHCPREQUEST": 1,
            "DHCPACK": 1,
        }))


class ArpResolutionTests(unittest.TestCase):

    def setUp(self):
        self.guest_mac = bytes.fromhex("525400abcdef")
        self.gateway_mac = bytes.fromhex("525400123456")
        self.request = _arp_frame(
            1, self.guest_mac, "10.0.2.15", bytes(6), "10.0.2.2",
            b"\xff" * 6)
        self.reply = _arp_frame(
            2, self.gateway_mac, "10.0.2.2", self.guest_mac,
            "10.0.2.15", self.guest_mac)

    def test_matches_request_and_reply_for_the_requested_neighbor(self):
        result = arp_resolution([self.request, self.reply],
                                "10.0.2.15", "10.0.2.2")

        self.assertEqual(result, {
            "requests": 1,
            "replies": 1,
            "matching_replies": 1,
            "responder_mac": "52:54:00:12:34:56",
            "resolved": True,
        })

    def test_unrelated_or_malformed_arp_does_not_resolve_neighbor(self):
        unrelated = _arp_frame(
            2, self.gateway_mac, "10.0.2.2", self.guest_mac,
            "10.0.2.15", self.guest_mac)

        result = arp_resolution([unrelated, b"\x00" * 41],
                                "10.0.2.15", "10.0.2.2")

        self.assertFalse(result["resolved"])
        self.assertEqual(result["matching_replies"], 0)

    def test_reads_only_a_resolved_unicast_neighbor(self):
        self.assertEqual(arp_neighbor_mac(
            "? (10.0.2.2) at 52:54:00:12:34:56",
            "10.0.2.2"), "52:54:00:12:34:56")
        self.assertEqual(arp_neighbor_mac(
            "? (10.0.2.2) at 52:55:a:0:2:2", "10.0.2.2"),
            "52:55:0a:00:02:02")
        self.assertIsNone(arp_neighbor_mac("? (10.0.2.2) at 00:00:00:00:00:00",
                                           "10.0.2.2"))

    def test_cold_check_detects_an_incomplete_neighbor_entry(self):
        output = "? (10.0.2.2) at (incomplete)"

        self.assertEqual(arp_neighbor_line(output, "10.0.2.2"), output)
        self.assertIsNone(arp_neighbor_mac(output, "10.0.2.2"))


class ArpFrameTests(unittest.TestCase):

    def test_request_encoding_and_reply_fields(self):
        peer_mac = bytes.fromhex("02005d270002")
        guest_mac = bytes.fromhex("02005d270001")
        request = arp_request_frame(peer_mac, "10.0.2.2", "10.0.2.15")

        self.assertEqual(len(request), 42)
        self.assertEqual(request[:6], b"\xff" * 6)
        self.assertEqual(request[6:14], peer_mac + b"\x08\x06")
        self.assertEqual(parse_arp_frame(request), {
            "ethernet_dst": b"\xff" * 6,
            "ethernet_src": peer_mac,
            "opcode": 1,
            "sender_mac": peer_mac,
            "sender_ip": "10.0.2.2",
            "target_mac": b"\x00" * 6,
            "target_ip": "10.0.2.15",
        })

        reply = _arp_frame(2, guest_mac, "10.0.2.15", peer_mac,
                           "10.0.2.2", peer_mac)
        self.assertEqual(parse_arp_frame(reply), {
            "ethernet_dst": peer_mac,
            "ethernet_src": guest_mac,
            "opcode": 2,
            "sender_mac": guest_mac,
            "sender_ip": "10.0.2.15",
            "target_mac": peer_mac,
            "target_ip": "10.0.2.2",
        })

    def test_parser_rejects_non_arp_and_truncated_frames(self):
        self.assertIsNone(parse_arp_frame(b"\x00" * 42))
        self.assertIsNone(parse_arp_frame(b"\xff" * 41))


class UdpFragmentTests(unittest.TestCase):

    def setUp(self):
        self.src_mac = bytes.fromhex("525400abcdef")
        self.dst_mac = bytes.fromhex("525400123456")
        self.src_ip = "10.0.2.15"
        self.dst_ip = "10.0.2.2"
        self.payload = bytes(range(256)) * 9 + bytes(range(244))
        self.frames = build_udp_fragments(
            self.src_mac, self.dst_mac, self.src_ip, self.dst_ip,
            49152, 9000, self.payload)

    def test_builds_and_reassembles_a_2548_byte_udp_payload(self):
        self.assertEqual(len(self.payload), 2548)  # 9*256 + 244
        parsed = [parse_ipv4_udp_fragment(frame) for frame in self.frames]
        self.assertEqual(len(self.frames), 2)
        self.assertEqual([fragment["offset"] for fragment in parsed], [0, 185])
        self.assertEqual([fragment["more"] for fragment in parsed],
                         [True, False])
        self.assertEqual(reassemble_udp_fragments(
            self.frames, self.src_ip, self.dst_ip, 49152, 9000,
            self.payload), 2)

    def test_rejects_an_ipv4_mtu_below_68(self):
        with self.assertRaisesRegex(ValueError, "MTU"):
            build_udp_fragments(
                self.src_mac, self.dst_mac, self.src_ip, self.dst_ip,
                49152, 9000, self.payload, mtu=67)

    def test_rejects_a_gap(self):
        second = bytearray(self.frames[1])
        second[20:22] = (186).to_bytes(2, "big")
        second[24:26] = b"\x00\x00"
        second[24:26] = _internet_checksum_for_test(
            second[14:34]).to_bytes(2, "big")
        with self.assertRaisesRegex(ValueError, "gap"):
            reassemble_udp_fragments(
                [self.frames[0], bytes(second)], self.src_ip, self.dst_ip,
                49152, 9000, self.payload)

    def test_rejects_overlap(self):
        second = bytearray(self.frames[1])
        second[20:22] = (184).to_bytes(2, "big")
        second[24:26] = b"\x00\x00"
        second[24:26] = _internet_checksum_for_test(
            second[14:34]).to_bytes(2, "big")
        with self.assertRaisesRegex(ValueError, "overlap"):
            reassemble_udp_fragments(
                [self.frames[0], bytes(second)], self.src_ip, self.dst_ip,
                49152, 9000, self.payload)

    def test_rejects_duplicate_after_complete_set(self):
        with self.assertRaisesRegex(ValueError, "overlap"):
            reassemble_udp_fragments(
                self.frames + [self.frames[1]], self.src_ip, self.dst_ip,
                49152, 9000, self.payload)

    def test_rejects_corrupt_ipv4_checksum(self):
        corrupt = bytearray(self.frames[0])
        corrupt[24] ^= 1
        with self.assertRaisesRegex(ValueError, "checksum"):
            parse_ipv4_udp_fragment(bytes(corrupt))

    def test_rejects_wrong_payload_and_udp_port(self):
        with self.assertRaisesRegex(ValueError, "payload"):
            reassemble_udp_fragments(
                self.frames, self.src_ip, self.dst_ip, 49152, 9000,
                self.payload + b"x")
        wrong_port = bytearray(self.frames[0])
        wrong_port[34:36] = (49153).to_bytes(2, "big")
        with self.assertRaisesRegex(ValueError, "UDP ports"):
            reassemble_udp_fragments(
                [bytes(wrong_port), self.frames[1]], self.src_ip, self.dst_ip,
                49152, 9000, self.payload)

    def test_rejects_a_mismatched_udp_length(self):
        first = bytearray(self.frames[0])
        first[38:40] = (2555).to_bytes(2, "big")
        with self.assertRaisesRegex(ValueError, "UDP length"):
            reassemble_udp_fragments(
                [bytes(first), self.frames[1]], self.src_ip, self.dst_ip,
                49152, 9000, self.payload)

    def test_ignores_unrelated_frames(self):
        self.assertIsNone(parse_ipv4_udp_fragment(b"\x00" * 40))
        arp = b"\x00" * 12 + b"\x08\x06" + b"\x00" * 28
        self.assertIsNone(parse_ipv4_udp_fragment(arp))
        tcp = bytearray(self.frames[0])
        tcp[23] = 6
        tcp[24:26] = b"\x00\x00"
        tcp[24:26] = _internet_checksum_for_test(tcp[14:34]).to_bytes(2, "big")
        self.assertIsNone(parse_ipv4_udp_fragment(bytes(tcp)))
        with self.assertRaisesRegex(ValueError, "fewer than two"):
            reassemble_udp_fragments(
                [arp], self.src_ip, self.dst_ip, 49152, 9000, self.payload)


def _internet_checksum_for_test(header):
    if len(header) % 2:
        header += b"\x00"
    total = sum(int.from_bytes(header[i:i + 2], "big")
                for i in range(0, len(header), 2))
    while total >> 16:
        total = (total & 0xffff) + (total >> 16)
    return (~total) & 0xffff


class IrixTgVerdictTests(unittest.TestCase):

    def setUp(self):
        self.traffic = {
            "ipkts_before": 0,
            "ipkts_after": 1,
            "opkts_before": 0,
            "opkts_after": 1,
            "ierrs_after": 0,
            "arp_cache_cold": True,
            "arp_requests": 1,
            "arp_replies": 1,
            "arp_matching_replies": 1,
            "arp_neighbor_mac": "52:54:00:12:34:56",
            "arp_responder_mac": "52:54:00:12:34:56",
            "icmp_guest_to_gateway": 1,
            "icmp_gateway_to_guest": 1,
            "icmp_invalid_packets": 0,
            "icmp_echo_matching_replies": 1,
        }
        self.tg_flags = {"L2IPFRAG", "L2TCPSEG"}
        self.board = {
            "bare_clean": True,
            "saved_status": True,
            "saved": {
                "autoneg": "1",
                "link_pref": "AUTO",
                "flow_ctrl": "1",
            },
            "check_mode": True,
            "options_status": True,
            "options": {
                "autoneg": "1",
                "link_pref": "AUTO",
                "flow_ctrl": "1",
            },
        }

    def verdict(self, traffic):
        return tg_acceptance_verdict(
            True, True, True, True, self.tg_flags, self.board, traffic)

    def test_requires_the_existing_traffic_verdict_and_arp_resolution(self):
        report = self.verdict(self.traffic)

        self.assertEqual(report, {"verdict": "PASS", "problems": []})

    def test_requires_complete_tgconfig_readback_and_offload_flags(self):
        self.board["options"].pop("flow_ctrl")
        self.tg_flags.remove("L2TCPSEG")

        report = self.verdict(self.traffic)

        self.assertEqual(report["verdict"], "FAIL")
        self.assertTrue(any("tgconfig -g" in problem
                            for problem in report["problems"]))
        self.assertTrue(any("L2IPFRAG or L2TCPSEG" in problem
                            for problem in report["problems"]))

    def test_rejects_a_non_cold_cache_or_missing_wire_exchange(self):
        traffic = dict(self.traffic, arp_cache_cold=False,
                       arp_matching_replies=0)

        report = self.verdict(traffic)

        self.assertEqual(report["verdict"], "FAIL")
        self.assertEqual(len(report["problems"]), 2)

    def test_rejects_guest_neighbor_mac_that_differs_from_the_reply(self):
        traffic = dict(self.traffic, arp_responder_mac="52:54:00:aa:bb:cc")

        report = self.verdict(traffic)

        self.assertEqual(report["verdict"], "FAIL")
        self.assertEqual(len(report["problems"]), 1)

    def test_reports_an_unreadable_packet_capture(self):
        traffic = dict(self.traffic, arp_capture_problem="file missing")

        report = self.verdict(traffic)

        self.assertEqual(report["verdict"], "FAIL")
        self.assertTrue(any("file missing" in problem
                            for problem in report["problems"]))

    def test_requires_bidirectional_icmp_with_valid_ipv4_checksums(self):
        traffic = dict(self.traffic, icmp_guest_to_gateway=0,
                       icmp_invalid_packets=1)

        report = self.verdict(traffic)

        self.assertEqual(report["verdict"], "FAIL")
        self.assertEqual(len(report["problems"]), 2)

    def test_requires_a_matched_echo_payload(self):
        traffic = dict(self.traffic, icmp_echo_matching_replies=0)

        report = self.verdict(traffic)

        self.assertEqual(report["verdict"], "FAIL")
        self.assertTrue(any("sequence and exact payload" in problem
                            for problem in report["problems"]))


class IcmpIpv4ExchangeTests(unittest.TestCase):

    def test_counts_only_the_pair_and_checks_both_directions(self):
        summary = icmp_ipv4_exchange([
            _icmp_ipv4_frame("10.0.2.15", "10.0.2.2"),
            _icmp_ipv4_frame("10.0.2.2", "10.0.2.15"),
            _icmp_ipv4_frame("10.0.2.15", "224.0.0.1"),
        ], "10.0.2.15", "10.0.2.2")

        self.assertEqual(summary, {
            "guest_to_gateway": 1,
            "gateway_to_guest": 1,
            "invalid_packets": 0,
        })

    def test_reports_invalid_header_checksum_on_matching_traffic(self):
        summary = icmp_ipv4_exchange([
            _icmp_ipv4_frame("10.0.2.15", "10.0.2.2", False),
        ], "10.0.2.15", "10.0.2.2")

        self.assertEqual(summary["guest_to_gateway"], 1)
        self.assertEqual(summary["invalid_packets"], 1)

    def test_counts_a_matching_malformed_header_as_invalid(self):
        frame = bytearray(
            _icmp_ipv4_frame("10.0.2.15", "10.0.2.2"))
        frame[14] = 0x44  # IHL claims a 16-byte header.

        summary = icmp_ipv4_exchange(
            [bytes(frame)], "10.0.2.15", "10.0.2.2")

        self.assertEqual(summary["guest_to_gateway"], 1)
        self.assertEqual(summary["invalid_packets"], 1)


class IcmpEchoExchangeTests(unittest.TestCase):

    def test_matches_sequence_and_exact_payload_but_ignores_identifier(self):
        payload = b"timestamp and data"
        result = icmp_echo_exchange([
            _icmp_ipv4_frame("10.0.2.15", "10.0.2.2", sequence=3,
                             identifier=0x040f, payload=payload),
            _icmp_ipv4_frame("10.0.2.2", "10.0.2.15", icmp_type=0,
                             sequence=3, identifier=0x0001, payload=payload),
        ], "10.0.2.15", "10.0.2.2")

        self.assertEqual(result, {
            "requests": 1, "replies": 1, "matching_replies": 1,
        })

    def test_rejects_mismatched_sequence_payload_and_unrelated_frames(self):
        request = _icmp_ipv4_frame(
            "10.0.2.15", "10.0.2.2", sequence=3, payload=b"request")
        wrong_sequence = _icmp_ipv4_frame(
            "10.0.2.2", "10.0.2.15", icmp_type=0, sequence=4,
            payload=b"request")
        wrong_payload = _icmp_ipv4_frame(
            "10.0.2.2", "10.0.2.15", icmp_type=0, sequence=3,
            payload=b"changed")
        unrelated = _icmp_ipv4_frame(
            "10.0.2.15", "224.0.0.1", sequence=3, payload=b"request")

        result = icmp_echo_exchange(
            [request, wrong_sequence, wrong_payload, unrelated, b"bad"],
            "10.0.2.15", "10.0.2.2")

        self.assertEqual(result, {
            "requests": 1, "replies": 2, "matching_replies": 0,
        })

class TftpTransferTests(unittest.TestCase):

    def test_unreadable_capture_reports_parse_error(self):
        report = tftp_session_report(None, 'boot/sash64', 9)

        self.assertIsNone(report['complete_session'])
        self.assertEqual(report['problems'],
                         ['the TFTP packet capture is unreadable'])

    def test_complete_transfer_matches_file_size_and_path(self):
        raw = _tftp_capture(b'123456789')
        transfer = tftp_transfer(raw)[0]

        self.assertEqual(transfer['filename'], 'boot/sash64')
        self.assertEqual(transfer['data_blocks'], [1])
        self.assertEqual(transfer['ack_blocks'], [1])
        self.assertEqual(transfer['data_bytes'], 9)
        self.assertEqual(transfer['final_data_bytes'], 9)
        self.assertEqual(
            tftp_transfer_problems(transfer, 'boot/sash64', 9), [])

    def test_reports_progression_size_path_and_error_problems(self):
        transfer = tftp_transfer(_tftp_capture(error=True))[0]

        problems = tftp_transfer_problems(transfer, 'wrong/path', 99)

        self.assertTrue(any('requested' in problem for problem in problems))
        self.assertTrue(any('bytes' in problem for problem in problems))
        self.assertTrue(any('ERROR' in problem for problem in problems))

    def test_rejects_a_skipped_data_block_from_the_capture(self):
        transfer = tftp_transfer(_tftp_capture(
            b'x' * 513, skip_data_block=True))[0]

        self.assertEqual(transfer['data_blocks'], [1, 3])
        self.assertTrue(any(
            'DATA progression' in problem
            for problem in tftp_transfer_problems(
                transfer, 'boot/sash64', 513)))

    def test_exact_block_multiple_ends_with_empty_data_packet(self):
        transfer = tftp_transfer(_tftp_capture(b'x' * 512))[0]

        self.assertEqual(transfer['data_blocks'], [1, 2])
        self.assertEqual(transfer['ack_blocks'], [1, 2])
        self.assertEqual(transfer['data_bytes'], 512)
        self.assertEqual(transfer['final_data_bytes'], 0)
        self.assertEqual(
            tftp_transfer_problems(transfer, 'boot/sash64', 512), [])

    def test_rejects_an_rrq_without_a_mode(self):
        transfer = tftp_transfer(_tftp_capture(malformed_rrq=True))[0]

        self.assertFalse(transfer['request_seen'])
        self.assertTrue(any(
            'request was not captured' in problem
            for problem in tftp_transfer_problems(transfer, 'boot/sash64', 13)))

    def test_rejects_data_and_ack_in_the_wrong_direction(self):
        transfer = tftp_transfer(_tftp_capture(wrong_direction=True))[0]

        self.assertEqual(transfer['direction_errors'], 2)
        self.assertTrue(any(
            'wrong direction' in problem
            for problem in tftp_transfer_problems(
                transfer, 'boot/sash64', len(b'file contents'))))

    def test_sessions_isolate_complete_and_partial_repeated_requests(self):
        frames = []
        for port, partial in ((49152, False), (49152, True),
                              (49153, True)):
            frames.append(_tftp_frame(
                1, port, 69, b'boot/sash64\x00octet\x00'))
            data = b'x' * (512 if partial else 9)
            frames.append(_tftp_frame(3, 69, port, b'\x00\x01' + data))
            frames.append(_tftp_frame(4, port, 69, b'\x00\x01'))
            if partial:
                frames.append(_tftp_frame(
                    3, 69, port, b'\x00\x02' + b'x' * 512))
                # The client aborts after another full DATA block.
                frames.append(_tftp_frame(
                    5, port, 69, b'\x00\x03partial\x00'))

        sessions = tftp_transfer(_pcap_bytes(frames))
        report = tftp_session_report(sessions, 'boot/sash64', 9)

        self.assertEqual(len(sessions), 3)
        self.assertEqual(report['complete_session'], 1)
        self.assertEqual(report['count'], 3)
        self.assertTrue(report['sessions'][0]['complete'])
        self.assertFalse(report['sessions'][0]['partial'])
        self.assertFalse(report['sessions'][1]['complete'])
        self.assertTrue(report['sessions'][1]['partial'])
        self.assertEqual(report['sessions'][1]['error_packets'], 1)
        self.assertEqual(report['sessions'][1]['data_blocks'], 2)
        self.assertTrue(report['sessions'][2]['partial'])
        self.assertEqual(report['sessions'][2]['error_packets'], 1)
        self.assertEqual(report['problems'], [])


# The network boot asserts these stages before checking transfers.
LOGIN_STAGES = OPENBSD_LOGIN_STAGES

# The install run's own progression, shortened to the three stages that
# exercise ordering: a banner, a prompt that must occur twice, and a
# completion line after the second occurrence.
INSTALL_STAGES = (
    ("never reached the installer's welcome banner",
     re.compile(r"Welcome to the OpenBSD/sgi 6\.9 installation program\.")),
    ("never reached the first Location of sets prompt",
     re.compile(r"Location of sets\?")),
    ("never reached the second Location of sets prompt",
     re.compile(r"Location of sets\?")),
    ("never reached the CONGRATULATIONS completion banner",
     re.compile(r"CONGRATULATIONS! Your OpenBSD install has been "
                r"successfully completed")),
)

A_LOGIN = ("root device: sd0a\r\nswap device (default sd0b): \r\n"
           "login: root\r\nPassword:\r\nLast login: Mon\r\n# ")


class OrderedStagesTests(unittest.TestCase):

    def test_a_complete_progression_is_a_pass(self):
        self.assertEqual(
            ordered_stages(A_LOGIN, LOGIN_STAGES, OPENBSD_TRIPWIRES), [])

    def test_automatic_root_selection_is_a_pass(self):
        transcript = ("root on sd0a\r\nlogin: root\r\nPassword:\r\n"
                      "Last login: Mon\r\n# ")
        self.assertEqual(
            ordered_stages(transcript, LOGIN_STAGES, OPENBSD_TRIPWIRES), [])

    def test_only_the_first_stage_not_reached_is_named(self):
        # A run that hung right after a correct password, before the shell
        # prompt printed, fails on the shell -- and says nothing about the
        # stages after it, which were unreachable anyway.
        transcript = ("root device: sd0a\r\nlogin: root\r\nPassword:\r\n"
                      "Last login: Mon\r\n")

        self.assertEqual(
            ordered_stages(transcript, LOGIN_STAGES, OPENBSD_TRIPWIRES),
            ["never reached a root shell prompt"])

    def test_a_later_marker_without_an_earlier_one_does_not_count(self):
        # A "login:" with no root selection first is not a run
        # that reached that stage.
        self.assertEqual(
            ordered_stages("login: root\r\nPassword:\r\n# \r\n",
                           LOGIN_STAGES),
            ["never selected the root device"])

    def test_a_marker_before_its_turn_does_not_satisfy_its_stage(self):
        # A bare "# " before "login:" is not an earlier shell: the search
        # for it starts only after the login prompt matched.
        self.assertEqual(
            ordered_stages("root device: sd0a\r\n# \r\nlogin: root\r\n",
                           LOGIN_STAGES),
            ["never reached a root shell prompt"])

    def test_a_repeated_stage_needs_its_own_occurrence(self):
        # One "Location of sets?" cannot satisfy both the first and the
        # second occurrence of that stage.
        transcript = (
            "Welcome to the OpenBSD/sgi 6.9 installation program.\r\n"
            "Location of sets? (disk http nfs or 'done') [http] disk\r\n"
            "CONGRATULATIONS! Your OpenBSD install has been successfully "
            "completed!\r\n")

        self.assertEqual(
            ordered_stages(transcript, INSTALL_STAGES),
            ["never reached the second Location of sets prompt"])

    def test_a_completion_line_before_its_turn_does_not_count(self):
        transcript = (
            "Welcome to the OpenBSD/sgi 6.9 installation program.\r\n"
            "Location of sets? [http] disk\r\n"
            "CONGRATULATIONS! Your OpenBSD install has been successfully "
            "completed!\r\n"
            "Location of sets? [done] done\r\n")

        self.assertEqual(
            ordered_stages(transcript, INSTALL_STAGES),
            ["never reached the CONGRATULATIONS completion banner"])

    def test_an_empty_transcript_fails_on_the_first_stage(self):
        self.assertEqual(ordered_stages("", LOGIN_STAGES),
                         ["never selected the root device"])

    def test_a_wedge_before_the_end_fails_and_is_named(self):
        # The property this scorer exists for: a guest that fell into its
        # kernel debugger before it ever reached a login must FAIL, even
        # though on timing alone it never got far enough to fail anything
        # else first.
        transcript = ("root device: sd0a\r\n"
                      "Trap cause = 7 Frame 0xffffffff82b2f9c8\r\nddb> ")

        problems = ordered_stages(transcript, LOGIN_STAGES,
                                  OPENBSD_TRIPWIRES)

        self.assertEqual(problems, [
            "the guest fell into its kernel debugger",
            "the guest took a bus error",
            "never reached the login prompt",
        ])

    def test_a_wedge_is_not_laundered_by_what_the_guest_prints_later(self):
        # "# " does eventually appear, after the panic. The run still
        # fails, because the trip wire fired before the shell did.
        transcript = ("root device: sd0a\r\nlogin: root\r\nPassword:\r\n"
                      "panic: kernel diagnostic assertion failed\r\nddb> # ")

        self.assertEqual(
            ordered_stages(transcript, LOGIN_STAGES, OPENBSD_TRIPWIRES),
            ["the guest fell into its kernel debugger", "the guest panicked"])

    def test_a_wedge_after_the_last_stage_is_still_a_pass(self):
        # The run already proved what it set out to prove; a debugger
        # prompt during whatever happens afterwards -- a post-install
        # reboot attempt, say -- does not retroactively fail it. Trip
        # wires are checked only up to where the last stage began.
        self.assertEqual(
            ordered_stages(A_LOGIN + "\r\nddb> ", LOGIN_STAGES,
                           OPENBSD_TRIPWIRES),
            [])

    def test_without_tripwires_only_the_progression_is_judged(self):
        transcript = A_LOGIN.replace("Last login: Mon", "panic: nope")

        self.assertEqual(ordered_stages(transcript, LOGIN_STAGES), [])


class LoginStepsTests(unittest.TestCase):

    NETBOOT = ((r"Option\?", "5\n"), (r">>", "boot -f bootp()/bsd.IP27\n"))

    def test_the_netboot_dialogue_comes_first_unchanged(self):
        steps = login_steps(self.NETBOOT, "origami123")

        self.assertEqual(steps[:len(self.NETBOOT)], self.NETBOOT)

    def test_login_questions_follow_the_netboot_dialogue(self):
        steps = login_steps(self.NETBOOT, "origami123")

        self.assertEqual(steps[len(self.NETBOOT):], (
            (r"login:", "root\n"),
            (r"[Pp]assword:", "origami123\n"),
        ))

    def test_as_bytes_encodes_only_the_patterns(self):
        steps = login_steps(self.NETBOOT, "origami123", as_bytes=True)

        for pattern, response in steps:
            self.assertIsInstance(pattern, bytes)
            self.assertIsInstance(response, str)
        self.assertEqual(steps[0], (rb"Option\?", "5\n"))


class _ScriptedGuest(threading.Thread):
    """Reads what the driver sends and answers each command with the
    scripted reply, so the console's marker handling can be exercised
    without a guest."""

    def __init__(self, sock, replies):
        super().__init__(daemon=True)
        self.sock = sock
        self.replies = list(replies)
        self.seen = []

    def run(self):
        self.sock.settimeout(5.0)
        pending = b""
        for reply in self.replies:
            while b"\n" not in pending:
                try:
                    chunk = self.sock.recv(4096)
                except OSError:
                    return
                if not chunk:
                    return
                pending += chunk
            line, pending = pending.split(b"\n", 1)
            self.seen.append(line)
            # The guest's shell echoes the command line back before it
            # runs: the marker must survive that echo.
            self.sock.sendall(line + b"\r\n" + reply)


class _TestConsole(Console):
    # Console.TRIPWIRES pairs a compiled BYTES pattern with its message,
    # the other way round from the str-pattern table the verdicts use.
    TRIPWIRES = tuple((re.compile(rx.pattern.encode()), why)
                      for why, rx in OPENBSD_TRIPWIRES)
    STEP_TIMEOUT = 5


class ConsoleTests(unittest.TestCase):

    def setUp(self):
        self.driver_end, self.guest_end = socket.socketpair()
        self.addCleanup(self.driver_end.close)
        self.addCleanup(self.guest_end.close)
        log_dir = tempfile.TemporaryDirectory()
        self.addCleanup(log_dir.cleanup)
        self.log_path = os.path.join(log_dir.name, "console.log")

    def _console(self, replies, exited=None):
        guest = _ScriptedGuest(self.guest_end, replies)
        guest.start()
        console = _TestConsole(self.driver_end, self.log_path, exited=exited)
        self.addCleanup(console.close)
        return console, guest

    def test_run_returns_the_commands_exit_code(self):
        console, guest = self._console([b"MARK-probe-3\r\n"])

        self.assertEqual(console.run("false", "probe"), 3)
        self.assertEqual(guest.seen[0],
                         b'false; echo "MARK""-probe-$?"')

    def test_the_shells_echo_of_the_command_does_not_match_the_marker(self):
        # The command line itself contains MARK, split across a quote.
        # Only the shell's own expansion produces a matching marker, so
        # the echo above cannot end the wait early with the wrong code.
        console, _guest = self._console([b"MARK-probe-0\r\n"])

        self.assertEqual(console.run("true", "probe"), 0)

    def test_capture_value_reads_only_this_commands_own_output(self):
        console, _guest = self._console([
            b"N=1\r\nMARK-first-0\r\n",
            b"N=2\r\nMARK-second-0\r\n",
        ])

        console.capture_value("echo N=1", "first", rb"N=(\d+)")

        self.assertEqual(
            console.capture_value("echo N=2", "second", rb"N=(\d+)"), "2")

    def test_two_values_pulls_a_digest_and_a_size_together(self):
        console, _guest = self._console([
            b"abc123 65536 /tmp/down.bin\r\nMARK-cksum-0\r\n"])

        digest, size = console.two_values(
            "cksum /tmp/down.bin", "cksum",
            rb"(\w+)\s+(\d+)\s+/tmp/down\.bin")

        self.assertEqual((digest, size), ("abc123", 65536))

    def test_transfer_result_waits_for_the_full_fragmented_byte_count(self):
        console = _TestConsole(self.driver_end, self.log_path)
        self.addCleanup(console.close)
        digest = b"a" * 64
        pattern = transfer_result_pattern(b"IN")

        self.guest_end.sendall(b"IN " + digest + b" 6")
        console._pump()
        self.assertIsNone(re.search(pattern, console.buf))

        self.guest_end.sendall(b"5536\r\n")
        match = console.expect(pattern, 1, what="the inbound digest")
        self.assertEqual(match.groups(), (digest, b"65536"))

    def test_output_that_does_not_match_is_a_harness_error(self):
        console, _guest = self._console([b"nothing useful\r\nMARK-x-0\r\n"])

        with self.assertRaises(HarnessError):
            console.capture_value("echo", "x", rb"N=(\d+)")

    def test_expect_from_an_offset_ignores_earlier_output(self):
        # After a reboot the previous boot's login prompt is still in the
        # buffer; a wait from the new boot's offset must not match it.
        console, _guest = self._console([b"login: MARK-first-0\r\n",
                                         b"MARK-second-0\r\n"])
        console.run("boot one", "first")
        console.run("boot two", "second")

        self.assertIsNotNone(console.expect(rb"login:", 1))
        with self.assertRaises(HarnessError):
            console.expect(rb"login:", 1, start=len(console.buf))

    def test_a_tripwire_while_waiting_is_a_guest_fault(self):
        console, _guest = self._console([b"panic: it fell over\r\n"])

        with self.assertRaises(GuestFault) as caught:
            console.run("uname", "probe", timeout=3)
        self.assertIn("the guest panicked", str(caught.exception))

    def test_a_timeout_is_a_harness_error_not_a_guest_fault(self):
        console, _guest = self._console([b"still thinking\r\n"])

        with self.assertRaises(HarnessError) as caught:
            console.run("uname", "probe", timeout=1)
        self.assertNotIsInstance(caught.exception, GuestFault)
        self.assertIn("timed out", str(caught.exception))

    def test_an_exited_emulator_ends_the_wait_without_burning_the_budget(self):
        # A run whose emulator is already gone must not sit out the whole
        # timeout: there is nothing further the console can ever receive.
        console, _guest = self._console([], exited=lambda: True)

        with self.assertRaises(HarnessError) as caught:
            console.run("uname", "probe", timeout=600)
        self.assertIn("the emulator exited", str(caught.exception))

    def test_every_byte_received_lands_in_the_log(self):
        console, _guest = self._console([b"hello\r\nMARK-probe-0\r\n"])
        console.run("echo hello", "probe")
        console.close()

        with open(self.log_path, "rb") as log:
            self.assertIn(b"hello", log.read())


    def test_irix_login_drives_the_prom_and_enters_a_bourne_shell(self):
        guest = _ScriptedGuest(self.guest_end, [
            b"login:", b"TERM = (vt100)", b"1# ", b"",
            b"MARK-shinit-0\r\n",
        ])
        guest.start()
        self.guest_end.sendall(b"Option? ")
        console = IrixConsole(self.driver_end, self.log_path)
        self.addCleanup(console.close)

        drive_irix_to_shell(console, budget=30)

        self.assertEqual(IrixConsole.STEP_TIMEOUT, 300)
        self.assertEqual(guest.seen, [
            b"1", b"root", b"", b"exec /bin/sh",
            b'PATH=/usr/sbin:/usr/bin:/sbin:/bin:/usr/etc:/usr/bsd:$PATH; '
            b'export PATH; echo "MARK""-shinit-$?"',
        ])

    def test_irix_console_reports_a_prom_exception(self):
        console = IrixConsole(self.driver_end, self.log_path)
        self.addCleanup(console.close)
        self.guest_end.sendall(b"Exception <bus error>\r\n")

        with self.assertRaisesRegex(GuestFault, "PROM took an exception"):
            console.expect(rb"login:", timeout=1)


if __name__ == "__main__":
    unittest.main()
