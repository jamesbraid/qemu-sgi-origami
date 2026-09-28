#!/usr/bin/env python3
#
# Bidirectional fragmented UDP on an IRIX eg adapter.
#
# SPDX-License-Identifier: GPL-2.0-or-later
"""Require eg0 to reassemble a peer datagram and transmit one UDP datagram."""

import os
import re
import socket
import subprocess
import time

from qemu_test import QemuSystemTest, skipBigDataTest, skipIfMissingEnv
from qemu_test import skipSlowTest
from qemu_test.asset import LocalAsset

from lib_net import (IrixConsole, build_udp_fragments, drive_irix_to_shell,
                     parse_ipv4_udp_fragment, reassemble_udp_fragments)
from lib_prom import FIRMWARE_REL, MACHINES, file_sha256

RELEASE = "6.5.30"
DISK_REL = os.path.join("irix", RELEASE, "root.qcow2")
PROM_SHA256 = (
    "c2c69bf33c375b43ec7ac511fab41ddf595be1b6838ae717e58a9a1273437fdd")
DISK_SHA256 = (
    "15b4bc1992845b6242948a9e944ce39682b7decdaee1e5474579418fb3a19c12")
GUEST_IP = "10.0.2.15"
PEER_IP = "10.0.2.2"
GUEST_MAC = bytes.fromhex("02005d270001")
PEER_MAC = bytes.fromhex("02005d270002")
GUEST_TX_PORT = 51100
PEER_RX_PORT = 51101
PEER_TX_PORT = 51102
GUEST_RX_PORT = 51103
PAYLOAD_LENGTH = 2012
GUEST_PAYLOAD = bytes(i % 251 for i in range(PAYLOAD_LENGTH))
PEER_PAYLOAD = bytes((i * 7 + 3) % 251 for i in range(PAYLOAD_LENGTH))
GUEST_TX_PATH = "/var/tmp/qemu-eg-frag-tx.pl"
GUEST_RX_PATH = "/var/tmp/qemu-eg-frag-rx.pl"
GUEST_RX_DATA = "/var/tmp/qemu-eg-frag-rx.bin"
GUEST_RX_READY = "QEMU_EG_FRAG_RX_READY_7b39"
GUEST_RX_DONE = "QEMU_EG_FRAG_RX_DONE_7b39"
GUEST_TX_DONE = "QEMU_EG_FRAG_TX_DONE_7b39"
BOOT_TIMEOUT_SECONDS = 1200
STEP_TIMEOUT_SECONDS = 90
FRAGMENT_TIMEOUT_SECONDS = 30


def _host_cksum(payload):
    result = subprocess.run(["cksum"], input=payload, capture_output=True,
                            check=True)
    fields = result.stdout.split()
    if len(fields) < 2:
        raise AssertionError(f"cksum returned an unexpected result: {fields!r}")
    return int(fields[0]), int(fields[1])


def _guest_tx_script(done=GUEST_TX_DONE):
    return f'''#!/usr/bin/perl
use strict;
use Socket;
$| = 1;
socket(SOCKET, PF_INET, SOCK_DGRAM, getprotobyname("udp")) or die "$!";
bind(SOCKET, sockaddr_in({GUEST_TX_PORT}, inet_aton("{GUEST_IP}")))
    or die "$!";
my $payload = pack("C*", map {{ $_ % 251 }} (0 .. {PAYLOAD_LENGTH - 1}));
my $destination = sockaddr_in({PEER_RX_PORT}, inet_aton("{PEER_IP}"));
my $sent = send(SOCKET, $payload, 0, $destination);
die "short UDP send" unless defined($sent) and $sent == length($payload);
close(SOCKET) or die "$!";
print "{done}\\n";
'''


def _guest_rx_script(ready=GUEST_RX_READY, done=GUEST_RX_DONE,
                    path=GUEST_RX_DATA, source_marker="QEMU_EG_FRAG_RX_SOURCE"):
    return f'''#!/usr/bin/perl
use strict;
use Socket;
$| = 1;
socket(SOCKET, PF_INET, SOCK_DGRAM, getprotobyname("udp")) or die "$!";
bind(SOCKET, sockaddr_in({GUEST_RX_PORT}, inet_aton("{GUEST_IP}")))
    or die "$!";
print "{ready}\\n";
my $payload = "";
my $source = recv(SOCKET, $payload, 65535, 0);
die "UDP receive failed: $!" unless defined($source);
my ($source_port, $source_addr) = sockaddr_in($source);
print "{source_marker}=" . inet_ntoa($source_addr) . ":$source_port\\n";
close(SOCKET) or die "$!";
open(PAYLOAD, ">{path}") or die "$!";
binmode(PAYLOAD);
print PAYLOAD $payload or die "$!";
close(PAYLOAD) or die "$!";
print "{done}\\n";
'''



def run_bidirectional_fragmented_udp(test, adapter, device_model,
                                    configured_mac):
    """Run the native fragmented UDP exchange against one SGI adapter."""
    guest_tx_path = GUEST_TX_PATH.replace("eg", adapter)
    guest_rx_path = GUEST_RX_PATH.replace("eg", adapter)
    guest_rx_data = GUEST_RX_DATA.replace("eg", adapter)
    tx_done = GUEST_TX_DONE.replace("EG", adapter.upper())
    rx_ready = GUEST_RX_READY.replace("EG", adapter.upper())
    rx_done = GUEST_RX_DONE.replace("EG", adapter.upper())
    source_marker = f"QEMU_{adapter.upper()}_FRAG_RX_SOURCE"
    prom = test.ASSET_FIRMWARE.fetch()
    disk = test.ASSET_DISK.fetch()
    if file_sha256(prom) != PROM_SHA256:
        test.fail("6.5.30 PROM SHA-256 mismatch")
    test.require_netdev("socket")

    peer, qemu_socket = socket.socketpair(
        socket.AF_UNIX, socket.SOCK_DGRAM)
    con = None
    try:
        vm = test.get_vm(name=f"irix_{adapter}_frag")
        vm.set_machine(MACHINES["origin200"])
        vm.set_console()
        qemu_socket.set_inheritable(True)
        vm.add_args(
            "-cpu", "R10000", "-smp", "1", "-m", "128M",
            "-bios", prom, "-audio", "none",
            "-chardev", f"file,id=elsc,path={test.log_file('elsc.log')}",
            "-serial", "none", "-serial", "chardev:elsc",
            "-drive", f"if=none,id=disk0,file={disk},format=qcow2",
            "-device", "scsi-hd,bus=scsi.0,scsi-id=1,lun=0,drive=disk0",
            "-snapshot", "-nic", "none",
            "-netdev", f"socket,id=netdev,fd={qemu_socket.fileno()}",
            "-device", (f"{device_model},netdev=netdev,addr=4,mac="
                        f"{configured_mac.hex(':')}")
        )
        vm.launch()
        qemu_socket.close()
        con = IrixConsole(vm.console_socket,
                          test.log_file(f"irix-{adapter}-frag-console.log"),
                          exited=lambda: vm.exitcode() is not None)
        drive_irix_to_shell(con, BOOT_TIMEOUT_SECONDS)
        if adapter == "eg":
            test.assertEqual(con.run("egconfig -f 1 eg0", "egconfig",
                                     STEP_TIMEOUT_SECONDS), 0)
            guest_ifname = "eg0"
        else:
            guest_ifname = "tg1"
        test.assertEqual(con.run(
            f"ifconfig {guest_ifname} inet {GUEST_IP} "
            "netmask 0xffffff00 up", "ifup", STEP_TIMEOUT_SECONDS), 0)
        if adapter == "tg":
            con.expect(rb"tg1: Link up[^\r\n]*", 180,
                       what="the tg1 link-up notice")
        con.capture_value(f"ifconfig {guest_ifname}", "ifcheck",
                          rb"(UP)[,>]", STEP_TIMEOUT_SECONDS)
        # Install the peer mapping so the guest's outbound datagram uses
        # the same raw socket peer as the injected inbound fragments.
        test.assertEqual(con.run(
            f"arp -s {PEER_IP} {PEER_MAC.hex(':')}", "arpentry",
            STEP_TIMEOUT_SECONDS), 0)

        test._stage_guest_script(con, guest_tx_path, _guest_tx_script(tx_done),
                                 "txscript")
        con.run(f"perl {guest_tx_path} & true", "txlaunch", 30)
        con.expect(tx_done.encode(), FRAGMENT_TIMEOUT_SECONDS,
                   what="guest UDP sender completion")
        guest_frames, guest_fragment_count = test._receive_guest_fragments(
            peer)
        test._assert_guest_fragment_mtu(guest_frames)
        test.assertGreaterEqual(guest_fragment_count, 2)
        source_macs = {
            frame[6:12] for frame in guest_frames
            if (fragment := parse_ipv4_udp_fragment(frame)) is not None
            and fragment["src_ip"] == GUEST_IP
            and fragment["dst_ip"] == PEER_IP
        }
        test.assertEqual(len(source_macs), 1,
                         "guest fragments used inconsistent Ethernet sources")
        guest_mac = source_macs.pop()
        test.assertNotEqual(guest_mac, bytes(6),
                            "guest fragment source MAC is zero")
        test.assertEqual(guest_mac[0] & 1, 0,
                         "guest fragment source MAC is multicast")
        if adapter == "eg":
            test.assertEqual(guest_mac, configured_mac,
                             "eg fragment source differs from configured MAC")

        test.assertEqual(con.run(f"rm -f {guest_rx_data}", "rxrm",
                                 STEP_TIMEOUT_SECONDS), 0)
        test._stage_guest_script(
            con, guest_rx_path,
            _guest_rx_script(rx_ready, rx_done, guest_rx_data, source_marker),
            "rxscript")
        con.run(f"perl {guest_rx_path} & true", "rxlaunch", 30)
        con.expect(rx_ready.encode(), FRAGMENT_TIMEOUT_SECONDS,
                   what="guest UDP receiver readiness")

        peer_frames = build_udp_fragments(
            PEER_MAC, guest_mac, PEER_IP, GUEST_IP,
            PEER_TX_PORT, GUEST_RX_PORT, PEER_PAYLOAD, mtu=1500,
            ident=0x4f52)
        test.assertGreaterEqual(len(peer_frames), 2)
        for frame in peer_frames:
            peer.send(frame)
        con.expect(re.escape(source_marker + "=10.0.2.2:51102").encode(),
                   FRAGMENT_TIMEOUT_SECONDS,
                   what="guest UDP sender address and port")
        con.expect(rx_done.encode(), FRAGMENT_TIMEOUT_SECONDS,
                   what="guest UDP receiver completion")

        guest_crc, guest_size = con.two_values(
            f"cksum {guest_rx_data}", "rxcksum",
            rf"(\d+)\s+(\d+)\s+{re.escape(guest_rx_data)}".encode(),
            STEP_TIMEOUT_SECONDS)
        guest_crc = int(guest_crc)
        host_crc, host_size = _host_cksum(PEER_PAYLOAD)
        test.assertEqual((guest_crc, guest_size), (host_crc, host_size),
                         "guest received UDP datagram differs from peer")
    finally:
        if con is not None:
            con.close()
        peer.close()
        qemu_socket.close()


class IrixEgFrag(QemuSystemTest):
    ASSET_FIRMWARE = LocalAsset(FIRMWARE_REL, PROM_SHA256)
    ASSET_DISK = LocalAsset(DISK_REL, DISK_SHA256)

    @skipBigDataTest()
    @skipSlowTest()
    @skipIfMissingEnv("QEMU_TEST_SGI_ASSET_DIR")
    def test_bidirectional_fragmented_udp(self):
        run_bidirectional_fragmented_udp(self, "eg", "tigon", GUEST_MAC)

    @staticmethod
    def _stage_guest_script(con, path, script, tag):
        payload = script.encode("ascii")
        if con.run(f"rm -f {path}", f"{tag}_rm", 60) != 0:
            raise AssertionError(f"could not remove old guest script {path}")
        for offset in range(0, len(payload), 40):
            encoded = payload[offset:offset + 40].hex()
            command = f'''perl -e 'print pack("H*","{encoded}")' >> {path}'''
            if con.run(command, f"{tag}_{offset}", 60) != 0:
                raise AssertionError(f"could not write guest script {path}")
        if con.run(f"chmod 700 {path}", f"{tag}_chmod", 60) != 0:
            raise AssertionError(f"could not chmod guest script {path}")

    def _assert_guest_fragment_mtu(self, frames):
        parsed = [parse_ipv4_udp_fragment(frame) for frame in frames]
        first = [fragment for fragment in parsed if fragment is not None
                 and fragment["src_ip"] == GUEST_IP
                 and fragment["dst_ip"] == PEER_IP
                 and fragment["offset"] == 0
                 and int.from_bytes(fragment["payload"][:2], "big")
                 == GUEST_TX_PORT
                 and int.from_bytes(fragment["payload"][2:4], "big")
                 == PEER_RX_PORT]
        self.assertEqual(len(first), 1, "guest UDP first fragment is missing")
        ident = first[0]["ident"]
        matching = [frame for frame, fragment in zip(frames, parsed)
                    if fragment is not None and fragment["ident"] == ident
                    and fragment["src_ip"] == GUEST_IP
                    and fragment["dst_ip"] == PEER_IP]
        self.assertGreaterEqual(len(matching), 2)
        self.assertEqual(max(map(len, matching)), 1514,
                         "guest did not transmit a full 1500-byte IP frame")

    @staticmethod
    def _receive_guest_fragments(peer):
        frames = []
        deadline = time.monotonic() + FRAGMENT_TIMEOUT_SECONDS
        last_error = None
        while time.monotonic() < deadline:
            remaining = deadline - time.monotonic()
            if remaining <= 0:
                break
            peer.settimeout(remaining)
            try:
                frame = peer.recv(65535)
            except socket.timeout:
                break
            if parse_ipv4_udp_fragment(frame) is None:
                continue
            frames.append(frame)
            try:
                reassemble_udp_fragments(
                    frames, GUEST_IP, PEER_IP, GUEST_TX_PORT, PEER_RX_PORT,
                    GUEST_PAYLOAD)
            except ValueError as error:
                last_error = error
                continue
            # The sender has finished. Drain any frames still queued after
            # the first complete set so a duplicate cannot evade the overlap
            # check by arriving immediately afterward.
            settle_deadline = time.monotonic() + 0.5
            while True:
                remaining = settle_deadline - time.monotonic()
                if remaining <= 0:
                    break
                peer.settimeout(remaining)
                try:
                    extra = peer.recv(65535)
                except socket.timeout:
                    break
                if parse_ipv4_udp_fragment(extra) is not None:
                    frames.append(extra)
            count = reassemble_udp_fragments(
                frames, GUEST_IP, PEER_IP, GUEST_TX_PORT, PEER_RX_PORT,
                GUEST_PAYLOAD)
            return frames, count
        raise AssertionError(
            "peer did not receive the complete fragmented guest UDP datagram"
            + (f": {last_error}" if last_error else ""))



if __name__ == "__main__":
    QemuSystemTest.main()
