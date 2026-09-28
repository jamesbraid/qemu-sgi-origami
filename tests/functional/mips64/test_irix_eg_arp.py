#!/usr/bin/env python3
#
# Peer-originated ARP reply on an IRIX eg adapter.
#
# SPDX-License-Identifier: GPL-2.0-or-later
"""Boot installed IRIX and require eg0 to answer one peer ARP request."""

import os
import socket
import time

from qemu_test import QemuSystemTest, skipBigDataTest, skipIfMissingEnv
from qemu_test import skipSlowTest
from qemu_test.asset import LocalAsset

from lib_net import IrixConsole, arp_request_frame, drive_irix_to_shell
from lib_net import parse_arp_frame
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
REPLY_TIMEOUT_SECONDS = 30


class IrixEgArp(QemuSystemTest):
    ASSET_FIRMWARE = LocalAsset(FIRMWARE_REL, PROM_SHA256)
    ASSET_DISK = LocalAsset(DISK_REL, DISK_SHA256)

    @skipBigDataTest()
    @skipSlowTest()
    @skipIfMissingEnv("QEMU_TEST_SGI_ASSET_DIR")
    def test_peer_arp_request_gets_guest_reply(self):
        prom = self.ASSET_FIRMWARE.fetch()
        disk = self.ASSET_DISK.fetch()
        if file_sha256(prom) != PROM_SHA256:
            self.fail("6.5.30 PROM SHA-256 mismatch")
        self.require_netdev("socket")

        peer, qemu_socket = socket.socketpair(
            socket.AF_UNIX, socket.SOCK_DGRAM)
        peer.settimeout(REPLY_TIMEOUT_SECONDS)
        con = None
        try:
            vm = self.get_vm(name="irix_eg_arp")
            vm.set_machine(MACHINES["origin200"])
            vm.set_console()
            qemu_socket.set_inheritable(True)
            vm.add_args(
                "-cpu", "R10000", "-smp", "1", "-m", "128M",
                "-bios", prom, "-audio", "none",
                "-chardev", f"file,id=elsc,path={self.log_file('elsc.log')}",
                "-serial", "none", "-serial", "chardev:elsc",
                "-drive", f"if=none,id=disk0,file={disk},format=qcow2",
                "-device", "scsi-hd,bus=scsi.0,scsi-id=1,lun=0,drive=disk0",
                "-snapshot", "-nic", "none",
                "-netdev", f"socket,id=egnet,fd={qemu_socket.fileno()}",
                "-device", (f"tigon,netdev=egnet,addr=4,mac="
                           f"{GUEST_MAC.hex(':')}")
            )
            vm.launch()
            qemu_socket.close()
            con = IrixConsole(vm.console_socket,
                              self.log_file("irix-eg-arp-console.log"),
                              exited=lambda: vm.exitcode() is not None)
            drive_irix_to_shell(con, 1200)
            self.assertEqual(con.run("egconfig -f 1 eg0", "egconfig", 180), 0)
            self.assertEqual(con.run(
                f"ifconfig eg0 inet {GUEST_IP} netmask 0xffffff00 up",
                "ifup", 60), 0)
            con.capture_value("ifconfig eg0", "ifcheck", rb"(UP)[,>]", 60)

            peer.send(arp_request_frame(PEER_MAC, PEER_IP, GUEST_IP))
            deadline = time.monotonic() + REPLY_TIMEOUT_SECONDS
            reply = None
            while True:
                remaining = deadline - time.monotonic()
                if remaining <= 0:
                    break
                peer.settimeout(remaining)
                try:
                    frame = peer.recv(65535)
                except socket.timeout:
                    break
                parsed = parse_arp_frame(frame)
                if (parsed and parsed["opcode"] == 2
                        and parsed["sender_ip"] == GUEST_IP
                        and parsed["target_ip"] == PEER_IP):
                    reply = parsed
                    break

            self.assertIsNotNone(
                reply, "eg0 did not answer the peer ARP request")
            self.assertEqual(reply["opcode"], 2)
            self.assertEqual(reply["sender_ip"], GUEST_IP)
            self.assertEqual(reply["target_ip"], PEER_IP)
            self.assertEqual(reply["ethernet_src"], GUEST_MAC)
            self.assertEqual(reply["ethernet_dst"], PEER_MAC)
            self.assertEqual(reply["sender_mac"], GUEST_MAC)
            self.assertEqual(reply["target_mac"], PEER_MAC)
        finally:
            if con is not None:
                con.close()
            peer.close()
            qemu_socket.close()


if __name__ == "__main__":
    QemuSystemTest.main()
